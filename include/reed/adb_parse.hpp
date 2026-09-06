#pragma once

// Parsing adb's output, separated from running adb.
//
// All of this was inline in src/adb.cpp behind private statics, which made it
// untestable -- and it is the code that decides WHICH cooler a command talks
// to. With one device that is academic. With two, picking wrong means driving
// the other panel, and docs/multi-cooler.md states as a correctness property
// that pinning a port makes both halves follow it.
//
// It also parses text produced by a program this project does not control, on
// a device whose filenames come from the user. Both are reasons to be able to
// check the rules without hardware attached.

#include <optional>
#include <string>
#include <vector>

namespace reed::adb_parse {

// adb complaining is not an answer. Its own messages are prefixed "error:" or
// "adb:", and neither contains a digit -- which is why `pidof` output was once
// read as "process absent" when adb had in fact failed to reach anything.
inline bool is_adb_error(const std::string& out) {
  return out.find("error:") != std::string::npos ||
         out.find("adb:") != std::string::npos;
}

// `adb devices`: is anything attached and in the `device` state? Lines for
// `unauthorized` or `offline` entries do not count.
inline bool has_online_device(const std::string& devices_output) {
  size_t pos = 0;
  while (pos <= devices_output.size()) {
    const size_t eol = devices_output.find('\n', pos);
    const std::string line = devices_output.substr(
        pos, eol == std::string::npos ? std::string::npos : eol - pos);
    if (line.find("\tdevice") != std::string::npos) return true;
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  return false;
}

// Which serial to pass to `adb -s`, from `adb devices -l`.
//
// Strongest match first: the device on the same USB port as the serial port in
// use, which is the same physical cooler by construction. Then the product
// string. Then, only if exactly one device is attached, that one -- so the
// single-cooler case keeps working even if the product string ever changes.
//
// Returns nothing when several devices are attached and none matches, rather
// than guessing: adb answers "more than one device/emulator", which contains
// no "error:" and was once parsed as a media filename.
inline std::optional<std::string> select_serial(
    const std::string& devices_l, const std::string& bound_usb_port,
    const std::string& product) {
  std::vector<std::string> online;
  size_t pos = 0;
  while (pos <= devices_l.size()) {
    const size_t eol = devices_l.find('\n', pos);
    const std::string line = devices_l.substr(
        pos, eol == std::string::npos ? std::string::npos : eol - pos);
    const size_t sep = line.find_first_of(" \t");
    if (sep != std::string::npos &&
        (line.find("\tdevice") != std::string::npos ||
         line.find(" device ") != std::string::npos)) {
      const std::string id = line.substr(0, sep);
      if (!id.empty() && id != "List") {
        online.push_back(id);
        if (!bound_usb_port.empty() &&
            line.find("usb:" + bound_usb_port) != std::string::npos) {
          return id;
        }
        if (bound_usb_port.empty() &&
            line.find("product:" + product) != std::string::npos) {
          return id;
        }
      }
    }
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  if (online.size() == 1) return online.front();
  return std::nullopt;
}

namespace detail {
inline std::vector<std::string> lines_of(const std::string& out) {
  std::vector<std::string> result;
  size_t pos = 0;
  while (pos <= out.size()) {
    const size_t eol = out.find('\n', pos);
    std::string line =
        out.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
      line.pop_back();
    }
    if (!line.empty()) result.push_back(line);
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  return result;
}
}  // namespace detail

// `ls -1 /sdcard/pcMedia/`. An absent directory is an empty listing, not a
// failure -- a cooler with no media uploaded yet is a normal state.
inline std::vector<std::string> media_list(const std::string& ls_output) {
  if (ls_output.find("No such file") != std::string::npos ||
      ls_output.find("error:") != std::string::npos) {
    return {};
  }
  return detail::lines_of(ls_output);
}

// `ls -1 /system/media/video/`, reduced to selectable preset names: only .mp4
// entries, without the extension, and never the standby clip -- that one is
// the firmware's sleep animation, not something to choose.
inline std::vector<std::string> preset_list(const std::string& ls_output) {
  if (ls_output.find("No such file") != std::string::npos ||
      ls_output.find("error:") != std::string::npos) {
    return {};
  }
  std::vector<std::string> presets;
  for (std::string line : detail::lines_of(ls_output)) {
    if (line.size() <= 4 || line.compare(line.size() - 4, 4, ".mp4") != 0) {
      continue;
    }
    line.erase(line.size() - 4);
    if (line == "standby") continue;
    presets.push_back(line);
  }
  return presets;
}

// Single-quote for the DEVICE's shell.
//
// `adb shell` is not execve: adb joins its arguments and hands the result to a
// shell on the cooler, so a filename containing ;, |, $(...) or a backtick is
// interpreted there. Closing the host-side injection did not close this one --
// two different shells, and only the first was fixed.
//
// POSIX single quotes protect everything except a single quote, which is
// emitted as '\'' -- close, escape, reopen.
inline std::string device_shell_quote(const std::string& in) {
  std::string out = "'";
  for (char c : in) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

}  // namespace reed::adb_parse
