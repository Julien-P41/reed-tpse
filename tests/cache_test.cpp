// StatusCache round-trips and staleness, plus what can be checked of the
// network sampler. No hardware required.
//
//   cmake .. -DREED_BUILD_TESTS=ON && make && ./reed-cache-test
//
// These exist because the status cache shipped with no coverage at all and
// three defects that only showed up in use: it was published from one code
// path a default install never took, it reported exit 0 for a device fault,
// and nothing bounded how old a snapshot could be before being presented as
// current.
//
// Several checks below are negative controls -- they assert that something
// FAILS. A suite where every assertion passes by construction is not a suite;
// the stale and unhealthy cases have to be shown to be reachable.
#include "reed/status_cache.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "reed/sysinfo.hpp"

namespace fs = std::filesystem;

static int failures = 0;

static void check(const std::string& what, bool ok) {
  std::printf("  %-56s %s\n", what.c_str(), ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

// Shift a published snapshot's timestamp, to reach ages a test cannot wait for.
static void shift_taken_at(const std::string& path, long long delta) {
  std::ostringstream ss;
  {
    std::ifstream f(path);
    ss << f.rdbuf();
  }
  std::string j = ss.str();
  const std::string key = "\"taken_at\":";
  const size_t at = j.find(key);
  if (at == std::string::npos) return;
  const size_t b = at + key.size();
  const size_t e = j.find_first_of(",}", b);
  if (e == std::string::npos) return;
  const long long t = std::atoll(j.substr(b, e - b).c_str());
  j = j.substr(0, b) + std::to_string(t + delta) + j.substr(e);
  std::ofstream out(path, std::ios::trunc);
  out << j;
}

int main() {
  const fs::path tmp = fs::temp_directory_path() / "reed-cache-test";
  fs::remove_all(tmp);
  fs::create_directories(tmp);
  setenv("XDG_STATE_HOME", tmp.c_str(), 1);

  // A runtime dir that is deliberately NOT where the file should go. The header
  // once promised XDG_RUNTIME_DIR and the code never used it; writer and reader
  // would have resolved different paths, each working perfectly, separately.
  const fs::path runtime = tmp / "runtime";
  fs::create_directories(runtime);
  setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);

  const std::string path = reed::StatusCache::path();
  check("path is under the state dir",
        path.rfind((tmp / "reed-tpse").string(), 0) == 0);
  check("XDG_RUNTIME_DIR is set (control for the above)",
        getenv("XDG_RUNTIME_DIR") != nullptr);

  reed::DeviceStatus st;
  st.fan_lcd = "2010";
  st.turbo_pump = "2910";
  st.available_storage = 2.85e9;
  st.warnings.push_back({"No ERROR", "Fan LCD"});

  reed::DeviceInfo in;
  in.product_id = "cm01";
  in.os = "Android";
  in.serial = "test-serial";
  in.app_version = "1.4";
  in.firmware = "V1.0.11";
  in.hardware = "V1.1";
  in.attributes = {"Status", "Fan LCD|rw"};

  check("publish succeeds", reed::StatusCache::publish(st, in));
  check("file exists", fs::exists(path));

  auto snap = reed::StatusCache::read();
  check("read returns a snapshot", snap.has_value());
  if (snap) {
    check("fan RPM round-trips", snap->status.fan_lcd == "2010");
    check("pump RPM round-trips", snap->status.turbo_pump == "2910");
    check("storage round-trips", snap->status.available_storage == 2.85e9);
    check("warning round-trips",
          snap->status.warnings.size() == 1 &&
              snap->status.warnings[0].description == "No ERROR" &&
              snap->status.warnings[0].type == "Fan LCD");
    check("firmware round-trips", snap->info.firmware == "V1.0.11");
    check("attributes round-trip", snap->info.attributes.size() == 2);
    check("fresh snapshot is not stale", !snap->stale());
    check("healthy on 'No ERROR'", snap->status.healthy());
  }

  // Negative control: a real fault must come back unhealthy, or the exit code
  // `status` reports from the cache means nothing.
  {
    reed::DeviceStatus bad = st;
    bad.warnings.clear();
    bad.warnings.push_back({"Fan blocked", "Fan LCD"});
    reed::StatusCache::publish(bad, in);
    auto s = reed::StatusCache::read();
    check("NEGATIVE: a real fault reads as unhealthy",
          s && !s->status.healthy());
  }

  // Negative control: staleness has to be reachable.
  {
    reed::StatusCache::publish(st, in);
    shift_taken_at(path, -(reed::StatusSnapshot::kStaleAfterSeconds + 60));
    auto s = reed::StatusCache::read();
    check("NEGATIVE: an old snapshot reads as stale", s && s->stale());
    check("...and reports an age past the bound",
          s && s->age_seconds() > reed::StatusSnapshot::kStaleAfterSeconds);
    check("stale_at agrees with stale() at the same age",
          s && reed::StatusSnapshot::stale_at(s->age_seconds()) == s->stale());
  }

  check("stale_at is false exactly at the bound",
        !reed::StatusSnapshot::stale_at(
            reed::StatusSnapshot::kStaleAfterSeconds));
  check("stale_at is true one second past it",
        reed::StatusSnapshot::stale_at(
            reed::StatusSnapshot::kStaleAfterSeconds + 1));

  // A clock stepping backwards must not produce a negative age, which would
  // read as a snapshot from the future and never go stale.
  {
    reed::StatusCache::publish(st, in);
    shift_taken_at(path, 100000);
    auto s = reed::StatusCache::read();
    check("future-dated snapshot clamps to age 0", s && s->age_seconds() == 0);
    check("...and is not stale", s && !s->stale());
  }

  // Malformed input must be an absence, never a crash or a half-read snapshot.
  {
    std::ofstream(path, std::ios::trunc) << "{\"taken_at\": 1, TRUNCATED";
    check("malformed file reads as nothing",
          !reed::StatusCache::read().has_value());
  }

  check("clear succeeds", reed::StatusCache::clear());
  check("file is gone", !fs::exists(path));
  check("read after clear returns nothing",
        !reed::StatusCache::read().has_value());
  check("clear on an absent file is still success",
        reed::StatusCache::clear());

  // Network sampling. The first sample primes the counters and must report
  // zero rather than the machine's traffic since boot.
  //
  // Not covered: the negative-delta path, for a wrapped counter or an
  // interface that disappeared. Reaching it needs control over /proc/net/dev,
  // which this suite deliberately does not fake.
  {
    reed::SystemMonitor mon;
    const reed::SystemMetrics first = mon.sample();
    check("first network sample is zero",
          first.network.download_kbps == 0 && first.network.upload_kbps == 0);
    const reed::SystemMetrics second = mon.sample();
    check("second sample is never negative",
          second.network.download_kbps >= 0 && second.network.upload_kbps >= 0);
  }

  fs::remove_all(tmp);
  std::printf("%s\n", failures ? "FAILURES" : "all checks passed");
  return failures != 0;
}
