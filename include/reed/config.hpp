#pragma once

#include <optional>
#include <string>
#include <vector>

namespace reed {

struct Config {
  std::string port;  // Empty = auto-detect
  int brightness = 75;  //default lower than max setting to reduce burn-in risk on display
  int keepalive_interval = 10;
  // Have the daemon mirror the host's power state to the device: lock/unlock
  // as the session locks, and `shutdown` when the daemon is stopped. Opt-in,
  // since it changes what the panel does without being asked.
  bool power_auto = false;
  // Shown while the session is locked, in place of the firmware's standby
  // clip. Unset means "use the firmware default", i.e. send the lock-screen
  // power event instead. Configuration, not runtime state -- it belongs here
  // rather than in display.json, which `display` rewrites.
  std::optional<std::string> lock_media;
  int lock_brightness = 40;
};

struct HudConfig {
  bool enabled = false;
  std::vector<std::string> metrics;  // firmware-defined labels, max 3
  std::string align = "Left";        // Left | Center | Right
  std::string color = "FFFFFF";  // bare hex, no `#`
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
  // Screen Splitting draws two independent overlays -- the wire carries
  // `settings` as a two-element array and `sysinfoDisplay` as two arrays.
  // Unset means the right zone mirrors the left, which is what a split
  // configured before this existed did.
  std::optional<HudConfig> hud_right;
  // Sleep-mode behaviour. Unset means "never configured, leave the device
  // alone"; the setting is volatile on the device (it is lost whenever USB
  // power is cut), so once set it has to be re-applied by the daemon.
  std::optional<bool> display_in_sleep;
  // Panel power. Unset means never configured; the daemon only asserts it
  // once it has been set explicitly, so an untouched device is left alone.
  std::optional<bool> screen_on;
  // Overlay filter drawn across the media -- "Rain" or "Smoke" on the wire,
  // empty for none -- with its opacity. Part of `settings`, so it rides along
  // with every screen-config change.
  std::string filter;
  int filter_opacity = 100;
  // Set when a firmware preset is showing. Mutually exclusive with `media`:
  // the daemon re-applies whichever one is active.
  std::optional<std::string> preset;
  // LCD fan. Volatile on the device and only applied while telemetry is being
  // pushed, so the daemon re-installs it on every connect.
  std::optional<std::string> fan_tier;  // vendor tier name
  std::optional<int> fan_duty;          // percent; unset means Smart Mode
};

// Why a load failed. "Not there yet" and "there but unreadable" were both
// reported as an empty optional, and every caller treated that as first-run:
// load, fall back to a default-constructed state, set one field, save. A
// partial read of a file being rewritten therefore persisted defaults over
// everything else in it.
enum class LoadStatus {
  Ok,
  Missing,    // nothing saved yet -- defaults are correct
  Unreadable, // exists but could not be opened
  Malformed,  // exists and did not parse -- do NOT overwrite
};

class ConfigManager {
 public:
  static std::string get_config_dir();
  static std::string get_state_dir();
  static std::string get_config_path();
  static std::string get_state_path();

  static std::optional<Config> load_config(LoadStatus* status = nullptr);
  static bool save_config(const Config& config);

  static std::optional<DisplayState> load_state(LoadStatus* status = nullptr);
  static bool save_state(const DisplayState& state);
};

}  // namespace reed
