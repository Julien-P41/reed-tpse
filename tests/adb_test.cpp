// Parsing adb's output. No adb, no device.
//
//   cmake .. -DREED_BUILD_TESTS=ON && make && ./reed-adb-test
//
// This is the code that decides WHICH cooler a command talks to, and it had no
// coverage because it lived behind private statics in src/adb.cpp. The
// listings also parse text produced by a program this project does not
// control, describing filenames the user chose.
//
// The device-selection cases below are the ones that have actually bitten:
// adb answering "more than one device/emulator" and being parsed as a media
// filename, and a serial resolved before the port binding was known.
#include "reed/adb_parse.hpp"

#include <cstdio>
#include <string>

static int failures = 0;

static void check(const std::string& what, bool ok) {
  std::printf("  %-58s %s\n", what.c_str(), ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

static void eq(const std::string& what, const std::string& got,
               const std::string& want) {
  const bool ok = got == want;
  std::printf("  %-58s %s\n", what.c_str(), ok ? "ok" : "FAIL");
  if (!ok) {
    std::printf("      got  [%s]\n      want [%s]\n", got.c_str(), want.c_str());
    ++failures;
  }
}

using namespace reed::adb_parse;

int main() {
  // --- is anything attached? ---
  check("one device in `device` state is seen",
        has_online_device("List of devices attached\nABC123\tdevice\n"));
  check("an empty list is not a device",
        !has_online_device("List of devices attached\n\n"));
  check("`unauthorized` does not count",
        !has_online_device("List of devices attached\nABC123\tunauthorized\n"));
  check("`offline` does not count",
        !has_online_device("List of devices attached\nABC123\toffline\n"));

  // --- which device? ---
  const std::string two =
      "List of devices attached\n"
      "DECOY123               device usb:9-99 product:cm01 model:cm01\n"
      "REALDEV1               device usb:1-11 product:cm01 model:cm01\n";

  {
    // The whole point of binding to a USB port: both report product cm01, so
    // the product match cannot tell them apart and the port can.
    auto s = select_serial(two, "1-11", "cm01");
    check("USB port picks the cooler on that port", s && *s == "REALDEV1");
    auto o = select_serial(two, "9-99", "cm01");
    check("...and the other port picks the other one", o && *o == "DECOY123");
  }
  {
    auto s = select_serial(two, "", "cm01");
    check("with no binding, the product match takes the first",
          s && *s == "DECOY123");
  }
  {
    // Two attached, neither matching: guessing here is how `list` once printed
    // adb's "more than one device/emulator" as a filename.
    const std::string other =
        "List of devices attached\n"
        "PHONE1                 device usb:3-3 product:sargo\n"
        "TABLET2                device usb:4-4 product:flame\n";
    check("several devices and no match yields nothing",
          !select_serial(other, "", "cm01").has_value());
  }
  {
    // But a single attached device is used regardless, so the ordinary case
    // survives the product string ever changing.
    const std::string one =
        "List of devices attached\nONLYONE   device usb:1-11 product:other\n";
    auto s = select_serial(one, "", "cm01");
    check("a lone device is used even without a product match",
          s && *s == "ONLYONE");
  }
  {
    const std::string unauth =
        "List of devices attached\nABC123    unauthorized usb:1-11\n";
    check("an unauthorized device is not selectable",
          !select_serial(unauth, "1-11", "cm01").has_value());
  }
  check("the header line is never taken as a serial",
        !select_serial("List of devices attached\n", "", "cm01").has_value());
  check("empty output selects nothing",
        !select_serial("", "", "cm01").has_value());

  // --- adb complaining is not an answer ---
  check("`error:` is an adb failure", is_adb_error("error: no devices found"));
  check("`adb:` is an adb failure", is_adb_error("adb: device offline"));
  check("ordinary output is not a failure", !is_adb_error("12345\n"));

  // --- media listing ---
  {
    auto m = media_list("a.mp4\r\nb.png\n\nc.gif \n");
    check("listing trims CR, spaces and blank lines", m.size() == 3);
    check("names survive intact",
          m.size() == 3 && m[0] == "a.mp4" && m[1] == "b.png" && m[2] == "c.gif");
  }
  check("a missing directory is an empty listing, not a failure",
        media_list("ls: /sdcard/pcMedia/: No such file or directory").empty());
  check("an adb error is an empty listing",
        media_list("error: no devices/emulators found").empty());

  // --- preset listing ---
  {
    auto p = preset_list("Cooling_delivery.mp4\nMigration.mp4\nstandby.mp4\n"
                         "notes.txt\nRacing.mp4\n");
    check("only .mp4 entries become presets", p.size() == 3);
    check("the extension is stripped",
          p.size() == 3 && p[0] == "Cooling_delivery");
    bool has_standby = false;
    for (const auto& n : p) if (n == "standby") has_standby = true;
    check("the standby clip is never offered as a preset", !has_standby);
    bool has_txt = false;
    for (const auto& n : p) if (n == "notes") has_txt = true;
    check("a non-mp4 file is not a preset", !has_txt);
  }

  // --- quoting for the device's shell ---
  eq("an ordinary name is quoted", device_shell_quote("a.mp4"), "'a.mp4'");
  eq("a space is contained", device_shell_quote("my clip.mp4"),
     "'my clip.mp4'");
  eq("a single quote is closed, escaped and reopened",
     device_shell_quote("it's.mp4"), "'it'\\''s.mp4'");
  // The case that made this necessary: metacharacters must stay literal,
  // because `adb shell` hands its arguments to a shell ON THE COOLER.
  eq("a command substitution stays literal",
     device_shell_quote("x;touch /tmp/pwned"), "'x;touch /tmp/pwned'");
  eq("backticks stay literal", device_shell_quote("`id`"), "'`id`'");
  eq("a dollar expansion stays literal", device_shell_quote("$IFS"), "'$IFS'");

  std::printf("%s\n", failures ? "FAILURES" : "all checks passed");
  return failures != 0;
}
