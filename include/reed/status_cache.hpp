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
};

class StatusCache {
 public:
  // Under XDG_RUNTIME_DIR when set -- tmpfs, cleared at logout, which is the
  // right lifetime for something describing a device that may be unplugged.
  // Falls back to the state directory when it is not.
  static std::string path();

  static bool publish(const DeviceStatus& status, const DeviceInfo& info);
  static std::optional<StatusSnapshot> read();
};

}  // namespace reed
