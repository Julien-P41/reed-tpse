#include "cli_common.hpp"

#include <algorithm>
#include <cstdio>
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

bool daemon_holds_port(const std::string& port) {
  auto holder = reed::find_port_holder(port.empty() ? "/dev/ttyACM0" : port);
  if (!holder || holder->comm.find("reed-tpse") == std::string::npos) {
    return false;
  }

  // Matching the process name alone is not enough: `display --keepalive` left
  // running in another terminal is also a reed-tpse holding the port, and it
  // will never apply a saved state. Telling the user "the daemon will do it"
  // in that case is a lie that looks like success. Check the argv.
  std::ifstream cmdline("/proc/" + std::to_string(holder->pid) + "/cmdline",
                        std::ios::binary);
  if (!cmdline) return false;
  std::string argv((std::istreambuf_iterator<char>(cmdline)),
                   std::istreambuf_iterator<char>());
  std::replace(argv.begin(), argv.end(), '\0', ' ');
  return argv.find(" daemon ") != std::string::npos;
}

int defer_to_daemon(const std::string& what) {
  std::cout << what << " saved. The daemon holds the port and applies it "
                       "within a second.\n";
  return 0;
}
