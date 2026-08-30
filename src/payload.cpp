#include "reed/device.hpp"
#include "reed/wire.hpp"

#include <string>

#include "reed/picojson.h"

// The JSON bodies, separated from the transport that carries them.
//
// This is where every protocol bug in this project has lived: a `fixedMode`
// that stopped the fan, an invented `Type` key, a filter value sent as """"
// instead of null, a split zone shipped blank. They are pure functions of their
// inputs, which is what lets tests/payload_test.cpp check them against captured
// vendor traffic with no device attached -- and why they are worth keeping out
// of device.cpp, where they sat between termios setup and poll() loops.

namespace reed {

namespace payload {
namespace {

// The `settings` block, shared by screen configs and presets.
picojson::object settings_object(const DisplaySettings& in) {
  picojson::object filter;
  // null, not "": the vendor's "no filter" is a JSON null.
  filter["value"] =
      in.filter.empty() ? picojson::value() : picojson::value(in.filter);
  filter["opacity"] = picojson::value(static_cast<double>(in.filter_opacity));

  picojson::array badges_arr;
  for (const auto& b : in.badges) {
    badges_arr.push_back(picojson::value(b));
  }

  picojson::object out;
  // The device wants `#RRGGBB`; everything host-side stores bare hex.
  out["color"] = picojson::value(
      in.color.empty() || in.color[0] == '#' ? in.color : "#" + in.color);
  out["align"] = picojson::value(in.align);
  out["badges"] = picojson::value(badges_arr);
  out["filter"] = picojson::value(filter);
  return out;
}

// The screen half: `id` + media + overlay. Sent bare as waterBlockScreenId,
// or nested under waterBlockScreen.id inside a `config` frame.
picojson::object screen_object(const ScreenConfig& config) {
  picojson::array media_arr;
  for (const auto& m : config.media) {
    media_arr.push_back(picojson::value(m));
  }
  picojson::array sysinfo_arr;
  for (const auto& label : config.sysinfo_display) {
    sysinfo_arr.push_back(picojson::value(label));
  }

  picojson::object out;

  // A preset is a different shape, not a variant with the media blanked. The
  // vendor's frame carries the preset id, settings and metrics and nothing
  // else. Sending kCustomization with an empty media list -- which is what a
  // preset used to produce, because selecting one clears the saved media --
  // tells the device to show custom media that is not there.
  if (!config.preset_id.empty()) {
    out["id"] = picojson::value(config.preset_id);
    out["settings"] = picojson::value(settings_object(config.settings));
    out["sysinfoDisplay"] = picojson::value(sysinfo_arr);
    return out;
  }

  // No "Type" key: that was ours. KANALI sends `id` alone to pick between
  // custom media (wire::kCustomization) and a preset ("Pre-set N: Name").
  out["id"] = picojson::value(wire::kCustomization);
  out["screenMode"] = picojson::value(config.screen_mode);
  out["playMode"] = picojson::value(config.play_mode);
  out["media"] = picojson::value(media_arr);

  if (config.split) {
    // Two zones as parallel arrays, and no `ratio` -- the split layout fixes
    // its own geometry.
    picojson::array settings_arr;
    settings_arr.push_back(picojson::value(settings_object(config.settings)));
    settings_arr.push_back(
        picojson::value(settings_object(config.split_settings_right)));
    out["settings"] = picojson::value(settings_arr);

    picojson::array right_arr;
    for (const auto& label : config.split_sysinfo_right) {
      right_arr.push_back(picojson::value(label));
    }
    picojson::array zones;
    zones.push_back(picojson::value(sysinfo_arr));
    zones.push_back(picojson::value(right_arr));
    out["sysinfoDisplay"] = picojson::value(zones);
  } else {
    out["ratio"] = picojson::value(config.ratio);
    out["settings"] = picojson::value(settings_object(config.settings));
    out["sysinfoDisplay"] = picojson::value(sysinfo_arr);
  }
  return out;
}

}  // namespace

std::string screen_config(const ScreenConfig& config) {
  return picojson::value(screen_object(config)).serialize();
}

std::string overlay(const DisplaySettings& settings,
                    const std::vector<std::string>& metrics) {
  picojson::array items;
  for (const auto& m : metrics) items.push_back(picojson::value(m));

  picojson::object obj;
  obj["settings"] = picojson::value(settings_object(settings));
  obj["sysinfoDisplay"] = picojson::value(items);
  return picojson::value(obj).serialize();
}

std::string preset(const std::string& id, const DisplaySettings& settings,
                   const std::vector<std::string>& metrics) {
  picojson::array items;
  for (const auto& m : metrics) items.push_back(picojson::value(m));

  picojson::object obj;
  obj["id"] = picojson::value(id);
  obj["settings"] = picojson::value(settings_object(settings));
  obj["sysinfoDisplay"] = picojson::value(items);
  return picojson::value(obj).serialize();
}

}  // namespace payload

namespace payload {

std::string full_config(const FullConfig& config,
                        const ScreenConfig& screen) {
  picojson::object id = screen_object(screen);

  picojson::object fan;
  fan["mode"] = picojson::value(config.fan_mode);
  picojson::array points;
  for (const auto& [temp, duty] : config.fan_curve) {
    picojson::array point;
    point.push_back(picojson::value(static_cast<double>(temp)));
    point.push_back(picojson::value(static_cast<double>(duty)));
    points.push_back(picojson::value(point));
  }
  fan["smartMode"] = picojson::value(points);
  fan["fixedMode"] = picojson::value(static_cast<double>(config.fan_fixed));

  picojson::object screen_block;
  screen_block["enable"] = picojson::value(config.screen_enable);
  screen_block["displayInSleep"] = picojson::value(config.display_in_sleep);
  screen_block["brightness"] =
      picojson::value(static_cast<double>(config.brightness));
  // Omitted unless explicitly set -- see the note on FullConfig::rotate.
  if (config.rotate) {
    screen_block["rotate"] =
        picojson::value(static_cast<double>(*config.rotate));
  }
  screen_block["id"] = picojson::value(id);
  screen_block["fanLCD"] = picojson::value(fan);

  picojson::object spec;
  spec["cpu"] = picojson::value(config.cpu_name);
  spec["gpu"] = picojson::value(config.gpu_name);

  picojson::object obj;
  obj["temperature"] = picojson::value(config.temperature_unit);
  obj["waterBlockScreen"] = picojson::value(screen_block);
  obj["spec"] = picojson::value(spec);

  return picojson::value(obj).serialize();
}

}  // namespace payload

namespace payload {

// {mode, smartMode, fixedMode} -- byte-for-byte the shape KANALI 1.2.1 puts on
// the wire. No `speed`, and fixedMode is always a number even in Smart Mode.
std::string fan(const std::string& mode, const FanCurve& curve,
                int fixed_duty) {
  picojson::array points;
  for (const auto& [temp, duty] : curve) {
    picojson::array point;
    point.push_back(picojson::value(static_cast<double>(temp)));
    point.push_back(picojson::value(static_cast<double>(duty)));
    points.push_back(picojson::value(point));
  }

  picojson::object obj;
  obj["mode"] = picojson::value(mode);
  obj["smartMode"] = picojson::value(points);
  obj["fixedMode"] = picojson::value(static_cast<double>(fixed_duty));
  return picojson::value(obj).serialize();
}

// The vendor's "low" tier, and the same curve its `config` blob ships as the
// factory default.
const FanCurve kDefaultCurve = {{0, 10},  {10, 20}, {30, 30},  {50, 40},
                                {65, 55}, {80, 70}, {90, 100}, {100, 100}};

}  // namespace payload

}  // namespace reed
