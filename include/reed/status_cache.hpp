#pragma once

// The daemon's last look at the device, published where the CLI can read it.
//
// The daemon holds the serial port exclusively, so `status` and `info` -- both
// pure reads -- could not run at all while it was up, which is most of the
// time. Failing to answer a read because something else is reading is a poor
// trade, and the daemon already has the answer: its telemetry push is a
// `STATE all`, and the device replies to that exchange with exactly the status
// `status` prints.
//
// So the daemon writes what it saw, and the CLI reads that instead of the
// port. The snapshot carries the time it was taken; a reader that cares about
// freshness can say how old it is rather than pretending it is live.
//
// This is a cache, not a channel: nothing is sent back, and a stale or missing
// file is never an error, only an absence.

#include <optional>
#include <string>

#include "reed/device.hpp"

namespace reed {

struct StatusSnapshot {
  DeviceStatus status;
  DeviceInfo info;
  long long unix_seconds = 0;  // when the daemon took it

  // Seconds since it was taken, from the caller's clock.
  long long age_seconds() const;

  // Past this, the snapshot is not evidence about the device any more: it
  // says what the daemon last saw, not what is true now.
  //
  // The daemon refreshes the snapshot at least once per keepalive interval,
  // and that interval is clamped to 55s at the top end. 180s therefore covers
  // three consecutive failures at the slowest legal cadence, with margin --
  // and eighteen of them at the default 10s. The bound has to hold for the
  // worst case, so it is loose for the common one.
  //
  // Reaching it means the daemon is up -- readers only consult this file when
  // it holds the port -- and has stopped getting answers, which is worth
  // reporting rather than papering over with old numbers.
  static constexpr long long kStaleAfterSeconds = 180;

  bool stale() const { return age_seconds() > kStaleAfterSeconds; }

  // For a caller that reports the age and the staleness together. stale() and
  // age_seconds() each read the clock, so two calls can straddle a second
  // boundary and emit `"ageSeconds":180,"stale":true` -- a JSON object that
  // contradicts itself by one second, in the two fields a machine reads.
  // Take the age once and pass it here instead.
  static bool stale_at(long long age) { return age > kStaleAfterSeconds; }
};

class StatusCache {
 public:
  // Beside the state file, NOT under XDG_RUNTIME_DIR.
  //
  // Runtime dir is the tidier home, but the writer and the reader have to
  // agree on it and they do not: a system-scope unit gets no XDG_RUNTIME_DIR
  // while an interactive shell has one, so the daemon published to the state
  // directory and the CLI looked in /run/user/<uid>. The state directory is
  // the one location both resolve identically. The full reasoning is in
  // src/status_cache.cpp.
  static std::string path();

  static bool publish(const DeviceStatus& status, const DeviceInfo& info);
  static std::optional<StatusSnapshot> read();

  // Remove the snapshot. Called by the daemon on a clean exit; a missing file
  // is success, since the postcondition is "no snapshot", not "one deleted".
  static bool clear();
};

}  // namespace reed
