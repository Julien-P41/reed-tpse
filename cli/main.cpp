#include <fcntl.h>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>

#include "reed/config.hpp"
#include "reed/device.hpp"
#include "cli_common.hpp"
#include "cli_commands.hpp"

#include <set>
#include <sstream>

namespace fs = std::filesystem;



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
         "  fan <low|mid|high|full> Show LCD fan RPM, or set a named tier\n"
         "                          (--smart follows temperature, --speed N pins\n"
         "                           a duty, --reset restores the vendor default)\n"
         "  screen <on|off>         Turn the panel itself on or off\n"
         "  rotate <normal|mirror>  Mirror Mode ! RESTARTS the cooler\n"
         "  sleep-display <on|off>  What the panel shows once the host stops\n"
         "                          handshaking: on = the firmware\'s standby\n"
         "                          animation, off = black\n"
         "  preset <name|list>      Show a firmware-bundled preset\n"
         "  upload <file>\n"
         "  list                    List media files on device\n"
         "  delete <file...>        Delete media files from device\n"
         "  display <file...>       Set display to specified media files\n"
         "                          (--play-mode single|shuffle|loop for a\n"
         "                           multi-file playlist;\n"
         "                           --split <left> <right> for two zones)\n"
         "  filter <Rain|Smoke|none> --opacity 0-100\n"
         "  brightness <0-100>      Set display brightness\n"
         "  lock-display <file>     Show <file> while the session is locked\n"
         "                          (--default restores the standby clip)\n"
         "  hud config              Configure on-device telemetry overlay\n"
         "                          (--zone right targets the right half when\n"
         "                           the screen is split)\n"
         "  hud clear               Disable the telemetry overlay\n"
         "  hud status\n"
         "  power <event>           Tell the device the host state: shutdown|\n"
         "                          lock|unlock|ac|battery|suspend|resume\n"
         "  daemon start            Start background daemon\n"
         "  daemon stop             Stop background daemon\n"
         "  daemon status           Show daemon status\n\n"
         "Options:\n"
         "  -p, --port <path>       Serial port (auto-detected if not "
         "specified)\n"
         "  -v, --verbose           Verbose output\n"
         "  --ratio <2:1|1:1>       Display ratio (default: 2:1)\n"
         "  --brightness <0-100>    Set brightness with display command\n"
         "  --foreground            Run daemon in foreground\n"
         "  --json                  Machine-readable output (status)\n"
         "  --watch <seconds>       Poll until interrupted (status)\n"
         "  --system                Act on the system-scope unit (daemon)\n"
         "  --speed <0-100>         Fan duty percent (fan), for finer "
         "control than\n"
         "                          the named tiers (low=35 mid=57 high=78 "
         "full=100)\n"
         "  --reset                 Reset the fan profile (fan)\n"
         "  --profile <file>        Send a fan profile (fan); refuses "
         "unvalidated\n"
         "                          curve data unless --force\n"
         "  --force                 Override the fan-profile refusal\n\n"
         "HUD options (with `hud configure`):\n"
         "  --metrics <csv>         Comma-separated labels, max 3. Known labels:\n"
         "                          CPU Temperature, CPU Frequency, CPU Usage,\n"
         "                          CPU Voltage, GPU Temperature, GPU Frequency,\n"
         "                          GPU Usage, GPU Voltage, Motherboard Temperature,\n"
         "                          Memory Frequency, Memory Utilization,\n"
         "                          Hard Disk Temperature, CPU Power, GPU Power,\n"
         "                          Memory Temperature, Date&Time\n"
         "  --align <align>         Left | Center | Right (default: Left)\n"
         "  --color <hex>           6 hex digits, no # -- e.g. 00FF00\n"
         "                          (default: FFFFFF)\n"
         "  --badges <csv>          cpu,gpu (or none). Default: none\n"
         "  --interval <sec>        Push interval in seconds (default: 5)\n"
         "  --unit <unit>           Celsius | Fahrenheit (default: Celsius)\n"
         "  --cpu-name <str>        Override auto-detected CPU name\n"
         "  --gpu-name <str>        Override auto-detected GPU name\n";
}

namespace {







}  // namespace

// The daemon holds the port exclusively (TIOCEXCL) and re-applies whenever the
// state file changes, so a command whose effect is stored in that file can
// simply save and let the daemon push it. Failing instead -- which is what
// every one of these used to do -- refuses a change the daemon is about to
// make anyway.
//
// Transient commands have nothing to save and still need the port for
// themselves: `power` and `rotate` are events, `raw` is arbitrary, `status`
// and `info` are reads.
// Load the state a command is about to modify and save back.
//
// A missing file is first run: defaults are the right answer. A file that
// exists but does not parse is NOT -- every caller here reads, changes one
// field and writes the whole thing back, so treating a bad read as first run
// persists defaults over the media, HUD, fan and filter settings that were in
// there. Refuse instead and say why.
// The `settings` block, derived from an overlay config plus the media filter.
// Shared by the screen config, the preset id and the overlay command, which
// all carry the same block.

// The one place a ScreenConfig is derived from saved state.
//
// This mapping used to be written out at each call site -- five copies of the
// HUD-to-settings assignment and three of the split right-zone block. They
// drifted: one copy assigned the right zone before its source was populated,
// shipping a zone with default styling, and that only showed up on the panel.







































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
  bool fan_smart = false;
  bool split = false;
  std::string play_mode;  // empty = keep whatever is saved
  int filter_opacity = 100;
  bool opacity_given = false;
  // Only read when --brightness is given, so the initial value never reaches
  // the device; DisplayState::brightness is the stored setting.
  int brightness = 0;
  bool foreground = false;
  bool json_output = false;
  bool brightness_given = false;
  bool system_scope = false;
  bool fan_reset = false;
  bool force = false;
  std::string fan_profile;
  int fan_speed = -1;
  int watch = 0;

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
    } else if (arg == "--split") {
      split = true;
    } else if (arg == "--smart") {
      fan_smart = true;
    } else if (arg == "--opacity") {
      if (i + 1 >= argc) {
        std::cerr << "--opacity needs a value 0-100\n";
        return 1;
      }
      filter_opacity = std::atoi(argv[++i]);
      if (filter_opacity < 0 || filter_opacity > 100) {
        std::cerr << "--opacity must be 0-100\n";
        return 1;
      }
      opacity_given = true;
    } else if (arg == "--play-mode") {
      if (i + 1 >= argc) {
        std::cerr << "--play-mode needs a value: single|shuffle|loop\n";
        return 1;
      }
      const std::string want = argv[++i];
      bool matched = false;
      for (const char* v : {"Single", "Shuffle", "Loop"}) {
        std::string lower;
        for (char c : std::string(v)) {
          lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::string given;
        for (char c : want) {
          given += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (given == lower) {
          play_mode = v;
          matched = true;
        }
      }
      if (!matched) {
        std::cerr << "Unknown play mode: \"" << want
                  << "\"  (use single|shuffle|loop)\n";
        return 1;
      }
    } else if (arg == "--ratio") {
      if (++i < argc) ratio = argv[i];
    } else if (arg == "--brightness") {
      if (++i < argc) {
        brightness = std::atoi(argv[i]);
        brightness_given = true;
      }
    } else if (arg == "--foreground") {
      foreground = true;
    } else if (arg == "--json") {
      json_output = true;
    } else if (arg == "--system") {
      system_scope = true;
    } else if (arg == "--reset") {
      fan_reset = true;
    } else if (arg == "--force") {
      force = true;
    } else if (arg == "--profile") {
      if (++i < argc) fan_profile = argv[i];
    } else if (arg == "--speed") {
      if (++i < argc) fan_speed = std::atoi(argv[i]);
    } else if (arg == "--watch") {
      if (++i < argc) watch = std::atoi(argv[i]);
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg.rfind("--", 0) == 0) {
      // Anything starting with -- that got this far is not a flag we know.
      // These used to fall through to the positional list, so a removed flag
      // like --keepalive was quietly taken as a media filename and reported as
      // "not on device" -- an error about the wrong thing entirely.
      std::cerr << "Unknown option: " << arg << "\n";
      print_usage(argv[0]);
      return 1;
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
                       command == "sleep-display" || command == "preset" ||
                       command == "fan" || command == "power");
  bool serial_optional = (command == "hud" || command == "preset");
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
  } else if (command == "fan") {
    return cmd_fan(port, fan_reset, args.empty() ? std::string() : args[0],
                   fan_speed, fan_smart, fan_profile, force, verbose);
  } else if (command == "lock-display") {
    return cmd_lock_display(port, args, brightness, brightness_given, verbose);
  } else if (command == "power") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse power <shutdown|lock|unlock|ac|battery>\n";
      return 1;
    }
    return cmd_power(port, args[0], verbose);
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
    return cmd_display(port, args, ratio, brightness, brightness_given,
                       play_mode, split, verbose);
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
  } else if (command == "rotate") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse rotate <normal|mirror>\n";
      return 1;
    }
    return cmd_rotate(port, args[0], force, verbose);
  } else if (command == "filter") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse filter <Rain|Smoke|none> "
                   "[--opacity 0-100]\n";
      return 1;
    }
    return cmd_filter(port, args[0], filter_opacity, opacity_given, verbose);
  } else if (command == "screen") {
    if (args.empty()) {
      std::cerr << "Usage: reed-tpse screen <on|off>\n";
      return 1;
    }
    return cmd_screen(port, args[0], verbose);
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
