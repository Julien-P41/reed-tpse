#include "reed/sysinfo.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace reed {

namespace {

std::string read_trim(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  std::string s;
  std::getline(f, s);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  return s;
}

std::string read_all(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

double parse_double(const std::string& s, double def = 0.0) {
  try {
    return std::stod(s);
  } catch (...) {
    return def;
  }
}

// Walks /sys/class/hwmon looking for a directory whose `name` file matches any
// of the given names. Returns the first hit.
std::string find_hwmon_by_name(const std::vector<std::string>& names) {
  const std::string root = "/sys/class/hwmon";
  std::error_code ec;
  if (!fs::exists(root, ec)) return {};
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (ec) break;
    std::string name_path = entry.path().string() + "/name";
    std::string name = read_trim(name_path);
    if (std::find(names.begin(), names.end(), name) != names.end()) {
      return entry.path().string();
    }
  }
  return {};
}

// Runs cmd, returns stdout as a string. Empty on failure / nonzero exit.
std::string run_capture(const std::string& cmd) {
  std::array<char, 256> buf;
  std::string out;
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return {};
  while (fgets(buf.data(), buf.size(), pipe)) {
    out.append(buf.data());
  }
  int rc = pclose(pipe);
  if (rc != 0) return {};
  return out;
}

bool have_command(const std::string& name) {
  std::string out = run_capture("command -v " + name + " 2>/dev/null");
  return !out.empty();
}

}  // namespace

SystemMonitor::SystemMonitor() = default;

void SystemMonitor::probe_hardware() {
  if (hw_probed_) return;
  hw_probed_ = true;

  // CPU temperature source — AMD Ryzen uses k10temp / zenpower, Intel uses coretemp.
  cpu_hwmon_dir_ = find_hwmon_by_name({"k10temp", "zenpower", "coretemp"});

  // GPU backend: prefer Nvidia dGPU over AMD iGPU.
  if (have_command("nvidia-smi")) {
    // Confirm nvidia-smi can actually query something.
    std::string probe =
        run_capture("nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null");
    if (!probe.empty()) {
      gpu_backend_ = GpuBackend::Nvidia;
      // Cache name (first non-empty line)
      std::istringstream ss(probe);
      std::string line;
      if (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
          line.pop_back();
        gpu_name_cache_ = line;
      }
      return;
    }
  }

  // AMD fallback: first /sys/class/drm/card*/device with gpu_busy_percent.
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
    if (ec) break;
    std::string name = entry.path().filename().string();
    if (name.rfind("card", 0) != 0) continue;
    if (name.find('-') != std::string::npos) continue;  // skip connector nodes
    std::string device_path = entry.path().string() + "/device";
    std::string busy_path = device_path + "/gpu_busy_percent";
    if (fs::exists(busy_path)) {
      gpu_backend_ = GpuBackend::Amd;
      amd_card_path_ = device_path;
      std::string marketing = read_trim(device_path + "/product_name");
      if (marketing.empty()) marketing = "AMD GPU";
      gpu_name_cache_ = marketing;
      break;
    }
  }
}

CpuMetrics SystemMonitor::sample_cpu() {
  CpuMetrics m;

  // Temperature
  if (!cpu_hwmon_dir_.empty()) {
    std::string raw = read_trim(cpu_hwmon_dir_ + "/temp1_input");
    if (!raw.empty()) m.temperature_c = parse_double(raw) / 1000.0;
  }

  // Usage — delta against previous /proc/stat snapshot.
  {
    std::ifstream f("/proc/stat");
    std::string line;
    if (std::getline(f, line)) {
      std::istringstream ss(line);
      std::string tag;
      int64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0,
              softirq = 0;
      ss >> tag >> user >> nice >> system >> idle >> iowait >> irq >> softirq;
      if (tag == "cpu") {
        int64_t total_idle = idle + iowait;
        int64_t total = user + nice + system + idle + iowait + irq + softirq;
        if (cpu_usage_primed_) {
          int64_t d_idle = total_idle - prev_cpu_idle_;
          int64_t d_total = total - prev_cpu_total_;
          if (d_total > 0) {
            m.usage_percent =
                (1.0 - static_cast<double>(d_idle) / d_total) * 100.0;
          }
        }
        prev_cpu_idle_ = total_idle;
        prev_cpu_total_ = total;
        cpu_usage_primed_ = true;
      }
    }
  }

  // Frequency — average of per-core scaling_cur_freq (kHz → MHz).
  {
    std::error_code ec;
    double sum = 0.0;
    int count = 0;
    for (const auto& entry :
         fs::directory_iterator("/sys/devices/system/cpu", ec)) {
      if (ec) break;
      std::string name = entry.path().filename().string();
      if (name.rfind("cpu", 0) != 0) continue;
      if (name.size() <= 3 || !std::isdigit(static_cast<unsigned char>(name[3])))
        continue;
      std::string freq_path =
          entry.path().string() + "/cpufreq/scaling_cur_freq";
      std::string raw = read_trim(freq_path);
      if (!raw.empty()) {
        sum += parse_double(raw) / 1000.0;
        ++count;
      }
    }
    if (count > 0) m.frequency_mhz = sum / count;
  }

  return m;
}

GpuMetrics SystemMonitor::sample_gpu() {
  GpuMetrics m;
  m.name = gpu_name_cache_;

  if (gpu_backend_ == GpuBackend::Nvidia) {
    // One shell-out gets everything. nvidia-smi is fast (~50ms) and stable.
    std::string csv = run_capture(
        "nvidia-smi --query-gpu=utilization.gpu,temperature.gpu,clocks.gr "
        "--format=csv,noheader,nounits -i 0 2>/dev/null");
    if (!csv.empty()) {
      std::istringstream ss(csv);
      std::string line;
      if (std::getline(ss, line)) {
        std::vector<std::string> parts;
        std::string token;
        std::istringstream ls(line);
        while (std::getline(ls, token, ',')) {
          // Trim whitespace
          size_t start = token.find_first_not_of(" \t\r\n");
          size_t end = token.find_last_not_of(" \t\r\n");
          if (start == std::string::npos) {
            parts.emplace_back();
          } else {
            parts.push_back(token.substr(start, end - start + 1));
          }
        }
        if (parts.size() >= 3) {
          m.usage_percent = parse_double(parts[0]);
          m.temperature_c = parse_double(parts[1]);
          m.frequency_mhz = parse_double(parts[2]);
        }
      }
    }
    return m;
  }

  if (gpu_backend_ == GpuBackend::Amd) {
    m.usage_percent =
        parse_double(read_trim(amd_card_path_ + "/gpu_busy_percent"));

    // Temperature via hwmon subdir under the card's device.
    std::error_code ec;
    std::string hwmon_root = amd_card_path_ + "/hwmon";
    if (fs::exists(hwmon_root, ec)) {
      for (const auto& entry : fs::directory_iterator(hwmon_root, ec)) {
        if (ec) break;
        std::string temp = read_trim(entry.path().string() + "/temp1_input");
        if (!temp.empty()) {
          m.temperature_c = parse_double(temp) / 1000.0;
          std::string volt = read_trim(entry.path().string() + "/in0_input");
          if (!volt.empty()) m.voltage_v = parse_double(volt) / 1000.0;
          break;
        }
      }
    }

    // Active sclk dpm level (line marked with '*').
    std::string sclk = read_all(amd_card_path_ + "/pp_dpm_sclk");
    std::istringstream ss(sclk);
    std::string line;
    std::regex mhz_re(R"((\d+)Mhz)");
    while (std::getline(ss, line)) {
      if (line.find('*') == std::string::npos) continue;
      std::smatch match;
      if (std::regex_search(line, match, mhz_re)) {
        m.frequency_mhz = parse_double(match[1].str());
      }
      break;
    }
    return m;
  }

  return m;
}

MemoryMetrics SystemMonitor::sample_memory() {
  MemoryMetrics m;
  std::ifstream f("/proc/meminfo");
  std::string line;
  int64_t total_kb = 0, avail_kb = 0;
  while (std::getline(f, line)) {
    if (line.rfind("MemTotal:", 0) == 0) {
      std::istringstream ss(line.substr(9));
      ss >> total_kb;
    } else if (line.rfind("MemAvailable:", 0) == 0) {
      std::istringstream ss(line.substr(13));
      ss >> avail_kb;
    }
  }
  if (total_kb > 0) {
    m.usage_percent =
        (static_cast<double>(total_kb - avail_kb) / total_kb) * 100.0;
  }
  // DRAM frequency: no stable cross-vendor sysfs path. Leave at 0.
  return m;
}

SystemMetrics SystemMonitor::sample() {
  probe_hardware();
  SystemMetrics s;
  s.cpu = sample_cpu();
  s.gpu = sample_gpu();
  s.memory = sample_memory();
  return s;
}

std::string SystemMonitor::detect_cpu_name() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line)) {
    // Accept either "model name" (x86) or "Model" (some ARM) — we only ship x86
    // here, but be permissive.
    if (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0) {
      auto colon = line.find(':');
      if (colon != std::string::npos) {
        std::string val = line.substr(colon + 1);
        size_t start = val.find_first_not_of(" \t");
        if (start != std::string::npos) return val.substr(start);
      }
    }
  }
  return "Unknown CPU";
}

std::string SystemMonitor::detect_gpu_name() {
  if (have_command("nvidia-smi")) {
    std::string out = run_capture(
        "nvidia-smi --query-gpu=name --format=csv,noheader -i 0 2>/dev/null");
    if (!out.empty()) {
      // Strip trailing newline
      while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
      if (!out.empty()) return out;
    }
  }
  // AMD sysfs fallback.
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
    if (ec) break;
    std::string name = entry.path().filename().string();
    if (name.rfind("card", 0) != 0) continue;
    if (name.find('-') != std::string::npos) continue;
    std::string device_path = entry.path().string() + "/device";
    if (!fs::exists(device_path + "/gpu_busy_percent")) continue;
    std::string marketing = read_trim(device_path + "/product_name");
    if (!marketing.empty()) return marketing;
    return "AMD GPU";
  }
  return "Unknown GPU";
}

}  // namespace reed
