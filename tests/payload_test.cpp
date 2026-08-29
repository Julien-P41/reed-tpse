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
#include "reed/config.hpp"
#include "reed/mapping.hpp"
#include "reed/picojson.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

static void check(const std::string& what, bool ok) {
  std::printf("  %-46s %s\n", what.c_str(), ok ? "ok" : "FAIL");
  if (!ok) ++failures;
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
    // (the golden below assigns the curve explicitly; the default is checked
    //  separately, further down)
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

  // An untouched install never sets a fan curve -- the daemon fills it only
  // when a tier has been configured -- so the default is what most `config`
  // frames actually carry. It must be the vendor's, not an empty array: no
  // captured vendor frame has ever carried an empty smartMode, and this is
  // the field that stopped the fan dead once already.
  std::puts("config -- an unconfigured fan still gets the vendor curve:");
  {
    reed::ScreenConfig screen;
    screen.media = {"a.mp4"};
    reed::FullConfig untouched;  // nothing assigned
    const std::string frame = reed::payload::full_config(untouched, screen);
    check("smartMode is not empty",
          frame.find("\"smartMode\":[]") == std::string::npos);
    check("smartMode is the vendor's 8-point curve",
          frame.find("[[0,10],[10,20],[30,30],[50,40],[65,55],[80,70],"
                     "[90,100],[100,100]]") != std::string::npos);
  }

  // The mapping from saved state to a payload. This is where the bugs that
  // reached the panel actually came from -- the serialisers above were never
  // the problem. Covered end to end: state in, wire bytes out.
  std::puts("saved state -> payload:");
  {
    reed::DisplayState st;
    st.media = {"clip.mp4"};
    st.brightness = 40;
    st.filter = "Smoke";
    st.filter_opacity = 60;
    st.hud.enabled = true;
    st.hud.metrics = {"CPU Temperature"};
    st.hud.align = "Right";
    st.hud.color = "00FF00";
    same("full screen with a HUD and a filter",
         reed::payload::screen_config(reed::screen_config_from(st)),
         R"({"id":"Customization","screenMode":"Full Screen",)"
         R"("playMode":"Single","ratio":"2:1","media":["clip.mp4"],)"
         R"("settings":{"color":"#00FF00","align":"Right",)"
         R"("filter":{"value":"Smoke","opacity":60},"badges":[]},)"
         R"("sysinfoDisplay":["CPU Temperature"]})");

    // The filter belongs to the media, so it survives the HUD being off --
    // while the metrics do not.
    st.hud.enabled = false;
    const reed::ScreenConfig no_hud = reed::screen_config_from(st);
    check("filter survives a disabled HUD", no_hud.settings.filter == "Smoke");
    check("metrics dropped with the HUD", no_hud.sysinfo_display.empty());
  }

  std::puts("saved state -> split payload:");
  {
    reed::DisplayState st;
    st.media = {"left.png", "right.png"};
    st.screen_mode = "Screen Splitting";
    st.hud.enabled = true;
    st.hud.metrics = {"CPU Temperature"};
    st.hud.color = "00FF00";
    st.hud.align = "Left";

    // No right-zone config: the right zone mirrors the left.
    reed::ScreenConfig mirrored = reed::screen_config_from(st);
    check("split detected from the saved mode", mirrored.split);
    check("right zone mirrors the left",
          mirrored.split_settings_right.color == "00FF00" &&
              mirrored.split_settings_right.align == "Left");

    // With one, the zones diverge. This is the case that shipped a blank zone
    // when the mapping existed as copies: one copy assigned the right zone
    // before its source was populated.
    reed::HudConfig right;
    right.enabled = true;
    right.metrics = {"GPU Temperature"};
    right.color = "FF0000";
    right.align = "Right";
    st.hud_right = right;
    const reed::ScreenConfig split = reed::screen_config_from(st);
    check("right zone takes its own colour",
          split.split_settings_right.color == "FF0000");
    check("right zone takes its own metrics",
          split.split_sysinfo_right == std::vector<std::string>{"GPU Temperature"});
    check("left zone unaffected", split.settings.color == "00FF00");
  }

  // The seam between the two files above: everything else here builds a
  // DisplayState in memory, and config_test round-trips a DisplayState without
  // ever turning one into a payload. Between them, a field could be dropped
  // from persistence entirely and both suites would stay green -- which is
  // exactly the "panel is wrong but the logs look right" failure this project
  // keeps hitting. Save, load, map, serialise, and compare the bytes.
  std::puts("state file -> payload, through the real save/load:");
  {
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "reed-payload-seam";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    ::setenv("XDG_STATE_HOME", tmp.c_str(), 1);

    reed::DisplayState st;
    st.media = {"left.png", "right.png"};
    st.screen_mode = "Screen Splitting";
    st.filter = "Rain";
    st.filter_opacity = 45;
    st.screen_on = false;
    st.hud.enabled = true;
    st.hud.metrics = {"CPU Temperature"};
    st.hud.color = "00FF00";
    st.hud.align = "Left";
    reed::HudConfig right;
    right.enabled = true;
    right.metrics = {"GPU Temperature"};
    right.color = "FF0000";
    right.align = "Right";
    st.hud_right = right;

    check("save_state succeeds", reed::ConfigManager::save_state(st));
    auto loaded = reed::ConfigManager::load_state();
    check("load_state returns a value", loaded.has_value());
    if (loaded) {
      // Everything below travels: media, the split mode, both overlays and
      // the filter. A field lost in save_state or load_state shows up here as
      // the user-visible symptom -- the right zone reverting to a copy of the
      // left, or the filter coming back null.
      same("payload survives the round trip",
           reed::payload::screen_config(reed::screen_config_from(*loaded)),
           R"({"id":"Customization","screenMode":"Screen Splitting",)"
           R"("playMode":"Single","media":["left.png","right.png"],)"
           R"("settings":[{"color":"#00FF00","align":"Left",)"
           R"("filter":{"value":"Rain","opacity":45},"badges":[]},)"
           R"({"color":"#FF0000","align":"Right",)"
           R"("filter":{"value":"Rain","opacity":45},"badges":[]}],)"
           R"("sysinfoDisplay":[["CPU Temperature"],["GPU Temperature"]]})");
      check("screen_on survives",
            loaded->screen_on.has_value() && *loaded->screen_on == false);
    }
    std::filesystem::remove_all(tmp);
  }

  // What `hud clear` must produce. It used to hand-assemble this, and the copy
  // had drifted twice over: it dropped the media filter, and it set the split
  // screen_mode without the flag screen_object branches on -- yielding a
  // payload labelled "Screen Splitting" in the single-zone body.
  std::puts("hud clear -> payload:");
  {
    reed::DisplayState st;
    st.media = {"left.png", "right.png"};
    st.screen_mode = "Screen Splitting";
    st.filter = "Rain";
    st.filter_opacity = 45;
    st.hud.enabled = true;
    st.hud.metrics = {"CPU Temperature"};
    reed::HudConfig right;
    right.enabled = true;
    right.metrics = {"GPU Temperature"};
    st.hud_right = right;

    // Exactly what the command does to the state before deriving the payload.
    st.hud = reed::HudConfig{};
    st.hud_right.reset();

    const reed::ScreenConfig cfg = reed::screen_config_from(st);
    check("filter survives the clear", cfg.settings.filter == "Rain");
    check("still a split payload", cfg.split);
    check("left zone has no metrics", cfg.sysinfo_display.empty());
    check("right zone has no metrics", cfg.split_sysinfo_right.empty());

    const std::string frame = reed::payload::screen_config(cfg);
    check("two settings blocks, not one",
          frame.find("\"settings\":[{") != std::string::npos);
    check("both metric lists empty",
          frame.find("\"sysinfoDisplay\":[[],[]]") != std::string::npos);
  }

  std::printf("%s\n", failures ? "FAILURES" : "all checks passed");
  return failures != 0;
}
