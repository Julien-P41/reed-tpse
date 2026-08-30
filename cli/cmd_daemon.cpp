// The daemon: the process that holds the serial port and keeps the device
// showing what the state file says.
//
// Moved out whole and deliberately not decomposed. cmd_daemon_start's lambdas
// capture each other in declaration order -- screen_config, then
// rebuild_screen_config, then the device, then wait_for_ui, then restore, then
// reconnect -- and that order is load-bearing rather than stylistic.
//
// The helpers below stay internal: reading the host's lock and power state is
// the daemon's own business, and nothing else asks.

#include "cli_common.hpp"
#include "cli_commands.hpp"

#include <chrono>
#include <fcntl.h>
#include <pwd.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "reed/adb.hpp"
#include "reed/config.hpp"
#include "reed/device.hpp"
#include "reed/mapping.hpp"
#include "reed/status_cache.hpp"
#include "reed/sysinfo.hpp"
#include "reed/wire.hpp"

// Capture a command's stdout. Fixed argv, no shell -- same reasoning as the
// adb wrapper.
static std::string run_capture(const std::vector<std::string>& args) {
  int fds[2];
  if (pipe(fds) != 0) return {};
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return {};
  }
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, STDERR_FILENO);
    close(fds[1]);
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(fds[1]);
  std::string out;
  char buf[1024];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, n);
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return out;
}

static std::string trim_copy(std::string v) {
  while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
    v.pop_back();
  return v;
}

// The session's lock state via logind. Returns nullopt when it cannot be
// determined -- a system-scope daemon has no session of its own, so the
// session is located by user name rather than with `self`.
static std::optional<bool> session_locked() {
  const char* user = std::getenv("USER");
  std::string name = user ? user : "";
  if (name.empty()) {
    if (struct passwd* pw = getpwuid(getuid())) name = pw->pw_name;
  }
  if (name.empty()) return std::nullopt;

  const std::string sessions = run_capture({"loginctl", "list-sessions", "--no-legend"});
  std::istringstream ss(sessions);
  std::string line, sid;
  while (std::getline(ss, line)) {
    std::istringstream ls(line);
    std::string id, uid, who;
    if (ls >> id >> uid >> who && who == name) {
      sid = id;
      break;
    }
  }
  if (sid.empty()) return std::nullopt;

  const std::string hint =
      trim_copy(run_capture({"loginctl", "show-session", sid, "-p", "LockedHint", "--value"}));
  if (hint == "yes") return true;
  if (hint == "no") return false;
  return std::nullopt;
}

// True when running on battery. A machine with no power-supply class at all
// (an ordinary desktop) counts as mains.
static std::optional<bool> on_battery() {
  namespace fs = std::filesystem;
  std::error_code ec;
  const std::string root = "/sys/class/power_supply";
  if (!fs::exists(root, ec)) return false;
  bool saw_mains = false, mains_online = false;
  for (const auto& e : fs::directory_iterator(root, ec)) {
    if (ec) break;
    std::ifstream tf(e.path() / "type");
    std::string type;
    if (!(tf >> type) || type != "Mains") continue;
    saw_mains = true;
    std::ifstream of(e.path() / "online");
    int online = 0;
    if (of >> online && online == 1) mains_online = true;
  }
  if (!saw_mains) return false;
  return !mains_online;
}

// The unit ships in two mutually exclusive scopes; never address both, or two
// daemons end up contending for the same serial port.
static std::string systemctl(bool system_scope) {
  return system_scope ? "systemctl" : "systemctl --user";
}

// First run has no saved state. Rather than refuse, seed the playlist from
// whatever media is already on the device. Idea taken from
// xiaotinglian/reed-tpse (bootstrap_display_state).
static std::optional<reed::DisplayState> bootstrap_display_state() {
  if (!reed::Adb::is_device_connected()) return std::nullopt;
  auto media = reed::Adb::list_media();
  if (!media || media->empty()) return std::nullopt;

  // Brightness is left at DisplayState's own default -- there is no
  // config-level brightness to seed it from any more.
  reed::DisplayState state;
  state.media = *media;
  if (!reed::ConfigManager::save_state(state)) {
    // Not fatal: the daemon can run from this state in memory. But it will
    // re-bootstrap on every start until the write succeeds, so say so once.
    std::cerr << "warning: could not save the bootstrapped playlist to "
              << reed::ConfigManager::get_state_path() << "\n";
  }
  return state;
}

int cmd_daemon_start(const std::string& port, bool foreground,
                            bool system_scope, bool verbose) {
  if (!foreground) {
    // Neither --port nor --verbose reaches a unit started through systemd:
    // the unit file fixes the command line, and this process only asks
    // systemctl to start it. Silently accepting them looked like they applied.
    if (!port.empty()) {
      std::cerr << "note: --port is ignored when starting through systemd -- "
                   "the unit\n"
                   "      runs its own command line. Set \"port\" in "
                   "config.json instead,\n"
                   "      or use --foreground.\n";
    }
    if (verbose) {
      std::cerr << "note: --verbose applies to the daemon process, not to "
                   "systemctl; use\n"
                   "      --foreground to see it, or `journalctl -u "
                   "reed-tpse.service`.\n";
    }
    const std::string sc = systemctl(system_scope);
    // Best effort, and the result is deliberately not acted on: `enable` fails
    // legitimately when the unit is already enabled or not installed, and the
    // `start` below is what decides the outcome. Consumed explicitly because
    // std::system is warn_unused_result -- a warning that only appears once
    // optimisation is on, which until now no documented build turned on.
    const int enable_rc =
        std::system((sc + " enable reed-tpse.service 2>/dev/null").c_str());
    (void)enable_rc;
    int ret = std::system((sc + " start reed-tpse.service 2>/dev/null").c_str());

    if (ret == 0) {
      std::cout << "Daemon started via systemd ("
                << (system_scope ? "system" : "user") << " scope).\n";
      std::cout << "Check status: reed-tpse daemon status"
                << (system_scope ? " --system" : "") << "\n";
      return 0;
    } else {
      std::cerr << "systemd service not installed in "
                << (system_scope ? "system" : "user")
                << " scope. Run with --foreground, install the unit, or try "
                << (system_scope ? "without --system" : "--system") << ".\n";
      return 1;
    }
  }

  // Foreground daemon mode
  auto config = reed::ConfigManager::load_config();

  reed::LoadStatus state_status = reed::LoadStatus::Ok;
  auto state = reed::ConfigManager::load_state(&state_status);
  if (!state && state_status != reed::LoadStatus::Missing) {
    // Starting from defaults here would push them to the device and, on the
    // next state write, persist them over whatever the file actually held.
    std::cerr << "Refusing to start: " << reed::ConfigManager::get_state_path()
              << " could not be read.\n"
              << "  Fix or delete it, then start again.\n";
    return 1;
  }
  if (!state) {
    state = bootstrap_display_state();
  }
  if (!state) {
    // Still nothing: carry on anyway. Exiting non-zero here put the unit into a
    // 5s Restart=on-failure loop on every fresh install, and the daemon is
    // useful without media -- it is what stops the panel reverting to firmware
    // content, and it applies the fan and sleep settings.
    std::cerr << "No saved display state; running keepalive only. "
                 "Set content with `reed-tpse display <file>` or "
                 "`reed-tpse preset <name>`.\n";
    state = reed::DisplayState{};
  }

  std::string actual_port =
      (config && !config->port.empty()) ? config->port : port;
  int keepalive_interval = config ? config->keepalive_interval : 10;
  // A zero or negative interval spins the loop; an absurd one is a typo. The
  // device reverts after ~60s without a handshake, so cap well under that.
  if (keepalive_interval < 1 || keepalive_interval > 55) {
    std::cerr << "keepalive_interval " << keepalive_interval
              << " out of range (1-55), using 10\n";
    keepalive_interval = 10;
  }
  if (state->hud.push_interval_sec < 1 || state->hud.push_interval_sec > 3600) {
    std::cerr << "hud push interval " << state->hud.push_interval_sec
              << " out of range (1-3600), using 5\n";
    state->hud.push_interval_sec = 5;
  }

  // Only the foreground daemon needs the device, so it does its own detection
  // rather than making every `daemon` subcommand depend on a free port.
  if (actual_port.empty()) {
    auto detected = reed::Device::find_device(verbose);
    if (!detected) {
      std::cerr
          << "No device found. Specify port with -p or check connection.\n";
      return 1;
    }
    actual_port = *detected;
  }

  // Point adb at the same physical cooler as the serial port we settled on.
  reed::Adb::bind_to_port(actual_port);

  bool report_ac = !config || config->report_ac_power;
  bool report_lock = !config || config->report_lock;
  bool report_shutdown = !config || config->report_shutdown;
  std::optional<bool> last_locked;

  reed::ScreenConfig screen_config;
  auto rebuild_screen_config = [&]() {
    screen_config = screen_config_from(*state);
  };

  rebuild_screen_config();

  auto device = std::make_unique<reed::Device>(actual_port, verbose);
  if (!device->connect()) {
    std::cerr << "Failed to connect to " << actual_port << "\n";
    return 1;
  }

  // The device answers on serial before its UI app is up -- adbd and the
  // serial link come back roughly 12s ahead of it after a restart. Settings
  // applied into that window are accepted and dropped: the panel stays black
  // and the fan keeps whatever the firmware defaulted to.
  //
  // Waiting for the UI beats guessing at a delay. When adb is unavailable
  // there is nothing to wait on, so the timed re-apply further down stays as
  // the fallback.
  auto wait_for_ui = [&](int timeout_sec) {
    if (!reed::Adb::is_device_connected()) return;
    for (int waited = 0; waited < timeout_sec; ++waited) {
      const auto ready = reed::Adb::ui_ready();
      if (!ready) {
        // adb cannot answer. Waiting changes nothing, so apply now rather than
        // stalling the whole restore for a question that will not be answered.
        if (verbose) {
          std::cerr << "adb cannot report the device UI; applying without "
                       "waiting\n";
        }
        return;
      }
      if (*ready) {
        if (waited && verbose) {
          std::cerr << "waited " << waited << "s for the device UI\n";
        }
        return;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cerr << "warning: device UI still not up after " << timeout_sec
              << "s; applying anyway\n";
  };

  // Everything below is deliberately best-effort: the individual sends are not
  // checked, because a per-command reply is not a reliable health signal here.
  // Replies cannot be correlated to requests (AckNumber is the device's own
  // counter, not an echo), and some commands are answered inconsistently. What
  // does tell us the link is broken is the handshake, and the loop below
  // reconnects on that. Checking each send would add branches that could only
  // repeat what the next handshake already reports.
  reed::DeviceInfo device_info;

  auto restore = [&](reed::Device& dev) {
    auto info = dev.handshake();
    if (!info) return false;
    device_info = *info;
    wait_for_ui(45);

    // One `config` frame carries temperature unit, panel power, sleep mode,
    // brightness, the whole screen block and the fan -- the vendor's own
    // post-connect apply. Doing it in one write closes the window where the
    // device was running half our settings and half the firmware defaults,
    // which is what the 20s re-apply below was working around.
    //
    // Everything here is volatile on the device: it lives in controller RAM
    // and is lost whenever USB power drops, which it does at S5. Hence the
    // re-apply on every connect rather than once at install time.
    reed::FullConfig full;
    full.temperature_unit = state->hud.temperature_unit;
    if (state->screen_on) full.screen_enable = *state->screen_on;
    if (state->display_in_sleep) full.display_in_sleep = *state->display_in_sleep;
    full.brightness = state->brightness;
    full.cpu_name = state->hud.cpu_name;
    full.gpu_name = state->hud.gpu_name;
    if (state->fan_tier) {
      const FanTier* t = lookup_fan_tier(*state->fan_tier);
      if (t) full.fan_curve = t->curve;
      full.fan_mode = state->fan_duty ? reed::wire::kFanFixed : reed::wire::kFanSmart;
      full.fan_fixed = state->fan_duty ? *state->fan_duty : (t ? t->duty : 40);
    }
    // `rotate` is left unset on purpose -- see FullConfig::rotate.
    dev.send_config(full, screen_config);

    // Brightness goes out again explicitly. Whether `config` honours its own
    // brightness field could not be settled: the device logs
    // `--onDoBrightness--100` for every value (an explicit {"value":40}
    // logs 100 too, so the line does not echo the request), and the backlight
    // sysfs needs root to read. One extra frame is cheaper than the doubt.
    dev.set_brightness(state->brightness);
    if (report_ac) {
      // Tell the device where the host stands as soon as we are talking to it.
      if (auto batt = on_battery()) {
        dev.send_power_event(*batt ? "on-battery" : "ac-power");
      }
    }
    if (report_lock) {
      // Lock state is applied after the media below, so that a locked session
      // does not get the normal media painted over its lock screen.
      if (auto locked = session_locked()) {
        if (*locked) {
          // With lock_media configured the media itself is the lock screen,
          // so the event would only duplicate it.
          if (!(config && config->lock_media)) {
            dev.send_power_event("lock-screen");
          }
        } else {
          // Always sent, even with lock_media configured. This daemon sends
          // `shutdown` when it exits, and standby is sticky: nothing but
          // unlock-screen/resume hides it again. Skipping this left the panel
          // on the standby loop across a daemon restart, with the media
          // applied underneath and every re-apply looking like it did
          // nothing.
          dev.send_power_event("unlock-screen");
        }
      }
    }
    // Two things `config` cannot express, both sent separately afterwards.
    //
    // A preset, because the `id` block inside `config` is built for custom
    // media.
    if (state->preset) {
      dev.set_preset(reed::wire::kPresetPrefix + *state->preset, screen_config.settings,
                     screen_config.sysinfo_display);
    }
    // And a split screen. The device parses the two-zone form inside `config`
    // -- the log shows both media entries and both settings blocks arriving --
    // but only ever calls setLayout1Path, leaving one half on the previous
    // media and the other on the standby loop. The same payload sent as its
    // own waterBlockScreenId drives both zones correctly.
    if (screen_config.split) {
      dev.set_screen_config(screen_config);
    }

    // Reconnecting during a locked session must land on the lock screen, not
    // on the normal media. The loop only sees *transitions*, so without this a
    // daemon restart while locked would show the desktop media until the next
    // unlock.
    if (report_lock && config && config->lock_media) {
      if (auto locked = session_locked(); locked && *locked) {
        reed::ScreenConfig lock_cfg = screen_config;
        lock_cfg.media = {*config->lock_media};
        lock_cfg.sysinfo_display.clear();
        dev.set_screen_config(lock_cfg);
        dev.set_brightness(config->lock_brightness);
      }
    }
    return true;
  };

  // Before the first restore, not after. restore() calls wait_for_ui(45), so
  // a SIGTERM arriving during startup used to hit the default disposition and
  // kill the process outright -- no `shutdown` event, no snapshot cleanup.
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  const bool restored_at_startup = restore(*device);

  if (state->hud.enabled) {
    std::cout << "Display restored with HUD (" << state->hud.metrics.size()
              << " metric" << (state->hud.metrics.size() == 1 ? "" : "s")
              << ", push every " << state->hud.push_interval_sec << "s).\n";
  } else if (state->preset) {
    std::cout << "Preset restored (" << *state->preset
              << "). Running keepalive...\n";
  } else if (screen_config.media.empty()) {
    // Nothing was restored -- saying otherwise sent me chasing a phantom.
    std::cout << "Running keepalive only (no media configured).\n";
  } else {
    std::cout << "Display restored. Running keepalive...\n";
  }

  reed::SystemMonitor monitor;
  bool first_handshake_ok = false;

  // Reconnect after a handshake failure: the serial fd can die silently on USB
  // suspend/resume or when the device renumbers (/dev/ttyACM0 -> ttyACM1). Try
  // the current port first, then rescan.
  auto reconnect = [&]() -> bool {
    device->disconnect();
    if (device->connect() && restore(*device)) {
      std::cerr << "keepalive: reconnected on " << device->port() << "\n";
      return true;
    }
    // Release the port before scanning. connect() may have succeeded above
    // with only restore() failing, in which case we still hold the tty --
    // and TIOCEXCL makes find_device()'s open() of our own port fail with
    // EBUSY, so the rescan could never find the device it is sitting on.
    device->disconnect();

    std::cerr << "keepalive: scanning for device...\n";
    auto found = reed::Device::find_device(verbose);
    if (!found) {
      std::cerr << "keepalive: no device found\n";
      return false;
    }
    device = std::make_unique<reed::Device>(*found, verbose);
    // Re-pair adb with the tty we actually landed on. A rescan can return a
    // different port than the one bound at startup -- that is what the whole
    // reconnect path exists for -- and without this adb keeps driving whatever
    // it resolved first.
    reed::Adb::bind_to_port(*found);
    if (device->connect() && restore(*device)) {
      std::cerr << "keepalive: reconnected on " << *found << "\n";
      return true;
    }
    std::cerr << "keepalive: found " << *found << " but handshake failed\n";
    return false;
  };

  using clock = std::chrono::steady_clock;
  auto now = clock::now();
  auto next_handshake = now + std::chrono::seconds(keepalive_interval);
  // Telemetry is needed for the HUD *and* for a fixed fan duty -- the device
  // only honours the fan profile while host data keeps arriving, and reverts
  // to 100% when it stops. Schedule pushes if either wants them.
  bool push_telemetry = state->hud.enabled || state->fan_duty.has_value() ||
                        (state->hud_right && state->hud_right->enabled);
  auto next_sysinfo =
      push_telemetry
          ? now + std::chrono::seconds(state->hud.push_interval_sec)
          : clock::time_point::max();

  // Settings can change while the daemon runs -- `lock-display` needs no
  // serial port, so it can be edited with the daemon up, and a startup
  // snapshot then silently serves stale values. Reload when either file's
  // mtime moves.
  auto mtime_of = [](const std::string& path) -> long long {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<long long>(t.time_since_epoch().count());
  };
  const std::string cfg_path = reed::ConfigManager::get_config_path();
  const std::string state_path = reed::ConfigManager::get_state_path();
  long long cfg_seen = mtime_of(cfg_path);
  long long state_seen = mtime_of(state_path);

  // One late re-apply, always. The readiness wait above covers the case it was
  // written for, but not every way a startup apply can fail to stick -- a
  // daemon start has been observed leaving the panel on the standby loop with
  // correct state on disk and no further `config` frame until something
  // touched the state file. Making this conditional on adb removed the only
  // net under that, and it costs one frame per daemon start.
  auto reapply_at = clock::now() + std::chrono::seconds(25);
  bool reapplied = false;

  // The lock poll runs on the keepalive cadence, not on the loop's 1s tick.
  // Starting at `now` makes the first check immediate.
  auto next_lock_check = now;

  // When the snapshot was last written, so the two publishers do not duplicate
  // each other's work. Unset means "never", which makes the first tick publish.
  std::optional<clock::time_point> last_publish;

  // Seed it before the loop starts.
  //
  // Neither publisher below runs until its first deadline -- a keepalive tick
  // at the earliest -- and this process deletes the file on the way out. So
  // without this, every restart left `status` and `info` with nothing to read
  // for up to a full keepalive interval, failing with "already open by PID
  // ..." exactly as they did before the cache existed. Measured: exit 1 for
  // the whole window, on a daemon that was up and healthy.
  //
  // Cheap to close: one `STATE all` at a point where the link has just been
  // proven by the handshake inside restore().
  if (restored_at_startup && device->is_connected()) {
    if (auto seen = device->get_status()) {
      if (reed::StatusCache::publish(*seen, device_info)) {
        last_publish = clock::now();
      }
    }
  }

  int failures = 0;
  while (g_running) {
    if (!reapplied && clock::now() >= reapply_at && device->is_connected()) {
      reapplied = true;
      restore(*device);
      if (verbose) std::cerr << "settings re-applied after startup\n";
    }

    const long long cfg_now = mtime_of(cfg_path);
    const long long state_now = mtime_of(state_path);
    if (cfg_now != cfg_seen || state_now != state_seen) {
      cfg_seen = cfg_now;
      state_seen = state_now;
      if (auto c = reed::ConfigManager::load_config()) config = c;
      if (auto st = reed::ConfigManager::load_state()) {
        state = st;
        rebuild_screen_config();
      }
      report_ac = !config || config->report_ac_power;
      report_lock = !config || config->report_lock;
      report_shutdown = !config || config->report_shutdown;
      push_telemetry = state->hud.enabled || state->fan_duty.has_value() ||
                       (state->hud_right && state->hud_right->enabled);
      // Reloading without applying left the daemon holding settings it never
      // sent: any CLI change made while the daemon runs cannot touch the
      // device itself -- the port is exclusive -- so it would sit unapplied
      // until the next reconnect.
      if (device && device->is_connected()) restore(*device);
      if (verbose) std::cerr << "settings reloaded and applied\n";
    }

    // Tick once a second so we're responsive to both cadences and SIGTERM.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!g_running) break;

    now = clock::now();

    if (now >= next_handshake) {
      next_handshake = now + std::chrono::seconds(keepalive_interval);
      if (auto info = device->handshake()) {
        failures = 0;
        first_handshake_ok = true;
        device_info = *info;

        // Publish from here when nothing else will.
        //
        // The telemetry push below is the cheaper publisher -- it reads the
        // status off a frame it was sending anyway -- but it only runs when
        // the HUD or a fixed fan duty asked for telemetry. A daemon showing a
        // preset with no overlay pushes nothing, so `status` and `info` found
        // no snapshot and fell through to a live read that cannot succeed
        // while this process holds the port. That is the exact failure the
        // cache exists to remove, in the configuration it is most likely to
        // be in.
        //
        // `STATE all` with an empty body is the same read `status` performs
        // when no daemon is running. Unlike `conn` it does not re-initialise
        // the panel, so this costs one frame and nothing visible.
        //
        // Gated on the snapshot's age rather than on `push_telemetry`, so
        // neither publisher can leave a gap. With the HUD on at its 5s
        // default the push below always gets there first and this never
        // fires; with the HUD off, or its interval set long -- it is clamped
        // at 3600s -- this is what keeps the file inside
        // StatusSnapshot::kStaleAfterSeconds instead of letting a healthy
        // device read as stale between pushes.
        //
        // Not a strict either/or: when the two cadences coincide (a push
        // interval that is a multiple of the keepalive), this fires and the
        // push publishes again a moment later in the same tick. That costs
        // one redundant `STATE all` at the coincidence, which is not worth a
        // second piece of state to avoid.
        if (!last_publish ||
            now - *last_publish >= std::chrono::seconds(keepalive_interval)) {
          if (auto seen = device->get_status()) {
            if (reed::StatusCache::publish(*seen, device_info)) {
              last_publish = now;
            }
          }
        }
      } else {
        ++failures;
        std::cerr << "keepalive: handshake failed (#" << failures
                  << "), reconnecting...\n";
        if (reconnect()) {
          failures = 0;
          first_handshake_ok = true;
        }
      }
    }

    if (report_lock && now >= next_lock_check) {
      next_lock_check = now + std::chrono::seconds(keepalive_interval);
      // On the keepalive cadence, which is what the comment here always
      // claimed. It was not: this sat bare in the loop body, whose only pacing
      // is the 1s sleep, so every other periodic action was behind a deadline
      // and this one ran every second -- two loginctl forks a second, roughly
      // 170k processes a day, to answer a question that changes a handful of
      // times.
      //
      // The cost is latency: a lock or unlock is now noticed within
      // keepalive_interval rather than within a second. For a decorative panel
      // that is a fair trade; if it ever is not, cache the answer and re-run
      // loginctl on the deadline rather than reverting to a 1s poll.
      //
      // Polling at all avoids needing a session bus -- a system-scope daemon
      // has no session of its own.
      if (auto locked = session_locked()) {
        if (!last_locked || *last_locked != *locked) {
          if (last_locked && device->is_connected()) {
            if (config && config->lock_media) {
              // A custom lock screen replaces the firmware standby rather than
              // layering on it: sending the power event and then setting media
              // would immediately wake the panel again (hindStandby).
              if (*locked) {
                reed::ScreenConfig lock_cfg = screen_config;
                lock_cfg.media = {*config->lock_media};
                lock_cfg.sysinfo_display.clear();
                device->set_screen_config(lock_cfg);
                device->set_brightness(config->lock_brightness);
              } else {
                device->set_screen_config(screen_config);
                device->set_brightness(state->brightness);
              }
            } else {
              device->send_power_event(*locked ? "lock-screen"
                                               : "unlock-screen");
            }
            if (verbose) {
              std::cerr << "power: session "
                        << (*locked ? "locked" : "unlocked")
                        << ((config && config->lock_media) ? " (custom lock media)" : "")
                        << "\n";
            }
          }
          last_locked = *locked;
        }
      }
    }

    if (push_telemetry && now >= next_sysinfo) {
      next_sysinfo = now + std::chrono::seconds(state->hud.push_interval_sec);
      // Gate first push on a successful handshake — otherwise startup issues
      // surface as "HUD shows zeros" instead of "device didn't respond".
      if (first_handshake_ok && device->is_connected()) {
        auto metrics = monitor.sample();
        // Both zones' metrics, or a split screen shows 0 in the half whose
        // labels were never pushed -- the device only fills values it was
        // given, and the overlay renders the label regardless.
        std::vector<std::string> labels = state->hud.metrics;
        if (state->hud_right) {
          for (const auto& m : state->hud_right->metrics) {
            if (std::find(labels.begin(), labels.end(), m) == labels.end()) {
              labels.push_back(m);
            }
          }
        }
        // Read the reply rather than discarding it: `STATE all` answers with
        // the device's status, so publishing it costs no extra round trip and
        // lets `status` and `info` work while the daemon holds the port.
        if (auto seen = device->push_sysinfo(build_sysinfo(labels, metrics),
                                              metrics.network)) {
          if (reed::StatusCache::publish(*seen, device_info)) {
            last_publish = now;
          }
        }
      }
    }
  }

  // Loop exited, so we were asked to stop -- which during a host shutdown is
  // the shutdown itself. Say so: with sleep-display enabled the panel blanks
  // immediately instead of waiting out the ~60s keepalive timeout.
  if (report_shutdown && device && device->is_connected()) {
    device->send_power_event("shutdown");
    if (verbose) std::cerr << "power: sent shutdown on exit\n";
  }

  // Drop the snapshot on the way out. It describes a device this process was
  // talking to, and once it stops there is nothing keeping it current. Leaving
  // it behind matters most across a restart: the new daemon holds the port
  // immediately, so `status` would answer from the previous run's file during
  // the window before the first publish -- pre-restart data presented as
  // current, which is worse than no answer.
  //
  // Only the clean exit is covered. A kill -9 leaves the file, which is what
  // StatusSnapshot::stale() is for.
  reed::StatusCache::clear();

  return 0;
}

int cmd_daemon_stop(bool system_scope) {
  int ret = std::system(
      (systemctl(system_scope) + " stop reed-tpse.service 2>/dev/null")
          .c_str());
  if (ret == 0) {
    std::cout << "Daemon stopped.\n";
    return 0;
  } else {
    std::cerr << "Failed to stop daemon (or not running).\n";
    return 1;
  }
}

int cmd_daemon_status(bool system_scope) {
  int ret = std::system(
      (systemctl(system_scope) + " status reed-tpse.service 2>/dev/null")
          .c_str());
  return ret == 0 ? 0 : 1;
}
