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

std::optional<Config> ConfigManager::load_config() {
  std::string path = get_config_path();

  if (!fs::exists(path)) {
    return Config{};
  }

  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }

  std::ostringstream ss;
  ss << file.rdbuf();

  picojson::value json;
  std::string err = picojson::parse(json, ss.str());
  if (!err.empty()) {
    return std::nullopt;
  }

  Config config;
  config.port = get_string(json, "port", config.port);
  config.brightness = get_int(json, "brightness", config.brightness);
  config.keepalive_interval = get_int(json, "keepalive_interval", config.keepalive_interval);
  config.power_auto = get_bool(json, "power_auto", config.power_auto);
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
  fs::create_directories(dir);

  std::string path = get_config_path();
  std::ofstream file(path);
  if (!file) {
    return false;
  }

  picojson::object obj;
  obj["port"] = picojson::value(config.port);
  obj["brightness"] = picojson::value(static_cast<double>(config.brightness));
  obj["keepalive_interval"] =
      picojson::value(static_cast<double>(config.keepalive_interval));
  obj["power_auto"] = picojson::value(config.power_auto);
  if (config.lock_media) {
    obj["lock_media"] = picojson::value(*config.lock_media);
    obj["lock_brightness"] =
        picojson::value(static_cast<double>(config.lock_brightness));
  }

  file << picojson::value(obj).serialize() << "\n";
  return file.good();
}

std::optional<DisplayState> ConfigManager::load_state() {
  std::string path = get_state_path();

  if (!fs::exists(path)) {
    return std::nullopt;
  }

  std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }

  std::ostringstream ss;
  ss << file.rdbuf();

  picojson::value json;
  std::string err = picojson::parse(json, ss.str());
  if (!err.empty()) {
    return std::nullopt;
  }

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
  if (json.is<picojson::object>() &&
      json.get<picojson::object>().count("display_in_sleep")) {
    state.display_in_sleep = get_bool(json, "display_in_sleep", false);
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

  const auto& hud_val = get_value(json, "hud");
  if (hud_val.is<picojson::object>()) {
    HudConfig& h = state.hud;
    h.enabled = get_bool(hud_val, "enabled", h.enabled);
    h.metrics = get_string_array(hud_val, "metrics");
    h.position = get_string(hud_val, "position", h.position);
    h.align = get_string(hud_val, "align", h.align);
    h.color = get_string(hud_val, "color", h.color);
    h.badges = get_string_array(hud_val, "badges");
    h.push_interval_sec =
        get_int(hud_val, "push_interval_sec", h.push_interval_sec);
    h.temperature_unit =
        get_string(hud_val, "temperature_unit", h.temperature_unit);
    h.cpu_name = get_string(hud_val, "cpu_name", h.cpu_name);
    h.gpu_name = get_string(hud_val, "gpu_name", h.gpu_name);
  }

  return state;
}

bool ConfigManager::save_state(const DisplayState& state) {
  std::string dir = get_state_dir();
  fs::create_directories(dir);

  std::string path = get_state_path();
  std::ofstream file(path);
  if (!file) {
    return false;
  }

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
  // Written only once configured, so an untouched device is never overridden.
  if (state.display_in_sleep) {
    obj["display_in_sleep"] = picojson::value(*state.display_in_sleep);
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

  picojson::array hud_metrics_arr;
  for (const auto& m : state.hud.metrics) {
    hud_metrics_arr.push_back(picojson::value(m));
  }
  picojson::array hud_badges_arr;
  for (const auto& b : state.hud.badges) {
    hud_badges_arr.push_back(picojson::value(b));
  }
  picojson::object hud_obj;
  hud_obj["enabled"] = picojson::value(state.hud.enabled);
  hud_obj["metrics"] = picojson::value(hud_metrics_arr);
  hud_obj["position"] = picojson::value(state.hud.position);
  hud_obj["align"] = picojson::value(state.hud.align);
  hud_obj["color"] = picojson::value(state.hud.color);
  hud_obj["badges"] = picojson::value(hud_badges_arr);
  hud_obj["push_interval_sec"] =
      picojson::value(static_cast<double>(state.hud.push_interval_sec));
  hud_obj["temperature_unit"] = picojson::value(state.hud.temperature_unit);
  hud_obj["cpu_name"] = picojson::value(state.hud.cpu_name);
  hud_obj["gpu_name"] = picojson::value(state.hud.gpu_name);
  obj["hud"] = picojson::value(hud_obj);

  file << picojson::value(obj).serialize() << "\n";
  return file.good();
}

}  // namespace reed
