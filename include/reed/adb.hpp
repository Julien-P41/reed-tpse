#pragma once

#include <optional>
#include <string>
#include <vector>

namespace reed {

class Adb {
 public:
  static constexpr const char* MEDIA_PATH = "/sdcard/pcMedia/";
  // Firmware-bundled preset clips, shipped read-only in the system image.
  static constexpr const char* PRESET_PATH = "/system/media/video/";

  static bool is_device_connected();
  static bool push(const std::string& local_path,
                   const std::string& remote_name);
  static std::optional<std::vector<std::string>> list_media();
  // Preset names (no .mp4, no standby clip), as the device actually has them.
  static std::optional<std::vector<std::string>> list_presets();
  static bool remove(const std::string& filename);

  // Restart the cooler alone -- an ordinary Android reboot of the internal
  // SoC, ~20s to a usable UI, with the PC untouched. This is how settings
  // that only apply at boot (rotation) actually take effect; the vendor app
  // bundles adb.exe and does the same thing.
  static bool reboot();

  // Is the cooler's UI app running? The serial port and adbd both come up
  // well before it does, so a device that answers is not necessarily a device
  // that will act on what it is told. Anything applied before this is true is
  // silently lost.
  static bool ui_ready();

  // `adb devices -l`, used to pick the cooler when more than one device is
  // attached. Public only so the selection helper can reach it.
  static std::optional<std::string> devices_verbose();

 private:
  static std::optional<std::string> run_command(
      const std::vector<std::string>& args);
};

}  // namespace reed
