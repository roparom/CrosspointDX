#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class WledControlActivity final : public Activity {
 public:
  explicit WledControlActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WledControl", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Callback invoked after the Wi‑Fi selection sub‑activity finishes
  void onWifiSelectionComplete(const bool connected);

 private:
  enum State {
    MENU,
    WIFI_SETUP,
    VIEWING_DEVICES,
    CONTROLLING_DEVICE,
    COLOUR_SELECTION,
    ADDING_DEVICE_NAME,
    ADDING_DEVICE_IP,
    REMOVING_DEVICE,
    ERROR_STATE
  };

  enum BrightnessLevel { BRIGHTNESS_OFF = 0, BRIGHTNESS_LOW = 1, BRIGHTNESS_MEDIUM = 2, BRIGHTNESS_HIGH = 3, BRIGHTNESS_MAX = 4 };

  struct WledDevice {
    std::string nickname;
    std::string ipAddress;
    bool powerOn = false;
    uint8_t brightness = 127;
    uint16_t effect = 0;
    std::string colourName = "Black";
    unsigned long lastUpdate = 0;
  };

  State state = MENU;
  int menuIndex = 0;
  int selectedDeviceIndex = -1;
  int controlIndex = 0;  // 0=power, 1=brightness, 2=effect, 3=color
  int colorPresetIndex = 0; // Index of selected colour preset when controlling colour
  // Transient status message shown after a command is sent (e.g., "Power ON")
  std::string commandMessage;
  unsigned long commandMessageTime = 0;
  static constexpr int COMMAND_MSG_DISPLAY_MS = 2000;
  ButtonNavigator buttonNavigator;
  bool consumeConfirm = false;
  bool consumeBack = false;
  // Track confirm button hold for long‑press colour selection
  bool confirmHeld = false;
  bool confirmLongHandled = false;
  // Duration (ms) required for a long press on Confirm to open the colour picker
  static constexpr int LONG_PRESS_MS = 500;

  std::vector<WledDevice> devices;
  std::string errorMessage;
  unsigned long errorTime = 0;
  std::string newDeviceName;
  std::string newDeviceIp;

  static constexpr int MENU_ITEM_COUNT = 4;  // WiFi, Add, View, Remove
  static constexpr const char* DEVICES_FILE = "/biscuit/wled_devices.dat";
  static constexpr int HTTP_TIMEOUT_MS = 5000;
  static constexpr int ERROR_DISPLAY_MS = 3000;
  static constexpr const char* BRIGHTNESS_NAMES[] = {"Off", "Low", "Med", "High", "Max"};

  void loadDevices();
  void saveDevices();
  bool pollDeviceStatus(int deviceIndex);
  bool setDevicePower(int deviceIndex, bool on);
  bool setDeviceBrightness(int deviceIndex, uint8_t brightness);
  bool setDeviceEffect(int deviceIndex, uint16_t effect);
  // Set RGB colour of the device. Values are 0‑255.
  bool setDeviceColor(int deviceIndex, uint8_t r, uint8_t g, uint8_t b);
  void showError(const std::string& msg);
  int getMenuItemCount() const { return MENU_ITEM_COUNT; }
  BrightnessLevel getBrightnessLevel(uint8_t value) const;
};
