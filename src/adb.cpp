#include "reed/adb.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <sstream>

namespace reed {

namespace {
// The cooler's own UI app -- the thing that acts on screen and fan commands.
constexpr const char* kUiPackage = "com.baiyi.homeui.tkcfanhomeui";
}  // namespace


namespace {

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

// Runs adb directly via fork/execvp -- no shell.
//
// This used to build a command string and hand it to popen(), quoting an
// argument only when it contained a space or a single quote. Anything else went
// through verbatim, so a filename carrying shell metacharacters executed on the
// host: `reed-tpse delete 'x;touch$IFS/tmp/pwned'` created /tmp/pwned. Media
// names reach here from `upload` and `delete`, and on the device they come from
// `ls`, so they are not all operator-controlled.
//
// Quoting harder would have worked; not invoking a shell removes the entire
// class instead. stderr is merged into stdout because callers match on adb's
// error text ("No such file", "error:").
std::optional<std::string> Adb::run_command(
    const std::vector<std::string>& args) {
  int fds[2];
  if (pipe(fds) != 0) return std::nullopt;

  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return std::nullopt;
  }

  if (pid == 0) {
    close(fds[0]);
    if (dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0) {
      _exit(127);
    }
    close(fds[1]);

    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>("adb"));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    execvp("adb", argv.data());
    _exit(127);  // adb not on PATH
  }

  close(fds[1]);

  std::string result;
  std::array<char, 4096> buffer;
  ssize_t n;
  while ((n = read(fds[0], buffer.data(), buffer.size())) > 0) {
    result.append(buffer.data(), static_cast<size_t>(n));
  }
  close(fds[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }

  // execvp failure (adb missing) is the one case worth distinguishing from
  // "adb ran and complained": callers match on its error text, so a non-zero
  // exit is still returned as output rather than swallowed.
  if (WIFEXITED(status) && WEXITSTATUS(status) == 127 && result.empty()) {
    return std::nullopt;
  }

  return result;
}

bool Adb::is_device_connected() {
  auto result = run_command({"devices"});
  if (result && devices_output_has_device(*result)) {
    return true;
  }

  // The adb server occasionally loses track of a connected device after a USB
  // hotplug event -- `adb devices` returns an empty list even though the cooler
  // is physically attached. Bouncing the server recovers it.
  //
  // ONCE per process, though. This kills every adb session the user has, to
  // other devices included, and the daemon asks this question on every connect
  // -- so on a machine where the cooler is simply absent it was bouncing the
  // server forever, on a timer. One attempt recovers the hotplug case; after
  // that, absent means absent.
  static bool bounced = false;
  if (bounced) return false;
  bounced = true;

  std::cerr << "adb: no device visible, bouncing server and retrying once...\n";
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

bool Adb::ui_ready() {
  auto out = run_command({"shell", "pidof", kUiPackage});
  if (!out) return false;
  // pidof prints nothing and exits non-zero when the process is absent.
  return out->find_first_of("0123456789") != std::string::npos;
}

bool Adb::reboot() {
  return run_command({"reboot"}).has_value();
}

// Single-quote for the DEVICE's shell.
//
// `adb shell` is not execve: adb joins its arguments and hands the result to a
// shell on the cooler, so a filename containing ;, |, $(...) or a backtick is
// interpreted there. Closing the host-side injection did not close this one --
// they are two different shells, and only the first was fixed.
//
// POSIX single quotes protect everything except a single quote itself, which
// is emitted as '\'' -- close, escape, reopen.
static std::string device_shell_quote(const std::string& in) {
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

bool Adb::remove(const std::string& filename) {
  std::string remote_path = std::string(MEDIA_PATH) + filename;
  auto result = run_command({"shell", "rm", device_shell_quote(remote_path)});

  return result && result->find("No such file") == std::string::npos;
}

}  // namespace reed
