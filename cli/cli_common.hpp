#pragma once

// Everything that crosses a CLI translation-unit boundary.
//
// Deliberately small and deliberately the only such header: keeping the whole
// shared surface in one place is what makes it auditable at a glance. An
// earlier attempt at this split moved the daemon out first and missed helpers
// it depended on, which is the failure this file exists to prevent.
//
// Command prototypes live in cli_commands.hpp instead -- that is main()'s
// view of the world, and it has no business here.

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include "reed/config.hpp"
#include "reed/device.hpp"
#include "reed/sysinfo.hpp"

// Cleared by SIGINT/SIGTERM. DECLARATION only -- the single definition lives
// in cli_common.cpp. Spelling this `static` in a header would give every
// translation unit its own object: it would compile, link and run, and the
// daemon would simply never notice the signal that stops it.
extern std::atomic<bool> g_running;
void signal_handler(int sig);

// Telemetry values for the given firmware metric labels.
std::vector<reed::SysinfoData> build_sysinfo(
    const std::vector<std::string>& labels, const reed::SystemMetrics& m);

// Load the state a command is about to modify and save back. Returns nullopt
// after printing a diagnostic when the file exists but cannot be parsed --
// saving over it would persist defaults across everything it held.
std::optional<reed::DisplayState> load_state_for_update();

// Is the port held by a running daemon, as opposed to something else that
// merely has it open? Only a daemon will apply a saved state, so only a daemon
// justifies telling the user their change will be applied.
bool daemon_holds_port(const std::string& port);
int defer_to_daemon(const std::string& what);

// A named LCD-fan tier: the vendor pairs a fixed duty with a specific curve,
// so a tier is the pair rather than just a percentage. Shared because the
// daemon re-applies the saved tier on every connect, and looks it up by the
// wire name stored in the state file.
struct FanTier {
  const char* alias;
  const char* wire;
  int duty;
  reed::FanCurve curve;
};
const FanTier* lookup_fan_tier(const std::string& in);
