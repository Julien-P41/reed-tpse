#include "reed/status_cache.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "reed/config.hpp"
#include "reed/picojson.h"

namespace fs = std::filesystem;

namespace reed {

namespace {

long long now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

picojson::array to_array(const std::vector<std::string>& in) {
  picojson::array out;
  for (const auto& s : in) out.push_back(picojson::value(s));
  return out;
}

std::string get_string(const picojson::value& v, const std::string& key) {
  if (!v.is<picojson::object>()) return {};
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<std::string>()) return {};
  return it->second.get<std::string>();
}

}  // namespace

long long StatusSnapshot::age_seconds() const {
  const long long age = now_seconds() - unix_seconds;
  return age < 0 ? 0 : age;  // a clock step backwards is not negative age
}

std::string StatusCache::path() {
  // Beside the state file, deliberately, and NOT under XDG_RUNTIME_DIR.
  //
  // Runtime dir is the tidier home for this, but the writer and the reader
  // have to agree on it and they do not: a system-scope unit gets no
  // XDG_RUNTIME_DIR, while an interactive shell has one. The daemon published
  // to the state directory and the CLI looked in /run/user/<uid> -- both
  // working perfectly, at different paths.
  //
  // The state directory is the one location both already resolve identically,
  // because it is where display.json lives and they agree on that or nothing
  // works at all.
  return ConfigManager::get_state_dir() + "/status.json";
}

bool StatusCache::publish(const DeviceStatus& status, const DeviceInfo& info) {
  const std::string file = path();
  std::error_code ec;
  fs::create_directories(fs::path(file).parent_path(), ec);
  if (ec) return false;

  picojson::array warnings;
  for (const auto& w : status.warnings) {
    picojson::object o;
    o["type"] = picojson::value(w.type);
    o["description"] = picojson::value(w.description);
    warnings.push_back(picojson::value(o));
  }

  picojson::object st;
  st["fan_lcd"] = picojson::value(status.fan_lcd);
  st["turbo_pump"] = picojson::value(status.turbo_pump);
  st["available_storage"] = picojson::value(status.available_storage);
  st["warnings"] = picojson::value(warnings);

  picojson::object in;
  in["product_id"] = picojson::value(info.product_id);
  in["os"] = picojson::value(info.os);
  in["serial"] = picojson::value(info.serial);
  in["app_version"] = picojson::value(info.app_version);
  in["firmware"] = picojson::value(info.firmware);
  in["hardware"] = picojson::value(info.hardware);
  in["attributes"] = picojson::value(to_array(info.attributes));

  picojson::object obj;
  obj["taken_at"] = picojson::value(static_cast<double>(now_seconds()));
  obj["status"] = picojson::value(st);
  obj["info"] = picojson::value(in);

  // Same temp-and-rename as the state file: a reader either sees the whole
  // previous snapshot or the whole new one, never a half-written middle.
  const std::string tmp = file + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << picojson::value(obj).serialize() << "\n";
    out.flush();
    if (!out.good()) {
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

std::optional<StatusSnapshot> StatusCache::read() {
  std::error_code ec;
  const std::string file = path();
  if (!fs::exists(file, ec) || ec) return std::nullopt;

  std::ifstream in(file);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();

  picojson::value json;
  if (!picojson::parse(json, ss.str()).empty()) return std::nullopt;
  if (!json.is<picojson::object>()) return std::nullopt;
  const auto& obj = json.get<picojson::object>();

  StatusSnapshot snap;
  if (auto it = obj.find("taken_at");
      it != obj.end() && it->second.is<double>()) {
    snap.unix_seconds = static_cast<long long>(it->second.get<double>());
  }

  if (auto it = obj.find("status"); it != obj.end()) {
    const picojson::value& s = it->second;
    snap.status.fan_lcd = get_string(s, "fan_lcd");
    snap.status.turbo_pump = get_string(s, "turbo_pump");
    if (s.is<picojson::object>()) {
      const auto& so = s.get<picojson::object>();
      if (auto a = so.find("available_storage");
          a != so.end() && a->second.is<double>()) {
        snap.status.available_storage = a->second.get<double>();
      }
      if (auto w = so.find("warnings");
          w != so.end() && w->second.is<picojson::array>()) {
        for (const auto& entry : w->second.get<picojson::array>()) {
          Warning warn;
          warn.type = get_string(entry, "type");
          warn.description = get_string(entry, "description");
          snap.status.warnings.push_back(warn);
        }
      }
    }
  }

  if (auto it = obj.find("info"); it != obj.end()) {
    const picojson::value& i = it->second;
    snap.info.product_id = get_string(i, "product_id");
    snap.info.os = get_string(i, "os");
    snap.info.serial = get_string(i, "serial");
    snap.info.app_version = get_string(i, "app_version");
    snap.info.firmware = get_string(i, "firmware");
    snap.info.hardware = get_string(i, "hardware");
    if (i.is<picojson::object>()) {
      const auto& io = i.get<picojson::object>();
      if (auto a = io.find("attributes");
          a != io.end() && a->second.is<picojson::array>()) {
        for (const auto& e : a->second.get<picojson::array>()) {
          if (e.is<std::string>()) {
            snap.info.attributes.push_back(e.get<std::string>());
          }
        }
      }
    }
  }

  return snap;
}

}  // namespace reed
