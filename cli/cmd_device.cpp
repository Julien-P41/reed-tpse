// Device-level commands: what the cooler is, what it reports, and the
// settings that are events rather than stored state.
//
// `power` and `rotate` are the two that cannot be handed to the daemon when
// the port is busy -- they carry no state to save, so there would be nothing
// for the daemon to apply.

#include "cli_common.hpp"
#include "cli_commands.hpp"

#include <algorithm>
#include <csignal>
#include <iomanip>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "reed/adb.hpp"
#include "reed/device.hpp"
#include "reed/picojson.h"
#include "reed/status_cache.hpp"

struct PowerEvent {
  const char* alias;
  const char* wire;
};

static const PowerEvent kPowerEvents[] = {
    {"shutdown", "shutdown"},   {"lock", "lock-screen"},
    {"unlock", "unlock-screen"}, {"ac", "ac-power"},
    {"battery", "on-battery"},  {"suspend", "suspend"},
    {"resume", "resume"},
};

// Host power events. Friendly aliases map to the wire vocabulary.
static const char* lookup_power_event(const std::string& in) {
  for (const auto& e : kPowerEvents) {
    if (in == e.alias || in == e.wire) return e.wire;
  }
  return nullptr;
}

int cmd_info(const std::string& port, bool verbose) {
  // Device identity does not change while the device is plugged in, so the
  // daemon's snapshot is as good as a live read -- and it is the only one
  // available while the daemon holds the port.
  if (daemon_holds_port(port)) {
    if (auto snap = reed::StatusCache::read();
        snap && !snap->info.product_id.empty()) {
      std::cout << "Device Information:\n"
                << "  Product: " << snap->info.product_id << "\n"
                << "  OS: " << snap->info.os << "\n"
                << "  Serial: " << snap->info.serial << "\n"
                << "  App Version: " << snap->info.app_version << "\n"
                << "  Firmware: " << snap->info.firmware << "\n"
                << "  Hardware: " << snap->info.hardware << "\n";
      // Same fields and the same labels as the live path below. This printed
      // "App:" where the live one prints "App Version:", and dropped the
      // Attributes line entirely, so the two outputs disagreed depending on
      // whether a daemon happened to be running.
      if (!snap->info.attributes.empty()) {
        std::cout << "  Attributes: ";
        for (size_t i = 0; i < snap->info.attributes.size(); ++i) {
          if (i > 0) std::cout << ", ";
          std::cout << snap->info.attributes[i];
        }
        std::cout << "\n";
      }
      // Say how old it is. Printing a bare "(from the daemon)" made a snapshot
      // from an hour ago read exactly like one from a second ago.
      const long long age = snap->age_seconds();
      std::cout << "  (from the daemon, " << age << "s ago)\n";
      if (reed::StatusSnapshot::stale_at(age)) {
        // Identity does not change while a device is plugged in, so this is
        // still the best answer available -- but the daemon has stopped
        // hearing from the cooler, and that is worth saying out loud.
        std::cerr << "  ⚠ The daemon has not had an answer from the device for "
                  << age
                  << "s. This identity is the last one it saw.\n";
      }
      return 0;
    }
  }

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

int cmd_status(const std::string& port, bool json_output, int watch,
                      bool verbose) {
  // The daemon holds the port exclusively, so a live read is impossible while
  // it runs -- which is most of the time. It publishes what it last saw, and
  // that is the same `STATE all` body this command would ask for, so read
  // that instead of failing. Only for a one-shot: --watch wants live values,
  // and a cache cannot provide them.
  if (watch == 0 && daemon_holds_port(port)) {
    if (auto snap = reed::StatusCache::read()) {
      // The cached answer carries the same exit code as a live one.
      //
      // It used to return 0 whatever the device reported, while the live path
      // below returned 2 on a fault. Since the cache is only consulted when
      // the daemon holds the port -- the recommended setup -- a monitoring
      // check built on the documented contract stopped detecting faults the
      // moment the user followed the README.
      //
      // A stale snapshot is also a fault, and arguably the more urgent one:
      // the daemon is running and no longer getting answers.
      // One clock read, so the printed age and the staleness cannot disagree.
      const long long age = snap->age_seconds();
      const bool stale = reed::StatusSnapshot::stale_at(age);
      const bool healthy = snap->status.healthy();

      if (json_output) {
        // Same shape as the live path, plus the two fields only a cached read
        // can report. Built with the JSON writer rather than concatenated, so
        // a value containing a quote cannot produce unparseable output.
        picojson::array warnings;
        for (const auto& w : snap->status.warnings) {
          picojson::object entry;
          entry["description"] = picojson::value(w.description);
          entry["type"] = picojson::value(w.type);
          warnings.push_back(picojson::value(entry));
        }

        picojson::object out;
        out["fanLCD"] = picojson::value(snap->status.fan_lcd);
        out["turboPump"] = picojson::value(snap->status.turbo_pump);
        out["availableStorage"] =
            picojson::value(snap->status.available_storage);
        out["warning"] = picojson::value(warnings);
        out["healthy"] = picojson::value(healthy);
        out["ageSeconds"] = picojson::value(static_cast<double>(age));
        out["stale"] = picojson::value(stale);

        std::cout << picojson::value(out).serialize() << std::endl;
      } else {
        print_status(snap->status);
        std::cout << "  (from the daemon, " << age << "s ago)\n";
      }

      if (stale) {
        std::cerr << "Stale: the daemon holds the port but has had no answer "
                     "from the device\n"
                     "       for "
                  << age
                  << "s. The figures above are the last ones it saw.\n";
      }
      return (healthy && !stale) ? 0 : 2;
    }
  }

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

int cmd_raw(const std::string& port, const std::string& method,
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

// Panel power, the vendor's screen on/off. Not the same as brightness 0:
// `enable:false` blanks the panel entirely, and the setting is volatile like
// everything else here, so the daemon re-asserts it on connect.
// Mirror Mode. Two things make this different from every other command here.
//
// It does NOT take effect when sent: `POST rotate` is stored, and the panel
// only turns when the cooler next restarts. Measured -- `rotate--90` is
// dispatched, nothing changes, and the new orientation appears after a reboot.
// (KANALI looks like it restarts on confirm because it bundles adb.exe and
// reboots the cooler itself; no `reboot` command goes over the wire.) So this
// reboots the device for you, or the command would appear to do nothing.
//
// And there is no read-back: nothing on the host can tell you the current
// rotation, so a wrong value only shows itself after the restart.
int cmd_rotate(const std::string& port, const std::string& arg,
                      bool force, bool verbose) {
  int degree;
  if (arg == "normal") {
    degree = 270;  // upright on the unit this was captured from
  } else if (arg == "mirror") {
    degree = 90;
  } else {
    if (!parse_int(arg, &degree)) {
      std::cerr << "Usage: reed-tpse rotate <normal|mirror|0|90|180|270>\n";
      return 1;
    }
    if (degree != 0 && degree != 90 && degree != 180 && degree != 270) {
      std::cerr << "Usage: reed-tpse rotate <normal|mirror|0|90|180|270>\n";
      return 1;
    }
  }

  if (degree != 90 && degree != 270) {
    std::cerr << "⚠ Only 90 and 270 are known-good. The vendor never sends "
                 "0 or 180,\n"
                 "  and on this hardware upright IS 270 -- so 0 or 180 leaves "
                 "the panel\n"
                 "  90° out, which looks exactly like waterfall mode.\n";
    if (!force) {
      std::cerr << "  Re-run with --force if you meant it.\n";
      return 1;
    }
  }

  if (!force) {
    std::cout << "This reboots the cooler -- rotation only applies at its next\n"
                 "start. The panel goes dark for ~20s. The PC is unaffected.\n"
                 "Continue? [y/N] ";
    std::string answer;
    std::getline(std::cin, answer);
    if (answer != "y" && answer != "Y") {
      std::cout << "Cancelled.\n";
      return 0;
    }
  }

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }
  device.drain();

  if (!device.set_rotation(degree)) {
    std::cerr << "No response to 'POST rotate'\n";
    return 1;
  }

  std::cout << "Rotation stored: " << degree << "°.\n";

  if (!reed::Adb::is_device_connected()) {
    std::cout << "  ⚠ adb is not available, so the cooler was not restarted.\n"
                 "    Nothing changes until it next starts -- and it will then\n"
                 "    come up rotated, which is easy to forget about.\n";
    return 0;
  }

  std::cout << "  Restarting the cooler to apply it...\n";
  if (!reed::Adb::reboot()) {
    std::cerr << "  Reboot failed. The setting is stored and will apply at the "
                 "next start.\n";
    return 1;
  }
  std::cout << "  Done. Give it ~20s, then start the daemon again.\n";
  return 0;
}

int cmd_screen(const std::string& port, const std::string& arg,
                      bool verbose) {
  bool enable;
  if (arg == "on") {
    enable = true;
  } else if (arg == "off") {
    enable = false;
  } else {
    std::cerr << "Usage: reed-tpse screen <on|off>\n";
    return 1;
  }

  auto state = load_state_for_update();
  if (!state) return 1;
  state->screen_on = enable;
  if (!save_state_or_report(*state)) return 1;

  if (daemon_holds_port(port)) {
    return defer_to_daemon("Panel power");
  }

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }
  device.drain();

  if (!device.set_screen_power(enable)) {
    std::cerr << "No response to 'POST waterBlockScreen'\n";
    return 1;
  }

  std::cout << "Panel " << (enable ? "on" : "off") << "\n";
  return 0;
}

int cmd_sleep_display(const std::string& port, const std::string& arg,
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

  // `displayInSleep` reads backwards from the outside: it is the device's own
  // "display something while the host is asleep", so ON gives the standby
  // animation and OFF gives a black panel. Measured both ways through a full
  // disconnect timeout. This tool passes the value straight through to match
  // the vendor's own toggle.
  //
  // The device answers 200 with an empty body either way, so the reply proves
  // nothing. Persist it instead and let the daemon re-apply, since the setting
  // lives in controller RAM and is lost whenever USB power drops.
  auto state = load_state_for_update();
  if (!state) return 1;
  state->display_in_sleep = enable;
  // Saved once, here, and checked. This used to write the file twice -- once
  // unchecked before the device call and again after it.
  if (!save_state_or_report(*state)) return 1;

  if (daemon_holds_port(port)) {
    return defer_to_daemon("Sleep-display");
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

  std::cout << "Sleep display " << (enable ? "on" : "off") << ".\n";
  if (enable) {
    std::cout << "  Once the host stops handshaking the panel runs the "
                 "firmware's standby\n  animation.\n";
  } else {
    std::cout << "  Once the host stops handshaking the panel goes black.\n";
  }
  std::cout << "  Either way it takes about 60s -- the device waits out its "
               "own timeout\n  before switching.\n";
  return 0;
}

int cmd_power(const std::string& port, const std::string& arg,
                     bool verbose) {
  const char* wire = lookup_power_event(arg);
  if (!wire) {
    std::cerr << "Unknown power event: \"" << arg << "\"\n"
              << "Use: shutdown | lock | unlock | ac | battery | suspend |\n"
              << "     resume\n";
    return 1;
  }

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }
  device.drain();

  if (!device.send_power_event(wire)) {
    std::cerr << "No response to 'POST power'\n";
    return 1;
  }

  std::cout << "Sent power event: " << wire << "\n";
  if (std::string(wire) == "shutdown" || std::string(wire) == "lock-screen" ||
      std::string(wire) == "suspend") {
    std::cout << "  ⚠ The panel wakes again as soon as something reopens the\n"
                 "    serial port -- the device treats a reconnect as a wake.\n"
                 "    Stop the daemon first if the panel should stay dark.\n";
  }
  return 0;
}
