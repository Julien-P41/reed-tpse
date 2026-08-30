#include "reed/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "reed/picojson.h"

namespace fs = std::filesystem;

namespace reed {

namespace {

std::string get_string(const picojson::value& v, const std::string& key,
                       const std::string& def = "") {
  if (!v.is<picojson::object>()) return def;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<std::string>()) return def;
  return it->second.get<std::string>();
}

int get_int(const picojson::value& v, const std::string& key, int def = 0) {
  if (!v.is<picojson::object>()) return def;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<double>()) return def;
  return static_cast<int>(it->second.get<double>());
}

bool get_bool(const picojson::value& v, const std::string& key, bool def = false) {
  if (!v.is<picojson::object>()) return def;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<bool>()) return def;
  return it->second.get<bool>();
}

std::vector<std::string> get_string_array(const picojson::value& v,
                                          const std::string& key) {
  std::vector<std::string> out;
  if (!v.is<picojson::object>()) return out;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<picojson::array>()) return out;
  for (const auto& elem : it->second.get<picojson::array>()) {
    if (elem.is<std::string>()) out.push_back(elem.get<std::string>());
  }
  return out;
}

const picojson::value& get_value(const picojson::value& v,
                                 const std::string& key) {
  static picojson::value null_val;
  if (!v.is<picojson::object>()) return null_val;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end()) return null_val;
  return it->second;
}

}  // namespace

namespace {

// Write via a temporary in the same directory, then rename. rename(2) is
// atomic within a filesystem, so a reader either sees the whole previous file
// or the whole new one -- never the truncated middle, which a plain ofstream
// leaves visible for as long as the write takes.
bool write_atomically(const std::string& path, const std::string& content) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream file(tmp, std::ios::trunc);
    if (!file) return false;
    file << content;
    file.flush();
    if (!file.good()) {
      std::error_code ec;
      fs::remove(tmp, ec);
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

}  // namespace

std::string ConfigManager::get_config_dir() {
  const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
  if (xdg_config && *xdg_config) {
    return std::string(xdg_config) + "/reed-tpse";
  }

  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.config/reed-tpse";
  }

  return ".config/reed-tpse";
}

std::string ConfigManager::get_state_dir() {
  const char* xdg_state = std::getenv("XDG_STATE_HOME");
  if (xdg_state && *xdg_state) {
    return std::string(xdg_state) + "/reed-tpse";
  }

  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.local/state/reed-tpse";
  }

  return ".local/state/reed-tpse";
}

std::string ConfigManager::get_config_path() {
  return get_config_dir() + "/config.json";
}

std::string ConfigManager::get_state_path() {
  return get_state_dir() + "/display.json";
}

std::optional<Config> ConfigManager::load_config(LoadStatus* status) {
  auto report = [status](LoadStatus s) {
    if (status) *status = s;
  };
  std::string path = get_config_path();

  // error_code overload: the throwing one terminates the process on a path it
  // cannot stat. Running as a user without access to the config directory --
  // a boot script with the wrong SERVICE_USER, say -- printed a raw
  // std::filesystem_error and aborted instead of saying what was wrong.
  std::error_code ec;
  if (!fs::exists(path, ec) || ec) {
    report(ec ? LoadStatus::Unreadable : LoadStatus::Missing);
    return ec ? std::optional<Config>{} : std::optional<Config>{Config{}};
  }

  std::ifstream file(path);
  if (!file) {
    report(LoadStatus::Unreadable);
    return std::nullopt;
  }

  std::ostringstream ss;
  ss << file.rdbuf();

  picojson::value json;
  std::string err = picojson::parse(json, ss.str());
  if (!err.empty()) {
    report(LoadStatus::Malformed);
    return std::nullopt;
  }
  report(LoadStatus::Ok);

  Config config;
  config.port = get_string(json, "port", config.port);
  config.keepalive_interval = get_int(json, "keepalive_interval", config.keepalive_interval);
  // Legacy: one `power_auto` flag covered all three. Honour it as the default
  // for each, so a config that switched the behaviour off keeps it off, then
  // let any specific key override.
  const bool legacy = get_bool(json, "power_auto", true);
  config.report_ac_power = get_bool(json, "report_ac_power", legacy);
  config.report_lock = get_bool(json, "report_lock", legacy);
  config.report_shutdown = get_bool(json, "report_shutdown", legacy);
  {
    const std::string lm = get_string(json, "lock_media", "");
    if (!lm.empty()) config.lock_media = lm;
    config.lock_brightness =
        get_int(json, "lock_brightness", config.lock_brightness);
  }

  return config;
}

bool ConfigManager::save_config(const Config& config) {
  std::string dir = get_config_dir();
  // error_code overload, like the loaders. The throwing one aborts the whole
  // process on a directory it cannot create -- `terminate called after
  // throwing an instance of std::filesystem_error`, SIGABRT, exit 134 -- which
  // is what a read-only or unwritable XDG path produces. The read path was
  // fixed for exactly this; the write path was not, so a save turned a
  // reportable failure into a crash.
  std::error_code dir_ec;
  fs::create_directories(dir, dir_ec);
  if (dir_ec) return false;

  picojson::object obj;
  obj["port"] = picojson::value(config.port);
  obj["keepalive_interval"] =
      picojson::value(static_cast<double>(config.keepalive_interval));
  obj["report_ac_power"] = picojson::value(config.report_ac_power);
  obj["report_lock"] = picojson::value(config.report_lock);
  obj["report_shutdown"] = picojson::value(config.report_shutdown);
  if (config.lock_media) {
    obj["lock_media"] = picojson::value(*config.lock_media);
  }
  // Written whether or not a lock clip is set. Nesting it inside the media
  // meant `lock-display --remove` silently discarded the chosen brightness,
  // so re-enabling a clip later came back at the default -- upward, on a panel
  // this project warns about burn-in for.
  obj["lock_brightness"] =
      picojson::value(static_cast<double>(config.lock_brightness));

  return write_atomically(get_config_path(),
                          picojson::value(obj).serialize() + "\n");
}

std::optional<DisplayState> ConfigManager::load_state(LoadStatus* status) {
  auto report = [status](LoadStatus s) {
    if (status) *status = s;
  };
  std::string path = get_state_path();

  std::error_code ec;
  if (!fs::exists(path, ec) || ec) {
    report(ec ? LoadStatus::Unreadable : LoadStatus::Missing);
    return std::nullopt;
  }

  std::ifstream file(path);
  if (!file) {
    report(LoadStatus::Unreadable);
    return std::nullopt;
  }

  std::ostringstream ss;
  ss << file.rdbuf();

  picojson::value json;
  std::string err = picojson::parse(json, ss.str());
  if (!err.empty()) {
    report(LoadStatus::Malformed);
    return std::nullopt;
  }
  report(LoadStatus::Ok);

  DisplayState state;

  const auto& media_val = get_value(json, "media");
  if (media_val.is<picojson::array>()) {
    for (const auto& m : media_val.get<picojson::array>()) {
      if (m.is<std::string>()) {
        state.media.push_back(m.get<std::string>());
      }
    }
  }

  state.ratio = get_string(json, "ratio", state.ratio);
  state.screen_mode = get_string(json, "screen_mode", state.screen_mode);
  state.play_mode = get_string(json, "play_mode", state.play_mode);
  state.brightness = get_int(json, "brightness", state.brightness);
  state.filter = get_string(json, "filter", state.filter);
  state.filter_opacity = get_int(json, "filter_opacity", state.filter_opacity);
  if (json.is<picojson::object>() &&
      json.get<picojson::object>().count("display_in_sleep")) {
    state.display_in_sleep = get_bool(json, "display_in_sleep", false);
  }

  if (json.is<picojson::object>() &&
      json.get<picojson::object>().count("screen_on")) {
    state.screen_on = get_bool(json, "screen_on", true);
  }

  {
    const std::string p = get_string(json, "preset", "");
    if (!p.empty()) state.preset = p;
  }

  {
    const std::string t = get_string(json, "fan_tier", "");
    if (!t.empty()) state.fan_tier = t;
    if (json.is<picojson::object>() &&
        json.get<picojson::object>().count("fan_duty")) {
      state.fan_duty = get_int(json, "fan_duty", 0);
    }
  }

  auto read_hud = [](const picojson::value& v, HudConfig& h) {
    h.enabled = get_bool(v, "enabled", h.enabled);
    h.metrics = get_string_array(v, "metrics");
    h.align = get_string(v, "align", h.align);
    h.color = get_string(v, "color", h.color);
    // Colours were once stored as `#RRGGBB`; carry those files forward.
    if (!h.color.empty() && h.color[0] == '#') h.color = h.color.substr(1);
    h.badges = get_string_array(v, "badges");
    h.push_interval_sec =
        get_int(v, "push_interval_sec", h.push_interval_sec);
    h.temperature_unit =
        get_string(v, "temperature_unit", h.temperature_unit);
    h.cpu_name = get_string(v, "cpu_name", h.cpu_name);
    h.gpu_name = get_string(v, "gpu_name", h.gpu_name);
  };

  const auto& hud_val = get_value(json, "hud");
  if (hud_val.is<picojson::object>()) read_hud(hud_val, state.hud);

  const auto& right_val = get_value(json, "hud_right");
  if (right_val.is<picojson::object>()) {
    HudConfig right;
    read_hud(right_val, right);
    state.hud_right = right;
  }

  return state;
}

bool ConfigManager::save_state(const DisplayState& state) {
  std::string dir = get_state_dir();
  // See save_config: the throwing overload aborts the process rather than
  // letting the caller report that nothing was saved.
  std::error_code dir_ec;
  fs::create_directories(dir, dir_ec);
  if (dir_ec) return false;

  picojson::array media_arr;
  for (const auto& m : state.media) {
    media_arr.push_back(picojson::value(m));
  }

  picojson::object obj;
  obj["media"] = picojson::value(media_arr);
  obj["ratio"] = picojson::value(state.ratio);
  obj["screen_mode"] = picojson::value(state.screen_mode);
  obj["play_mode"] = picojson::value(state.play_mode);
  obj["brightness"] = picojson::value(static_cast<double>(state.brightness));
  obj["filter"] = picojson::value(state.filter);
  obj["filter_opacity"] =
      picojson::value(static_cast<double>(state.filter_opacity));
  // Written only once configured, so an untouched device is never overridden.
  if (state.display_in_sleep) {
    obj["display_in_sleep"] = picojson::value(*state.display_in_sleep);
  }
  if (state.screen_on) {
    obj["screen_on"] = picojson::value(*state.screen_on);
  }
  if (state.preset) {
    obj["preset"] = picojson::value(*state.preset);
  }
  if (state.fan_tier) {
    obj["fan_tier"] = picojson::value(*state.fan_tier);
  }
  if (state.fan_duty) {
    obj["fan_duty"] = picojson::value(static_cast<double>(*state.fan_duty));
  }

  auto write_hud = [](const HudConfig& h) {
    picojson::array metrics_arr;
    for (const auto& m : h.metrics) metrics_arr.push_back(picojson::value(m));
    picojson::array badges_arr;
    for (const auto& b : h.badges) badges_arr.push_back(picojson::value(b));

    picojson::object out;
    out["enabled"] = picojson::value(h.enabled);
    out["metrics"] = picojson::value(metrics_arr);
    out["align"] = picojson::value(h.align);
    out["color"] = picojson::value(h.color);
    out["badges"] = picojson::value(badges_arr);
    out["push_interval_sec"] =
        picojson::value(static_cast<double>(h.push_interval_sec));
    out["temperature_unit"] = picojson::value(h.temperature_unit);
    out["cpu_name"] = picojson::value(h.cpu_name);
    out["gpu_name"] = picojson::value(h.gpu_name);
    return out;
  };

  obj["hud"] = picojson::value(write_hud(state.hud));
  if (state.hud_right) {
    obj["hud_right"] = picojson::value(write_hud(*state.hud_right));
  }

  return write_atomically(get_state_path(),
                          picojson::value(obj).serialize() + "\n");
}

}  // namespace reed
