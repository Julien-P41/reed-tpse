// The telemetry overlay.
//
// The helpers below keep internal linkage: the label vocabulary and the
// host-side availability checks are this command's business alone. They were
// in an anonymous namespace in the old single-file CLI and stay in one here,
// which is what stops the label table becoming a second source of truth
// alongside the firmware's.

#include "cli_common.hpp"
#include "cli_commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "reed/adb.hpp"
#include "reed/device.hpp"
#include "reed/mapping.hpp"
#include "reed/sysinfo.hpp"

namespace {

// Colours are handled as bare hex -- `00FF00`, no `#`.
//
// `--color #00FF00` is a trap: unquoted, every common shell treats `#` as the
// start of a comment and drops it and everything after, so the flag arrives
// empty and the value is silently lost. Dropping the `#` from the CLI, the
// config file and every printed value removes the trap rather than warning
// about it. A `#` is still accepted on input, and added back when the value
// goes on the wire -- the device wants `#RRGGBB`.
std::string normalise_hex_colour(const std::string& in) {
  return (!in.empty() && in[0] == '#') ? in.substr(1) : in;
}

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

// Accept the human-readable spelling people will reasonably type (and that
// downstream GUIs already send) but put the firmware's spelling on the wire.
std::string canonical_hud_label(const std::string& label) {
  if (label == "Date & Time" || label == "Date and Time") return "Date&Time";
  return label;
}

}  // namespace

int cmd_hud(const std::string& port, const std::vector<std::string>& args,
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

  // Load existing state so we preserve media/brightness/ratio. Guarded: this
  // function writes the whole state back, so a bad read must not silently
  // become a reset.
  auto loaded = load_state_for_update();
  if (!loaded) return 1;
  reed::DisplayState state = *loaded;

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

  if (action != "config" && action != "configure") {
    std::cerr << "Unknown hud action: " << action << "\n";
    return 1;
  }

  // Parse hud configure flags from args[1..].
  // --zone is parsed below, so start from the left zone and switch to the
  // right one once we know. Editing the right zone starts from whatever it
  // already had, or from the left zone the first time.
  bool zone_right = false;
  for (size_t probe = 0; probe + 1 < args.size(); ++probe) {
    if (args[probe] == "--zone" && args[probe + 1] == "right") zone_right = true;
  }
  reed::HudConfig h = (zone_right && state.hud_right) ? *state.hud_right
                                                      : state.hud;
  h.enabled = true;
  bool metrics_provided = false;

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](const char* flag) -> std::string {
      if (++i >= args.size()) {
        std::cerr << "Missing value for " << flag << "\n";
        if (flag == std::string("--color")) {
          // Overwhelmingly the cause: `--color #00FF00` unquoted, where the
          // shell treats # as a comment and drops the rest of the line.
          std::cerr << "  If you wrote `--color #RRGGBB`, the # started a "
                       "shell comment and\n"
                       "  the value never arrived. Drop the # or quote it: "
                       "--color 00FF00\n";
        }
        std::exit(1);
      }
      return args[i];
    };
    if (a == "--zone") {
      const std::string z = next("--zone");
      if (z == "right") {
        zone_right = true;
      } else if (z != "left") {
        std::cerr << "Invalid --zone: " << z << "  (left | right)\n";
        return 1;
      }
    } else if (a == "--metrics") {
      h.metrics = split_csv(next("--metrics"));
      for (auto& m : h.metrics) m = canonical_hud_label(m);
      metrics_provided = true;
    } else if (a == "--align") {
      h.align = next("--align");
    } else if (a == "--color") {
      h.color = normalise_hex_colour(next("--color"));
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
  if (h.color.size() != 6 ||
      h.color.find_first_not_of("0123456789abcdefABCDEF") !=
          std::string::npos) {
    std::cerr << "Invalid --color: " << h.color
              << "  (want 6 hex digits, e.g. 00FF00)\n";
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

  if (zone_right) {
    state.hud_right = h;
  } else {
    state.hud = h;
  }
  if (!reed::ConfigManager::save_state(state)) {
    std::cerr << "Failed to save state\n";
    return 1;
  }

  // Apply live if we have a device. Non-fatal if not connected — state is
  // saved and the daemon will apply it on next start.
  if (daemon_holds_port(port)) {
    std::cout << "HUD saved. The daemon holds the port and applies it within "
                 "a second.\n";
    return 0;
  }

  if (!port.empty()) {
    reed::Device device(port, verbose);
    if (device.connect() && device.handshake()) {
      device.send_spec(h.cpu_name, h.gpu_name);
      device.set_temperature_unit(h.temperature_unit);

      // `POST preset` carries styling and metrics together and leaves the
      // media alone -- the vendor's own overlay path. Re-sending a whole
      // screen config here used to reload the video on every HUD tweak.
      const reed::DisplaySettings settings =
          settings_from(h, state.filter, state.filter_opacity);
      device.set_overlay(settings, h.enabled ? h.metrics
                                             : std::vector<std::string>{});

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
