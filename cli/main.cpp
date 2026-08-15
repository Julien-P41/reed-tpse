#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <thread>

#include "reed/adb.hpp"
#include "reed/config.hpp"
#include "reed/device.hpp"
#include "reed/media.hpp"

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
         "  list                    List media files on device\n"
         "  delete <file...>        Delete media files from device\n"
         "  daemon start            Start background daemon\n"
         "  daemon stop             Stop background daemon\n"
         "  daemon status           Show daemon status\n\n"
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
         "  --system                Act on the system-scope unit (daemon)\n";
}

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

  // Save state for daemon
  reed::DisplayState state;
  state.media = media_files;
  state.ratio = ratio;
  state.brightness = brightness;
  reed::ConfigManager::save_state(state);

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

  reed::Device device(actual_port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << actual_port << "\n";
    return 1;
  }

  device.handshake();

  reed::ScreenConfig screen_config;
  screen_config.media = state->media;
  screen_config.ratio = state->ratio;
  screen_config.screen_mode = state->screen_mode;
  screen_config.play_mode = state->play_mode;

  device.set_screen_config(screen_config);
  device.set_brightness(state->brightness);

  std::cout << "Display restored. Running keepalive...\n";

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  while (g_running) {
    std::this_thread::sleep_for(std::chrono::seconds(keepalive_interval));
    if (!g_running) break;
    device.handshake();
  }

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

  // Auto-detect port for commands that need serial connection
  bool needs_serial = (command == "info" || command == "display" ||
                       command == "brightness" || command == "daemon" ||
                       command == "status" || command == "raw");
  if (needs_serial && port.empty()) {
    if (verbose) {
      std::cout << "Auto-detecting device...\n";
    }
    auto detected = reed::Device::find_device(verbose);
    if (!detected) {
      std::cerr
          << "No device found. Specify port with -p or check connection.\n";
      return 1;
    }
    port = *detected;
    if (!verbose) {
      // Keep this on stdout, where downstream consumers already parse it for
      // the port (e.g. koconnorgit/tryx-panorama's GUI), but move it aside
      // for --json so that output stays machine-readable.
      (json_output ? std::cerr : std::cout) << "Found device at " << port
                                            << "\n";
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
