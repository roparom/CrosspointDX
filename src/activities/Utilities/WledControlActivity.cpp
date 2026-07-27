#include "WledControlActivity.h"

#include <cstdio>
#include <cstring>

#include <HTTPClient.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include "activities/network/WifiSelectionActivity.h"

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include <utility>

// ---- Device I/O ----

void WledControlActivity::loadDevices() {
  devices.clear();

  if (!Storage.exists(DEVICES_FILE)) {
    LOG_DBG("WLED", "No devices file found");
    return;
  }

  FsFile file;
  if (!Storage.openFileForRead("MODULE", DEVICES_FILE, file)) {
    LOG_ERR("WLED", "Failed to open devices file");
    return;
  }

  char buffer[512];
  int bytesRead = file.read(buffer, sizeof(buffer) - 1);
  if (bytesRead <= 0) {
    LOG_DBG("WLED", "Devices file is empty");
    return;
  }
  buffer[bytesRead] = '\0';

  char* line = strtok(buffer, "\n");
  while (line != nullptr) {
    char* pipe = strchr(line, '|');
    if (pipe != nullptr) {
      *pipe = '\0';
      std::string nickname(line);
      std::string ipAddress(pipe + 1);

      // Trim whitespace
      while (!nickname.empty() && nickname.back() == ' ') nickname.pop_back();
      while (!nickname.empty() && nickname[0] == ' ') nickname.erase(0, 1);
      while (!ipAddress.empty() && ipAddress.back() == ' ') ipAddress.pop_back();
      while (!ipAddress.empty() && ipAddress[0] == ' ') ipAddress.erase(0, 1);

      if (!nickname.empty() && !ipAddress.empty()) {
        WledDevice device;
        device.nickname = nickname;
        device.ipAddress = ipAddress;
        devices.push_back(device);
      }
    }
    line = strtok(nullptr, "\n");
  }

  LOG_DBG("WLED", "Loaded %d devices", (int)devices.size());
}

void WledControlActivity::saveDevices() {
  Storage.mkdir("/biscuit");

  FsFile file;
  if (!Storage.openFileForWrite("MODULE", DEVICES_FILE, file)) {
    LOG_ERR("WLED", "Failed to save devices file");
    showError("Failed to save devices");
    return;
  }

  for (const auto& device : devices) {
    char line[256];
    snprintf(line, sizeof(line), "%s|%s\n", device.nickname.c_str(), device.ipAddress.c_str());
    file.write(reinterpret_cast<const uint8_t*>(line), strlen(line));
  }

  LOG_DBG("WLED", "Saved %d devices", (int)devices.size());
}

// ---- HTTP API ----

bool WledControlActivity::pollDeviceStatus(int deviceIndex) {
  if (deviceIndex < 0 || deviceIndex >= (int)devices.size()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    showError("WiFi not connected");
    return false;
  }

  WledDevice& device = devices[deviceIndex];
  std::string url = "http://" + device.ipAddress + "/json";

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url.c_str())) {
    showError("HTTP init failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    showError("Device unreachable");
    return false;
  }

  String payload = http.getString();
  http.end();

  // Very basic JSON parsing (no JSON library to keep RAM light)
  // Look for: "on":true/false, "bri":0-255, "effect":0-N
  if (payload.indexOf("\"on\":true") >= 0) {
    device.powerOn = true;
  } else if (payload.indexOf("\"on\":false") >= 0) {
    device.powerOn = false;
  }

  // Extract brightness: "bri":128
  int briPos = payload.indexOf("\"bri\":");
  if (briPos >= 0) {
    int briVal = 0;
    if (sscanf(payload.c_str() + briPos, "\"bri\":%d", &briVal) == 1) {
      device.brightness = static_cast<uint8_t>(briVal < 0 ? 0 : (briVal > 255 ? 255 : briVal));
    }
  }

  int effectPos = payload.indexOf("\"effect\":");
  if (effectPos >= 0) {
    int effectVal = 0;
    if (sscanf(payload.c_str() + effectPos, "\"effect\":%d", &effectVal) == 1) {
      device.effect = static_cast<uint16_t>(effectVal < 0 ? 0 : effectVal);
    }
  }

  device.lastUpdate = millis();
  return true;
}

// ---------------------------------------------------------------------------
// Colour presets – a small selection of common colours. The UI will allow the
// user to cycle through these presets when the controlIndex is set to colour.
// ---------------------------------------------------------------------------
struct ColourPreset {
  const char* name;
  uint8_t r, g, b;
};

static constexpr ColourPreset COLOUR_PRESETS[] = {
    {"Red",   255,   0,   0},
    {"Green",   0, 255,   0},
    {"Blue",    0,   0, 255},
    {"White", 255, 255, 255},
    {"Yellow", 255, 255,   0},
    {"Cyan",     0, 255, 255},
    {"Magenta",255,   0, 255},
    {"Black",     0,   0,   0}
};

// The earlier duplicate implementation of `fetchPresets` (lines 172‑224) has been
// removed to resolve the redefinition error. The later implementation starting
// at line 445 provides the same functionality with clearer parsing logic and
// is retained.

bool WledControlActivity::setDeviceColor(int deviceIndex, uint8_t r, uint8_t g, uint8_t b) {
  if (deviceIndex < 0 || deviceIndex >= (int)devices.size()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    showError("WiFi not connected");
    return false;
  }

  WledDevice& device = devices[deviceIndex];
  std::string url = "http://" + device.ipAddress + "/json";

  char bodyBuf[128];
  // Payload format accepted by WLED for setting colour of the first segment.
  snprintf(bodyBuf, sizeof(bodyBuf), "{\"seg\":{\"col\":[[%d,%d,%d]]}}", r, g, b);
  std::string body(bodyBuf);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  if (!http.begin(url.c_str())) {
    showError("HTTP init failed");
    return false;
  }

  int httpCode = http.POST((uint8_t*)body.c_str(), body.length());
  http.end();

  if (httpCode != 200) {
    showError("Colour change failed");
    return false;
  }

  // Show transient "sending" message until status is refreshed
  commandMessage = "Sending colour...";
  commandMessageTime = millis();

  // Update the stored colour name based on the preset that matches the RGB values.
  // This is a best‑effort lookup; if no preset matches, keep the previous name.
  for (const auto &cp : COLOUR_PRESETS) {
    if (cp.r == r && cp.g == g && cp.b == b) {
      devices[deviceIndex].colourName = cp.name;
      break;
    }
  }

  // Refresh status to keep UI in sync.
  pollDeviceStatus(deviceIndex);
  // Ensure UI updates after status refresh
  requestUpdate();
  return true;
}

bool WledControlActivity::setDevicePower(int deviceIndex, bool on) {
  if (deviceIndex < 0 || deviceIndex >= (int)devices.size()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    showError("WiFi not connected");
    return false;
  }

  WledDevice& device = devices[deviceIndex];
  std::string url = "http://" + device.ipAddress + "/json";
  std::string body = on ? "{\"on\":true}" : "{\"on\":false}";

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  if (!http.begin(url.c_str())) {
    showError("HTTP init failed");
    return false;
  }

  int httpCode = http.POST((uint8_t*)body.c_str(), body.length());
  http.end();

  if (httpCode != 200) {
    showError("Power change failed");
    return false;
  }

  // Show transient message while the command is being processed.
  commandMessage = on ? "Sending power ON..." : "Sending power OFF...";
  commandMessageTime = millis();

  // Optimistically update the cached power state so the UI reflects the change
  // immediately. The subsequent poll will correct the state if the device reports
  // something different.
  device.powerOn = on;
  device.lastUpdate = millis();
  // Refresh full status to keep UI in sync (e.g., brightness may have changed on the device).
  pollDeviceStatus(deviceIndex);
  requestUpdate();
  return true;
}

bool WledControlActivity::setDeviceBrightness(int deviceIndex, uint8_t brightness) {
  if (deviceIndex < 0 || deviceIndex >= (int)devices.size()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    showError("WiFi not connected");
    return false;
  }

  WledDevice& device = devices[deviceIndex];
  std::string url = "http://" + device.ipAddress + "/json";

  char bodyBuf[64];
  snprintf(bodyBuf, sizeof(bodyBuf), "{\"bri\":%d}", brightness);
  std::string body(bodyBuf);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  if (!http.begin(url.c_str())) {
    showError("HTTP init failed");
    return false;
  }

  int httpCode = http.POST((uint8_t*)body.c_str(), body.length());
  http.end();

  if (httpCode != 200) {
    showError("Brightness change failed");
    return false;
  }

  commandMessage = "Sending brightness...";
  commandMessageTime = millis();

  device.brightness = brightness;
  device.lastUpdate = millis();
  pollDeviceStatus(deviceIndex);
  requestUpdate();
  return true;
}

bool WledControlActivity::setDeviceEffect(int deviceIndex, uint16_t effect) {
  if (deviceIndex < 0 || deviceIndex >= (int)devices.size()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    showError("WiFi not connected");
    return false;
  }

  WledDevice& device = devices[deviceIndex];
  std::string url = "http://" + device.ipAddress + "/json";

  char bodyBuf[64];
  snprintf(bodyBuf, sizeof(bodyBuf), "{\"effect\":%d}", effect);
  std::string body(bodyBuf);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  if (!http.begin(url.c_str())) {
    showError("HTTP init failed");
    return false;
  }

  int httpCode = http.POST((uint8_t*)body.c_str(), body.length());
  http.end();

  if (httpCode != 200) {
    showError("Effect change failed");
    return false;
  }

  commandMessage = "Sending effect...";
  commandMessageTime = millis();

  device.effect = effect;
  device.lastUpdate = millis();
  pollDeviceStatus(deviceIndex);
  requestUpdate();
  return true;
}

// ---------------------------------------------------------------------------
// Activate a stored preset on the device. The WLED API expects a JSON payload
// of the form {"ps":<presetId>}. This mirrors the colour‑setting helpers.
// ---------------------------------------------------------------------------
bool WledControlActivity::activatePreset(int deviceIndex, int presetId) {
    if (deviceIndex < 0 || deviceIndex >= (int)devices.size()) return false;
    if (WiFi.status() != WL_CONNECTED) {
        showError("WiFi not connected");
        return false;
    }

    WledDevice& dev = devices[deviceIndex];
    std::string url = "http://" + dev.ipAddress + "/json";

    char bodyBuf[64];
    snprintf(bodyBuf, sizeof(bodyBuf), "{\"ps\":%d}", presetId);
    std::string body(bodyBuf);

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    if (!http.begin(url.c_str())) {
        showError("HTTP init failed");
        return false;
    }

    int httpCode = http.POST((uint8_t*)body.c_str(), body.length());
    http.end();
    if (httpCode != 200) {
        showError("Preset activation failed");
        return false;
    }
    // Optimistically update UI – the preset name will be reflected after a status poll.
    commandMessage = "Activating preset...";
    commandMessageTime = millis();
    pollDeviceStatus(deviceIndex);
    requestUpdate();
    return true;
}

// ---------------------------------------------------------------------------
// Fetch preset list from the selected WLED device.
// The JSON returned by "/json/presets" is a simple map of ID to an object
// containing a "n" field with the preset name. We perform a lightweight parse
// without pulling in a full JSON library to keep RAM usage low.
// ---------------------------------------------------------------------------
bool WledControlActivity::fetchPresets(int deviceIndex) {
    if (deviceIndex < 0 || deviceIndex >= (int)devices.size()) return false;
    if (WiFi.status() != WL_CONNECTED) {
        showError("WiFi not connected");
        return false;
    }

    WledDevice& dev = devices[deviceIndex];
    std::string url = "http://" + dev.ipAddress + "/json/presets";

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(url.c_str())) {
        showError("HTTP init failed");
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != 200) {
        http.end();
        showError("Failed to fetch presets");
        return false;
    }

    String payload = http.getString();
    http.end();

    // Clear any existing presets for this device.
    dev.presets.clear();

    const char* data = payload.c_str();
    const char* p = data;
    while (p && *p) {
        // Look for an ID string followed by a colon
        const char* idStart = strchr(p, '"');
        if (!idStart) break;
        const char* idEnd = strchr(idStart + 1, '"');
        if (!idEnd) break;
        std::string idStr(idStart + 1, idEnd - idStart - 1);
        // Move past the closing quote and any whitespace/colon
        const char* afterId = strchr(idEnd, ':');
        if (!afterId) break;
        // Find the name field within this object
        const char* nameKey = strstr(afterId, "\"n\"");
        if (!nameKey) break;
        const char* nameStart = strchr(nameKey + 3, '"');
        if (!nameStart) break;
        const char* nameEnd = strchr(nameStart + 1, '"');
        if (!nameEnd) break;
        std::string name(nameStart + 1, nameEnd - nameStart - 1);
        int id = std::stoi(idStr);
        dev.presets.emplace_back(id, name);
        p = nameEnd + 1;
    }
    return true;
}


void WledControlActivity::showError(const std::string& msg) {
  errorMessage = msg;
  errorTime = millis();
  LOG_DBG("WLED", "Error displayed: %s (time=%lu)", msg.c_str(), errorTime);
}

WledControlActivity::BrightnessLevel WledControlActivity::getBrightnessLevel(uint8_t value) const {
  if (value == 0) return BRIGHTNESS_OFF;
  if (value <= 75) return BRIGHTNESS_LOW;
  if (value <= 175) return BRIGHTNESS_MEDIUM;
  if (value <= 225) return BRIGHTNESS_HIGH;
  return BRIGHTNESS_MAX;
}

  // ---- Lifecycle ----

  void WledControlActivity::onEnter() {
  Activity::onEnter();
  LOG_DBG("WLED", "onEnter called - initializing activity");
  // Ensure UI theme metrics are up‑to‑date before any rendering.
  UITheme::getInstance().reload();

  // Initialise activity state.
  state = MENU;
  menuIndex = 0;
  selectedDeviceIndex = -1;
  controlIndex = 0;
  // Consume flags control whether the first button press after entering the activity is ignored.
  // Previously both flags were set to true, causing the first Confirm press to be consumed and
  // requiring a second press to trigger actions. This resulted in the reported double‑press issue.
  // Initialise them to false so that button presses work immediately.
  consumeConfirm = false;
  consumeBack = false;

  // Load any persisted devices.
  loadDevices();
  LOG_DBG("WLED", "Loaded %zu devices after onEnter", devices.size());
  // Request a UI update now that state and theme are ready.
  requestUpdate();
}

// Callback invoked when the Wi‑Fi selection sub‑activity finishes.
void WledControlActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    showError("WiFi not connected");
    // Remain in MENU; user can retry.
    state = MENU;
    requestUpdate();
    return;
  }
  // Wi‑Fi now connected – reload devices and refresh UI.
  loadDevices();
  requestUpdate();
}

void WledControlActivity::onExit() {
  Activity::onExit();
  devices.clear();
}

// ---- Input Handling ----

  void WledControlActivity::loop() {
    // Clear transient error overlay
    if (millis() - errorTime > ERROR_DISPLAY_MS && !errorMessage.empty()) {
      LOG_DBG("WLED", "Clearing error overlay after %lu ms", millis() - errorTime);
      errorMessage.clear();
    }
    // Clear transient command message overlay
    if (millis() - commandMessageTime > COMMAND_MSG_DISPLAY_MS && !commandMessage.empty()) {
      LOG_DBG("WLED", "Clearing command overlay after %lu ms", millis() - commandMessageTime);
      commandMessage.clear();
    }


  // Preserve original navigation handling using consume flags.
  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  switch (state) {
    case MENU: {
      buttonNavigator.onNext([this] {
        menuIndex = ButtonNavigator::nextIndex(menuIndex, getMenuItemCount());
        requestUpdate();
      });

      buttonNavigator.onPrevious([this] {
        menuIndex = ButtonNavigator::previousIndex(menuIndex, getMenuItemCount());
        requestUpdate();
      });

        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
          // Menu indices after reordering: 0=WiFi, 1=View Devices, 2=Add Device, 3=Remove Device
          if (menuIndex == 0) {
            // Wi‑Fi setup
            startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                                   [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
          } else if (menuIndex == 1) {
            // View Devices
            if (devices.empty()) {
              showError("No devices configured");
              requestUpdate();
            } else {
              state = VIEWING_DEVICES;
              selectedDeviceIndex = 0;
              requestUpdate();
            }
          } else if (menuIndex == 2) {
            // Add Device
            state = ADDING_DEVICE_NAME;
            newDeviceName.clear();
            newDeviceIp.clear();
            startActivityForResult(
                std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Device Name", "", 64),
                [this](const ActivityResult& result) {
                  if (!result.isCancelled) {
                    newDeviceName = std::get<KeyboardResult>(result.data).text;
                    state = ADDING_DEVICE_IP;
                    startActivityForResult(
                        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "IP Address", "192.168.1.", 64),
                        [this](const ActivityResult& ipResult) {
                          if (!ipResult.isCancelled) {
                            newDeviceIp = std::get<KeyboardResult>(ipResult.data).text;
                            WledDevice newDev;
                            newDev.nickname = newDeviceName;
                            newDev.ipAddress = newDeviceIp;
                            devices.push_back(newDev);
                            saveDevices();
                            showError("Device added!");
                          }
                          state = MENU;
                          requestUpdate();
                        });
                  } else {
                    state = MENU;
                    requestUpdate();
                  }
                });
          } else if (menuIndex == 3) {
            // Remove Device
            if (devices.empty()) {
              showError("No devices to remove");
            } else {
              state = REMOVING_DEVICE;
              selectedDeviceIndex = 0;
              requestUpdate();
            }
          }
        }

      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
      }
      break;
    }

        case VIEWING_DEVICES: {
       // Refresh status of all devices when first entering the list to ensure up‑to‑date info.
       static bool firstEntry = true;
       if (firstEntry) {
         for (size_t i = 0; i < devices.size(); ++i) {
           pollDeviceStatus(i);
         }
         firstEntry = false;
       }

       buttonNavigator.onNext([this] {
         selectedDeviceIndex = ButtonNavigator::nextIndex(selectedDeviceIndex, devices.size());
         requestUpdate();
       });

       buttonNavigator.onPrevious([this] {
         selectedDeviceIndex = ButtonNavigator::previousIndex(selectedDeviceIndex, devices.size());
         requestUpdate();
       });

        // Short press opens the control screen for the selected device.
        // Short press opens the control screen only if a long‑press hasn't already handled the action.
        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !confirmLongHandled) {
          state = CONTROLLING_DEVICE;
          controlIndex = 0;
          pollDeviceStatus(selectedDeviceIndex);
          requestUpdate();
        }

        // Long press toggles power directly from the device list, preserving previous brightness.
        if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
          if (!confirmHeld) {
            confirmHeld = true;
            confirmLongHandled = false;
          } else if (confirmHeld && !confirmLongHandled && mappedInput.getHeldTime() > LONG_PRESS_MS) {
            if (selectedDeviceIndex >= 0 && selectedDeviceIndex < (int)devices.size()) {
              bool newPower = !devices[selectedDeviceIndex].powerOn;
              setDevicePower(selectedDeviceIndex, newPower);
              // Do not modify brightness; keep stored value.
              pollDeviceStatus(selectedDeviceIndex);
              requestUpdate();
            }
            confirmLongHandled = true;
          }
        }

       // Reset hold flags on release.
       if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
         // Reset flags; short‑press action will be ignored if a long‑press was handled.
         confirmHeld = false;
         // keep confirmLongHandled true until next loop to suppress short press
       }

       if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
         state = MENU;
         menuIndex = 1;
         requestUpdate();
       }
      break;
    }

        case CONTROLLING_DEVICE: {
          // Navigation between the four control items (highlight only)
          buttonNavigator.onNext([this] {
            // When the colour control is highlighted and the device provides presets,
            // advance through the preset list instead of the generic colour index.
            if (controlIndex == 3 && !devices[selectedDeviceIndex].presets.empty()) {
              presetIndex = (presetIndex + 1) % devices[selectedDeviceIndex].presets.size();
            } else {
              controlIndex = (controlIndex + 1) % 4;
            }
            requestUpdate();
          });

          buttonNavigator.onPrevious([this] {
            if (controlIndex == 3 && !devices[selectedDeviceIndex].presets.empty()) {
              presetIndex = (presetIndex == 0) ? (devices[selectedDeviceIndex].presets.size() - 1) : presetIndex - 1;
            } else {
              controlIndex = (controlIndex == 0) ? 3 : controlIndex - 1;
            }
            requestUpdate();
          });

          // Short press Confirm triggers the action for the highlighted item
          if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            if (controlIndex == 0) {
              setDevicePower(selectedDeviceIndex, !devices[selectedDeviceIndex].powerOn);
            } else if (controlIndex == 1) {
                // Cycle through defined brightness levels (OFF, LOW, MEDIUM, HIGH, MAX)
                static const uint8_t levels[] = {0, 50, 127, 200, 255};
                uint8_t cur = devices[selectedDeviceIndex].brightness;
                // Find current level index
                size_t idx = 0;
                for (size_t i = 0; i < sizeof(levels); ++i) {
                    if (cur <= levels[i]) { idx = i; break; }
                }
                // Advance to next level, wrap around
                size_t nextIdx = (idx + 1) % (sizeof(levels) / sizeof(levels[0]));
                uint8_t newBri = levels[nextIdx];
                setDeviceBrightness(selectedDeviceIndex, newBri);
            } else if (controlIndex == 2) {
                uint16_t newEffect = devices[selectedDeviceIndex].effect + 1;
                if (newEffect > 255) newEffect = 255;
                setDeviceEffect(selectedDeviceIndex, newEffect);
            } else if (controlIndex == 3) {
                // Short press cycles colour presets directly
                size_t presetCount = sizeof(COLOUR_PRESETS) / sizeof(COLOUR_PRESETS[0]);
                size_t nextIdx = (colorPresetIndex + 1) % presetCount;
                const auto &cp = COLOUR_PRESETS[nextIdx];
                setDeviceColor(selectedDeviceIndex, cp.r, cp.g, cp.b);
                // Update UI index to reflect the new preset
                colorPresetIndex = nextIdx;
            }
            requestUpdate();
          }

          // Detect long press on Confirm to open colour selection popup when colour is highlighted
            if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
              if (!confirmHeld) {
                confirmHeld = true;
                confirmLongHandled = false;
              } else if (confirmHeld && !confirmLongHandled && mappedInput.getHeldTime() > LONG_PRESS_MS && controlIndex == 3) {
                // Transition to preset selection popup. Fetch presets for the device.
                state = COLOUR_SELECTION;
                presetIndex = 0;
                fetchPresets(selectedDeviceIndex);
                // Reset hold flags to avoid re‑entering immediately on next loop
                confirmHeld = false;
                confirmLongHandled = true;
                requestUpdate();
              }
            }

          // Reset hold flags on release of Confirm (or when leaving this state)
          if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            confirmHeld = false;
            confirmLongHandled = false;
          }

          if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            state = VIEWING_DEVICES;
            requestUpdate();
          }
          break;
        }

        // ---------------------------------------------------------------------
        // Colour selection popup – shows the preset list and allows the user to
        // pick a colour. Navigation uses the same Next/Previous handlers as the
        // main control screen. Confirm applies the selected colour and returns
        // to CONTROLLING_DEVICE. Back cancels without changing the colour.
        // ---------------------------------------------------------------------
        case COLOUR_SELECTION: {
          // Navigation cycles through the preset vector.
          buttonNavigator.onNext([this] {
            if (!devices.empty()) {
              const auto& list = devices[selectedDeviceIndex].presets;
              if (!list.empty()) {
                presetIndex = (presetIndex + 1) % list.size();
                requestUpdate();
              }
            }
          });

          buttonNavigator.onPrevious([this] {
            if (!devices.empty()) {
              const auto& list = devices[selectedDeviceIndex].presets;
              if (!list.empty()) {
                presetIndex = (presetIndex == 0) ? (list.size() - 1) : presetIndex - 1;
                requestUpdate();
              }
            }
          });

          // Confirm activates the selected preset.
          if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            if (!devices.empty()) {
              const auto& list = devices[selectedDeviceIndex].presets;
              if (!list.empty() && presetIndex < (int)list.size()) {
                int presetId = list[presetIndex].first;
                activatePreset(selectedDeviceIndex, presetId);
              }
            }
            state = CONTROLLING_DEVICE;
            requestUpdate();
          }

          // Back cancels the popup.
          if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            state = CONTROLLING_DEVICE;
            requestUpdate();
          }
          break;
        }

    case REMOVING_DEVICE: {
      buttonNavigator.onNext([this] {
        selectedDeviceIndex = ButtonNavigator::nextIndex(selectedDeviceIndex, devices.size());
        requestUpdate();
      });

      buttonNavigator.onPrevious([this] {
        selectedDeviceIndex = ButtonNavigator::previousIndex(selectedDeviceIndex, devices.size());
        requestUpdate();
      });

      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        if (selectedDeviceIndex >= 0 && selectedDeviceIndex < (int)devices.size()) {
          devices.erase(devices.begin() + selectedDeviceIndex);
          saveDevices();
          showError("Device removed");
        }
        state = MENU;
        menuIndex = 2;
        requestUpdate();
      }

      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = MENU;
        menuIndex = 2;
        requestUpdate();
      }
      break;
    }

    default:
      break;
  }
}

// ---- Rendering ----

void WledControlActivity::render(RenderLock&&) {
  LOG_DBG("WLED", "render start - state=%d", state);
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  LOG_DBG("WLED", "Screen size %dx%d, topPadding=%d, headerHeight=%d", pageWidth, pageHeight, metrics.topPadding, metrics.headerHeight);
  // Verify font IDs are non‑zero (valid) – these are compile‑time constants but we log them for safety
  LOG_DBG("WLED", "Font IDs: UI_12_FONT_ID=%d, SMALL_FONT_ID=%d", UI_12_FONT_ID, SMALL_FONT_ID);


   // Draw main title
   GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "WLED Control");

     // ---- WiFi status in header, top left ----
     char wifiStatus[64];
     if (WiFi.status() == WL_CONNECTED) {
       int32_t rssi = WiFi.RSSI();
       int bars = 0;
       if (rssi > -55) bars = 4;
       else if (rssi > -65) bars = 3;
       else if (rssi > -75) bars = 2;
       else if (rssi > -85) bars = 1;
       else bars = 0;
       // Unicode block chars for bars (▁▂▃▄▅), or use ASCII fallback
       const char* barStr[] = {"▁", "▂", "▃", "▄", "▅"};
       snprintf(wifiStatus, sizeof(wifiStatus), "%s %s", barStr[bars], WiFi.SSID().c_str());
     } else {
       snprintf(wifiStatus, sizeof(wifiStatus), "off N/A");
     }
     // Top-left in header: 12px padding from left, vertically centered in header
     int wifiX = 12;
     int wifiY = metrics.topPadding + (metrics.headerHeight - 12) / 2;
     renderer.drawText(SMALL_FONT_ID, wifiX, wifiY, wifiStatus, true);

   // Start drawing the main content below the header (no subtitle)
   int contentY = metrics.topPadding + metrics.headerHeight + 10;

      // Error message overlay
      if (!errorMessage.empty()) {
        const int msgX = 20;
        const int msgY = pageHeight - 60;
        const int msgW = pageWidth - 40;
        const int msgH = 50;
        LOG_DBG("WLED", "Rendering error overlay at (%d,%d) size %dx%d: %s", msgX, msgY, msgW, msgH, errorMessage.c_str());
        renderer.fillRect(msgX, msgY, msgW, msgH, true);
        renderer.drawText(SMALL_FONT_ID, msgX + 10, msgY + 18, errorMessage.c_str(), false);
      }
      // Command message overlay (e.g., "Sending ...")
      if (!commandMessage.empty()) {
        const int msgX = 20;
        const int msgY = pageHeight - 110; // above error overlay
        const int msgW = pageWidth - 40;
        const int msgH = 40;
        LOG_DBG("WLED", "Rendering command overlay at (%d,%d) size %dx%d: %s", msgX, msgY, msgW, msgH, commandMessage.c_str());
        renderer.fillRect(msgX, msgY, msgW, msgH, true);
        renderer.drawText(SMALL_FONT_ID, msgX + 10, msgY + 14, commandMessage.c_str(), false);
      }

  switch (state) {
   case MENU: {
   LOG_DBG("WLED", "rendering MENU, menuIndex=%d", menuIndex);
    // Updated order: WiFi, View Devices, Add Device, Remove Device
    const char* items[] = {"WiFi", "View Devices", "Add Device", "Remove Device"};
   // Add extra top padding so the menu does not overlap the subtitle.
   int menuStartY = contentY + 20; // 20px additional spacing
   for (int i = 0; i < 4; i++) {
         bool selected = (i == menuIndex);
         int y = menuStartY + i * 40;
         if (selected) {
           renderer.fillRect(20, y - 5, pageWidth - 40, 30, true);
         }
         renderer.drawText(UI_12_FONT_ID, 40, y, items[i], !selected);
   }
   break;
   }

    case VIEWING_DEVICES: {
      LOG_DBG("WLED", "rendering VIEWING_DEVICES, selectedDeviceIndex=%d", selectedDeviceIndex);
      // Render status bar in the menu bar area for the selected device.
      if (selectedDeviceIndex >= 0 && selectedDeviceIndex < (int)devices.size()) {
        const auto& dev = devices[selectedDeviceIndex];
        const char* briName = BRIGHTNESS_NAMES[(int)getBrightnessLevel(dev.brightness)];
        char status[160];
        if (dev.powerOn) {
          snprintf(status, sizeof(status), "%s [ON %s %s]", dev.nickname.c_str(), briName, dev.colourName.c_str());
        } else {
          snprintf(status, sizeof(status), "%s [OFF]", dev.nickname.c_str());
        }
        // Position status in the top‑left of the menu bar (below the main title).
        int statusY = metrics.topPadding + metrics.headerHeight + 2;
        int statusX = 20; // left margin
        renderer.drawText(SMALL_FONT_ID, statusX, statusY, status, true);
      }

      renderer.drawText(UI_12_FONT_ID, 30, contentY, "Devices:", true);
      contentY += 35;

       for (int i = 0; i < (int)devices.size(); i++) {
         const auto& dev = devices[i];
         bool selected = (i == selectedDeviceIndex);
         int y = contentY + i * 45;

         if (selected) {
           renderer.fillRect(20, y - 5, pageWidth - 40, 40, true);
         }
         // Show only nickname in the list.
         renderer.drawText(UI_12_FONT_ID, 30, y, dev.nickname.c_str(), !selected);
       }
       break;
     }

      case CONTROLLING_DEVICE: {
        LOG_DBG("WLED", "rendering CONTROLLING_DEVICE, selectedDeviceIndex=%d, controlIndex=%d", selectedDeviceIndex, controlIndex);
        if (selectedDeviceIndex >= 0 && selectedDeviceIndex < (int)devices.size()) {
          const auto& dev = devices[selectedDeviceIndex];

          char header[128];
          snprintf(header, sizeof(header), "%s (%s)", dev.nickname.c_str(), dev.ipAddress.c_str());
          renderer.drawText(UI_12_FONT_ID, 30, contentY, header, true);
          contentY += 40;

          // Power
          bool powerSelected = (controlIndex == 0);
          char powerStr[64];
          snprintf(powerStr, sizeof(powerStr), "Power: %s", dev.powerOn ? "ON" : "OFF");
          int powerY = contentY;
          if (powerSelected) renderer.fillRect(20, powerY - 5, pageWidth - 40, 30, true);
          renderer.drawText(UI_12_FONT_ID, 30, powerY, powerStr, !powerSelected);
          contentY += 40;

          // Brightness
          bool briSelected = (controlIndex == 1);
          char briStr[64];
          const char* briName = BRIGHTNESS_NAMES[(int)getBrightnessLevel(dev.brightness)];
          snprintf(briStr, sizeof(briStr), "Brightness: %s (%d)", briName, dev.brightness);
          int briY = contentY;
          if (briSelected) renderer.fillRect(20, briY - 5, pageWidth - 40, 30, true);
          renderer.drawText(UI_12_FONT_ID, 30, briY, briStr, !briSelected);
          contentY += 40;

          // Effect
          bool effectSelected = (controlIndex == 2);
          char effectStr[64];
          snprintf(effectStr, sizeof(effectStr), "Effect: %d", dev.effect);
          int effectY = contentY;
          if (effectSelected) renderer.fillRect(20, effectY - 5, pageWidth - 40, 30, true);
          renderer.drawText(UI_12_FONT_ID, 30, effectY, effectStr, !effectSelected);
          contentY += 40;

          // Colour preset
          bool colourSelected = (controlIndex == 3);
          const auto& cp = COLOUR_PRESETS[colorPresetIndex];
          char colourStr[64];
          snprintf(colourStr, sizeof(colourStr), "Colour: %s", cp.name);
          int colourY = contentY;
          if (colourSelected) renderer.fillRect(20, colourY - 5, pageWidth - 40, 30, true);
          renderer.drawText(UI_12_FONT_ID, 30, colourY, colourStr, !colourSelected);
          contentY += 40;

          //renderer.drawText(SMALL_FONT_ID, 30, pageHeight - 50, "< Left/Right > | Back", true); // Hide Footer Navigation for now
        }
        break;
      }

    case REMOVING_DEVICE: {
      LOG_DBG("WLED", "rendering REMOVING_DEVICE, selectedDeviceIndex=%d", selectedDeviceIndex);
      renderer.drawText(UI_12_FONT_ID, 30, contentY, "Select to remove:", true);
      contentY += 35;

      for (int i = 0; i < (int)devices.size(); i++) {
        const auto& dev = devices[i];
        bool selected = (i == selectedDeviceIndex);
        int y = contentY + i * 40;

        if (selected) {
          renderer.fillRect(20, y - 5, pageWidth - 40, 35, true);
        }

        renderer.drawText(UI_12_FONT_ID, 30, y, dev.nickname.c_str(), !selected);
      }
      break;
    }

  default:
    renderer.drawText(UI_12_FONT_ID, 30, contentY, "Loading...", true);
  }
  // Flush the drawing commands to the e‑ink display.
  renderer.displayBuffer();
}
