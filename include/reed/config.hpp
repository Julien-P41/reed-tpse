#pragma once

#include <optional>
#include <string>
#include <vector>

namespace reed {

struct Config {
  std::string port;  // Empty = auto-detect
  int brightness = 75;  //default lower than max setting to reduce burn-in risk on display
  int keepalive_interval = 10;
};

struct HudConfig {
  bool enabled = false;
  std::vector<std::string> metrics;  // firmware-defined labels, max 3
  std::string position = "Top";      // Top | Center | Bottom
  std::string align = "Left";        // Left | Center | Right
  std::string color = "#FFFFFF";
  std::vector<std::string> badges;   // "CPU Badge", "GPU Badge"
  int push_interval_sec = 5;
  std::string temperature_unit = "Celsius";  // Celsius | Fahrenheit
  std::string cpu_name;
  std::string gpu_name;
};

struct DisplayState {
  std::vector<std::string> media;
  std::string ratio = "2:1";
  std::string screen_mode = "Full Screen";
  std::string play_mode = "Single";
  int brightness = 75;  // default lower than max setting to reduce burn-in risk on the display
  HudConfig hud;
  // Sleep-mode behaviour. Unset means "never configured, leave the device
  // alone"; the setting is volatile on the device (it is lost whenever USB
  // power is cut), so once set it has to be re-applied by the daemon.
  std::optional<bool> display_in_sleep;
};

class ConfigManager {
 public:
  static std::string get_config_dir();
  static std::string get_state_dir();
  static std::string get_config_path();
  static std::string get_state_path();

  static std::optional<Config> load_config();
  static bool save_config(const Config& config);

  static std::optional<DisplayState> load_state();
  static bool save_state(const DisplayState& state);
};

}  // namespace reed
