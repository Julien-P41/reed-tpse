// Payload builders against captured vendor traffic.
//
//   cmake .. -DREED_BUILD_TESTS=ON && make && ./reed-payload-test
//
// Every JSON literal below was read off the wire from KANALI 1.2.1 driving a
// Panorama 360 ARGB on firmware V1.0.11. These are the payloads this project
// has actually got wrong -- a `fixedMode` that stopped the fan at 0 RPM, an
// invented `Type` key, a filter value sent as "" instead of null, a split zone
// shipped with empty styling. A diff against the vendor is the only check that
// would have caught any of them.
//
// Comparison is structural: both sides are parsed and re-serialised, so key
// order and float formatting do not matter, only content.
#include "reed/device.hpp"
#include "reed/picojson.h"

#include <cstdio>
#include <iostream>
#include <string>

static int failures = 0;

static std::string normalise(const std::string& json) {
  picojson::value v;
  const std::string err = picojson::parse(v, json);
  if (!err.empty()) return "<unparseable: " + err + ">";
  return v.serialize();
}

static void same(const std::string& what, const std::string& got,
                 const std::string& vendor) {
  const std::string a = normalise(got), b = normalise(vendor);
  const bool ok = a == b;
  std::printf("  %-46s %s\n", what.c_str(), ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
    std::cout << "    ours:   " << a << "\n    vendor: " << b << "\n";
  }
}

int main() {
  const reed::FanCurve low = {{0, 10},  {10, 20}, {30, 30},  {50, 40},
                              {65, 55}, {80, 70}, {90, 100}, {100, 100}};
  const reed::FanCurve mid = {{0, 10},  {10, 20}, {30, 35},  {50, 50},
                              {65, 75}, {80, 80}, {90, 100}, {100, 100}};

  std::puts("fanLCDSet:");
  same("low tier, Fixed Mode", reed::payload::fan("Fixed Mode", low, 40),
       R"({"mode":"Fixed Mode","smartMode":[[0,10],[10,20],[30,30],[50,40],)"
       R"([65,55],[80,70],[90,100],[100,100]],"fixedMode":40})");
  same("mid tier, Smart Mode", reed::payload::fan("Smart Mode", mid, 60),
       R"({"mode":"Smart Mode","smartMode":[[0,10],[10,20],[30,35],[50,50],)"
       R"([65,75],[80,80],[90,100],[100,100]],"fixedMode":60})");
  // The shape that stopped the fan: an array where a number belongs. Building
  // it is impossible through this API -- fixed_duty is an int -- which is the
  // point of the check.
  same("default curve is the vendor's low tier",
       reed::payload::fan("Smart Mode", reed::payload::kDefaultCurve, 40),
       reed::payload::fan("Smart Mode", low, 40));

  std::puts("waterBlockScreenId -- custom media:");
  {
    reed::ScreenConfig c;
    c.media = {"a.mp4", "b.mp4"};
    c.play_mode = "Shuffle";
    c.ratio = "2:1";
    c.settings.color = "dcdcdc";  // stored bare; `#` added on the wire
    c.settings.align = "Left";
    c.settings.filter_opacity = 25;
    same("full screen", reed::payload::screen_config(c),
         R"({"id":"Customization","screenMode":"Full Screen",)"
         R"("playMode":"Shuffle","ratio":"2:1","media":["a.mp4","b.mp4"],)"
         R"("settings":{"color":"#dcdcdc","align":"Left",)"
         R"("filter":{"value":null,"opacity":25},"badges":[]},)"
         R"("sysinfoDisplay":[]})");
  }

  std::puts("waterBlockScreenId -- screen splitting:");
  {
    reed::ScreenConfig c;
    c.media = {"left.png", "right.png"};
    c.screen_mode = "Screen Splitting";
    c.split = true;
    c.settings.color = "000000";
    c.settings.align = "Left";
    c.split_settings_right = c.settings;
    // Two zones as parallel arrays, and no `ratio` -- the split layout fixes
    // its own geometry.
    same("two zones", reed::payload::screen_config(c),
         R"({"id":"Customization","screenMode":"Screen Splitting",)"
         R"("playMode":"Single","media":["left.png","right.png"],)"
         R"("settings":[{"color":"#000000","align":"Left",)"
         R"("filter":{"value":null,"opacity":100},"badges":[]},)"
         R"({"color":"#000000","align":"Left",)"
         R"("filter":{"value":null,"opacity":100},"badges":[]}],)"
         R"("sysinfoDisplay":[[],[]]})");
  }

  std::puts("waterBlockScreenId -- preset:");
  {
    reed::DisplaySettings s;
    s.color = "000000";
    s.align = "Left";
    s.filter = "Rain";
    s.filter_opacity = 50;
    same("id carries settings and metrics",
         reed::payload::preset("Pre-set 12: Cyber Bunker", s, {}),
         R"({"id":"Pre-set 12: Cyber Bunker",)"
         R"("settings":{"color":"#000000","align":"Left",)"
         R"("filter":{"value":"Rain","opacity":50},"badges":[]},)"
         R"("sysinfoDisplay":[]})");
  }

  std::puts("preset -- the overlay command:");
  {
    reed::DisplaySettings s;
    s.color = "dcdcdc";
    s.align = "Left";
    s.filter_opacity = 50;
    s.badges = {"CPU Badge", "GPU Badge"};
    same("styling and metrics together",
         reed::payload::overlay(
             s, {"CPU Temperature", "GPU Temperature", "Memory Frequency"}),
         R"({"settings":{"color":"#dcdcdc","align":"Left",)"
         R"("filter":{"value":null,"opacity":50},)"
         R"("badges":["CPU Badge","GPU Badge"]},)"
         R"("sysinfoDisplay":["CPU Temperature","GPU Temperature",)"
         R"("Memory Frequency"]})");
  }

  std::puts("config -- the post-connect frame:");
  {
    reed::ScreenConfig screen;
    screen.media = {"a.mp4"};
    screen.play_mode = "Shuffle";
    screen.settings.color = "dcdcdc";
    screen.settings.align = "Left";
    screen.settings.filter_opacity = 25;

    reed::FullConfig full;
    full.brightness = 100;
    full.fan_curve = low;
    full.fan_fixed = 40;
    full.cpu_name = "CPU";
    full.gpu_name = "GPU";
    same("no rotate when unset", reed::payload::full_config(full, screen),
         R"({"temperature":"Celsius","waterBlockScreen":{"enable":true,)"
         R"("displayInSleep":false,"brightness":100,)"
         R"("id":{"id":"Customization","screenMode":"Full Screen",)"
         R"("playMode":"Shuffle","ratio":"2:1","media":["a.mp4"],)"
         R"("settings":{"color":"#dcdcdc","align":"Left",)"
         R"("filter":{"value":null,"opacity":25},"badges":[]},)"
         R"("sysinfoDisplay":[]},)"
         R"("fanLCD":{"mode":"Smart Mode","smartMode":[[0,10],[10,20],[30,30],)"
         R"([50,40],[65,55],[80,70],[90,100],[100,100]],"fixedMode":40}},)"
         R"("spec":{"cpu":"CPU","gpu":"GPU"}})");

    // Rotation is applied at the device's next start and cannot be read back,
    // so an unset value must never appear in the frame.
    full.rotate = 270;
    const std::string with_rotate = reed::payload::full_config(full, screen);
    const bool present = with_rotate.find("\"rotate\":270") != std::string::npos;
    std::printf("  %-46s %s\n", "rotate appears only when set",
                present ? "ok" : "FAIL");
    if (!present) ++failures;
  }

  std::printf("%s\n", failures ? "FAILURES" : "all checks passed");
  return failures != 0;
}
