#include "reed/adb.hpp"

#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>

namespace reed {

namespace {
struct PipeCloser {
  void operator()(FILE* f) const {
    if (f) pclose(f);
  }
};
}  // namespace

std::optional<std::string> Adb::run_command(
    const std::vector<std::string>& args) {
  std::string cmd = "adb";
  for (const auto& arg : args) {
    cmd += " ";
    // Shell escape
    if (arg.find(' ') != std::string::npos ||
        arg.find('\'') != std::string::npos) {
      cmd += "'";
      for (char c : arg) {
        if (c == '\'') {
          cmd += "'\\''";
        } else {
          cmd += c;
        }
      }
      cmd += "'";
    } else {
      cmd += arg;
    }
  }
  cmd += " 2>&1";

  std::array<char, 4096> buffer;
  std::string result;

  std::unique_ptr<FILE, PipeCloser> pipe(popen(cmd.c_str(), "r"));
  if (!pipe) {
    return std::nullopt;
  }

  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }

  return result;
}

namespace {

// True if `adb devices` output lists at least one device in state "device".
bool devices_output_has_device(const std::string& output) {
  std::istringstream iss(output);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.find("\tdevice") != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool Adb::is_device_connected() {
  auto result = run_command({"devices"});
  if (result && devices_output_has_device(*result)) {
    return true;
  }

  // The adb server occasionally loses track of a connected device after a USB
  // hotplug event — `adb devices` returns an empty list even though the cooler
  // is physically attached. Recover by bouncing the server and retrying once.
  std::cerr << "adb: no device visible, bouncing server and retrying...\n";
  run_command({"kill-server"});
  run_command({"start-server"});
  result = run_command({"devices"});
  return result && devices_output_has_device(*result);
}

bool Adb::push(const std::string& local_path, const std::string& remote_name) {
  std::string remote_path = std::string(MEDIA_PATH) + remote_name;
  auto result = run_command({"push", local_path, remote_path});

  if (!result) {
    return false;
  }

  return result->find("pushed") != std::string::npos ||
         result->find("1 file") != std::string::npos;
}

std::optional<std::vector<std::string>> Adb::list_media() {
  auto result = run_command({"shell", "ls", "-1", MEDIA_PATH});

  if (!result) {
    return std::nullopt;
  }

  if (result->find("No such file") != std::string::npos ||
      result->find("error:") != std::string::npos) {
    return std::vector<std::string>{};
  }

  std::vector<std::string> files;
  std::istringstream iss(*result);
  std::string line;

  while (std::getline(iss, line)) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
      line.pop_back();
    }
    if (!line.empty()) {
      files.push_back(line);
    }
  }

  return files;
}

std::optional<std::vector<std::string>> Adb::list_presets() {
  auto result = run_command({"shell", "ls", "-1", PRESET_PATH});
  if (!result) return std::nullopt;
  if (result->find("No such file") != std::string::npos ||
      result->find("error:") != std::string::npos) {
    return std::vector<std::string>{};
  }

  std::vector<std::string> presets;
  std::istringstream iss(*result);
  std::string line;
  while (std::getline(iss, line)) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
      line.pop_back();
    }
    if (line.size() <= 4 || line.compare(line.size() - 4, 4, ".mp4") != 0) {
      continue;
    }
    line.erase(line.size() - 4);
    // The sleep animation is not a selectable preset.
    if (line == "standby") continue;
    presets.push_back(line);
  }
  return presets;
}

bool Adb::remove(const std::string& filename) {
  std::string remote_path = std::string(MEDIA_PATH) + filename;
  auto result = run_command({"shell", "rm", remote_path});

  return result && result->find("No such file") == std::string::npos;
}

}  // namespace reed
