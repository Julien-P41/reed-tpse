#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

#include "reed/adb.hpp"
#include "reed/config.hpp"
#include "reed/device.hpp"
#include "reed/media.hpp"
#include "reed/sysinfo.hpp"

#include <set>
#include <sstream>

namespace fs = std::filesystem;

static std::atomic<bool> g_running{true};

static void signal_handler(int sig) {
  if (sig == SIGTERM || sig == SIGINT) {
    g_running = false;
  }
}

static void print_usage(const char* prog) {
  std::cout
      << "Usage: " << prog
      << " <command> [options]\n\n"
         "Commands:\n"
         "  info                    Show device info\n"
         "  status                  Show fan/pump RPM, warnings, storage\n"
         "  raw <METHOD> <ENDPOINT> [JSON]\n"
         "                          Send an arbitrary command, print the "
         "response\n"
         "  upload <file>           Upload media file (converts GIF to MP4)\n"
         "  display <file...>       Set display to specified media files\n"
         "  brightness <0-100>      Set display brightness\n"
         "  sleep-display <on|off>  Black screen (vs demo loop) when the host\n"
         "                          stops handshaking\n"
         "  preset <name|list>      Show a firmware-bundled preset\n"
         "  list                    List media files on device\n"
         "  delete <file...>        Delete media files from device\n"
         "  daemon start            Start background daemon\n"
         "  daemon stop             Stop background daemon\n"
         "  daemon status           Show daemon status\n"
         "  hud configure           Configure on-device telemetry overlay\n"
         "  hud clear               Disable the telemetry overlay\n"
         "  hud status              Show current HUD configuration\n\n"
         "Options:\n"
         "  -p, --port <path>       Serial port (auto-detected if not "
         "specified)\n"
         "  -v, --verbose           Verbose output\n"
         "  --ratio <2:1|1:1>       Display ratio (default: 2:1)\n"
         "  --brightness <0-100>    Set brightness with display command\n"
         "  --keepalive             Stay running with keepalive (default: exit)\n"
         "  --foreground            Run daemon in foreground\n"
         "  --json                  Machine-readable output (status)\n"
         "  --watch <seconds>       Poll until interrupted (status)\n"
         "  --system                Act on the system-scope unit (daemon)\n\n"
         "HUD options (with `hud configure`):\n"
         "  --metrics <csv>         Comma-separated labels, max 3. Known labels:\n"
         "                          CPU Temperature, CPU Frequency, CPU Usage,\n"
         "                          CPU Voltage, GPU Temperature, GPU Frequency,\n"
         "                          GPU Usage, GPU Voltage, Motherboard Temperature,\n"
         "                          Memory Frequency, Memory Utilization,\n"
         "                          Hard Disk Temperature, CPU Power, GPU Power,\n"
         "                          Memory Temperature, Date&Time\n"
         "  --position <pos>        Top | Center | Bottom (default: Top)\n"
         "  --align <align>         Left | Center | Right (default: Left)\n"
         "  --color <hex>           e.g. #FFFFFF (default: #FFFFFF)\n"
         "  --badges <csv>          cpu,gpu (or none). Default: none\n"
         "  --interval <sec>        Push interval in seconds (default: 5)\n"
         "  --unit <unit>           Celsius | Fahrenheit (default: Celsius)\n"
         "  --cpu-name <str>        Override auto-detected CPU name\n"
         "  --gpu-name <str>        Override auto-detected GPU name\n";
}

namespace {

// Firmware-defined label set. Anything outside this is rejected with a clear
// error so typos don't silently produce a dead overlay slot.
// Labels are passed through to the firmware verbatim, so they must match its
// vocabulary exactly. The date label is "Date&Time" -- unspaced. The spaced
// "Date & Time" is the vendor app's *UI* string (it sits in the renderer i18n
// next to the French "Date et heure"); the unspaced form is what the vendor's
// main process puts on the wire. Verified on firmware V1.0.11: "Date&Time"
// renders the clock, "Date & Time" is silently dropped while the other
// metrics in the same request still render.
const std::set<std::string>& known_hud_labels() {
  static const std::set<std::string> labels = {
      "CPU Temperature",         "CPU Frequency",       "CPU Usage",
      "CPU Voltage",             "GPU Temperature",     "GPU Frequency",
      "GPU Usage",               "GPU Voltage",         "Motherboard Temperature",
      "Memory Frequency",        "Memory Utilization",  "Hard Disk Temperature",
      "CPU Power",               "GPU Power",           "Memory Temperature",
      "Date&Time",
  };
  return labels;
}

// Accept the human-readable spelling people will reasonably type (and that
// downstream GUIs already send) but put the firmware's spelling on the wire.
std::string canonical_hud_label(const std::string& label) {
  if (label == "Date & Time" || label == "Date and Time") return "Date&Time";
  return label;
}

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> out;
  std::string token;
  std::istringstream ss(s);
  while (std::getline(ss, token, ',')) {
    size_t start = token.find_first_not_of(" \t\r\n");
    size_t end = token.find_last_not_of(" \t\r\n");
    if (start != std::string::npos) {
      out.push_back(token.substr(start, end - start + 1));
    }
  }
  return out;
}

// Build the SysinfoData payload the device expects for the given labels.
std::vector<reed::SysinfoData> build_sysinfo(
    const std::vector<std::string>& labels, const reed::SystemMetrics& m) {
  std::vector<reed::SysinfoData> out;
  auto fmt = [](double v, int precision = 0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return std::string(buf);
  };
  for (const auto& label : labels) {
    reed::SysinfoData d;
    d.label = label;
    if (label == "CPU Temperature") {
      d.value = fmt(m.cpu.temperature_c);
      d.unit = "°C";
    } else if (label == "CPU Frequency") {
      d.value = fmt(m.cpu.frequency_mhz);
      d.unit = "MHz";
    } else if (label == "CPU Usage") {
      d.value = fmt(m.cpu.usage_percent, 1);
      d.unit = "%";
    } else if (label == "GPU Temperature") {
      d.value = fmt(m.gpu.temperature_c);
      d.unit = "°C";
    } else if (label == "GPU Frequency") {
      d.value = fmt(m.gpu.frequency_mhz);
      d.unit = "MHz";
    } else if (label == "GPU Usage") {
      d.value = fmt(m.gpu.usage_percent);
      d.unit = "%";
    } else if (label == "GPU Voltage") {
      d.value = fmt(m.gpu.voltage_v, 3);
      d.unit = "V";
    } else if (label == "Memory Utilization") {
      d.value = fmt(m.memory.usage_percent, 1);
      d.unit = "%";
    } else if (label == "Memory Frequency") {
      d.value = fmt(m.memory.frequency_mhz.value_or(0.0));
      d.unit = "MHz";
    } else if (label == "CPU Voltage") {
      d.value = fmt(m.cpu.voltage_v.value_or(0.0), 3);
      d.unit = "V";
    } else if (label == "CPU Power") {
      d.value = fmt(m.cpu.power_w.value_or(0.0), 1);
      d.unit = "W";
    } else if (label == "GPU Power") {
      d.value = fmt(m.gpu.power_w.value_or(0.0), 1);
      d.unit = "W";
    } else if (label == "Motherboard Temperature") {
      d.value = fmt(m.motherboard.temperature_c.value_or(0.0));
      d.unit = "°C";
    } else if (label == "Hard Disk Temperature") {
      d.value = fmt(m.disk.temperature_c.value_or(0.0));
      d.unit = "°C";
    } else if (label == "Memory Temperature") {
      d.value = fmt(m.memory.temperature_c.value_or(0.0));
      d.unit = "°C";
    } else {
      // Date&Time is drawn from the device's own clock; it needs no value.
      d.value = "0";
    }
    out.push_back(d);
  }
  return out;
}

// Whether this machine can actually source a label. Date&Time is always fine
// (device clock); everything else must have a real reading behind it, or the
// overlay burns one of three slots rendering a permanent 0.
bool metric_available(const std::string& label, const reed::SystemMetrics& m) {
  if (label == "CPU Voltage") return m.cpu.voltage_v.has_value();
  if (label == "CPU Power") return m.cpu.power_w.has_value();
  if (label == "GPU Power") return m.gpu.power_w.has_value();
  if (label == "Motherboard Temperature")
    return m.motherboard.temperature_c.has_value();
  if (label == "Hard Disk Temperature") return m.disk.temperature_c.has_value();
  if (label == "Memory Temperature") return m.memory.temperature_c.has_value();
  if (label == "Memory Frequency") return m.memory.frequency_mhz.has_value();
  return true;
}

// Why a metric is missing, so the warning is actionable rather than a shrug.
std::string metric_hint(const std::string& label) {
  if (label == "CPU Power")
    return "RAPL energy_uj is root-only on kernels >= 5.10; the daemon runs "
           "unprivileged";
  if (label == "CPU Voltage" || label == "Motherboard Temperature")
    return "needs a super-I/O sensor -- try `sudo modprobe nct6775`";
  if (label == "Memory Temperature")
    return "needs a DIMM sensor (jc42/spd5118); most desktop boards have none";
  if (label == "Hard Disk Temperature")
    return "needs an nvme hwmon or `sudo modprobe drivetemp` for SATA";
  if (label == "Memory Frequency")
    return "DIMM speed needs an SPD/DMI read, which requires root";
  return "no data source on this system";
}

}  // namespace

static int cmd_info(const std::string& port, bool verbose) {
  reed::Device device(port, verbose);

  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }

  auto info = device.handshake();
  if (!info) {
    std::cerr << "Failed to get device info\n";
    return 1;
  }

  std::cout << "Device Information:\n"
            << "  Product: " << info->product_id << "\n"
            << "  OS: " << info->os << "\n"
            << "  Serial: " << info->serial << "\n"
            << "  App Version: " << info->app_version << "\n"
            << "  Firmware: " << info->firmware << "\n"
            << "  Hardware: " << info->hardware << "\n";

  if (!info->attributes.empty()) {
    std::cout << "  Attributes: ";
    for (size_t i = 0; i < info->attributes.size(); ++i) {
      if (i > 0) std::cout << ", ";
      std::cout << info->attributes[i];
    }
    std::cout << "\n";
  }

  return 0;
}

static void print_status(const reed::DeviceStatus& status) {
  const double gib = status.available_storage / (1024.0 * 1024.0 * 1024.0);

  std::cout << "Fan LCD:   " << (status.fan_lcd.empty() ? "-" : status.fan_lcd)
            << " RPM\n"
            << "Pump:      "
            << (status.turbo_pump.empty() ? "-" : status.turbo_pump)
            << " RPM\n";

  std::cout << "Storage:   " << std::fixed << std::setprecision(2) << gib
            << " GiB free\n";

  if (status.warnings.empty()) {
    std::cout << "Warnings:  (none reported)\n";
    return;
  }

  for (size_t i = 0; i < status.warnings.size(); ++i) {
    std::cout << (i == 0 ? "Warnings:  " : "           ")
              << status.warnings[i].type << ": "
              << status.warnings[i].description << "\n";
  }
}

static int cmd_status(const std::string& port, bool json_output, int watch,
                      bool verbose) {
  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }

  // No handshake here: `POST conn` triggers a full screen re-initialisation
  // on the device (~2s), which a read-only poll has no business causing.
  device.drain();

  if (watch > 0) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
  }

  bool healthy = true;

  while (g_running) {
    auto status = device.get_status();
    if (!status) {
      std::cerr << "No response to 'STATE all'\n";
      return 1;
    }

    healthy = status->healthy();

    if (json_output) {
      picojson::object warnings_obj;
      picojson::array warnings;
      for (const auto& w : status->warnings) {
        picojson::object entry;
        entry["description"] = picojson::value(w.description);
        entry["type"] = picojson::value(w.type);
        warnings.push_back(picojson::value(entry));
      }

      picojson::object out;
      out["fanLCD"] = picojson::value(status->fan_lcd);
      out["turboPump"] = picojson::value(status->turbo_pump);
      out["availableStorage"] = picojson::value(status->available_storage);
      out["warning"] = picojson::value(warnings);
      out["healthy"] = picojson::value(healthy);

      std::cout << picojson::value(out).serialize() << std::endl;
    } else {
      print_status(*status);
    }

    if (watch <= 0) break;

    if (!json_output) std::cout << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(watch));
  }

  return healthy ? 0 : 2;
}

static int cmd_raw(const std::string& port, const std::string& method,
                   const std::string& endpoint, const std::string& body,
                   bool verbose) {
  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }

  device.drain();

  auto response = device.send_command(method, endpoint, body);
  if (!response) {
    std::cerr << "No response to '" << method << " " << endpoint << "'\n";
    return 1;
  }

  std::cout << "Status:  " << response->version << " " << response->status
            << "\n";

  const size_t separator = response->raw.find("\r\n\r\n");
  if (separator != std::string::npos) {
    std::string headers = response->raw.substr(0, separator);
    // Headers are CRLF-separated; print one per line without the CRs.
    size_t start = 0;
    while (start < headers.size()) {
      size_t end = headers.find("\r\n", start);
      if (end == std::string::npos) end = headers.size();
      std::cout << "Header:  " << headers.substr(start, end - start) << "\n";
      start = end + 2;
    }
  }

  if (response->body.empty()) {
    // An empty body with 200 means the endpoint took no action. It is not an
    // error, and it is not proof of success either.
    std::cout << "Body:    (empty -- the endpoint accepted the frame but "
                 "returned nothing)\n";
    return 0;
  }

  std::cout << "Body:    " << response->body << "\n";

  if (response->json) {
    std::cout << "JSON:    " << response->json->serialize(true) << "\n";
  } else {
    std::cout << "JSON:    (body is not valid JSON)\n";
  }

  return 0;
}

static int cmd_sleep_display(const std::string& port, const std::string& arg,
                             bool verbose) {
  bool enable;
  if (arg == "on" || arg == "true" || arg == "1") {
    enable = true;
  } else if (arg == "off" || arg == "false" || arg == "0") {
    enable = false;
  } else {
    std::cerr << "Usage: reed-tpse sleep-display <on|off>\n";
    return 1;
  }

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }
  device.drain();

  if (!device.set_display_in_sleep(enable)) {
    std::cerr << "No response to 'POST displayInSleep'\n";
    return 1;
  }

  // The device answers 200 with an empty body either way, so the reply proves
  // nothing. Persist it instead and let the daemon re-apply, since the setting
  // lives in controller RAM and is lost whenever USB power drops.
  auto state = reed::ConfigManager::load_state();
  if (!state) state = reed::DisplayState{};
  state->display_in_sleep = enable;
  if (!reed::ConfigManager::save_state(*state)) {
    std::cerr << "Warning: could not persist sleep-display state\n";
  }

  std::cout << "Sleep display " << (enable ? "enabled" : "disabled") << ".\n";
  if (enable) {
    std::cout << "  Panel goes black when the host stops handshaking.\n";
  } else {
    std::cout << "  Panel falls back to the firmware's demo loop when the "
                 "host stops handshaking.\n";
  }
  return 0;
}

// Presets are named by the file the firmware ships, e.g. "Cooling delivery"
// for Cooling_delivery.mp4. Accept either spelling from the user.
static std::string preset_display_name(const std::string& file_stem) {
  std::string out = file_stem;
  std::replace(out.begin(), out.end(), '_', ' ');
  return out;
}

static int cmd_preset(const std::string& port, const std::vector<std::string>& args,
                      bool verbose) {
  if (!reed::Adb::is_device_connected()) {
    std::cerr << "No ADB device connected (needed to list the built-in "
                 "presets)\n";
    return 1;
  }
  auto presets = reed::Adb::list_presets();
  if (!presets || presets->empty()) {
    std::cerr << "Could not read the preset list from the device\n";
    return 1;
  }

  if (args.empty() || args[0] == "list") {
    std::cout << "Built-in presets:\n";
    for (const auto& p : *presets) {
      std::cout << "  " << preset_display_name(p) << "\n";
    }
    return 0;
  }

  // Match on the display name, case-insensitively, and tolerate underscores.
  const std::string wanted = preset_display_name(args[0]);
  auto ieq = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i])))
        return false;
    }
    return true;
  };

  std::string match;
  for (const auto& p : *presets) {
    if (ieq(preset_display_name(p), wanted)) {
      match = preset_display_name(p);
      break;
    }
  }
  if (match.empty()) {
    // The device resolves the id straight to a path without checking it
    // exists, so an unmatched name would silently blank the panel. Refuse.
    std::cerr << "Unknown preset: \"" << args[0] << "\"\n"
              << "Run `reed-tpse preset list` to see what this firmware "
                 "ships.\n";
    return 1;
  }

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }
  device.drain();
  device.handshake();

  // The leading number is not used by the firmware -- it splits on ": " and
  // keeps the name -- but the prefix must be present or the command is not
  // dispatched at all.
  const std::string id = "Pre-set 1: " + match;
  if (!device.set_preset(id)) {
    std::cerr << "No response to 'POST waterBlockScreenId'\n";
    return 1;
  }

  auto state = reed::ConfigManager::load_state();
  if (!state) state = reed::DisplayState{};
  state->preset = match;
  reed::ConfigManager::save_state(*state);

  std::cout << "Preset set to: " << match << "\n";
  return 0;
}

static int cmd_upload(const std::string& file, bool verbose) {
  if (verbose) std::cout << "Checking file: " << file << "\n";

  if (!fs::exists(file)) {
    std::cerr << "File not found: " << file << "\n";
    return 1;
  }

  if (verbose) {
    std::cout << "File size: " << fs::file_size(file) << " bytes\n";
    std::cout << "Checking ADB connection...\n";
  }

  if (!reed::Adb::is_device_connected()) {
    std::cerr << "No ADB device connected\n";
    return 1;
  }

  auto type = reed::Media::detect_type(file);
  std::string upload_path = file;
  std::string remote_name = reed::Media::get_filename(file);

  if (verbose) std::cout << "Detected type: " << static_cast<int>(type) << "\n";

  if (type == reed::MediaType::Gif) {
    if (!reed::Media::is_ffmpeg_available()) {
      std::cerr << "ffmpeg not found. Install ffmpeg to upload GIF files.\n";
      return 1;
    }

    std::string converted_name = reed::Media::get_converted_name(file);
    std::string converted_path =
        std::string(reed::Media::TMP_DIR) + converted_name;

    std::cout << "Converting GIF to MP4...\n";
    if (verbose) std::cout << "Output path: " << converted_path << "\n";

    if (!reed::Media::convert_gif_to_mp4(file, converted_path)) {
      std::cerr << "Failed to convert GIF to MP4\n";
      return 1;
    }

    upload_path = converted_path;
    remote_name = converted_name;
    std::cout << "Converted: " << reed::Media::get_filename(file) << " -> "
              << remote_name << "\n";
  }

  if (verbose)
    std::cout << "Pushing via ADB: " << upload_path << " -> " << remote_name
              << "\n";

  std::cout << "Uploading " << remote_name << "...\n";
  if (!reed::Adb::push(upload_path, remote_name)) {
    std::cerr << "Failed to upload file\n";
    return 1;
  }

  std::cout << "Upload complete.\n";
  std::cout << "Display with: reed-tpse display " << remote_name << "\n";

  return 0;
}

static int cmd_display(const std::string& port,
                       const std::vector<std::string>& files,
                       const std::string& ratio, int brightness, bool keepalive,
                       int keepalive_interval, bool verbose) {
  if (brightness < 0 || brightness > 100) {
    std::cerr << "Brightness must be 0-100\n";
    return 1;
  }

  // Convert GIF filenames to MP4
  std::vector<std::string> media_files;
  for (const auto& f : files) {
    if (reed::Media::detect_type(f) == reed::MediaType::Gif) {
      media_files.push_back(reed::Media::get_converted_name(f));
    } else {
      media_files.push_back(f);
    }
  }

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }

  device.handshake();

  reed::ScreenConfig config;
  config.media = media_files;
  config.ratio = ratio;

  device.set_screen_config(config);
  device.set_brightness(brightness);

  std::cout << "Display set to: ";
  for (size_t i = 0; i < media_files.size(); ++i) {
    if (i > 0) std::cout << ", ";
    std::cout << media_files[i];
  }
  std::cout << "\n";
  std::cout << "Brightness: " << brightness << "\n";

  // Save state for daemon. Load first and mutate only the display fields:
  // save_state() truncates, so building a fresh DisplayState here would drop
  // every other setting sharing this file -- the HUD config, display_in_sleep,
  // and any non-default screen/play mode -- on each `display` call.
  auto state = reed::ConfigManager::load_state();
  if (!state) state = reed::DisplayState{};
  state->media = media_files;
  state->ratio = ratio;
  state->brightness = brightness;
  state->preset.reset();  // custom media and a preset are mutually exclusive
  reed::ConfigManager::save_state(*state);

  if (!keepalive) {
    std::cout << "Run 'reed-tpse daemon start' to keep display persistent.\n";
    return 0;
  }

  std::cout << "Keeping connection alive (Ctrl+C to exit)...\n";

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  while (g_running) {
    std::this_thread::sleep_for(std::chrono::seconds(keepalive_interval));
    if (!g_running) break;
    device.handshake();
    if (verbose) {
      std::cout << "  keepalive sent\n";
    }
  }

  std::cout << "Stopping.\n";
  return 0;
}

static int cmd_brightness(const std::string& port, int value, bool verbose) {
  if (value < 0 || value > 100) {
    std::cerr << "Brightness must be 0-100\n";
    return 1;
  }

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }

  device.handshake();
  device.set_brightness(value);

  std::cout << "Brightness set to " << value << "\n";
  return 0;
}

static int cmd_list() {
  if (!reed::Adb::is_device_connected()) {
    std::cerr << "No ADB device connected\n";
    return 1;
  }

  auto files = reed::Adb::list_media();
  if (!files) {
    std::cerr << "Failed to list media files\n";
    return 1;
  }

  if (files->empty()) {
    std::cout << "No media files on device.\n";
    return 0;
  }

  std::cout << "Media files on device:\n";
  for (const auto& f : *files) {
    std::cout << "  " << f << "\n";
  }

  return 0;
}

static int cmd_delete(const std::vector<std::string>& files) {
  if (!reed::Adb::is_device_connected()) {
    std::cerr << "No ADB device connected\n";
    return 1;
  }

  for (const auto& f : files) {
    if (reed::Adb::remove(f)) {
      std::cout << "Deleted: " << f << "\n";
    } else {
      std::cerr << "Failed to delete: " << f << "\n";
    }
  }

  return 0;
}

// The unit ships in two mutually exclusive scopes; never address both, or two
// daemons end up contending for the same serial port.
static std::string systemctl(bool system_scope) {
  return system_scope ? "systemctl" : "systemctl --user";
}

static int cmd_daemon_start(const std::string& port, bool foreground,
                            bool system_scope, bool verbose) {
  if (!foreground) {
    const std::string sc = systemctl(system_scope);
    std::system((sc + " enable reed-tpse.service 2>/dev/null").c_str());
    int ret = std::system((sc + " start reed-tpse.service 2>/dev/null").c_str());

    if (ret == 0) {
      std::cout << "Daemon started via systemd ("
                << (system_scope ? "system" : "user") << " scope).\n";
      std::cout << "Check status: reed-tpse daemon status"
                << (system_scope ? " --system" : "") << "\n";
      return 0;
    } else {
      std::cerr << "systemd service not installed in "
                << (system_scope ? "system" : "user")
                << " scope. Run with --foreground, install the unit, or try "
                << (system_scope ? "without --system" : "--system") << ".\n";
      return 1;
    }
  }

  // Foreground daemon mode
  auto state = reed::ConfigManager::load_state();
  if (!state || state->media.empty()) {
    std::cerr
        << "No display state saved. Run 'reed-tpse display <file>' first.\n";
    return 1;
  }

  auto config = reed::ConfigManager::load_config();
  std::string actual_port =
      (config && !config->port.empty()) ? config->port : port;
  int keepalive_interval = config ? config->keepalive_interval : 10;

  // Only the foreground daemon needs the device, so it does its own detection
  // rather than making every `daemon` subcommand depend on a free port.
  if (actual_port.empty()) {
    auto detected = reed::Device::find_device(verbose);
    if (!detected) {
      std::cerr
          << "No device found. Specify port with -p or check connection.\n";
      return 1;
    }
    actual_port = *detected;
  }

  reed::ScreenConfig screen_config;
  screen_config.media = state->media;
  screen_config.ratio = state->ratio;
  screen_config.screen_mode = state->screen_mode;
  screen_config.play_mode = state->play_mode;
  if (state->hud.enabled) {
    screen_config.sysinfo_display = state->hud.metrics;
    screen_config.settings.position = state->hud.position;
    screen_config.settings.align = state->hud.align;
    screen_config.settings.color = state->hud.color;
    screen_config.settings.badges = state->hud.badges;
  }

  auto device = std::make_unique<reed::Device>(actual_port, verbose);
  if (!device->connect()) {
    std::cerr << "Failed to connect to " << actual_port << "\n";
    return 1;
  }

  auto restore = [&](reed::Device& dev) {
    if (!dev.handshake()) return false;
    if (state->hud.enabled) {
      dev.send_spec(state->hud.cpu_name, state->hud.gpu_name);
      dev.set_temperature_unit(state->hud.temperature_unit);
    }
    // Volatile on the device -- controller RAM, lost whenever USB power drops
    // (which it does at S5) -- so re-apply it on every connect, not just once.
    if (state->display_in_sleep) {
      dev.set_display_in_sleep(*state->display_in_sleep);
    }
    if (state->preset) {
      dev.set_preset("Pre-set 1: " + *state->preset);
    } else {
      dev.set_screen_config(screen_config);
    }
    dev.set_brightness(state->brightness);
    return true;
  };

  restore(*device);

  if (state->hud.enabled) {
    std::cout << "Display restored with HUD (" << state->hud.metrics.size()
              << " metric" << (state->hud.metrics.size() == 1 ? "" : "s")
              << ", push every " << state->hud.push_interval_sec << "s).\n";
  } else {
    std::cout << "Display restored. Running keepalive...\n";
  }

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  reed::SystemMonitor monitor;
  bool first_handshake_ok = false;

  // Reconnect after a handshake failure: the serial fd can die silently on USB
  // suspend/resume or when the device renumbers (/dev/ttyACM0 -> ttyACM1). Try
  // the current port first, then rescan.
  auto reconnect = [&]() -> bool {
    device->disconnect();
    if (device->connect() && restore(*device)) {
      std::cerr << "keepalive: reconnected on " << device->port() << "\n";
      return true;
    }
    // Release the port before scanning. connect() may have succeeded above
    // with only restore() failing, in which case we still hold the tty --
    // and TIOCEXCL makes find_device()'s open() of our own port fail with
    // EBUSY, so the rescan could never find the device it is sitting on.
    device->disconnect();

    std::cerr << "keepalive: scanning for device...\n";
    auto found = reed::Device::find_device(verbose);
    if (!found) {
      std::cerr << "keepalive: no device found\n";
      return false;
    }
    device = std::make_unique<reed::Device>(*found, verbose);
    if (device->connect() && restore(*device)) {
      std::cerr << "keepalive: reconnected on " << *found << "\n";
      return true;
    }
    std::cerr << "keepalive: found " << *found << " but handshake failed\n";
    return false;
  };

  using clock = std::chrono::steady_clock;
  auto now = clock::now();
  auto next_handshake = now + std::chrono::seconds(keepalive_interval);
  auto next_sysinfo =
      state->hud.enabled
          ? now + std::chrono::seconds(state->hud.push_interval_sec)
          : clock::time_point::max();

  int failures = 0;
  while (g_running) {
    // Tick once a second so we're responsive to both cadences and SIGTERM.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!g_running) break;

    now = clock::now();

    if (now >= next_handshake) {
      next_handshake = now + std::chrono::seconds(keepalive_interval);
      if (device->handshake()) {
        failures = 0;
        first_handshake_ok = true;
      } else {
        ++failures;
        std::cerr << "keepalive: handshake failed (#" << failures
                  << "), reconnecting...\n";
        if (reconnect()) {
          failures = 0;
          first_handshake_ok = true;
        }
      }
    }

    if (state->hud.enabled && now >= next_sysinfo) {
      next_sysinfo = now + std::chrono::seconds(state->hud.push_interval_sec);
      // Gate first push on a successful handshake — otherwise startup issues
      // surface as "HUD shows zeros" instead of "device didn't respond".
      if (first_handshake_ok && device->is_connected()) {
        auto metrics = monitor.sample();
        device->send_sysinfo(build_sysinfo(state->hud.metrics, metrics));
      }
    }
  }

  return 0;
}

static int cmd_hud(const std::string& port, const std::vector<std::string>& args,
                   bool verbose) {
  if (args.empty()) {
    std::cerr << "Usage: reed-tpse hud <configure|clear|status> [options]\n";
    return 1;
  }
  const std::string& action = args[0];

  if (action == "status") {
    auto state = reed::ConfigManager::load_state();
    if (!state) {
      std::cout << "No saved display state.\n";
      return 0;
    }
    const auto& h = state->hud;
    std::cout << "HUD: " << (h.enabled ? "enabled" : "disabled") << "\n";
    std::cout << "  Metrics:";
    if (h.metrics.empty()) std::cout << " (none)";
    for (const auto& m : h.metrics) std::cout << " [" << m << "]";
    std::cout << "\n";
    std::cout << "  Position: " << h.position << "\n";
    std::cout << "  Align: " << h.align << "\n";
    std::cout << "  Color: " << h.color << "\n";
    std::cout << "  Badges:";
    if (h.badges.empty()) std::cout << " (none)";
    for (const auto& b : h.badges) std::cout << " [" << b << "]";
    std::cout << "\n";
    std::cout << "  Push interval: " << h.push_interval_sec << "s\n";
    std::cout << "  Temperature unit: " << h.temperature_unit << "\n";
    std::cout << "  CPU: " << (h.cpu_name.empty() ? "(auto)" : h.cpu_name) << "\n";
    std::cout << "  GPU: " << (h.gpu_name.empty() ? "(auto)" : h.gpu_name) << "\n";
    return 0;
  }

  // Load existing state so we preserve media/brightness/ratio.
  reed::DisplayState state;
  if (auto loaded = reed::ConfigManager::load_state()) {
    state = *loaded;
  }

  if (action == "clear") {
    state.hud = reed::HudConfig{};  // reset
    if (!reed::ConfigManager::save_state(state)) {
      std::cerr << "Failed to save state\n";
      return 1;
    }
    // Push a screen config without HUD fields so the device stops rendering
    // the overlay immediately. Requires media to be present in state.
    if (!port.empty() && !state.media.empty()) {
      reed::Device device(port, verbose);
      if (device.connect()) {
        device.handshake();
        reed::ScreenConfig cfg;
        cfg.media = state.media;
        cfg.ratio = state.ratio;
        cfg.screen_mode = state.screen_mode;
        cfg.play_mode = state.play_mode;
        device.set_screen_config(cfg);
      }
    }
    std::cout << "HUD disabled.\n";
    return 0;
  }

  if (action != "configure") {
    std::cerr << "Unknown hud action: " << action << "\n";
    return 1;
  }

  // Parse hud configure flags from args[1..].
  reed::HudConfig h = state.hud;  // start from current so unspecified flags stick
  h.enabled = true;
  bool metrics_provided = false;

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](const char* flag) -> std::string {
      if (++i >= args.size()) {
        std::cerr << "Missing value for " << flag << "\n";
        std::exit(1);
      }
      return args[i];
    };
    if (a == "--metrics") {
      h.metrics = split_csv(next("--metrics"));
      for (auto& m : h.metrics) m = canonical_hud_label(m);
      metrics_provided = true;
    } else if (a == "--position") {
      h.position = next("--position");
    } else if (a == "--align") {
      h.align = next("--align");
    } else if (a == "--color") {
      h.color = next("--color");
    } else if (a == "--badges") {
      h.badges.clear();
      for (const auto& tok : split_csv(next("--badges"))) {
        if (tok == "cpu" || tok == "CPU" || tok == "CPU Badge") {
          h.badges.push_back("CPU Badge");
        } else if (tok == "gpu" || tok == "GPU" || tok == "GPU Badge") {
          h.badges.push_back("GPU Badge");
        } else if (tok == "none") {
          h.badges.clear();
          break;
        } else {
          std::cerr << "Unknown badge: " << tok
                    << " (expected cpu, gpu, or none)\n";
          return 1;
        }
      }
    } else if (a == "--interval") {
      h.push_interval_sec = std::atoi(next("--interval").c_str());
      if (h.push_interval_sec < 1) h.push_interval_sec = 1;
    } else if (a == "--unit") {
      h.temperature_unit = next("--unit");
    } else if (a == "--cpu-name") {
      h.cpu_name = next("--cpu-name");
    } else if (a == "--gpu-name") {
      h.gpu_name = next("--gpu-name");
    } else {
      std::cerr << "Unknown hud option: " << a << "\n";
      return 1;
    }
  }

  if (!metrics_provided && h.metrics.empty()) {
    std::cerr << "Specify at least one metric via --metrics\n";
    return 1;
  }

  // Validate labels.
  const auto& known = known_hud_labels();
  for (const auto& m : h.metrics) {
    if (known.count(m) == 0) {
      std::cerr << "Unknown metric label: \"" << m << "\"\n";
      std::cerr << "Run `reed-tpse hud configure --help` (or see `reed-tpse -h`) "
                   "for the known label list.\n";
      return 1;
    }
  }
  // Warn about anything this machine cannot source. The firmware allows only
  // three metrics, so a slot that can only ever render 0 is worth flagging
  // loudly rather than letting it look like a device fault later.
  {
    reed::SystemMonitor probe;
    probe.sample();  // primes the CPU-usage and RAPL deltas
    const reed::SystemMetrics sample = probe.sample();
    for (const auto& m : h.metrics) {
      if (metric_available(m, sample)) continue;
      std::cerr << "Warning: \"" << m
                << "\" has no data source on this system -- it will render 0.\n"
                << "         " << metric_hint(m) << "\n";
    }
  }

  if (h.metrics.size() > 3) {
    std::cerr << "Firmware supports at most 3 HUD metrics; got "
              << h.metrics.size() << ".\n";
    return 1;
  }

  // Validate enums.
  auto one_of = [](const std::string& v,
                   std::initializer_list<const char*> opts) {
    for (const char* o : opts)
      if (v == o) return true;
    return false;
  };
  if (!one_of(h.position, {"Top", "Center", "Bottom"})) {
    std::cerr << "Invalid --position: " << h.position << "\n";
    return 1;
  }
  if (!one_of(h.align, {"Left", "Center", "Right"})) {
    std::cerr << "Invalid --align: " << h.align << "\n";
    return 1;
  }
  if (!one_of(h.temperature_unit, {"Celsius", "Fahrenheit"})) {
    std::cerr << "Invalid --unit: " << h.temperature_unit << "\n";
    return 1;
  }

  // Auto-detect CPU/GPU names if not already set.
  if (h.cpu_name.empty()) h.cpu_name = reed::SystemMonitor::detect_cpu_name();
  if (h.gpu_name.empty()) h.gpu_name = reed::SystemMonitor::detect_gpu_name();

  state.hud = h;
  if (!reed::ConfigManager::save_state(state)) {
    std::cerr << "Failed to save state\n";
    return 1;
  }

  // Apply live if we have a device. Non-fatal if not connected — state is
  // saved and the daemon will apply it on next start.
  if (!port.empty()) {
    reed::Device device(port, verbose);
    if (device.connect() && device.handshake()) {
      device.send_spec(h.cpu_name, h.gpu_name);
      device.set_temperature_unit(h.temperature_unit);

      reed::ScreenConfig cfg;
      cfg.media = state.media;
      cfg.ratio = state.ratio;
      cfg.screen_mode = state.screen_mode;
      cfg.play_mode = state.play_mode;
      cfg.sysinfo_display = h.metrics;
      cfg.settings.position = h.position;
      cfg.settings.align = h.align;
      cfg.settings.color = h.color;
      cfg.settings.badges = h.badges;
      device.set_screen_config(cfg);

      // Prime the overlay with a first sample so the user sees values
      // immediately instead of waiting for the daemon to tick.
      reed::SystemMonitor mon;
      mon.sample();  // first sample primes CPU usage delta
      auto metrics = mon.sample();
      device.send_sysinfo(build_sysinfo(h.metrics, metrics));
    } else {
      std::cerr << "Warning: could not apply HUD live (device not reachable). "
                   "State saved; daemon will apply on next start.\n";
    }
  }

  std::cout << "HUD configured. Daemon will push updates every "
            << h.push_interval_sec << "s.\n";
  std::cout << "Restart the daemon (`reed-tpse daemon stop && reed-tpse daemon "
               "start`) to pick up the new config.\n";
  return 0;
}

static int cmd_daemon_stop(bool system_scope) {
  int ret = std::system(
      (systemctl(system_scope) + " stop reed-tpse.service 2>/dev/null")
          .c_str());
  if (ret == 0) {
    std::cout << "Daemon stopped.\n";
    return 0;
  } else {
    std::cerr << "Failed to stop daemon (or not running).\n";
    return 1;
  }
}

static int cmd_daemon_status(bool system_scope) {
  int ret = std::system(
      (systemctl(system_scope) + " status reed-tpse.service 2>/dev/null")
          .c_str());
  return ret == 0 ? 0 : 1;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  reed::Config config;
  if (auto loaded = reed::ConfigManager::load_config()) {
    config = *loaded;
  }

  std::string port = config.port;
  bool verbose = false;
  std::string ratio = "2:1";
  int brightness = config.brightness;
  bool keepalive = false;
  bool foreground = false;
  bool json_output = false;
  bool system_scope = false;
  int watch = 0;
  int keepalive_interval = config.keepalive_interval;

  std::string command;
  std::vector<std::string> args;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-p" || arg == "--port") {
      if (++i < argc) {
        port = argv[i];
      }
    } else if (arg == "-v" || arg == "--verbose") {
      verbose = true;
    } else if (arg == "--ratio") {
      if (++i < argc) ratio = argv[i];
    } else if (arg == "--brightness") {
      if (++i < argc) brightness = std::atoi(argv[i]);
    } else if (arg == "--keepalive") {
      keepalive = true;
    } else if (arg == "--foreground") {
      foreground = true;
    } else if (arg == "--json") {
      json_output = true;
    } else if (arg == "--system") {
      system_scope = true;
    } else if (arg == "--watch") {
      if (++i < argc) watch = std::atoi(argv[i]);
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (command.empty()) {
      command = arg;
    } else {
      args.push_back(arg);
    }
  }

  if (command.empty()) {
    print_usage(argv[0]);
    return 1;
  }

  // Auto-detect port for commands that need serial connection. `hud` uses the
  // port if available but tolerates absence (state is still saved).
  // `daemon` is deliberately absent: stop/status only talk to systemd, and
  // start (without --foreground) does too. Probing the port here made
  // `daemon stop` impossible while the daemon was running -- auto-detect
  // opened the tty, hit the holder's TIOCEXCL, and bailed out with "No device
  // found" before ever reaching systemctl. The --foreground path resolves the
  // port itself, since it is the one that actually needs the device.
  bool needs_serial = (command == "info" || command == "display" ||
                       command == "brightness" || command == "status" ||
                       command == "raw" || command == "hud" ||
                       command == "sleep-display" || command == "preset");
  bool serial_optional = (command == "hud");
  if (needs_serial && port.empty()) {
    if (verbose) {
      std::cout << "Auto-detecting device...\n";
    }
    auto detected = reed::Device::find_device(verbose);
    if (!detected) {
      if (serial_optional) {
        if (verbose) {
          std::cerr << "No device found; continuing without live apply.\n";
        }
      } else {
        std::cerr
            << "No device found. Specify port with -p or check connection.\n";
        return 1;
      }
    } else {
      port = *detected;
      if (!verbose) {
        // Keep this on stdout, where downstream consumers already parse it for
        // the port (e.g. koconnorgit/tryx-panorama's GUI), but move it aside
        // for --json so that output stays machine-readable.
        (json_output ? std::cerr : std::cout)
            << "Found device at " << port << "\n";
      }
    }
  }

  if (command == "info") {
    return cmd_info(port, verbose);
  } else if (command == "status") {
    return cmd_status(port, json_output, watch, verbose);
  } else if (command == "raw") {
    if (args.size() < 2) {
      std::cerr << "Usage: reed-tpse raw <METHOD> <ENDPOINT> [JSON]\n"
                   "       METHOD is POST (write) or STATE (read).\n";
      return 1;
    }
    return cmd_raw(port, args[0], args[1], args.size() > 2 ? args[2] : "",
                   verbose);
  } else if (command == "sleep-display") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse sleep-display <on|off>\n";
      return 1;
    }
    return cmd_sleep_display(port, args[0], verbose);
  } else if (command == "preset") {
    return cmd_preset(port, args, verbose);
  } else if (command == "upload") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse upload <file>\n";
      return 1;
    }
    return cmd_upload(args[0], verbose);
  } else if (command == "display") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse display <file...>\n";
      return 1;
    }
    return cmd_display(port, args, ratio, brightness, keepalive,
                       keepalive_interval, verbose);
  } else if (command == "brightness") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse brightness <0-100>\n";
      return 1;
    }
    return cmd_brightness(port, std::atoi(args[0].c_str()), verbose);
  } else if (command == "list") {
    return cmd_list();
  } else if (command == "delete") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse delete <file...>\n";
      return 1;
    }
    return cmd_delete(args);
  } else if (command == "hud") {
    return cmd_hud(port, args, verbose);
  } else if (command == "daemon") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse daemon <start|stop|status>\n";
      return 1;
    }
    if (args[0] == "start") {
      return cmd_daemon_start(port, foreground, system_scope, verbose);
    } else if (args[0] == "stop") {
      return cmd_daemon_stop(system_scope);
    } else if (args[0] == "status") {
      return cmd_daemon_status(system_scope);
    } else {
      std::cerr << "Unknown daemon command: " << args[0] << "\n";
      return 1;
    }
  } else {
    std::cerr << "Unknown command: " << command << "\n";
    print_usage(argv[0]);
    return 1;
  }
}
