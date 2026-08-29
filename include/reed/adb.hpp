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
  // Is the cooler's UI app running?
  //
  // Tri-state on purpose. `false` means adb answered and the process is not
  // there; `nullopt` means adb could not answer at all -- not installed, no
  // device, the wrong device selected. Collapsing those into one `false` made
  // an unanswerable question look like a slow boot, so the daemon sat through
  // its full 45s wait on every connect for a condition no amount of waiting
  // would change.
  static std::optional<bool> ui_ready();

  // `adb devices -l`, used to pick the cooler when more than one device is
  // attached. Public only so the selection helper can reach it.
  static std::optional<std::string> devices_verbose();

  // Tie adb to the same physical cooler as the serial port in use.
  //
  // The two halves of this device -- the CDC-ACM tty and the ADB interface --
  // are separate endpoints on one USB device, and nothing forced them to be
  // the SAME device. With one cooler that is academic. With two, the tty could
  // be pinned with --port while adb quietly drove the other panel: both
  // report product cm01, so selecting by product picks whichever enumerated
  // first.
  //
  // sysfs gives the tty's USB port (/sys/class/tty/ttyACM0/device ->
  // .../usb1/1-11/1-11:1.0) and `adb devices -l` prints the same path as
  // usb:1-11, so the pair can be matched exactly. Call this once the serial
  // port is known; without it, selection falls back to product then to "the
  // only device attached".
  static void bind_to_port(const std::string& tty_path);

 private:
  static std::optional<std::string> run_command(
      const std::vector<std::string>& args);
};

}  // namespace reed
