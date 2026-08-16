#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace reed {

// std::optional marks "this machine has no source for it", which is distinct
// from a real reading of zero. The CLI warns instead of sending a silent 0.
struct CpuMetrics {
  double temperature_c = 0.0;
  double usage_percent = 0.0;
  double frequency_mhz = 0.0;
  std::optional<double> voltage_v;  // super-I/O VCore, e.g. nct6798 in0
  std::optional<double> power_w;    // RAPL; root-only on kernels >= 5.10
};

struct GpuMetrics {
  std::string name;
  double temperature_c = 0.0;
  double usage_percent = 0.0;
  double frequency_mhz = 0.0;
  double voltage_v = 0.0;
  std::optional<double> power_w;
};

struct MemoryMetrics {
  double usage_percent = 0.0;
  // DIMM speed is not exposed anywhere unprivileged -- /proc/meminfo has no
  // such field, and SPD/DMI reads need root. Left unset rather than reported
  // as a real 0.
  std::optional<double> frequency_mhz;
  std::optional<double> temperature_c;  // needs a DIMM sensor (jc42/spd5118)
};

struct MotherboardMetrics {
  std::optional<double> temperature_c;  // super-I/O SYSTIN
};

struct DiskMetrics {
  std::optional<double> temperature_c;  // nvme or drivetemp hwmon
};

struct SystemMetrics {
  CpuMetrics cpu;
  GpuMetrics gpu;
  MemoryMetrics memory;
  MotherboardMetrics motherboard;
  DiskMetrics disk;
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
  std::string superio_hwmon_dir_;  // nct6xxx etc: VCore + SYSTIN
  std::string mb_temp_path_;       // resolved SYSTIN input, by label
  std::string disk_temp_path_;     // nvme/drivetemp temp1_input
  std::string mem_temp_path_;      // DIMM sensor temp1_input, if any
  std::string rapl_energy_path_;   // intel-rapl package-0 energy_uj
  int64_t prev_energy_uj_ = -1;
  int64_t prev_energy_us_ = 0;

  void probe_hardware();

  CpuMetrics sample_cpu();
  GpuMetrics sample_gpu();
  MemoryMetrics sample_memory();
  MotherboardMetrics sample_motherboard();
  DiskMetrics sample_disk();
};

}  // namespace reed
