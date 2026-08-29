// LCD fan control.
//
// kFanTiers is a single static definition here and never in a header: as
// `static const` in a header every translation unit gets its own copy, which
// compiles and links silently while allowing the tiers to drift apart.

#include "cli_common.hpp"
#include "cli_commands.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "reed/device.hpp"
#include "reed/picojson.h"
#include "reed/sysinfo.hpp"
#include "reed/wire.hpp"

// Named tiers, spanning the useful range: `low` is the firmware's own default
// (35% -> ~2010 RPM, inside the 2010-2070 band the default itself drifts
// across) and the rest climb linearly to 100%. Measured on firmware V1.0.11:
// 35 -> 2010, 57 -> 2850, 78 -> 3570, 100 -> 4170 RPM.
//
// The tier name sent on the wire is decorative on this firmware -- with empty
// curve arrays all four measured identical RPM -- so the duty is what matters.

// Duties and curves are KANALI 1.2.1's own, read off the wire -- one
// fanLCDSet capture per tier. The app sends the curve and the fixed duty
// together every time, whichever mode is active, so a tier is really the
// pair. The earlier 35/57/78/100 here was interpolated by hand.
static const FanTier kFanTiers[] = {
    {"low", "Low Speed", 40,
     {{0, 10}, {10, 20}, {30, 30}, {50, 40}, {65, 55}, {80, 70}, {90, 100}, {100, 100}}},
    {"mid", "Mid Speed", 60,
     {{0, 10}, {10, 20}, {30, 35}, {50, 50}, {65, 75}, {80, 80}, {90, 100}, {100, 100}}},
    {"high", "High Speed", 80,
     {{0, 10}, {10, 20}, {30, 50}, {40, 70}, {55, 85}, {70, 90}, {90, 100}, {100, 100}}},
    {"full", "Full Speed", 100,
     {{0, 10}, {10, 20}, {30, 70}, {40, 100}, {65, 100}, {80, 100}, {90, 100}, {100, 100}}},
};

const FanTier* lookup_fan_tier(const std::string& in) {
  std::string k;
  for (char c : in) {
    k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  for (const auto& t : kFanTiers) {
    std::string wire;
    for (char c : std::string(t.wire)) {
      wire += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (k == t.alias || k == wire) return &t;
  }
  return nullptr;
}

// Which tier name to send alongside an arbitrary duty: the nearest one, purely
// so the device's own model carries a sensible label.
static const char* nearest_tier_name(int duty) {
  const FanTier* best = &kFanTiers[0];
  for (const auto& t : kFanTiers) {
    if (std::abs(t.duty - duty) < std::abs(best->duty - duty)) best = &t;
  }
  return best->wire;
}

// True if any tier's smartMode/fixedMode array is non-empty.
static bool profile_is_unsafe(const picojson::value& v, std::string* why) {
  if (!v.is<picojson::object>()) return false;
  const auto& top = v.get<picojson::object>();

  // `fixedMode` is an int and is required even in Smart Mode -- KANALI sends
  // it every time. Anything non-numeric there coerces to 0 and stops the fan;
  // that is the one failure this guard exists for.
  auto fixed = top.find("fixedMode");
  if (fixed == top.end()) {
    *why = "no `fixedMode` -- the vendor sends a number in both modes";
    return true;
  }
  if (!fixed->second.is<double>()) {
    *why = "`fixedMode` must be a number -- an array or string coerces to 0";
    return true;
  }

  // `smartMode` is an array of [degC, duty%] pairs, confirmed from captured
  // vendor traffic. An empty array is accepted (that is what Fixed-only
  // payloads used to carry), but a malformed one is not.
  auto smart = top.find("smartMode");
  if (smart != top.end()) {
    if (!smart->second.is<picojson::array>()) {
      *why = "`smartMode` must be an array of [degC, duty] pairs";
      return true;
    }
    for (const auto& point : smart->second.get<picojson::array>()) {
      if (!point.is<picojson::array>() ||
          point.get<picojson::array>().size() != 2 ||
          !point.get<picojson::array>()[0].is<double>() ||
          !point.get<picojson::array>()[1].is<double>()) {
        *why = "`smartMode` points must each be [degC, duty], both numbers";
        return true;
      }
    }
  }

  // The vendor's nested per-tier shape. This firmware does not parse it, and
  // an earlier attempt to send it -- with `fixedMode: []` inside each tier --
  // is what stopped the fan. Refuse it outright.
  for (const auto& [key, tier] : top) {
    if (!tier.is<picojson::object>()) continue;
    const auto& t = tier.get<picojson::object>();
    if (t.count("smartMode") || t.count("fixedMode")) {
      *why = std::string("tier sub-object `") + key +
             "` -- this firmware takes a flat {mode, smartMode, fixedMode}";
      return true;
    }
  }
  return false;
}

int cmd_fan(const std::string& port,
                   const std::string& tier_arg, int duty_arg, bool smart,
                   const std::string& profile_path, bool force, bool verbose) {
  // Checked before the port is opened: connect() prints its own "already open"
  // diagnostic, which is noise when handing over to the daemon is the expected
  // path. `fan` with no arguments is a read and --profile needs the port;
  // only a tier or duty is state-backed.
  if (daemon_holds_port(port) && profile_path.empty() &&
      (!tier_arg.empty() || duty_arg >= 0)) {
    const FanTier* t = tier_arg.empty()
                           ? lookup_fan_tier(nearest_tier_name(duty_arg))
                           : lookup_fan_tier(tier_arg);
    if (!t) {
      std::cerr << "Unknown fan tier: \"" << tier_arg << "\"\n"
                << "Use: low | mid | high | full   (or --speed <0-100>)\n";
      return 1;
    }
    auto st = load_state_for_update();
    if (!st) return 1;
    st->fan_tier = t->wire;
    if (smart) {
      st->fan_duty.reset();
    } else {
      st->fan_duty = tier_arg.empty() ? duty_arg : t->duty;
    }
    reed::ConfigManager::save_state(*st);
    return defer_to_daemon("Fan setting");
  }

  reed::Device device(port, verbose);
  if (!device.connect()) {
    std::cerr << "Failed to connect to " << port << "\n";
    return 1;
  }
  device.drain();

  if (!profile_path.empty()) {
    std::ifstream f(profile_path);
    if (!f) {
      std::cerr << "Cannot read profile: " << profile_path << "\n";
      return 1;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string json = ss.str();

    picojson::value parsed;
    const std::string err = picojson::parse(parsed, json);
    if (!err.empty()) {
      std::cerr << "Profile is not valid JSON: " << err << "\n";
      return 1;
    }

    std::string why;
    if (profile_is_unsafe(parsed, &why) && !force) {
      std::cerr
          << "Refusing to send this profile: " << why << ".\n\n"
          << "  A valid profile is flat and looks like the vendor's own:\n"
          << "    {\"mode\": \"Smart Mode\",\n"
          << "     \"smartMode\": [[0,10],[10,20],[30,30],[50,40],\n"
          << "                    [65,55],[80,70],[90,100],[100,100]],\n"
          << "     \"fixedMode\": 40}\n\n"
          << "  8 [degC, duty%] points, and a NUMERIC fixedMode in both\n"
          << "  modes. A non-numeric fixedMode coerces to 0 and stops the LCD\n"
          << "  fan dead on firmware V1.0.11.\n"
          << "  Override with --force if you know better.\n";
      return 1;
    }

    if (!device.set_fan_profile(json)) {
      std::cerr << "No response to 'POST fanLCDSet'\n";
      return 1;
    }
    std::cout << "Fan profile sent"
              << (force ? " (--force: unvalidated curve data)" : "") << ".\n"
              << "Note: a profile does nothing until host telemetry is being\n"
              << "pushed -- run the daemon with the HUD enabled.\n";
    return 0;
  }

  // Either a named tier (positional) or an explicit duty via --speed.
  int duty = -1;
  std::string wire_tier;
  reed::FanCurve curve;
  if (!tier_arg.empty()) {
    const FanTier* tier = lookup_fan_tier(tier_arg);
    if (!tier) {
      std::cerr << "Unknown fan tier: \"" << tier_arg << "\"\n"
                << "Use: low | mid | high | full   (or --speed <0-100>)\n";
      return 1;
    }
    duty = tier->duty;
    wire_tier = tier->wire;
    curve = tier->curve;
  } else if (duty_arg >= 0) {
    if (duty_arg > 100) {
      std::cerr << "--speed must be 0-100 (got " << duty_arg << ")\n";
      return 1;
    }
    duty = duty_arg;
    wire_tier = nearest_tier_name(duty);
    // KANALI never sends a fixed duty without a curve beside it, so pair an
    // arbitrary --speed with the curve of the closest named tier.
    if (const FanTier* t = lookup_fan_tier(wire_tier)) curve = t->curve;
  }

  if (duty >= 0) {
    // Smart Mode follows the curve; Fixed Mode pins the duty. The vendor
    // sends both fields either way, so the only difference is `mode`.
    const bool ok = smart ? static_cast<bool>(device.set_fan_smart(curve, duty))
                          : static_cast<bool>(device.set_fan_fixed(duty, curve));
    if (!ok) {
      std::cerr << "No response to 'POST fanLCDSet'\n";
      return 1;
    }

    // A profile alone does nothing: the device only evaluates it once host
    // telemetry arrives. Push one frame so the setting takes effect now rather
    // than waiting for a daemon -- which matters at boot, where the fan would
    // otherwise sit at the firmware default until the daemon starts.
    //
    // The frame must carry a REAL CPU temperature. An empty push is all
    // zeroes, and in Smart Mode the curve reads 0 degC and drops the fan to
    // its floor -- 10% on every vendor curve.
    reed::SystemMonitor monitor;
    monitor.sample();  // primes the /proc/stat delta; first sample is cold
    const reed::SystemMetrics metrics = monitor.sample();
    device.send_sysinfo(build_sysinfo({"CPU Temperature", "GPU Temperature"},
                                      metrics));

    auto state = load_state_for_update();
    if (!state) return 1;
    state->fan_tier = wire_tier;
    // An unset duty is the daemon's marker for Smart Mode, so it re-applies
    // the same mode it was given rather than pinning the curve's fixed value.
    if (smart) {
      state->fan_duty.reset();
    } else {
      state->fan_duty = duty;
    }
    reed::ConfigManager::save_state(*state);

    if (smart) {
      std::cout << "Fan set to Smart Mode on the " << wire_tier
                << " curve (fixed fallback " << duty << "%).\n"
                << "  Duty now follows the CPU temperature being pushed ("
                << static_cast<int>(metrics.cpu.temperature_c) << "°C).\n"
                << "  ⚠ Smart Mode needs the daemon running: with nothing\n"
                << "    pushing, the device keeps the last value it saw.\n";
    } else {
      std::cout << "Fan set to " << duty << "% duty.\n";
    }
    if (duty == 0) {
      std::cout << "  ⚠ 0% stops the fan. It cools the panel and SoC, and no "
                   "temperature is readable.\n";
    }
    std::cout << "  Applied now; the daemon re-applies it on every connect.\n";
    return 0;
  }


  auto status = device.get_status();
  if (!status) {
    std::cerr << "No response to 'STATE all'\n";
    return 1;
  }
  std::cout << "Fan LCD: " << (status->fan_lcd.empty() ? "-" : status->fan_lcd)
            << " RPM\n";
  return 0;
}
