#pragma once

#include <optional>
#include <string>
#include <vector>

#include "reed/wire.hpp"

namespace reed {

struct Config {
  std::string port;  // Empty = auto-detect
  // No brightness here. It lived in config.json and looked authoritative, but
  // nothing honoured it: it seeded a CLI default that was then gated away by
  // `--brightness` being given or not, so the value that reached the device
  // was always either the flag or DisplayState::brightness. A dead knob in a
  // config file is worse than no knob.
  int keepalive_interval = 10;
  // Have the daemon mirror the host's power state to the device: lock/unlock
  // as the session locks, and `shutdown` when the daemon is stopped. Opt-in,
  // since it changes what the panel does without being asked.
  // Three independent behaviours, all on by default.
  //
  // They used to be one `power_auto` flag, and bundling them caused a real
  // bug: with lock_media configured, the branch that would have sent the
  // unlock event was skipped along with the lock event, so the panel stayed
  // on the sticky standby state across a daemon restart. They are unrelated
  // to each other and are now switched separately.
  //
  // Defaulting to on because there is nothing to opt into: a daemon that
  // holds the device and does not tell it what the host is doing is simply
  // worse. Set any of them false in config.json to suppress it.
  bool report_ac_power = true;      // ac-power / on-battery
  bool report_lock = true;          // lock-screen / unlock-screen, or lock_media
  bool report_shutdown = true;      // `shutdown` when the daemon exits
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
  std::string temperature_unit = wire::kCelsius;  // Celsius | Fahrenheit
  std::string cpu_name;
  std::string gpu_name;
};

struct DisplayState {
  std::vector<std::string> media;
  std::string ratio = "2:1";
  std::string screen_mode = wire::kFullScreen;
  std::string play_mode = wire::kPlaySingle;
  int brightness = 75;  // default lower than max setting to reduce burn-in risk on the display
  HudConfig hud;
  // Screen Splitting draws two independent overlays -- the wire carries
  // `settings` as a two-element array and `sysinfoDisplay` as two arrays.
  // Unset means the right zone mirrors the left, which is what a split
  // configured before this existed did.
  std::optional<HudConfig> hud_right;
  // Sleep-mode behaviour and panel power. Both are volatile on the device --
  // lost whenever USB power drops -- so the daemon re-applies them on every
  // connect.
  //
  // Unset means "the user never chose", NOT "leave the device alone". These
  // comments used to promise the latter and the code did not keep it: the
  // daemon's post-connect `config` frame carries every field FullConfig has,
  // so an unset value simply means that struct's default goes out -- panel on,
  // standby animation, Celsius, brightness 75, the vendor fan curve. The
  // guards below only stop an unset optional being read; they do not stop the
  // default being sent.
  //
  // That is deliberate. One atomic frame is what closed the startup race, and
  // "one frame carrying everything" cannot also be "assert only what the user
  // chose" -- you cannot send half a frame. The vendor's app does the same:
  // its `config` carries every field too. The cost is that reed-tpse asserts
  // its defaults over anything else touching the panel, on every reconnect.
  std::optional<bool> display_in_sleep;
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
