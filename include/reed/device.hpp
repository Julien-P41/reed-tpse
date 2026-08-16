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
  std::string position = "Top";     // "Top", "Center", "Bottom"
  std::string color = "#FFFFFF";    // hex
  std::string align = "Center";     // "Left", "Center", "Right"
  std::vector<std::string> badges;  // "CPU Badge", "GPU Badge"
  int filter_opacity = 0;           // 0-100
};

struct ScreenConfig {
  std::vector<std::string> media;
  std::string screen_mode = "Full Screen";
  std::string ratio = "2:1";
  std::string play_mode = "Single";
  std::vector<std::string> sysinfo_display;  // max 3 firmware-defined labels
  DisplaySettings settings;
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

std::optional<PortHolder> find_port_holder(const std::string& port);

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
  std::optional<Response> query(const std::string& cmd_type,
                                const std::string& content = "");

  std::optional<DeviceInfo> handshake();
  std::optional<DeviceStatus> get_status();
  std::optional<Response> set_screen_config(const ScreenConfig& config);
  std::optional<Response> set_brightness(int value);
  std::optional<Response> delete_media(const std::vector<std::string>& files);

  // HUD: on-device telemetry overlay support.
  // Firmware renders up to 3 metrics from a fixed label set on top of the
  // configured media. Labels and the PcInfo shape are defined by the cooler.
  std::optional<Response> send_sysinfo(const std::vector<SysinfoData>& data);
  std::optional<Response> set_sysinfo_display(
      const std::vector<std::string>& labels);
  std::optional<Response> send_spec(const std::string& cpu_name,
                                    const std::string& gpu_name);
  std::optional<Response> set_temperature_unit(const std::string& unit);

  // Sleep-mode behaviour. Enabled, the panel goes black once the host stops
  // handshaking (PC off, or the controlling process exits) instead of falling
  // back to the firmware's demo loop. Verified on firmware V1.0.11; the field
  // is `enable` and a `value` field is silently ignored.
  std::optional<Response> set_display_in_sleep(bool enable);

  // Select a firmware-bundled preset. `id` must be "Pre-set <n>: <Name>":
  // the device splits on ": " and loads /system/media/video/<Name>.mp4 with
  // spaces turned into underscores. It does not check the file exists -- a
  // name it cannot resolve simply blanks the panel -- and a bare name with no
  // prefix is not dispatched at all.
  std::optional<Response> set_preset(const std::string& id);

 private:
  std::string port_;
  bool verbose_;
  int fd_ = -1;
  int seq_number_ = 0;

  std::vector<uint8_t> read_response(int timeout_ms = 1000);
};

}  // namespace reed
