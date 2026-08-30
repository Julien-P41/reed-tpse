// Media and display commands: what is on the panel, and how bright.
//
// These all persist their effect to the state file, which is why they can hand
// over to the daemon when it holds the port -- it re-applies that file on
// every connect, so saving and letting it push is not a fallback but the
// normal path whenever the daemon is running.

#include "cli_common.hpp"
#include "cli_commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "reed/adb.hpp"
#include "reed/device.hpp"
#include "reed/mapping.hpp"
#include "reed/media.hpp"
#include "reed/wire.hpp"

namespace fs = std::filesystem;

// Presets are named by the file the firmware ships, e.g. "Cooling delivery"
// for Cooling_delivery.mp4. Accept either spelling from the user.
static std::string preset_display_name(const std::string& file_stem) {
  std::string out = file_stem;
  std::replace(out.begin(), out.end(), '_', ' ');
  return out;
}

// Overlay filter drawn across the media. Part of `settings`, so it rides on a
// screen-config frame -- which means re-sending whatever media is current.
int cmd_filter(const std::string& port, const std::string& name,
                      int opacity, bool opacity_given, bool verbose) {
  std::string wire;
  for (const char* known : {"Rain", "Smoke"}) {
    std::string a, b;
    for (char c : name) a += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char c : std::string(known)) b += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (a == b) wire = known;
  }
  if (wire.empty() && name != "none" && name != "None") {
    std::cerr << "Unknown filter: \"" << name << "\"  (Rain | Smoke | none)\n";
    return 1;
  }
  if (opacity_given && (opacity < 0 || opacity > 100)) {
    std::cerr << "--opacity must be 0-100\n";
    return 1;
  }

  auto state = load_state_for_update();
  if (!state) return 1;
  if (state->media.empty() && !state->preset) {
    std::cerr << "Nothing on screen to filter. Set media with `reed-tpse "
                 "display <file>` first.\n";
    return 1;
  }

  state->filter = wire;
  if (opacity_given) state->filter_opacity = opacity;

  if (!save_state_or_report(*state)) return 1;
  if (daemon_holds_port(port)) {
    return defer_to_daemon("Filter");
  }

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }
  device.drain();
  device.handshake();

  const reed::ScreenConfig cfg = screen_config_from(*state);

  if (!device.set_screen_config(cfg)) {
    std::cerr << "No response to 'POST waterBlockScreenId'\n";
    return 1;
  }

  if (wire.empty()) {
    std::cout << "Filter cleared.\n";
  } else {
    std::cout << "Filter: " << wire << " at " << state->filter_opacity
              << "% opacity\n";
  }
  return 0;
}

int cmd_preset(const std::string& port, const std::vector<std::string>& args,
                      bool verbose) {
  if (!reed::Adb::is_device_connected()) {
    std::cerr << "No ADB device connected (needed to list the built-in "
                 "presets)\n";
    return 1;
  }
  auto presets = reed::Adb::list_presets();
  if (!presets || presets->empty()) {
    std::cerr << "Could not read the preset list from the device\n";
    return 1;
  }

  if (args.empty() || args[0] == "list") {
    std::cout << "Built-in presets:\n";
    for (const auto& p : *presets) {
      std::cout << "  " << preset_display_name(p) << "\n";
    }
    return 0;
  }

  // Match on the display name, case-insensitively, and tolerate underscores.
  const std::string wanted = preset_display_name(args[0]);
  auto ieq = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i])))
        return false;
    }
    return true;
  };

  std::string match;
  for (const auto& p : *presets) {
    if (ieq(preset_display_name(p), wanted)) {
      match = preset_display_name(p);
      break;
    }
  }
  if (match.empty()) {
    // The device resolves the id straight to a path without checking it
    // exists, so an unmatched name would silently blank the panel. Refuse.
    std::cerr << "Unknown preset: \"" << args[0] << "\"\n"
              << "Run `reed-tpse preset list` to see what this firmware "
                 "ships.\n";
    return 1;
  }

  {
    // Saved before the device is touched, so a busy port hands over to the
    // daemon instead of failing.
    auto st = load_state_for_update();
    if (!st) return 1;
    st->preset = match;
    st->media.clear();  // a preset and custom media are mutually exclusive
    if (!save_state_or_report(*st)) return 1;
  }
  if (daemon_holds_port(port)) return defer_to_daemon("Preset " + match);

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }
  device.drain();
  device.handshake();

  // The leading number is not used by the firmware -- it splits on ": " and
  // keeps the name -- but the prefix must be present or the command is not
  // dispatched at all.
  const std::string id = reed::wire::kPresetPrefix + match;

  // Carry the stored overlay styling and metrics with the preset, the way the
  // vendor does -- otherwise selecting a preset silently drops the HUD.
  auto prev = reed::ConfigManager::load_state();
  reed::DisplaySettings settings;
  std::vector<std::string> sysinfo;
  if (prev) {
    settings = settings_from(prev->hud, prev->filter, prev->filter_opacity);
    if (prev->hud.enabled) sysinfo = prev->hud.metrics;
  }

  if (!device.set_preset(id, settings, sysinfo)) {
    std::cerr << "No response to 'POST waterBlockScreenId'\n";
    return 1;
  }

  std::cout << "Preset set to: " << match << "\n";
  return 0;
}

int cmd_upload(const std::string& file, bool verbose) {
  if (verbose) std::cout << "Checking file: " << file << "\n";

  if (!fs::exists(file)) {
    std::cerr << "File not found: " << file << "\n";
    return 1;
  }

  if (verbose) {
    std::cout << "File size: " << fs::file_size(file) << " bytes\n";
    std::cout << "Checking ADB connection...\n";
  }

  if (!reed::Adb::is_device_connected()) {
    std::cerr << "No ADB device connected\n";
    return 1;
  }

  auto type = reed::Media::detect_type(file);
  std::string upload_path = file;
  std::string remote_name = reed::Media::get_filename(file);

  if (verbose) std::cout << "Detected type: " << static_cast<int>(type) << "\n";

  if (type == reed::MediaType::Gif) {
    if (!reed::Media::is_ffmpeg_available()) {
      std::cerr << "ffmpeg not found. Install ffmpeg to upload GIF files.\n";
      return 1;
    }

    std::string converted_name = reed::Media::get_converted_name(file);
    std::string converted_path =
        std::string(reed::Media::TMP_DIR) + converted_name;

    std::cout << "Converting GIF to MP4...\n";
    if (verbose) std::cout << "Output path: " << converted_path << "\n";

    if (!reed::Media::convert_gif_to_mp4(file, converted_path)) {
      std::cerr << "Failed to convert GIF to MP4\n";
      return 1;
    }

    upload_path = converted_path;
    remote_name = converted_name;
    std::cout << "Converted: " << reed::Media::get_filename(file) << " -> "
              << remote_name << "\n";
  }

  if (verbose)
    std::cout << "Pushing via ADB: " << upload_path << " -> " << remote_name
              << "\n";

  std::cout << "Uploading " << remote_name << "...\n";
  if (!reed::Adb::push(upload_path, remote_name)) {
    std::cerr << "Failed to upload file\n";
    return 1;
  }

  std::cout << "Upload complete.\n";
  std::cout << "Display with: reed-tpse display " << remote_name << "\n";

  return 0;
}

int cmd_display(const std::string& port,
                       const std::vector<std::string>& files,
                       const std::string& ratio, int brightness,
                       bool brightness_given, const std::string& play_mode,
                       bool split, bool verbose) {
  if (brightness < 0 || brightness > 100) {
    std::cerr << "Brightness must be 0-100\n";
    return 1;
  }

  if (split && files.size() != 2) {
    std::cerr << "--split needs exactly two files: left then right (got "
              << files.size() << ")\n";
    return 1;
  }

  // `display` takes names of media already on the device. A .gif was converted
  // to .mp4 at upload time, so the name is rewritten to match -- but nothing
  // here converts or uploads, so asking for a GIF that was never uploaded used
  // to reference a file the device does not have. The firmware does not check
  // either: an unresolvable name simply blanks the panel, which reads as a
  // display fault. Verify against the device's own listing first.
  std::vector<std::string> media_files;
  for (const auto& f : files) {
    if (reed::Media::detect_type(f) == reed::MediaType::Gif) {
      media_files.push_back(reed::Media::get_converted_name(f));
    } else {
      media_files.push_back(f);
    }
  }

  if (reed::Adb::is_device_connected()) {
    if (auto on_device = reed::Adb::list_media()) {
      std::vector<std::string> missing;
      for (const auto& m : media_files) {
        if (std::find(on_device->begin(), on_device->end(), m) ==
            on_device->end()) {
          missing.push_back(m);
        }
      }
      if (!missing.empty()) {
        for (const auto& m : missing) {
          std::cerr << "Not on device: " << m << "\n";
        }
        std::cerr << "Upload it first (`reed-tpse upload <file>`), or check "
                     "`reed-tpse list`.\n";
        if (files.size() != media_files.size() || files != media_files) {
          std::cerr << "Note: a .gif is stored as .mp4 after upload, so ask "
                       "for the name shown by `list`.\n";
        }
        return 1;
      }
    }
  }

  // Loaded before touching the device: the effective brightness falls back to
  // whatever is already stored when --brightness was not given.
  auto state = load_state_for_update();
  if (!state) return 1;

  // Apply this call's arguments to the saved state, then derive the payload
  // from it -- so what goes to the device and what is stored cannot disagree.
  state->media = media_files;
  state->ratio = ratio;
  // Without this the struct default ("Single") went out on every call, so a
  // multi-file `display` only ever showed the first file.
  if (!play_mode.empty()) state->play_mode = play_mode;
  // Set BOTH ways. Only ever setting it on --split made Screen Splitting a
  // one-way door: every later `display` kept shipping a two-zone payload, and
  // --ratio was silently dropped along with it, with no way back short of
  // editing the state file.
  state->screen_mode =
      split ? reed::wire::kScreenSplitting : reed::wire::kFullScreen;
  const reed::ScreenConfig config = screen_config_from(*state);

  const int effective_brightness =
      brightness_given ? brightness : state->brightness;

  // Save first, then treat a busy port as "the daemon will do it". The daemon
  // holds the port exclusively and now re-applies on any state change, so
  // failing here refuses a change it is about to make anyway -- which is what
  // `display` did whenever the daemon was running.
  if (brightness_given) state->brightness = brightness;
  state->preset.reset();  // custom media and a preset are mutually exclusive
  if (!save_state_or_report(*state)) return 1;

  // Checked before the port is opened: connect() prints its own "already open
  // by PID ..." diagnostic, which is noise when handing over to the daemon is
  // the expected path rather than a failure.
  if (daemon_holds_port(port)) return defer_to_daemon("Display");

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }

  device.handshake();
  device.set_screen_config(config);
  device.set_brightness(effective_brightness);

  std::cout << "Display set to: ";
  for (size_t i = 0; i < media_files.size(); ++i) {
    if (i > 0) std::cout << ", ";
    std::cout << media_files[i];
  }
  std::cout << "\n";
  std::cout << "Brightness: " << effective_brightness
            << (brightness_given ? "\n" : " (unchanged)\n");

  // Save state for daemon. Load first and mutate only the display fields:
  // save_state() truncates, so building a fresh DisplayState here would drop
  // every other setting sharing this file -- the HUD config, display_in_sleep,
  // and any non-default screen/play mode -- on each `display` call.
  // Brightness is its own setting. Changing what is on screen should not
  // silently reset it to the config default -- that quietly undid any
  // `reed-tpse brightness N` the moment the media changed.
  // No keepalive loop here. `display` used to be able to hold the connection
  // itself, which was a second, subtly different implementation of what the
  // daemon does -- and a `display --keepalive` left running in another
  // terminal held the port against everything else while looking like an
  // ordinary finished command.
  std::cout << "Run 'reed-tpse daemon start' to keep the display persistent -- "
               "the device\n reverts to its standby content about 60s after "
               "the last handshake.\n";
  return 0;
}

int cmd_brightness(const std::string& port, int value, bool verbose) {
  if (value < 0 || value > 100) {
    std::cerr << "Brightness must be 0-100\n";
    return 1;
  }

  // Persist it: the device forgets on power loss and the daemon re-applies
  // state->brightness on connect, so without this the value silently reverts
  // at the next reboot.
  auto state = load_state_for_update();
  if (!state) return 1;
  state->brightness = value;
  if (!save_state_or_report(*state)) return 1;

  if (daemon_holds_port(port)) return defer_to_daemon("Brightness");

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }

  device.handshake();
  device.set_brightness(value);

  std::cout << "Brightness set to " << value << "\n";
  return 0;
}

int cmd_list() {
  if (!reed::Adb::is_device_connected()) {
    std::cerr << "No ADB device connected\n";
    return 1;
  }

  auto files = reed::Adb::list_media();
  if (!files) {
    std::cerr << "Failed to list media files\n";
    return 1;
  }

  if (files->empty()) {
    std::cout << "No media files on device.\n";
    return 0;
  }

  std::cout << "Media files on device:\n";
  for (const auto& f : *files) {
    std::cout << "  " << f << "\n";
  }

  return 0;
}

int cmd_delete(const std::vector<std::string>& files) {
  if (!reed::Adb::is_device_connected()) {
    std::cerr << "No ADB device connected\n";
    return 1;
  }

  // Report failures in the exit status, not only on stderr. This returned 0
  // even when every deletion failed, so a script could not tell a cleared
  // device from an untouched one.
  int failed = 0;
  for (const auto& f : files) {
    if (reed::Adb::remove(f)) {
      std::cout << "Deleted: " << f << "\n";
    } else {
      std::cerr << "Failed to delete: " << f << "\n";
      ++failed;
    }
  }

  if (failed) {
    std::cerr << failed << " of " << files.size() << " not deleted.\n";
    return 1;
  }
  return 0;
}

// Media shown while the session is locked, replacing the firmware's standby
// clip. Only meaningful with `report_lock`, which is what notices the lock.
int cmd_lock_display(const std::string& port, const std::vector<std::string>& args,
                            int brightness, bool brightness_given, bool verbose) {
  (void)port;
  auto cfg = reed::ConfigManager::load_config();
  if (!cfg) cfg = reed::Config{};

  if (args.empty()) {
    if (cfg->lock_media) {
      std::cout << "Lock display: " << *cfg->lock_media << " at "
                << cfg->lock_brightness << "% brightness\n";
    } else {
      std::cout << "Lock display: (firmware standby clip)\n";
    }
    std::cout << "  Applies while the daemon is running, unless "
                 "\"report_lock\" is false.\n";
    return 0;
  }

  const std::string& arg = args[0];
  if (arg == "--default" || arg == "--remove" || arg == "default") {
    cfg->lock_media.reset();
    if (!reed::ConfigManager::save_config(*cfg)) {
      std::cerr << "Failed to save config\n";
      return 1;
    }
    std::cout << "Lock display reset to the firmware standby clip.\n"
              << "  The daemon will send the lock-screen power event again.\n";
    return 0;
  }

  // Same rule as `display`: a .gif is stored as .mp4 once uploaded.
  std::string media = arg;
  if (reed::Media::detect_type(media) == reed::MediaType::Gif) {
    media = reed::Media::get_converted_name(media);
  }

  if (reed::Adb::is_device_connected()) {
    if (auto on_device = reed::Adb::list_media()) {
      if (std::find(on_device->begin(), on_device->end(), media) ==
          on_device->end()) {
        std::cerr << "Not on device: " << media << "\n"
                  << "Upload it first (`reed-tpse upload <file>`), or check "
                     "`reed-tpse list`.\n";
        return 1;
      }
    }
  }

  const int level = brightness_given ? brightness : cfg->lock_brightness;
  if (level < 0 || level > 100) {
    std::cerr << "Brightness must be 0-100\n";
    return 1;
  }

  cfg->lock_media = media;
  cfg->lock_brightness = level;
  if (!reed::ConfigManager::save_config(*cfg)) {
    std::cerr << "Failed to save config\n";
    return 1;
  }

  std::cout << "Lock display: " << media << " at " << level << "% brightness\n";
  if (level > 50) {
    // A locked machine sits untouched for hours, which is the worst case for
    // an AMOLED: bright, and often near-static.
    std::cout << "  ⚠ " << level
              << "% is bright for a screen that may sit locked for hours.\n"
                 "    This panel is AMOLED, so a bright near-static image is "
                 "the burn-in case.\n"
                 "    Consider --brightness 40 or lower, and a dark clip.\n";
  }
  std::cout << "  Applies while the daemon is running, unless "
               "\"report_lock\" is false.\n";
  if (verbose) std::cout << "  (saved to " << reed::ConfigManager::get_config_path() << ")\n";
  return 0;
}
