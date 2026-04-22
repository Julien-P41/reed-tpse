#pragma once

#include <cstdint>
#include <string>

namespace reed {

struct CpuMetrics {
  double temperature_c = 0.0;
  double usage_percent = 0.0;
  double frequency_mhz = 0.0;
};

struct GpuMetrics {
  std::string name;
  double temperature_c = 0.0;
  double usage_percent = 0.0;
  double frequency_mhz = 0.0;
  double voltage_v = 0.0;
};

struct MemoryMetrics {
  double usage_percent = 0.0;
  double frequency_mhz = 0.0;
};

struct SystemMetrics {
  CpuMetrics cpu;
  GpuMetrics gpu;
  MemoryMetrics memory;
};

// Collects CPU/GPU/memory telemetry from Linux sysfs, /proc, and nvidia-smi.
// Stateful: first sample primes the /proc/stat delta counters, so cpu.usage_percent
// is 0 until the second call.
class SystemMonitor {
 public:
  SystemMonitor();

  SystemMetrics sample();

  // Marketing names for CPU/GPU badges. Cheap to call; cache the result
  // at the caller if you don't want to re-read per frame.
  static std::string detect_cpu_name();
  static std::string detect_gpu_name();

 private:
  enum class GpuBackend { None, Nvidia, Amd };

  int64_t prev_cpu_idle_ = 0;
  int64_t prev_cpu_total_ = 0;
  bool cpu_usage_primed_ = false;

  bool hw_probed_ = false;
  std::string cpu_hwmon_dir_;  // e.g. /sys/class/hwmon/hwmon3
  GpuBackend gpu_backend_ = GpuBackend::None;
  std::string amd_card_path_;  // /sys/class/drm/cardN/device when AMD
  std::string gpu_name_cache_;

  void probe_hardware();

  CpuMetrics sample_cpu();
  GpuMetrics sample_gpu();
  MemoryMetrics sample_memory();
};

}  // namespace reed
