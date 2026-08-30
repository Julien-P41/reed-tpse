#include "cli_common.hpp"

#include "reed/hud.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <fstream>
#include <iostream>
#include <iterator>

// The one definition of g_running. See the note in cli_common.hpp: a second
// one would be silent.
std::atomic<bool> g_running{true};

void signal_handler(int sig) {
  if (sig == SIGTERM || sig == SIGINT) {
    g_running = false;
  }
}

// Build the SysinfoData payload the device expects for the given labels.
//
// Value, unit and precision all come from the one metric table in
// reed/hud.hpp. This was a fifteen-branch if/else chain that had to agree with
// the accept list in cmd_hud.cpp, the PcInfo mapping in device.cpp and the
// --help text, by hand.
std::vector<reed::SysinfoData> build_sysinfo(
    const std::vector<std::string>& labels, const reed::SystemMetrics& m) {
  std::vector<reed::SysinfoData> out;
  auto fmt = [](double v, int precision) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return std::string(buf);
  };
  for (const auto& label : labels) {
    reed::SysinfoData d;
    d.label = label;
    const reed::HudMetric* metric = reed::find_hud_metric(label);
    if (metric && metric->read) {
      // value_or(0.0): a metric this machine cannot source sends 0 rather than
      // nothing, which is what the firmware expects. `hud configure` warns at
      // the point the user picks it, so a permanent 0 is never a surprise.
      d.value = fmt(metric->read(m).value_or(0.0), metric->precision);
      d.unit = metric->unit;
    } else {
      // Date&Time is drawn from the device's own clock; it needs no value.
      d.value = "0";
    }
    out.push_back(d);
  }
  return out;
}

std::optional<reed::DisplayState> load_state_for_update() {
  reed::LoadStatus status = reed::LoadStatus::Ok;
  auto state = reed::ConfigManager::load_state(&status);
  if (state) return state;
  if (status == reed::LoadStatus::Missing) return reed::DisplayState{};

  std::cerr << "Refusing to continue: " << reed::ConfigManager::get_state_path()
            << (status == reed::LoadStatus::Malformed
                    ? " exists but does not parse.\n"
                    : " exists but could not be read.\n")
            << "  Saving now would overwrite it with defaults. Fix or delete "
               "the file first.\n";
  return std::nullopt;
}

bool parse_int(const std::string& in, int* out) {
  if (in.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const long v = std::strtol(in.c_str(), &end, 10);
  // Reject trailing junk ("50abc"), leading junk ("abc" -> end == start),
  // overflow, and anything outside int.
  if (end == in.c_str() || *end != '\0') return false;
  if (errno == ERANGE || v < INT_MIN || v > INT_MAX) return false;
  *out = static_cast<int>(v);
  return true;
}

bool save_state_or_report(const reed::DisplayState& state) {
  if (reed::ConfigManager::save_state(state)) return true;
  std::cerr << "Failed to write " << reed::ConfigManager::get_state_path()
            << "\n"
               "  The setting was NOT saved, so nothing will re-apply it.\n";
  return false;
}

bool daemon_holds_port(const std::string& port) {
  // Every holder, not the first one found. adb's fork-server keeps the CDC
  // device open alongside the daemon, and /proc is walked in whatever order
  // readdir gives -- so asking for a single holder made this answer depend on
  // directory order, and a command would defer or fail arbitrarily between
  // runs on an unchanged system.
  for (const auto& holder :
       reed::find_port_holders(port.empty() ? "/dev/ttyACM0" : port)) {
    if (holder.comm.find("reed-tpse") == std::string::npos) continue;

    // The name alone is not enough: any reed-tpse invocation could be holding
    // the port, and only a daemon will apply a saved state. Telling the user
    // "the daemon will do it" otherwise is a lie that looks like success.
    std::ifstream cmdline("/proc/" + std::to_string(holder.pid) + "/cmdline",
                          std::ios::binary);
    if (!cmdline) continue;
    std::string argv((std::istreambuf_iterator<char>(cmdline)),
                     std::istreambuf_iterator<char>());
    std::replace(argv.begin(), argv.end(), '\0', ' ');
    if (argv.find(" daemon ") != std::string::npos) return true;
  }
  return false;
}

int defer_to_daemon(const std::string& what) {
  std::cout << what << " saved. The daemon holds the port and applies it "
                       "within a second.\n";
  return 0;
}
