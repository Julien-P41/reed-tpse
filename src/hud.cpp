#include "reed/hud.hpp"

namespace reed {

namespace {

// Non-capturing lambdas so each row keeps a plain function pointer. A metric
// backed by a plain double is always readable; one backed by std::optional is
// readable only where the hardware exposes it.
const std::vector<HudMetric>& table() {
  static const std::vector<HudMetric> t = {
      {"CPU Temperature", "cpu", "temperature", PcInfoType::Number, "°C", 0,
       [](const SystemMetrics& m) -> std::optional<double> {
         return m.cpu.temperature_c;
       }},
      {"CPU Frequency", "cpu", "speedAverage", PcInfoType::Number, "MHz", 0,
       [](const SystemMetrics& m) -> std::optional<double> {
         return m.cpu.frequency_mhz;
       }},
      {"CPU Usage", "cpu", "load", PcInfoType::Number, "%", 1,
       [](const SystemMetrics& m) -> std::optional<double> {
         return m.cpu.usage_percent;
       }},
      {"CPU Voltage", "cpu", "voltage", PcInfoType::Number, "V", 3,
       [](const SystemMetrics& m) { return m.cpu.voltage_v; }},
      {"GPU Temperature", "gpu", "temperature", PcInfoType::String, "°C", 0,
       [](const SystemMetrics& m) -> std::optional<double> {
         return m.gpu.temperature_c;
       }},
      {"GPU Frequency", "gpu", "speed", PcInfoType::Number, "MHz", 0,
       [](const SystemMetrics& m) -> std::optional<double> {
         return m.gpu.frequency_mhz;
       }},
      {"GPU Usage", "gpu", "load", PcInfoType::Number, "%", 0,
       [](const SystemMetrics& m) -> std::optional<double> {
         return m.gpu.usage_percent;
       }},
      {"GPU Voltage", "gpu", "voltage", PcInfoType::Number, "V", 3,
       [](const SystemMetrics& m) -> std::optional<double> {
         return m.gpu.voltage_v;
       }},
      {"Motherboard Temperature", "motherboard", "temperature",
       PcInfoType::Number, "°C", 0,
       [](const SystemMetrics& m) { return m.motherboard.temperature_c; }},
      {"Memory Frequency", "memory", "speed", PcInfoType::Number, "MHz", 0,
       [](const SystemMetrics& m) { return m.memory.frequency_mhz; }},
      {"Memory Utilization", "memory", "load", PcInfoType::Number, "%", 1,
       [](const SystemMetrics& m) -> std::optional<double> {
         return m.memory.usage_percent;
       }},
      {"Hard Disk Temperature", "disk", "temperature", PcInfoType::Number, "°C",
       0, [](const SystemMetrics& m) { return m.disk.temperature_c; }},
      {"CPU Power", "cpu", "power", PcInfoType::Number, "W", 1,
       [](const SystemMetrics& m) { return m.cpu.power_w; }},
      {"GPU Power", "gpu", "power", PcInfoType::Number, "W", 1,
       [](const SystemMetrics& m) { return m.gpu.power_w; }},
      {"Memory Temperature", "memory", "temperature", PcInfoType::Number, "°C",
       0, [](const SystemMetrics& m) { return m.memory.temperature_c; }},
      // The device draws the clock itself; nothing is sent for it.
      {"Date&Time", nullptr, nullptr, PcInfoType::Number, "", 0, nullptr},
  };
  return t;
}

}  // namespace

const std::vector<HudMetric>& hud_metrics() { return table(); }

const HudMetric* find_hud_metric(const std::string& label) {
  for (const auto& m : table()) {
    if (label == m.label) return &m;
  }
  return nullptr;
}

}  // namespace reed
