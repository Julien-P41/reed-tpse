// Config/state round-trip checks. No hardware required.
//
//   cmake .. -DREED_BUILD_TESTS=ON && make && ./reed-config-test
//
// These exist because `display` once wrote a fresh DisplayState, silently
// wiping the HUD config, display_in_sleep and the fan setting that share the
// file. save_state() truncates, so anything it does not write is lost -- which
// stays invisible until someone notices their overlay disappeared.
//
// Scope: this covers the config layer, not the call site. It verifies that a
// load -> mutate -> save cycle preserves siblings, i.e. that the pattern
// `display` now uses is sound. It would NOT catch a caller going back to
// building a fresh DisplayState; only reading the caller catches that.
#include "reed/config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using reed::Config;
using reed::ConfigManager;
using reed::DisplayState;

static int failures = 0;

static void check(const std::string& what, bool ok) {
  std::printf("  %-52s %s\n", what.c_str(), ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

static DisplayState populated() {
  DisplayState s;
  s.media = {"a.mp4", "b.png"};
  s.ratio = "1:1";
  s.screen_mode = "Screen Splitting";
  s.play_mode = "Shuffle";
  s.brightness = 42;
  s.display_in_sleep = true;
  s.preset = "Cyber Bunker";
  s.fan_tier = "Mid Speed";
  s.fan_duty = 57;
  s.hud.enabled = true;
  s.hud.metrics = {"CPU Temperature", "Date&Time"};
  s.hud.align = "Right";
  s.hud.color = "00FF00";  // bare hex; `#` is added on the wire only
  s.hud.badges = {"CPU Badge"};
  s.hud.push_interval_sec = 9;
  s.hud.temperature_unit = "Fahrenheit";
  s.hud.cpu_name = "Test CPU";
  s.hud.gpu_name = "Test GPU";
  return s;
}

int main() {
  const fs::path tmp = fs::temp_directory_path() / "reed-config-test";
  fs::remove_all(tmp);
  fs::create_directories(tmp);
  ::setenv("XDG_STATE_HOME", tmp.c_str(), 1);

  std::puts("state round-trip:");
  const DisplayState in = populated();
  check("save_state succeeds", ConfigManager::save_state(in));

  auto out = ConfigManager::load_state();
  check("load_state returns a value", out.has_value());
  if (!out) return 1;

  check("media", out->media == in.media);
  check("ratio", out->ratio == in.ratio);
  check("screen_mode", out->screen_mode == in.screen_mode);
  check("play_mode", out->play_mode == in.play_mode);
  check("brightness", out->brightness == in.brightness);
  check("display_in_sleep", out->display_in_sleep == in.display_in_sleep);
  check("preset", out->preset == in.preset);
  check("fan_tier", out->fan_tier == in.fan_tier);
  check("fan_duty", out->fan_duty == in.fan_duty);
  check("hud.enabled", out->hud.enabled == in.hud.enabled);
  check("hud.metrics", out->hud.metrics == in.hud.metrics);
  check("hud.align", out->hud.align == in.hud.align);
  check("hud.color", out->hud.color == in.hud.color);
  check("hud.badges", out->hud.badges == in.hud.badges);
  check("hud.push_interval_sec",
        out->hud.push_interval_sec == in.hud.push_interval_sec);
  check("hud.temperature_unit",
        out->hud.temperature_unit == in.hud.temperature_unit);
  check("hud.cpu_name", out->hud.cpu_name == in.hud.cpu_name);
  check("hud.gpu_name", out->hud.gpu_name == in.hud.gpu_name);

  std::puts("unset optionals stay unset:");
  DisplayState bare;
  bare.media = {"only.mp4"};
  ConfigManager::save_state(bare);
  auto reread = ConfigManager::load_state();
  check("display_in_sleep absent", reread && !reread->display_in_sleep);
  check("preset absent", reread && !reread->preset);
  check("fan_duty absent", reread && !reread->fan_duty);

  std::puts("load-mutate-save preserves siblings:");
  ConfigManager::save_state(in);
  auto edited = ConfigManager::load_state();
  if (edited) {
    edited->media = {"new.mp4"};  // what `display` changes
    ConfigManager::save_state(*edited);
  }
  auto after = ConfigManager::load_state();
  check("media updated", after && after->media == std::vector<std::string>{"new.mp4"});
  check("hud survived", after && after->hud.enabled &&
                            after->hud.metrics == in.hud.metrics);
  check("fan survived", after && after->fan_duty == in.fan_duty);
  check("display_in_sleep survived",
        after && after->display_in_sleep == in.display_in_sleep);
  check("preset survived", after && after->preset == in.preset);

  std::puts("false is distinguishable from unset:");
  DisplayState off;
  off.media = {"x.mp4"};
  off.display_in_sleep = false;
  ConfigManager::save_state(off);
  auto off_read = ConfigManager::load_state();
  check("display_in_sleep=false round-trips",
        off_read && off_read->display_in_sleep.has_value() &&
            *off_read->display_in_sleep == false);

  std::puts("config.json round-trip:");
  ::setenv("XDG_CONFIG_HOME", tmp.c_str(), 1);
  Config c;
  c.port = "/dev/tryx-panorama";
  c.keepalive_interval = 12;
  c.power_auto = true;
  c.lock_media = "sunset.mp4";
  c.lock_brightness = 30;
  check("save_config succeeds", ConfigManager::save_config(c));
  auto rc = ConfigManager::load_config();
  check("port", rc && rc->port == c.port);
  check("keepalive_interval", rc && rc->keepalive_interval == c.keepalive_interval);
  check("power_auto", rc && rc->power_auto == c.power_auto);
  check("lock_media", rc && rc->lock_media == c.lock_media);
  check("lock_brightness", rc && rc->lock_brightness == c.lock_brightness);

  // The lock settings must survive a `display`, which rewrites the *state*
  // file -- that is why they live in config.json rather than beside it.
  DisplayState d;
  d.media = {"other.mp4"};
  ConfigManager::save_state(d);
  auto rc2 = ConfigManager::load_config();
  check("lock_media survives a state rewrite", rc2 && rc2->lock_media == c.lock_media);

  Config cleared = *rc;
  cleared.lock_media.reset();
  ConfigManager::save_config(cleared);
  auto rc3 = ConfigManager::load_config();
  check("cleared lock_media stays unset", rc3 && !rc3->lock_media);
  check("power_auto survives the clear", rc3 && rc3->power_auto);
  check("lock_brightness survives the clear",
        rc3 && rc3->lock_brightness == 30);

  // Why a load failed, and -- the part that matters -- that a file which
  // exists but does not parse is left alone. Reporting "missing" for a bad
  // read is what let a command load defaults and save them over everything
  // the file held.
  std::puts("a bad state file is not clobbered:");
  {
    const fs::path state_dir = tmp / "reed-tpse";
    fs::create_directories(state_dir);
    const fs::path state_file = state_dir / "display.json";

    const std::string garbage = "{\"media\":[\"a.mp4\"], TRUNCATED";
    { std::ofstream f(state_file); f << garbage; }

    reed::LoadStatus status = reed::LoadStatus::Ok;
    auto bad = ConfigManager::load_state(&status);
    check("malformed file does not load", !bad.has_value());
    check("reported as Malformed, not Missing",
          status == reed::LoadStatus::Malformed);

    std::ostringstream after;
    { std::ifstream f(state_file); after << f.rdbuf(); }
    check("malformed file left byte-identical", after.str() == garbage);

    fs::remove(state_file);
    status = reed::LoadStatus::Ok;
    auto gone = ConfigManager::load_state(&status);
    check("absent file does not load", !gone.has_value());
    check("reported as Missing", status == reed::LoadStatus::Missing);
  }

  fs::remove_all(tmp);
  std::printf("%s\n", failures ? "FAILURES" : "all checks passed");
  return failures != 0;
}
