#pragma once

// Reading values out of a picojson tree, defensively.
//
// Every accessor here answers "what is at this key, and is it the type I
// expect?" in one call, returning a default rather than throwing. That matters
// because all three inputs -- config.json, display.json and the device's own
// replies -- are outside this program's control: a hand-edited config, a file
// half-written by an older version, or a firmware that answers with a string
// where it used to answer with a number should all degrade to a default, not
// terminate the process.
//
// These lived as three separate copies in src/config.cpp, src/device.cpp and
// src/status_cache.cpp. Two of the get_string bodies were byte-identical and
// the third silently lacked the default-value parameter, which is exactly how
// copies drift: the odd one out was not obviously wrong at its call site.
//
// Header-only and inline: they are a few lines each, every caller is in this
// project, and a .cpp for them would be more build graph than code.

#include <string>
#include <vector>

#include "picojson.h"

namespace reed::json {

inline bool has_key(const picojson::value& v, const std::string& key) {
  if (!v.is<picojson::object>()) return false;
  return v.get<picojson::object>().count(key) > 0;
}

inline std::string get_string(const picojson::value& v, const std::string& key,
                              const std::string& def = "") {
  if (!v.is<picojson::object>()) return def;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<std::string>()) return def;
  return it->second.get<std::string>();
}

inline int get_int(const picojson::value& v, const std::string& key,
                   int def = 0) {
  if (!v.is<picojson::object>()) return def;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<double>()) return def;
  return static_cast<int>(it->second.get<double>());
}

inline bool get_bool(const picojson::value& v, const std::string& key,
                     bool def = false) {
  if (!v.is<picojson::object>()) return def;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<bool>()) return def;
  return it->second.get<bool>();
}

inline std::vector<std::string> get_string_array(const picojson::value& v,
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

// A reference to the value at `key`, or to a shared null when it is absent.
// Returning a reference keeps the caller's `const auto&` idiom working without
// copying a whole subtree.
inline const picojson::value& get_value(const picojson::value& v,
                                        const std::string& key) {
  static const picojson::value null_val;
  if (!v.is<picojson::object>()) return null_val;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end()) return null_val;
  return it->second;
}

}  // namespace reed::json
