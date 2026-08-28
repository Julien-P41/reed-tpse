#pragma once

#include <optional>
#include <string>
#include <vector>

#include "protocol.hpp"

namespace reed {

struct DeviceInfo {
  std::string product_id;
  std::string os;
  std::string serial;
  std::string app_version;
  std::string firmware;
  std::string hardware;
  std::vector<std::string> attributes;
};

struct DisplaySettings {
  std::string color = "FFFFFF";     // bare hex; `#` added on the wire
  std::string align = "Center";     // "Left", "Center", "Right"
  std::vector<std::string> badges;  // "CPU Badge", "GPU Badge"
  // Overlay filter drawn across the media. KANALI sends null for "none";
  // "Rain" and "Smoke" are the two names seen on the wire.
  std::string filter;               // empty = none
  // Matches DisplayState::filter_opacity. These defaulted differently (0 here,
  // 100 there), so any ScreenConfig built without copying from state produced
  // a fully transparent filter.
  int filter_opacity = 100;         // 0-100
};

// LCD-fan curve: [temperature in degC, duty in percent], ascending in both.
// KANALI always sends exactly 8 points, first at 0 degC and last at 100/100.
using FanCurve = std::vector<std::pair<int, int>>;

namespace payload {
// The vendor's "low" tier, and what its `config` blob ships as the factory
// default. Declared here so FullConfig can default to it.
extern const FanCurve kDefaultCurve;
}  // namespace payload

// Everything `POST config` carries. The vendor sends this immediately after
// `conn`, on every connect -- one frame instead of the five-plus we used to
// send, which is what the post-connect race was about.
struct FullConfig {
  std::string temperature_unit = "Celsius";
  bool screen_enable = true;
  bool display_in_sleep = false;
  int brightness = 75;
  // Panel rotation. Deliberately optional and unset by default: the value is
  // applied at the NEXT device restart and there is no way to read the
  // current one back, so sending a guess would silently arm a 90-degree
  // rotation. Only set this when the user asked for it.
  std::optional<int> rotate;
  std::string fan_mode = "Smart Mode";
  // Defaults to the vendor curve, not an empty array. The daemon only fills
  // this in when a fan tier has been configured, so an untouched install used
  // to send `"smartMode":[]` in every post-connect frame -- a shape no
  // captured vendor frame has, in the one field this project has repeatedly
  // been burned by.
  FanCurve fan_curve = payload::kDefaultCurve;
  int fan_fixed = 40;
  std::string cpu_name;
  std::string gpu_name;
  // Pump control is left out entirely -- the motherboard owns it here.
};

struct ScreenConfig {
  std::vector<std::string> media;
  std::string screen_mode = "Full Screen";
  std::string ratio = "2:1";
  std::string play_mode = "Single";
  std::vector<std::string> sysinfo_display;  // max 3 firmware-defined labels
  DisplaySettings settings;

  // Screen Splitting only. When screen_mode is "Screen Splitting" the vendor
  // sends `settings` as a two-element array and `sysinfoDisplay` as an array
  // of two arrays -- left zone, then right -- and drops `ratio` entirely.
  // `media` stays flat, one entry per zone.
  bool split = false;
  DisplaySettings split_settings_right;
  std::vector<std::string> split_sysinfo_right;
};

struct SysinfoData {
  std::string label;
  std::string value;
  std::string unit;
};

// One entry of the device's own health report. The device currently only
// reports a "Fan LCD" entry; description is "No ERROR" when healthy.
struct Warning {
  std::string description;
  std::string type;
};

// Aggregated read from `STATE all`, the only endpoint that returns a body.
struct DeviceStatus {
  // RPM values arrive as strings on the wire; kept verbatim so an
  // unrecognised value is reported rather than silently coerced to 0.
  std::string fan_lcd;
  std::string turbo_pump;
  std::vector<Warning> warnings;
  double available_storage = 0;

  bool healthy() const;
};

// A process holding the serial port, found by scanning /proc/*/fd. Only
// processes owned by the caller are visible unless running as root.
struct PortHolder {
  int pid = 0;
  std::string comm;
};

// Every process holding the port, not just one. More than one can: adb's
// fork-server keeps the CDC device open alongside the daemon, so asking for
// "the" holder returns whichever /proc happened to be walked first -- an
// arbitrary answer that changes between runs.
std::vector<PortHolder> find_port_holders(const std::string& port);

// The first holder found, for diagnostics. Prefer find_port_holders when the
// answer matters.
std::optional<PortHolder> find_port_holder(const std::string& port);

// The JSON bodies, separated from the transport that carries them.
//
// Exposed because this is where every protocol bug in this project has lived:
// a `fixedMode` that stopped the fan, an invented `Type` key, a filter value
// sent as "" instead of null, a split zone shipped blank. They are pure
// functions of their inputs, so they can be checked against captured vendor
// traffic without a device.
namespace payload {

std::string screen_config(const ScreenConfig& config);
std::string fan(const std::string& mode, const FanCurve& curve, int fixed_duty);
std::string overlay(const DisplaySettings& settings,
                    const std::vector<std::string>& metrics);
std::string preset(const std::string& id, const DisplaySettings& settings,
                   const std::vector<std::string>& metrics);
std::string full_config(const FullConfig& config, const ScreenConfig& screen);

// The vendor's default fan curve -- its "low" tier, and what its `config`
// blob ships as the factory setting.
extern const FanCurve kDefaultCurve;

}  // namespace payload

class Device {
 public:
  explicit Device(const std::string& port, bool verbose = false);
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  // Auto-detect device by scanning /dev/ttyACM* and attempting handshake
  static std::optional<std::string> find_device(bool verbose = false);

  bool connect();
  void disconnect();

  // Discard anything the device sends unprompted. Opening the port asserts
  // DTR and the device answers with an info frame; without draining it, the
  // first command reads that greeting instead of its own reply.
  void drain(int timeout_ms = 300);
  bool is_connected() const { return fd_ >= 0; }
  const std::string& port() const { return port_; }

  std::optional<Response> send_command(const std::string& request_state,
                                       const std::string& cmd_type,
                                       const std::string& content = "",
                                       bool wait_response = true);

  // Read via the STATE method. The firmware implements no GET; STATE is the
  // read verb and POST is the write verb.
  std::optional<DeviceInfo> handshake();
  std::optional<DeviceStatus> get_status();
  std::optional<Response> set_screen_config(const ScreenConfig& config);
  std::optional<Response> set_brightness(int value);

  // Panel power. `false` blanks the display outright -- distinct from
  // brightness 0 and from the sleep-mode fallback. The vendor's screen
  // on/off toggle, and the one thing it sends after `config` on every
  // connect.
  std::optional<Response> set_screen_power(bool enable);

  // One-shot apply of the whole device state, the vendor's own post-connect
  // frame. `screen` supplies the media/overlay half. Note the device sends no
  // response to this command -- nor to `conn` -- so a null return is normal.
  std::optional<Response> send_config(const FullConfig& config,
                                      const ScreenConfig& screen);

  // Mirror Mode. The device ACKs, then RESTARTS -- it drops off USB and
  // re-enumerates a few seconds later. Captured values are 90 and 270 only,
  // 180 degrees apart; on the unit this was captured from, 270 is upright and
  // 90 is mirrored. There is no way to read the current value back, so a
  // wrong guess is only visible after the restart.
  std::optional<Response> set_rotation(int degree);
  // No delete over serial. `mediaDelete` exists -- {"type":"custom"} with
  // either `include` (delete these) or `exclude` (delete everything else) --
  // but deleting over adb needs no serial port, and the daemon holds that
  // port exclusively. The shape is recorded in docs/vendor-protocol.md if it
  // is ever wanted.

  // HUD: on-device telemetry overlay support.
  // Firmware renders up to 3 metrics from a fixed label set on top of the
  // configured media. Labels and the PcInfo shape are defined by the cooler.
  std::optional<Response> send_sysinfo(const std::vector<SysinfoData>& data);
  // No set_sysinfo_display: `POST sysinfoDisplay {"items":[...]}` is accepted
  // by the firmware but KANALI never sends it, and it cannot carry colour or
  // badges. The overlay goes out through set_overlay instead.

  // The vendor's overlay command: styling and metric list in one frame,
  // without touching the media. `POST preset` -- unrelated to the screen
  // presets, despite the name. KANALI never sends `sysinfoDisplay` on its
  // own; this is how the HUD is configured.
  std::optional<Response> set_overlay(const DisplaySettings& settings,
                                      const std::vector<std::string>& metrics);
  std::optional<Response> send_spec(const std::string& cpu_name,
                                    const std::string& gpu_name);
  std::optional<Response> set_temperature_unit(const std::string& unit);

  // What the panel shows once the host stops handshaking -- PC off, or the
  // controlling process exited. The device waits out its own ~60s disconnect
  // timeout before switching.
  //
  // The name reads backwards: this is the device's "display something while
  // the host is asleep", not "blank the display".
  //
  //   enable=true   the firmware's standby animation
  //   enable=false  black
  //
  // Confirmed both ways on firmware V1.0.11 by watching the panel through a
  // full undisturbed timeout. It was documented as the inverse for months,
  // "verified" by sampling luminance over `adb screencap` -- which wakes this
  // panel, so the reading described the wake rather than the setting.
  //
  // The field is `enable`; a `value` field is silently ignored.
  std::optional<Response> set_display_in_sleep(bool enable);

  // Tell the device what the host is doing. The field is `event`, not a
  // boolean -- anything else throws "No value for event" on the device.
  // Vocabulary: ac-power, on-battery, shutdown, lock-screen, unlock-screen.
  // `shutdown` blanks the panel outright when displayInSleep is enabled;
  // `lock-screen` shows the standby clip; `unlock-screen` restores the media.
  std::optional<Response> send_power_event(const std::string& event);

  // Select a firmware-bundled preset. `id` must be "Pre-set <n>: <Name>":
  // the device splits on ": " and loads /system/media/video/<Name>.mp4 with
  // spaces turned into underscores. It does not check the file exists -- a
  // name it cannot resolve simply blanks the panel -- and a bare name with no
  // prefix is not dispatched at all.
  // The vendor sends `settings` and `sysinfoDisplay` alongside the id, which
  // is why picking a preset in KANALI also restores its overlay styling. Pass
  // the same values the current screen config uses.
  std::optional<Response> set_preset(const std::string& id,
                                     const DisplaySettings& settings,
                                     const std::vector<std::string>& sysinfo);

  // Restore the vendor's own default fan setting: Smart Mode on the "low"
  // curve with fixedMode 40, exactly the payload KANALI 1.2.1 sends. Replaces
  // an earlier recovery that installed empty curve arrays and a numeric-less
  // `fixedMode` -- that shape is what stopped the fan dead at 0 RPM.
  std::optional<Response> reset_fan_profile();

  // Set the LCD fan to a fixed duty. `tier` is the vendor's tier name and
  // `duty` a percentage. Measured RPM per duty is in the README's Fan section;
  // it is not repeated here, because two copies of a measurement table drift
  // and neither says which is current. In short: monotonic, not linear, and
  // roughly 1500-4200 RPM across the range on firmware V1.0.11.
  //
  // Both fan calls send the vendor's exact payload: {mode, smartMode,
  // fixedMode} and nothing else. KANALI never sends the `speed` field even
  // though the device's FanLCD model carries one, and it always sends BOTH
  // the curve and a numeric fixedMode whichever mode is selected -- sending
  // a non-numeric fixedMode is what stops the fan.
  //
  // A profile alone does nothing: the device only acts on it when host
  // telemetry arrives, so the daemon must be pushing.
  std::optional<Response> set_fan_fixed(int duty, const FanCurve& curve);

  // Temperature-following mode. The curve is now known from captured vendor
  // traffic, so this sends a real one rather than an empty array.
  std::optional<Response> set_fan_smart(const FanCurve& curve,
                                        int fixed_duty);

  // Install a raw fan profile. Callers are expected to have validated it.
  std::optional<Response> set_fan_profile(const std::string& json);

 private:
  std::string port_;
  bool verbose_;
  int fd_ = -1;
  int seq_number_ = 0;

  std::vector<uint8_t> read_response(int timeout_ms = 1000);
};

}  // namespace reed
