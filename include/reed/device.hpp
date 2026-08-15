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

struct ScreenConfig {
  std::vector<std::string> media;
  std::string screen_mode = "Full Screen";
  std::string ratio = "2:1";
  std::string play_mode = "Single";
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

 private:
  std::string port_;
  bool verbose_;
  int fd_ = -1;
  int seq_number_ = 0;

  std::vector<uint8_t> read_response(int timeout_ms = 1000);
};

}  // namespace reed
