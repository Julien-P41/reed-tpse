#include "reed/device.hpp"
#include "reed/wire.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>

namespace reed {

namespace {

std::string get_string(const picojson::value& v, const std::string& key,
                       const std::string& def = "") {
  if (!v.is<picojson::object>()) return def;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<std::string>()) return def;
  return it->second.get<std::string>();
}

bool has_key(const picojson::value& v, const std::string& key) {
  if (!v.is<picojson::object>()) return false;
  return v.get<picojson::object>().count(key) > 0;
}

const picojson::value& get_value(const picojson::value& v,
                                 const std::string& key) {
  static picojson::value null_val;
  if (!v.is<picojson::object>()) return null_val;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end()) return null_val;
  return it->second;
}

}  // namespace

bool DeviceStatus::healthy() const {
  for (const auto& w : warnings) {
    if (w.description != "No ERROR") return false;
  }
  return true;
}

std::vector<PortHolder> find_port_holders(const std::string& port) {
  namespace fs = std::filesystem;

  std::error_code ec;
  fs::path target = fs::canonical(port, ec);
  if (ec) target = port;

  std::vector<PortHolder> holders;
  const int self = getpid();

  for (const auto& proc : fs::directory_iterator("/proc", ec)) {
    if (ec) break;

    const std::string name = proc.path().filename().string();
    if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) {
      continue;
    }

    const int pid = std::atoi(name.c_str());
    if (pid == self) continue;

    // Unreadable for other users' processes unless root; skip those quietly.
    std::error_code iter_ec;
    fs::directory_iterator fds(proc.path() / "fd", iter_ec);
    if (iter_ec) continue;

    for (const auto& fd : fds) {
      std::error_code link_ec;
      fs::path link = fs::read_symlink(fd.path(), link_ec);
      if (link_ec || link != target) continue;

      PortHolder holder;
      holder.pid = pid;

      std::ifstream comm((proc.path() / "comm").string());
      if (comm) std::getline(comm, holder.comm);
      if (holder.comm.empty()) holder.comm = "unknown";

      holders.push_back(holder);
      break;  // one entry per process, however many fds it has
    }
  }

  return holders;
}

std::optional<PortHolder> find_port_holder(const std::string& port) {
  auto holders = find_port_holders(port);
  if (holders.empty()) return std::nullopt;
  return holders.front();
}

std::optional<std::string> Device::find_device(bool verbose) {
  namespace fs = std::filesystem;

  std::vector<std::string> candidates;

  // The stable name our udev rule installs is identification on its own, and
  // better identification than a handshake: the rule matched the USB vendor,
  // product and product string before creating it. Accepting it without
  // probing also keeps a promise `status` makes and could not previously
  // keep -- it skips `POST conn` because that triggers a ~2s screen
  // re-initialisation, which a read-only poll has no business causing, and
  // then auto-detect did exactly that to work out which port to use.
  //
  // fs::exists follows the link, so a stale symlink for an unplugged cooler
  // is not accepted.
  {
    std::error_code sym_ec;
    if (fs::exists("/dev/tryx-panorama", sym_ec) && !sym_ec) {
      if (verbose) {
        std::cout << "found /dev/tryx-panorama (udev)\n";
      }
      return std::string("/dev/tryx-panorama");
    }
  }

  // Scan /dev for ttyACM* devices
  for (const auto& entry : fs::directory_iterator("/dev")) {
    std::string name = entry.path().filename().string();
    if (name.rfind("ttyACM", 0) == 0) {
      candidates.push_back(entry.path().string());
    }
  }

  if (candidates.empty()) {
    if (verbose) {
      std::cerr << "No /dev/ttyACM* devices found\n";
    }
    return std::nullopt;
  }

  // Sort for consistent ordering (ttyACM0, ttyACM1, ...)
  std::sort(candidates.begin(), candidates.end());

  if (verbose) {
    std::cout << "Scanning " << candidates.size() << " device(s)...\n";
  }

  // A port already held by a reed-tpse process IS the device -- that is the
  // daemon, and it only ever holds the cooler. Probing it would fail with
  // EBUSY and be read as "not the device", so auto-detect used to report no
  // device found whenever the daemon was running, which is the recommended
  // setup. Returning it lets the caller hand over to the daemon or print an
  // accurate error, instead of claiming the cooler is absent.
  for (const auto& port : candidates) {
    for (const auto& holder : find_port_holders(port)) {
      if (holder.comm.find("reed-tpse") != std::string::npos) {
        if (verbose) {
          std::cout << "found " << port << " (held by PID " << holder.pid
                    << ")\n";
        }
        return port;
      }
    }
  }

  // Try each device
  for (const auto& port : candidates) {
    if (verbose) {
      std::cout << "  Trying " << port << "... ";
    }

    Device dev(port, false);
    if (!dev.connect()) {
      if (verbose) {
        std::cout << "failed to open\n";
      }
      continue;
    }

    auto info = dev.handshake();
    if (info && !info->product_id.empty() && info->product_id != "unknown") {
      if (verbose) {
        std::cout << "found " << info->product_id << "\n";
      }
      return port;
    }

    if (verbose) {
      std::cout << "no response\n";
    }
  }

  return std::nullopt;
}

Device::Device(const std::string& port, bool verbose)
    : port_(port), verbose_(verbose) {}

Device::~Device() {
  disconnect();
}

bool Device::connect() {
  // O_CLOEXEC is load-bearing. Without it every fork/exec in this program --
  // the adb calls, loginctl, nvidia-smi, ffmpeg -- inherits the serial fd.
  // Most children exit promptly and the copy dies with them, but adb's
  // fork-server does not: it is spawned by the daemon's own readiness probe,
  // inherits the descriptor, and outlives everything. The tty's exclusive flag
  // is cleared only when the last reference goes away, so from that moment the
  // daemon cannot reopen its own port -- permanently, not until some window
  // reopens. Observed: adb holding fd 3 on /dev/ttyACM0 while the daemon that
  // spawned it held none, and 147 consecutive EBUSY reconnect attempts.
  fd_ = open(port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    // EBUSY means another instance holds the port exclusively (see TIOCEXCL
    // below). Two readers on one tty split incoming frames between them at
    // random, so name the holder rather than failing with a bare errno.
    if (errno == EBUSY) {
      // Name the holder AND give a remedy that matches it. This printed the
      // reed-tpse remedies unconditionally, so a user whose port was held by
      // adb was told to stop the daemon -- which changed nothing, and pointed
      // away from the one command that would have helped.
      const auto holders = find_port_holders(port_);
      const auto adb = std::find_if(
          holders.begin(), holders.end(),
          [](const PortHolder& h) { return h.comm.find("adb") == 0; });

      std::cerr << "Error: " << port_ << " is already open";
      if (!holders.empty()) {
        std::cerr << " by PID " << holders.front().pid << " ("
                  << holders.front().comm << ")";
      }
      std::cerr << ".\n";

      if (adb != holders.end()) {
        std::cerr << "       adb is holding it. Release it with:\n"
                  << "         adb kill-server\n"
                  << "       This does not disturb the cooler; adb restarts "
                     "on its next use.\n";
      } else {
        std::cerr << "       Only one instance may hold the device. Stop it "
                     "with one of:\n"
                  << "         systemctl --user stop reed-tpse.service\n"
                  << "         sudo systemctl stop reed-tpse.service\n";
      }
    } else if (verbose_) {
      std::cerr << "Failed to open " << port_ << ": " << strerror(errno)
                << "\n";
    }
    return false;
  }

  // Make further open() calls on this tty fail with EBUSY for non-root.
  // Non-fatal: an older kernel or an unusual tty driver may refuse it, and
  // losing the lock is worse than losing the session.
  if (ioctl(fd_, TIOCEXCL) < 0 && verbose_) {
    std::cerr << "Warning: TIOCEXCL failed on " << port_ << ": "
              << strerror(errno) << "\n";
  }

  struct termios tty;
  memset(&tty, 0, sizeof(tty));

  if (tcgetattr(fd_, &tty) != 0) {
    if (verbose_) {
      std::cerr << "tcgetattr failed: " << strerror(errno) << "\n";
    }
    close(fd_);
    fd_ = -1;
    return false;
  }

  // 115200 baud
  cfsetospeed(&tty, B115200);
  cfsetispeed(&tty, B115200);

  // 8N1, no flow control
  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
  tty.c_cflag &= ~(PARENB | PARODD | CSTOPB);
  tty.c_cflag &= ~CRTSCTS;
  tty.c_cflag |= CLOCAL | CREAD;

  // Raw mode
  tty.c_iflag &=
      ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  tty.c_oflag &= ~OPOST;
  tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

  // Non-blocking reads
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;

  if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
    if (verbose_) {
      std::cerr << "tcsetattr failed: " << strerror(errno) << "\n";
    }
    close(fd_);
    fd_ = -1;
    return false;
  }

  tcflush(fd_, TCIOFLUSH);

  if (verbose_) {
    std::cout << "Connected to " << port_ << "\n";
  }

  return true;
}

void Device::disconnect() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

void Device::drain(int timeout_ms) {
  if (fd_ < 0) return;

  auto discarded = read_response(timeout_ms);
  if (verbose_ && !discarded.empty()) {
    std::cout << "Drained " << discarded.size()
              << " unprompted byte(s) from " << port_ << "\n";
  }
}

std::vector<uint8_t> Device::read_response(int timeout_ms) {
  std::vector<uint8_t> response;

  struct pollfd pfd;
  pfd.fd = fd_;
  pfd.events = POLLIN;

  auto start = std::chrono::steady_clock::now();

  while (true) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    if (elapsed >= timeout_ms) {
      break;
    }

    int remaining = timeout_ms - static_cast<int>(elapsed);
    int ret = poll(&pfd, 1, remaining);

    if (ret > 0 && (pfd.revents & POLLIN)) {
      uint8_t buf[256];
      ssize_t n = read(fd_, buf, sizeof(buf));
      if (n > 0) {
        response.insert(response.end(), buf, buf + n);

        // Check if we have a complete frame
        if (response.size() >= 2 && response.front() == FRAME_MARKER &&
            response.back() == FRAME_MARKER) {
          break;
        }
      }
    } else if (ret < 0 && errno != EINTR) {
      break;
    }
  }

  return response;
}

std::optional<Response> Device::send_command(const std::string& request_state,
                                             const std::string& cmd_type,
                                             const std::string& content,
                                             bool wait_response) {
  if (fd_ < 0) {
    return std::nullopt;
  }

  ++seq_number_;
  auto frame = build_frame(request_state, cmd_type, content, "1", seq_number_);

  if (verbose_) {
    std::cout << "Sending: " << cmd_type << "\n";
    std::cout << "Frame hex: ";
    for (uint8_t b : frame) {
      std::cout << std::hex << std::uppercase << std::setfill('0')
                << std::setw(2) << static_cast<int>(b);
    }
    std::cout << std::dec << "\n";
  }

  ssize_t written = write(fd_, frame.data(), frame.size());
  if (written != static_cast<ssize_t>(frame.size())) {
    if (verbose_) {
      std::cerr << "Write failed\n";
    }
    return std::nullopt;
  }

  tcdrain(fd_);

  if (!wait_response) {
    return std::nullopt;
  }

  // No sleep before reading. There used to be a flat 500ms wait here, which
  // cost every command half a second whatever the device did -- read_response
  // already blocks until a whole frame arrives or the timeout expires.
  //
  // Replies are matched by ORDER, not by sequence number, and that is forced:
  // AckNumber is the device's own counter, not an echo of the SeqNumber sent.
  // Measured -- SeqNumber=1 out, AckNumber=2 back, on a fresh connection. In
  // captured vendor traffic the two track each other, which reads like an
  // echo and is not one. Callers that care call drain() first to clear frames
  // already in flight.
  constexpr int kReplyTimeoutMs = 1000;
  std::vector<uint8_t> response = read_response(kReplyTimeoutMs);

  if (response.empty()) {
    if (verbose_) {
      std::cout << "No response received\n";
    }
    return std::nullopt;
  }

  if (verbose_) {
    std::cout << "Response hex: ";
    for (uint8_t b : response) {
      std::cout << std::hex << std::uppercase << std::setfill('0')
                << std::setw(2) << static_cast<int>(b);
    }
    std::cout << std::dec << "\n";
  }

  auto parsed = parse_response(response);
  if (verbose_ && parsed) {
    std::cout << "Parsed: " << parsed->raw << "\n";
  }

  return parsed;
}


namespace {

// The status body the device returns on a `STATE all` exchange. Shared by the
// bare read and by the telemetry push, which gets the same body back.
std::optional<DeviceStatus> status_from(const picojson::value& j) {
  DeviceStatus status;

  if (has_key(j, "status")) {
    const auto& s = get_value(j, "status");
    status.fan_lcd = get_string(s, "fanLCD");
    status.turbo_pump = get_string(s, "turboPump");
  }

  if (has_key(j, "availableStorage")) {
    const auto& v = get_value(j, "availableStorage");
    if (v.is<double>()) status.available_storage = v.get<double>();
  }

  // `warning` is a JSON *string* containing a JSON array, so it needs a
  // second parse pass.
  const std::string warning_raw = get_string(j, "warning");
  if (!warning_raw.empty()) {
    picojson::value warnings;
    if (picojson::parse(warnings, warning_raw).empty() &&
        warnings.is<picojson::array>()) {
      for (const auto& entry : warnings.get<picojson::array>()) {
        Warning w;
        w.description = get_string(entry, "description");
        w.type = get_string(entry, "type");
        status.warnings.push_back(w);
      }
    }
  }

  return status;
}

}  // namespace

std::optional<DeviceStatus> Device::get_status() {
  auto response = send_command("STATE", "all", "");
  if (!response || !response->json) return std::nullopt;
  return status_from(*response->json);
}

std::optional<DeviceInfo> Device::handshake() {
  auto response = send_command("POST", "conn", "");

  if (!response || !response->json) {
    return std::nullopt;
  }

  DeviceInfo info;
  const auto& j = *response->json;

  info.product_id = get_string(j, "productId", "unknown");
  info.os = get_string(j, "OS", "unknown");
  info.serial = get_string(j, "sn", "unknown");

  if (has_key(j, "version")) {
    const auto& v = get_value(j, "version");
    info.app_version = get_string(v, "app", "unknown");
    info.firmware = get_string(v, "firmware", "unknown");
    info.hardware = get_string(v, "hardware", "unknown");
  }

  if (has_key(j, "attribute")) {
    const auto& attr_val = get_value(j, "attribute");
    if (attr_val.is<picojson::array>()) {
      for (const auto& attr : attr_val.get<picojson::array>()) {
        if (attr.is<std::string>()) {
          info.attributes.push_back(attr.get<std::string>());
        }
      }
    }
  }

  return info;
}

namespace payload {
namespace {

// The `settings` block, shared by screen configs and presets.
picojson::object settings_object(const DisplaySettings& in) {
  picojson::object filter;
  // null, not "": the vendor's "no filter" is a JSON null.
  filter["value"] =
      in.filter.empty() ? picojson::value() : picojson::value(in.filter);
  filter["opacity"] = picojson::value(static_cast<double>(in.filter_opacity));

  picojson::array badges_arr;
  for (const auto& b : in.badges) {
    badges_arr.push_back(picojson::value(b));
  }

  picojson::object out;
  // The device wants `#RRGGBB`; everything host-side stores bare hex.
  out["color"] = picojson::value(
      in.color.empty() || in.color[0] == '#' ? in.color : "#" + in.color);
  out["align"] = picojson::value(in.align);
  out["badges"] = picojson::value(badges_arr);
  out["filter"] = picojson::value(filter);
  return out;
}

// The screen half: `id` + media + overlay. Sent bare as waterBlockScreenId,
// or nested under waterBlockScreen.id inside a `config` frame.
picojson::object screen_object(const ScreenConfig& config) {
  picojson::array media_arr;
  for (const auto& m : config.media) {
    media_arr.push_back(picojson::value(m));
  }
  picojson::array sysinfo_arr;
  for (const auto& label : config.sysinfo_display) {
    sysinfo_arr.push_back(picojson::value(label));
  }

  picojson::object out;

  // A preset is a different shape, not a variant with the media blanked. The
  // vendor's frame carries the preset id, settings and metrics and nothing
  // else. Sending kCustomization with an empty media list -- which is what a
  // preset used to produce, because selecting one clears the saved media --
  // tells the device to show custom media that is not there.
  if (!config.preset_id.empty()) {
    out["id"] = picojson::value(config.preset_id);
    out["settings"] = picojson::value(settings_object(config.settings));
    out["sysinfoDisplay"] = picojson::value(sysinfo_arr);
    return out;
  }

  // No "Type" key: that was ours. KANALI sends `id` alone to pick between
  // custom media (wire::kCustomization) and a preset ("Pre-set N: Name").
  out["id"] = picojson::value(wire::kCustomization);
  out["screenMode"] = picojson::value(config.screen_mode);
  out["playMode"] = picojson::value(config.play_mode);
  out["media"] = picojson::value(media_arr);

  if (config.split) {
    // Two zones as parallel arrays, and no `ratio` -- the split layout fixes
    // its own geometry.
    picojson::array settings_arr;
    settings_arr.push_back(picojson::value(settings_object(config.settings)));
    settings_arr.push_back(
        picojson::value(settings_object(config.split_settings_right)));
    out["settings"] = picojson::value(settings_arr);

    picojson::array right_arr;
    for (const auto& label : config.split_sysinfo_right) {
      right_arr.push_back(picojson::value(label));
    }
    picojson::array zones;
    zones.push_back(picojson::value(sysinfo_arr));
    zones.push_back(picojson::value(right_arr));
    out["sysinfoDisplay"] = picojson::value(zones);
  } else {
    out["ratio"] = picojson::value(config.ratio);
    out["settings"] = picojson::value(settings_object(config.settings));
    out["sysinfoDisplay"] = picojson::value(sysinfo_arr);
  }
  return out;
}

}  // namespace

std::string screen_config(const ScreenConfig& config) {
  return picojson::value(screen_object(config)).serialize();
}

std::string overlay(const DisplaySettings& settings,
                    const std::vector<std::string>& metrics) {
  picojson::array items;
  for (const auto& m : metrics) items.push_back(picojson::value(m));

  picojson::object obj;
  obj["settings"] = picojson::value(settings_object(settings));
  obj["sysinfoDisplay"] = picojson::value(items);
  return picojson::value(obj).serialize();
}

std::string preset(const std::string& id, const DisplaySettings& settings,
                   const std::vector<std::string>& metrics) {
  picojson::array items;
  for (const auto& m : metrics) items.push_back(picojson::value(m));

  picojson::object obj;
  obj["id"] = picojson::value(id);
  obj["settings"] = picojson::value(settings_object(settings));
  obj["sysinfoDisplay"] = picojson::value(items);
  return picojson::value(obj).serialize();
}

}  // namespace payload

std::optional<Response> Device::set_screen_config(const ScreenConfig& config) {
  std::string content = payload::screen_config(config);

  // One POST, like the vendor. This used to be sent twice with a 500ms gap on
  // the belief that the first was unreliable; ten fresh-connect single sends
  // in a row all applied, so the retry was masking something else -- most
  // likely the handshake timing, which `config` now covers.
  return send_command("POST", "waterBlockScreenId", content);
}

std::optional<Response> Device::set_brightness(int value) {
  picojson::object obj;
  obj["value"] = picojson::value(static_cast<double>(value));
  std::string content = picojson::value(obj).serialize();
  return send_command("POST", "brightness", content);
}

namespace payload {

std::string full_config(const FullConfig& config,
                        const ScreenConfig& screen) {
  picojson::object id = screen_object(screen);

  picojson::object fan;
  fan["mode"] = picojson::value(config.fan_mode);
  picojson::array points;
  for (const auto& [temp, duty] : config.fan_curve) {
    picojson::array point;
    point.push_back(picojson::value(static_cast<double>(temp)));
    point.push_back(picojson::value(static_cast<double>(duty)));
    points.push_back(picojson::value(point));
  }
  fan["smartMode"] = picojson::value(points);
  fan["fixedMode"] = picojson::value(static_cast<double>(config.fan_fixed));

  picojson::object screen_block;
  screen_block["enable"] = picojson::value(config.screen_enable);
  screen_block["displayInSleep"] = picojson::value(config.display_in_sleep);
  screen_block["brightness"] =
      picojson::value(static_cast<double>(config.brightness));
  // Omitted unless explicitly set -- see the note on FullConfig::rotate.
  if (config.rotate) {
    screen_block["rotate"] =
        picojson::value(static_cast<double>(*config.rotate));
  }
  screen_block["id"] = picojson::value(id);
  screen_block["fanLCD"] = picojson::value(fan);

  picojson::object spec;
  spec["cpu"] = picojson::value(config.cpu_name);
  spec["gpu"] = picojson::value(config.gpu_name);

  picojson::object obj;
  obj["temperature"] = picojson::value(config.temperature_unit);
  obj["waterBlockScreen"] = picojson::value(screen_block);
  obj["spec"] = picojson::value(spec);

  return picojson::value(obj).serialize();
}

}  // namespace payload

std::optional<Response> Device::send_config(const FullConfig& config,
                                           const ScreenConfig& screen) {
  return send_command("POST", "config", payload::full_config(config, screen));
}

std::optional<Response> Device::set_rotation(int degree) {
  picojson::object obj;
  obj["degree"] = picojson::value(static_cast<double>(degree));
  return send_command("POST", "rotate", picojson::value(obj).serialize());
}

std::optional<Response> Device::set_screen_power(bool enable) {
  picojson::object obj;
  obj["enable"] = picojson::value(enable);
  return send_command("POST", "waterBlockScreen",
                      picojson::value(obj).serialize());
}

std::string Device::sysinfo_body(
    const std::vector<SysinfoData>& data) {
  picojson::object cpu, gpu, memory, motherboard, disk, network;
  picojson::array fans;

  cpu["load"] = picojson::value(0.0);
  cpu["temperature"] = picojson::value(0.0);
  cpu["speedAverage"] = picojson::value(0.0);
  cpu["voltage"] = picojson::value(0.0);
  cpu["power"] = picojson::value(0.0);
  cpu["fanAverage"] = picojson::value(0.0);

  gpu["load"] = picojson::value(0.0);
  // GPU temperature is a string in the PcInfo blob, unlike every other temp.
  gpu["temperature"] = picojson::value(std::string("0"));
  gpu["speed"] = picojson::value(0.0);
  gpu["voltage"] = picojson::value(0.0);
  gpu["power"] = picojson::value(0.0);
  gpu["fan"] = picojson::value(0.0);

  memory["load"] = picojson::value(0.0);
  memory["speed"] = picojson::value(0.0);
  memory["temperature"] = picojson::value(0.0);
  memory["total"] = picojson::value(0.0);
  memory["used"] = picojson::value(0.0);

  motherboard["temperature"] = picojson::value(0.0);

  disk["load"] = picojson::value(0.0);
  disk["used"] = picojson::value(0.0);
  disk["total"] = picojson::value(0.0);
  disk["temperature"] = picojson::value(0.0);
  disk["activity"] = picojson::value(0.0);
  disk["readSpeed"] = picojson::value(0.0);
  disk["writeSpeed"] = picojson::value(0.0);

  network["download"] = picojson::value(0.0);
  network["upload"] = picojson::value(0.0);

  auto to_double = [](const std::string& s) -> double {
    try {
      return std::stod(s);
    } catch (...) {
      return 0.0;
    }
  };

  for (const auto& item : data) {
    if (item.label == "CPU Temperature") {
      cpu["temperature"] = picojson::value(to_double(item.value));
    } else if (item.label == "CPU Frequency") {
      cpu["speedAverage"] = picojson::value(to_double(item.value));
    } else if (item.label == "CPU Usage") {
      cpu["load"] = picojson::value(to_double(item.value));
    } else if (item.label == "CPU Voltage") {
      cpu["voltage"] = picojson::value(to_double(item.value));
    } else if (item.label == "GPU Temperature") {
      gpu["temperature"] = picojson::value(item.value);
    } else if (item.label == "GPU Frequency") {
      gpu["speed"] = picojson::value(to_double(item.value));
    } else if (item.label == "GPU Usage") {
      gpu["load"] = picojson::value(to_double(item.value));
    } else if (item.label == "GPU Voltage") {
      gpu["voltage"] = picojson::value(to_double(item.value));
    } else if (item.label == "Motherboard Temperature") {
      motherboard["temperature"] = picojson::value(to_double(item.value));
    } else if (item.label == "Memory Frequency") {
      memory["speed"] = picojson::value(to_double(item.value));
    } else if (item.label == "Memory Utilization") {
      memory["load"] = picojson::value(to_double(item.value));
    } else if (item.label == "Hard Disk Temperature") {
      disk["temperature"] = picojson::value(to_double(item.value));
    } else if (item.label == "CPU Power") {
      cpu["power"] = picojson::value(to_double(item.value));
    } else if (item.label == "GPU Power") {
      gpu["power"] = picojson::value(to_double(item.value));
    } else if (item.label == "Memory Temperature") {
      memory["temperature"] = picojson::value(to_double(item.value));
    }
  }

  picojson::object pc_info;
  pc_info["cpu"] = picojson::value(cpu);
  pc_info["gpu"] = picojson::value(gpu);
  pc_info["memory"] = picojson::value(memory);
  pc_info["motherboard"] = picojson::value(motherboard);
  pc_info["disk"] = picojson::value(disk);
  pc_info["network"] = picojson::value(network);
  pc_info["fans"] = picojson::value(fans);
  pc_info["timestamp"] = picojson::value(static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count()));

  return picojson::value(pc_info).serialize();
}

// STATE, not POST: the vendor pushes its telemetry as a `STATE all` request
// body and gets the device's status back on the same exchange, which is why it
// never needs a separate read. Both verbs are accepted here.
//
// This one discards the reply, which is right for the common case -- the HUD
// does not care what came back. push_sysinfo() is the same frame with the
// answer read.
std::optional<Response> Device::send_sysinfo(
    const std::vector<SysinfoData>& data) {
  return send_command("STATE", "all", sysinfo_body(data), false);
}

std::optional<DeviceStatus> Device::push_sysinfo(
    const std::vector<SysinfoData>& data) {
  // Identical frame to send_sysinfo, but waiting for the answer. Kept as a
  // separate entry point rather than a flag so the fire-and-forget path stays
  // the obvious default -- a caller that does not need the status should not
  // pay for a reply it will discard.
  auto response = send_command("STATE", "all", sysinfo_body(data));
  if (!response || !response->json) return std::nullopt;
  return status_from(*response->json);
}

std::optional<Response> Device::set_overlay(
    const DisplaySettings& settings, const std::vector<std::string>& metrics) {
  return send_command("POST", "preset", payload::overlay(settings, metrics));
}

std::optional<Response> Device::send_spec(const std::string& cpu_name,
                                          const std::string& gpu_name) {
  picojson::object obj;
  obj["cpu"] = picojson::value(cpu_name);
  obj["gpu"] = picojson::value(gpu_name);
  std::string content = picojson::value(obj).serialize();
  return send_command("POST", "spec", content);
}

std::optional<Response> Device::set_display_in_sleep(bool enable) {
  picojson::object obj;
  obj["enable"] = picojson::value(enable);
  std::string content = picojson::value(obj).serialize();
  return send_command("POST", "displayInSleep", content);
}

std::optional<Response> Device::send_power_event(const std::string& event) {
  picojson::object obj;
  obj["event"] = picojson::value(event);
  return send_command("POST", "power", picojson::value(obj).serialize());
}

std::optional<Response> Device::set_preset(
    const std::string& id, const DisplaySettings& settings,
    const std::vector<std::string>& sysinfo) {
  return send_command("POST", "waterBlockScreenId",
                      payload::preset(id, settings, sysinfo));
}

std::optional<Response> Device::set_fan_profile(const std::string& json) {
  return send_command("POST", "fanLCDSet", json);
}

namespace payload {

// {mode, smartMode, fixedMode} -- byte-for-byte the shape KANALI 1.2.1 puts on
// the wire. No `speed`, and fixedMode is always a number even in Smart Mode.
std::string fan(const std::string& mode, const FanCurve& curve,
                int fixed_duty) {
  picojson::array points;
  for (const auto& [temp, duty] : curve) {
    picojson::array point;
    point.push_back(picojson::value(static_cast<double>(temp)));
    point.push_back(picojson::value(static_cast<double>(duty)));
    points.push_back(picojson::value(point));
  }

  picojson::object obj;
  obj["mode"] = picojson::value(mode);
  obj["smartMode"] = picojson::value(points);
  obj["fixedMode"] = picojson::value(static_cast<double>(fixed_duty));
  return picojson::value(obj).serialize();
}

// The vendor's "low" tier, and the same curve its `config` blob ships as the
// factory default.
const FanCurve kDefaultCurve = {{0, 10},  {10, 20}, {30, 30},  {50, 40},
                                {65, 55}, {80, 70}, {90, 100}, {100, 100}};

}  // namespace payload

std::optional<Response> Device::set_fan_fixed(int duty, const FanCurve& curve) {
  return set_fan_profile(payload::fan(
      wire::kFanFixed, curve.empty() ? payload::kDefaultCurve : curve, duty));
}

std::optional<Response> Device::set_fan_smart(const FanCurve& curve,
                                              int fixed_duty) {
  return set_fan_profile(payload::fan(
      wire::kFanSmart, curve.empty() ? payload::kDefaultCurve : curve,
      fixed_duty));
}


std::optional<Response> Device::set_temperature_unit(const std::string& unit) {
  picojson::object obj;
  obj["value"] = picojson::value(unit);  // "Celsius" or "Fahrenheit"
  std::string content = picojson::value(obj).serialize();
  return send_command("POST", "temperature", content);
}

}  // namespace reed
