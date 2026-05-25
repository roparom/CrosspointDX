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

       // Long press toggles power directly from the device list.
        if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
          if (!confirmHeld) {
            confirmHeld = true;
            confirmLongHandled = false;
          } else if (confirmHeld && !confirmLongHandled && mappedInput.getHeldTime() > LONG_PRESS_MS) {
            // Long press toggles power directly from the list, always switching between OFF and 100% brightness.
            if (selectedDeviceIndex >= 0 && selectedDeviceIndex < (int)devices.size()) {
              bool newPower = !devices[selectedDeviceIndex].powerOn;
              setDevicePower(selectedDeviceIndex, newPower);
              if (newPower) {
                // When turning ON, set brightness to maximum (100%).
                setDeviceBrightness(selectedDeviceIndex, 255);
              }
              // Refresh status for the list entry.
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
            controlIndex = (controlIndex + 1) % 4;
            requestUpdate();
          });

          buttonNavigator.onPrevious([this] {
            controlIndex = (controlIndex == 0) ? 3 : controlIndex - 1;
            requestUpdate();
          });

          // Short press Confirm triggers the action for the highlighted item
          if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            if (controlIndex == 0) {
              setDevicePower(selectedDeviceIndex, !devices[selectedDeviceIndex].powerOn);
            } else if (controlIndex == 1) {
              uint8_t newBri = devices[selectedDeviceIndex].brightness;
              if (newBri < 255) {
                newBri = (newBri < 50) ? 50 : (newBri < 127) ? 127 : (newBri < 200) ? 200 : 255;
                setDeviceBrightness(selectedDeviceIndex, newBri);
              }
            } else if (controlIndex == 2) {
              uint16_t newEffect = devices[selectedDeviceIndex].effect + 1;
              if (newEffect > 255) newEffect = 255;
              setDeviceEffect(selectedDeviceIndex, newEffect);
            } else if (controlIndex == 3) {
               // Long press will open colour picker; short press does nothing
            }
            requestUpdate();
          }

          // Detect long press on Confirm to open colour selection popup when colour is highlighted
          if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
            if (!confirmHeld) {
              confirmHeld = true;
              confirmLongHandled = false;
            } else if (confirmHeld && !confirmLongHandled && mappedInput.getHeldTime() > LONG_PRESS_MS && controlIndex == 3) {
              // Transition to colour selection popup
              state = COLOUR_SELECTION;
              colorPresetIndex = 0;
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
          // Re‑use the buttonNavigator for cycling through presets
          buttonNavigator.onNext([this] {
            colorPresetIndex = (colorPresetIndex + 1) % (sizeof(COLOUR_PRESETS) / sizeof(COLOUR_PRESETS[0]));
            requestUpdate();
          });

          buttonNavigator.onPrevious([this] {
            colorPresetIndex = (colorPresetIndex == 0) ? (sizeof(COLOUR_PRESETS) / sizeof(COLOUR_PRESETS[0]) - 1) : colorPresetIndex - 1;
            requestUpdate();
          });

          // Confirm applies the colour
          if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            const auto& cp = COLOUR_PRESETS[colorPresetIndex];
            setDeviceColor(selectedDeviceIndex, cp.r, cp.g, cp.b);
            state = CONTROLLING_DEVICE;
            requestUpdate();
          }

          // Back cancels the popup
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

   // ---- Wi‑Fi status as subtitle below the main title ----
   const char* wifiStatus = (WiFi.status() == WL_CONNECTED) ? "On" : "Off";
   char wifiBuf[64];
   if (WiFi.status() == WL_CONNECTED) {
     const char* ssid = WiFi.SSID().c_str();
     snprintf(wifiBuf, sizeof(wifiBuf), "WiFi: %s | SSID:%s", wifiStatus, ssid);
   } else {
     snprintf(wifiBuf, sizeof(wifiBuf), "WiFi: %s | SSID: N/A", wifiStatus);
   }
   // Render subtitle centered horizontally just below the header.
   int subtitleY = metrics.topPadding + metrics.headerHeight + 2;
   int textWidth = strlen(wifiBuf) * 6; // approximate width for SMALL_FONT_ID
   int subtitleX = (pageWidth - textWidth) / 2;
   renderer.drawText(SMALL_FONT_ID, subtitleX, subtitleY, wifiBuf, true);

   // Start drawing the main content a few pixels below the header.
    // Leave space for the subtitle (Wi‑Fi status) rendered just below the header.
    int contentY = metrics.topPadding + metrics.headerHeight + 2 + 12; // subtitle height approx 12px + padding

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
      renderer.drawText(UI_12_FONT_ID, 30, contentY, "Devices:", true);
      contentY += 35;

       for (int i = 0; i < (int)devices.size(); i++) {
         const auto& dev = devices[i];
         bool selected = (i == selectedDeviceIndex);
         int y = contentY + i * 45;

         if (selected) {
           renderer.fillRect(20, y - 5, pageWidth - 40, 40, true);
         }

         const char* briName = BRIGHTNESS_NAMES[(int)getBrightnessLevel(dev.brightness)];
         // Show power, brightness level and colour name (if known).
          char status[160];
          if (dev.powerOn) {
            // Show full info when on.
            snprintf(status, sizeof(status), "%s [ON %s %s]",
                     dev.nickname.c_str(),
                     briName,
                     dev.colourName.c_str());
          } else {
            // When off, only show OFF.
            snprintf(status, sizeof(status), "%s [OFF]",
                     dev.nickname.c_str());
          }
         renderer.drawText(UI_12_FONT_ID, 30, y, status, !selected);
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

          renderer.drawText(SMALL_FONT_ID, 30, pageHeight - 50, "< Left/Right > | Back", true);
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
