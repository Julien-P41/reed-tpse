#pragma once

// The HUD metric vocabulary: one table, four consumers.
//
// These labels are protocol, not presentation. The firmware matches them
// exactly, renders the ones it recognises and silently drops the rest, so a
// typo produces an overlay slot that stays blank with no error anywhere.
//
// The same fifteen-odd strings were written out four times: the CLI's accept
// list, the CLI's label-to-host-value mapping, the library's label-to-PcInfo
// mapping, and the --help text. They agreed, but only by hand, and the file
// holding the accept list claimed in its own header comment that keeping it
// internal "stops the label table becoming a second source of truth" -- with
// three others live in the tree. Adding a metric meant four edits, and missing
// one of them fails silently in the direction this project has been burned by
// most: a command that returns 200 and does nothing.
//
// Everything about a metric now lives on one row: what the firmware calls it,
// where its value goes in the PcInfo blob, how to read it from this host, and
// how to format it.

#include <optional>
#include <string>
#include <vector>

#include "reed/sysinfo.hpp"

namespace reed {

// How the value is written into PcInfo. GPU temperature is a string there,
// unlike every other temperature -- captured vendor traffic, not a guess.
enum class PcInfoType { Number, String };

struct HudMetric {
  const char* label;  // exact firmware string

  // Where the value lands in the PcInfo blob. `pc_object` is null for a metric
  // the host does not supply -- Date&Time is drawn from the device's own clock.
  const char* pc_object;
  const char* pc_field;
  PcInfoType pc_type;

  // Host-side presentation.
  const char* unit;
  int precision;

  // Read it from this machine. Null for a metric with no host value at all;
  // nullopt for one this particular machine cannot source, which is the
  // difference between "no sensor here" and "genuinely zero" that the CLI
  // warns about rather than sending a silent 0.
  std::optional<double> (*read)(const SystemMetrics&);
};

// Every metric the firmware knows, in the order --help lists them.
const std::vector<HudMetric>& hud_metrics();

// Null when the label is not one the firmware knows.
const HudMetric* find_hud_metric(const std::string& label);

}  // namespace reed
