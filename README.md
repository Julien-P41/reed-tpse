# reed-tpse

Linux CLI for the Tryx Panorama AIO cooler's LCD — reverse-engineered protocol,
not affiliated with Tryx.

https://github.com/user-attachments/assets/1bc87fa9-cde9-4fd5-ab35-a1a15152c467

## About this fork

A fork of [fadli0029/reed-tpse](https://github.com/fadli0029/reed-tpse). All
credit for the original protocol work — the 0x5A framing, the CDC-ACM + ADB
split, the keepalive — goes to [@fadli0029](https://github.com/fadli0029);
without it none of the below would exist. Please star
[the upstream repo](https://github.com/fadli0029/reed-tpse), not this one.

It also carries the telemetry-HUD and daemon-reconnect work from
[koconnorgit/reed-tpse](https://github.com/koconnorgit/reed-tpse), cherry-picked
with authorship intact.

What this fork adds on top:

- **`STATE` reads and a `status` command** — fan/pump RPM, the device's own
  health warning, free storage. The firmware has no `GET`; `STATE` is the read
  verb, and `STATE all` is the only endpoint that returns a body.
- **LCD fan speed control** (`fan`) — named tiers or any duty 0-100%. Needed two
  endpoints acting together, which is why it had been written off as impossible.
- **Sleep behaviour** (`sleep-display`) — choose what the panel shows once the
  host stops talking: the firmware's standby animation, or black.
- **Firmware presets** (`preset`) — the clips shipped in the device's own system
  image, no upload required.
- **`raw` passthrough** — any method/endpoint/body, so an undiscovered endpoint
  is reachable without a code change.
- **Exclusive port locking** and a system/user systemd unit split, fixing a
  packaging arrangement that let two daemons hold the same tty and corrupt each
  other's replies.
- **HUD metric coverage** completed against the firmware's label set, with a
  warning for any metric the host cannot actually source instead of a silent 0.
- **The vendor's own payloads**, recovered from captured KANALI 1.2.1 traffic
  rather than guessed: the real fan tiers and their temperature curves, the
  one-frame `config` apply the app sends after connecting, and the overlay
  command. Checked in the test suite against the captured bytes.
- **Panel power, Mirror Mode, media filters, playlists and Screen Splitting**
  with independent metrics and colour per zone.
- **Seven host power events**, including `suspend` and `resume`, with a
  systemd sleep hook to drive them.

Everything above was verified against real hardware (Panorama 360 ARGB,
firmware V1.0.11); where something is only inferred, the text says so.

The protocol reference lives in
[docs/vendor-protocol.md](docs/vendor-protocol.md), and the measured firmware
behaviour behind these features in
[docs/firmware-notes.md](docs/firmware-notes.md).

## Currently supported features

- Upload images, videos, and GIFs (auto-converts to MP4)
- Set display content and brightness
- Read device status: fan/pump RPM, health warnings, free storage
- LCD fan speed control (named tiers, any duty 0-100%, or Smart Mode curves)
- Panel power, Mirror Mode, Screen Splitting, media filters, playlists
- Choose what the panel shows when the host is off -- the firmware's standby
  animation or a black screen (`sleep-display`)
- Firmware presets, played from the device's own storage (`preset`)
- Host power events -- lock, unlock, shutdown, suspend, resume, AC and battery
  -- manually or mirrored automatically by the daemon (`power`)
- Raw protocol passthrough for reaching any endpoint
- List and delete media files on device
- On-device system telemetry overlay (CPU/GPU temp, usage, frequency, voltage, power; RAM; motherboard/disk temps; date & time), with a warning for any metric this host cannot source
- systemd service (user or system scope) for persistent display across reboots
- Exclusive port locking, so a second instance can't corrupt the first's replies
- Auto-detects device (scans /dev/ttyACM*)
- Minimal dependencies (picojson header-only)

## TODO

Open:

- [ ] `waterfallMode` -- the payload key is unknown. The handler does not
      validate, so it never names its field in an exception, and KANALI 1.2.1
      has no UI control for it, so there is nothing to capture. Needs a build
      of the vendor app that exposes the toggle.
- [ ] Custom overlay layouts. The firmware places metrics itself: three at
      most, mid-height, with only `align` (Left/Center/Right) under host
      control. Anything else means compositing frames host-side.

Done in this fork: `status`, `raw`, `fan` (vendor tiers, arbitrary duty and
Smart Mode curves), `screen`, `rotate`, `preset`, `sleep-display`, `filter`,
`power` (all seven events, including suspend/resume), playlists and Screen
Splitting with independent per-zone overlays, the full HUD metric set, the
one-frame `config` apply, real network throughput in the telemetry push,
`status`/`info` answering from the daemon's
published snapshot while it holds the port, frame CRC/length validation,
payload tests against
captured vendor traffic, exclusive port locking, the system/user unit split
and a udev rule.

`rotate` (Mirror Mode) works but applies only at the cooler's next start, so
`reed-tpse rotate` reboots the device for you. See
[docs/firmware-notes.md](docs/firmware-notes.md).

### Coming from upstream

One flag is gone: **`display --keepalive`**. It held the connection open in the
foreground so the device would not revert to firmware content, and it was a
second implementation of what the daemon does -- one that also held the serial
port against every other command while looking like an ordinary finished
process. Run the daemon instead; it keeps the connection alive, and `display`
hands over to it rather than failing when it holds the port.

Scripts that drove the panel with `--keepalive` no longer need to take the port
at all:

```bash
reed-tpse display intro.mp4     # the daemon applies it and keeps it alive
sleep 120
reed-tpse display loop.mp4
```

Everything else upstream accepts still works, with the same defaults.

## Documentation

| | |
|---|---|
| this file | installing, and what each command does |
| [docs/vendor-protocol.md](docs/vendor-protocol.md) | the wire protocol, from captured vendor traffic -- authoritative on payloads |
| [docs/firmware-notes.md](docs/firmware-notes.md) | how the firmware behaves: the startup race, what persists, what it ignores |
| [docs/multi-cooler.md](docs/multi-cooler.md) | what assumes a single cooler, and what supporting two would take |

## Requirements

**Build:**
- CMake >= 3.16
- C++17 compiler (GCC 8+ / Clang 7+)

**Runtime:**
- `adb` - for file transfer (android-tools on Arch, adb on Debian/Ubuntu)
- `ffmpeg` - for GIF to MP4 conversion (.gif don't seem to work, so we convert any .gif uploaded to mp4 under-the-hood)

**Permissions:**
- User must be in `uucp` group (Arch) or `dialout` (Debian/Ubuntu) for serial access
- Or run with sudo

## Build and install

`make install` installs the binary and nothing else. Everything that touches
the system — a systemd unit, the udev rule, the suspend hook — is opt-in, so
installing never silently enables a daemon or writes outside the prefix. Turn
on what you want at configure time, in one go:

```bash
cd reed-tpse
cmake -S . -B build \
    -DREED_SYSTEMD_SCOPE=user \
    -DREED_INSTALL_UDEV=ON \
    -DREED_INSTALL_SLEEP_HOOK=ON
cmake --build build -j
sudo cmake --install build
```

| option | |
|---|---|
| `REED_SYSTEMD_SCOPE` | `user`, `system`, or `none` (the default) |
| `REED_INSTALL_UDEV` | the stable `/dev/tryx-panorama` symlink |
| `REED_INSTALL_SLEEP_HOOK` | tells the cooler about suspend and resume |

Then enable it:

```bash
reed-tpse daemon start            # add --system for the system scope
```

⚠ The options are read at **configure** time, so adding one later means
re-running the `cmake -S . -B build` line and installing again — a bare
`sudo cmake --install build` will not pick it up. If you install the binary
without a unit and then run `daemon start`, it says so rather than failing
obscurely:

```
systemd service not installed in user scope. Run with --foreground,
install the unit, or try --system.
```

Other options: `-DREED_SERVICE_USER=<name>` for the account the system unit
runs as, `-DREED_BUILD_TESTS=ON` for the three hardware-free test binaries,
and `-DCMAKE_INSTALL_PREFIX=<path>` if `/usr/local` is not where you want it —
both units are templated on it.

### Choosing a systemd scope

```
user    session-scoped, WantedBy=default.target       -- starts at login
system  boot-scoped,    WantedBy=multi-user.target    -- starts at boot
```

The two are **mutually exclusive**. The daemon holds `/dev/ttyACM0`
exclusively, and two readers on one tty split incoming frames between them at
random — commands start returning empty or mismatched responses. Do not
symlink one unit file into both scopes; a single file installed in both places
can legitimately be enabled and started twice.

The system unit runs as a named account (defaults to `$SUDO_USER`, else
`$USER`; override with `-DREED_SERVICE_USER=<name>`). That account needs to be
in the serial group and owns the config/state files.

If a second instance does start, it now fails immediately and names the holder:

```
Error: /dev/ttyACM0 is already open by PID 4898 (reed-tpse).
       Only one instance may hold the device. Stop it with one of:
         systemctl --user stop reed-tpse.service
         sudo systemctl stop reed-tpse.service
```

⚠ If you have a boot or shutdown script that wraps `reed-tpse` in a
`Type=oneshot` unit and sleeps for the length of a video, set
`TimeoutStartSec` above that sleep — systemd's default is 90s and will
otherwise kill the script partway through.

## udev rule

`udev/71-reed-tpse.rules` grants the device to the logged-in user and, more
usefully, creates a stable `/dev/tryx-panorama` symlink so re-enumeration
(`ttyACM0` becoming `ttyACM1` after a USB suspend) stops mattering:

```bash
cd ~/reed-tpse          # repo root -- `make install` leaves you in build/
sudo cp udev/71-reed-tpse.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger

# confirm the symlink appeared, then point the config at it
ls -l /dev/tryx-panorama
mkdir -p ~/.config/reed-tpse
echo '{"port":"/dev/tryx-panorama"}' > ~/.config/reed-tpse/config.json
```

or `cmake .. -DREED_INSTALL_UDEV=ON` (installs to `/etc/udev/rules.d` by
default -- deliberately not under `CMAKE_INSTALL_PREFIX`, since udev does not
read `/usr/local/lib/udev/rules.d` and installing there would silently do
nothing).

`18d1:2d03` is Google's Android Open Accessory VID/PID and is not unique to this
cooler, so the product string is matched too (`cm01*` -- ours reports `cm01`,
the SE reports `cm01_se`). Note `uaccess` is seat-based and does not cover a
system-scope daemon running as a user who is not logged in; keep that account in
the serial group.

Verified on Ubuntu 24.04 with a Panorama 360 ARGB:

```
/dev/tryx-panorama -> ttyACM0
crw-rw----+ root dialout /dev/ttyACM0     # '+' = ACL present
user:atlance:rw-                          # granted by uaccess
```

Pinning `port` to the symlink is worth doing beyond surviving renumbering:
without it the tool auto-detects by opening every `/dev/ttyACM*` in turn and
handshaking, which means sending frames to unrelated CDC-ACM hardware.

## Usage

```bash
# Upload media to device
reed-tpse upload video.mp4
reed-tpse upload animation.gif  # Auto-converts to MP4

# Set display and start daemon (recommended)
reed-tpse display video.mp4 --brightness 80
reed-tpse daemon start

# That's it. Display persists across reboots.
```

### All commands

```bash
reed-tpse info                   # Show device info
reed-tpse status                 # Fan/pump RPM, warnings, free storage
reed-tpse raw <METHOD> <ENDPOINT> [JSON]
                                 # Send any command, print the response
reed-tpse fan <low|mid|high|full> # LCD fan RPM, or set a tier
reed-tpse screen <on|off>        # Panel power
reed-tpse rotate <normal|mirror> # Mirror Mode (reboots the cooler)
reed-tpse sleep-display <on|off> # on = standby animation, off = black
reed-tpse preset <name|list>     # Show a firmware-bundled preset
reed-tpse upload <file>          # Upload media file
reed-tpse list                   # List files on device
reed-tpse delete <file>          # Delete file from device
reed-tpse display <file...>      # Set display content (playlist if >1 file)
reed-tpse filter <Rain|Smoke|none> --opacity 0-100
reed-tpse brightness <0-100>     # Adjust brightness
reed-tpse lock-display <file>    # custom screen while the session is locked
reed-tpse hud config             # Configure on-device telemetry overlay
reed-tpse hud clear              # Disable the telemetry overlay
reed-tpse hud status             # Show current HUD configuration
reed-tpse power <event>          # shutdown|lock|unlock|ac|battery|suspend|resume
reed-tpse daemon start           # Start background keepalive
reed-tpse daemon stop            # Stop daemon
reed-tpse daemon status          # Check daemon status
```

Add `--system` to any `daemon` subcommand to address the system-scope unit
instead of the user-scope one.

### Playlists

`display` takes more than one file, and `--play-mode` decides how the device
walks the list:

```bash
reed-tpse display clip-a.mp4 clip-b.mp4 --play-mode loop
reed-tpse display a.png b.png c.png --play-mode shuffle
reed-tpse display just-this.mp4 --play-mode single
```

| Mode | Behaviour |
|---|---|
| `single` | plays the first entry only (the firmware default) |
| `loop` | walks the list in order, repeating |
| `shuffle` | walks the list in random order |

`media` is a flat array on the wire, and the device accepts `mp4`, `png` and
`gif`, so a playlist can mix stills and video. The mode is saved in
`display.json` and re-applied by the daemon.

⚠ Before this existed, `display` always sent `playMode: "Single"` regardless of
how many files were listed -- so a multi-file `display` showed only the first.

### Screen Splitting

```bash
reed-tpse display left.png right.mp4 --split
reed-tpse hud config --zone right --metrics "GPU Temperature" --color FF0000
```

Two zones, each with its own media, metrics, colour and alignment. An unset
right zone mirrors the left.

⚠ **Two heavy videos will not both play.** With two high-bitrate 2160x1080
clips one half falls back to the standby animation -- the decoder runs out, not
the protocol. Two images, two small clips, or one heavy clip beside a light one
all work. If a half goes to standby, re-encode one side smaller.

### Filters

An overlay the firmware draws across the media, independent of the HUD:

```bash
reed-tpse filter Rain --opacity 60
reed-tpse filter Smoke
reed-tpse filter none
```

`Rain` and `Smoke` are the only names seen on the wire; `none` clears it.
Opacity is 0-100 and persists, so `filter Smoke` on its own keeps whatever
opacity was set last. The filter travels in `settings` on a screen-config
frame, so this re-sends the current media -- expect the clip to restart.

### Status

```bash
reed-tpse status                 # human-readable table
reed-tpse status --json          # one JSON object on stdout, for scripting
reed-tpse status --watch 5       # poll every 5s until interrupted
```

```
Fan LCD:   4170 RPM
Pump:      2910 RPM
Storage:   2.65 GiB free
Warnings:  Fan LCD: No ERROR
```

Exit code is `2` if the device reports any warning whose description is not
`No ERROR`, so it drops straight into a monitoring check. `status` does not
send `conn`, so it will not disturb what is on screen.

### Fan (LCD / pump-head fan)

```bash
reed-tpse fan              # LCD fan RPM
reed-tpse fan low          # named tiers
reed-tpse fan mid
reed-tpse fan high
reed-tpse fan full
reed-tpse fan --speed 45   # explicit duty, 0-100, for finer control
```

Pump RPM is not repeated here -- `reed-tpse status` reports it, and it is
driven by the motherboard/BIOS rather than by this tool.

The tiers are the vendor's own, read off the wire from KANALI 1.2.1 -- one
`fanLCDSet` capture per tier. A tier is a **pair**: a fixed duty *and* a
temperature curve, and the app sends both together whichever mode is active.

| Tier | `fixedMode` | Smart curve `[°C, duty%]` |
|---|---|---|
| `low` | 40% | `[0,10] [10,20] [30,30] [50,40] [65,55] [80,70] [90,100] [100,100]` |
| `mid` | 60% | `[0,10] [10,20] [30,35] [50,50] [65,75] [80,80] [90,100] [100,100]` |
| `high` | 80% | `[0,10] [10,20] [30,50] [40,70] [55,85] [70,90] [90,100] [100,100]` |
| `full` | 100% | `[0,10] [10,20] [30,70] [40,100] [65,100] [80,100] [90,100] [100,100]` |

⚠ An earlier version of this table read 35/57/78/100. Those were interpolated
by hand from RPM measurements before the vendor's real values were known;
`low` in particular was 35%, not 40%. RPM measured here at the old duties:
35% ≈ 2010, 57% ≈ 2880, 78% ≈ 3570, 100% = 4170, and the response is monotonic
but not linear (45% ≈ 2460, 65% ≈ 3150).

`--speed <0-100>` sets any duty directly, pairing it with the curve of the
nearest named tier -- KANALI never sends a duty without a curve beside it.

#### How it actually works

**Two endpoints, and neither does anything alone.** This is why several rounds
of investigation concluded the fan was not host-controllable:

1. `POST fanLCDSet` installs the profile. No immediate effect.
2. `POST all` -- the telemetry push -- is what makes the device evaluate it.

Set a profile with no telemetry flowing, or push telemetry without setting a
profile, and both look inert.

**The payload is flat, not per-tier.** The device's own `FanLCD` model, read out
of `HomeUI.apk`'s DEX field table, is:

```
speed      String        mode       String
fixedMode  int           smartMode  ArrayList
```

There are no `lowSpeed`/`midSpeed`/`highSpeed`/`fullSpeed` sub-objects, so the
vendor app's four-tier payload is largely discarded by this firmware. What
works is:

```json
POST fanLCDSet {"mode":"Fixed Mode",
                "smartMode":[[0,10],[10,20],[30,30],[50,40],
                             [65,55],[80,70],[90,100],[100,100]],
                "fixedMode":40}
```

Three keys, and that is all -- the vendor never sends the `speed` field even
though the device's model carries one, and it always sends **both** the curve
and a numeric `fixedMode` regardless of mode. `smartMode` is 8 `[°C, duty%]`
points, ascending in both axes, pinned at temp 0 and `[100,100]`.

⚠ **`fixedMode` must be a number.** Sending an array there coerces to 0 and
**stops the fan dead** -- 0 RPM through every telemetry value tried, including
an all-100% profile. The `[[tempC, dutyPercent], ...]` curve shape guessed
earlier was in fact right; what killed the fan was the `fixedMode` beside it.
`fan --profile` now validates the pairs and requires a numeric `fixedMode`.

**A telemetry push latches the profile; after that it holds on its own.**
Installing `fixedMode: 100` and waiting 48s changed nothing, then a *single*
`POST all` moved the fan immediately. Once latched, a Fixed Mode duty survives
with no telemetry at all -- measured steady for minutes after stopping the
daemon, and it persists across a warm reboot, since that never cuts USB power.
It even survives being set from the vendor's Windows app and then running under
Linux with nothing pushing.

The daemon still pushes telemetry when a duty is configured, because it
re-installs the profile on every connect and that fresh install needs a push to
latch.

**Smart Mode is the exception, and explains the 100% mystery.** Smart Mode
evaluates against live host data, so with no telemetry it has nothing to work
from and runs the fan at 100%. That is why the fan sits at 4170 RPM after a cold
boot: a full power cut resets the controller to the factory profile, which is
Smart Mode, and nothing is pushing yet. It is not a fail-safe reacting to the
daemon dying -- a latched Fixed Mode duty does *not* revert.

`smartMode` is a flat `ArrayList` of numbers -- not `[x, y]` pairs; there is no
curve-point class anywhere in the APK. Its element values are still unknown, so
`fan low` restores the vendor default -- Smart Mode on the low curve with a
fixed fallback of 40%, which is exactly what the app ships.

#### `--profile` and `--force`

`fan --profile <file.json>` sends a raw profile but **refuses** any file
containing non-empty nested curve arrays unless `--force` is passed. A correct
curve has to come from a capture, not from inference.

### Host power events

```bash
reed-tpse power shutdown    # switches the panel to the standby clip
reed-tpse power lock        # standby clip
reed-tpse power unlock      # back to the media
reed-tpse power ac          # host on mains
reed-tpse power battery     # host on battery
```

`power` tells the device what the *host* is doing; it is not a screen switch.
The wire details are in
[docs/firmware-notes.md](docs/firmware-notes.md).

⚠ The panel wakes again the moment anything reopens the serial port -- the
device treats a reconnect as a wake (`--onReConnect--` then `hindStandby`). So
stop the daemon before sending `shutdown` or `lock` if the panel is meant to
stay dark, or use auto mode below, which sends it from inside the daemon as it
exits.

#### Auto mode

The daemon mirrors the host's state to the device by default. Three separate
switches, each on unless you turn it off:

```json
// ~/.config/reed-tpse/config.json
{"report_ac_power": true, "report_lock": true, "report_shutdown": true}
```

| key | what it does |
|---|---|
| `report_ac_power` | announces mains/battery on connect |
| `report_lock` | watches the session's `LockedHint` via `loginctl` on the keepalive cadence and sends `lock-screen` / `unlock-screen` as it changes -- and drives `lock-display` |
| `report_shutdown` | sends `shutdown` when the daemon exits |

⚠ These were one `power_auto` flag. Bundling them caused a real bug: with
`lock_media` set, the branch that sends the *unlock* event was skipped along
with the lock event, and the panel stayed stuck on standby across a daemon
restart. A config with `power_auto` still works -- its value becomes the
default for all three -- so an existing file that switched the behaviour off
keeps it off.

That last one is the useful one: the daemon is stopped as part of the host
shutting down, so the device is told rather than left to time out.

⚠ It does not blank the panel. `shutdown` switches to the standby clip, the
same as `lock-screen` -- see [docs/firmware-notes.md](docs/firmware-notes.md).
For a dark panel at shutdown, either send `reed-tpse screen off`, or set
`sleep-display off` and let the ~60s timeout expire.

#### A custom lock screen

```bash
reed-tpse lock-display sunset.mp4 --brightness 40   # shown while locked
reed-tpse lock-display                              # show the current setting
reed-tpse lock-display --default                    # back to the standby clip
```

The daemon re-reads `config.json` and the state file whenever either changes,
so `lock-display` takes effect on a running daemon without a restart. (It has
to: `lock-display` needs no serial port, so it is editable while the daemon
holds the device.)

The setting lives in `config.json` alongside `report_lock`, not in the state
file -- `display` rewrites the state file, and this is configuration rather
than runtime state.

With `lock_media` set, the daemon swaps the panel to that media on lock and
back to your normal media on unlock, instead of sending the `lock-screen` power
event. The two are mutually exclusive by nature: sending the event and *then*
setting media would immediately wake the panel (`hindStandby`), so a custom
lock screen replaces the firmware standby rather than layering on it.
`--default` (or `--remove`) restores the firmware behaviour.

Brightness is stored per-state, defaulting to 40, and a value above 50 warns:
a locked machine sits untouched for hours, which on an AMOLED is precisely the
burn-in case. A dark clip is worth more than a low number here.

⚠ **Suspend behaves like shutdown, not like lock.** While the host is
suspended nothing is pushing keepalive, so the device falls back to
firmware-drawn content after ~60s whatever `lock_media` says. The custom screen
reappears on resume if the session is still locked. Only `sleep-display`
changes what that fallback looks like.

⚠ **This only works for lock, not shutdown.** The device reverts to
firmware-drawn content ~60s after the last handshake, and at shutdown the
daemon is gone by definition -- so nothing is holding a custom video on screen.
At shutdown your options are the firmware's: the standby clip, or black via
`sleep-display`. Replacing `standby.mp4` itself would need write access to the
read-only `/system` partition, i.e. root on the device.

Lock state is read with `loginctl` rather than a D-Bus signal because a
system-scope daemon has no session of its own; the session is located by user
name. It degrades quietly to doing nothing if logind cannot answer. Off by
default, since it changes what the panel does without being asked.

### Presets

The firmware ships its own clips in `/system/media/video/`, independent of
anything you upload. Nothing has to be transferred to use them.

```bash
reed-tpse preset list             # read the list off the device
reed-tpse preset "Cyber Bunker"   # select one (case- and underscore-tolerant)
```

Selection goes through `waterBlockScreenId` with only an `id`:

```
POST waterBlockScreenId {"id":"Pre-set 1: Cooling delivery"}
  -> device loads /system/media/video/Cooling_delivery.mp4
```

Three things about that id, all established on firmware V1.0.11:

- The device splits on `": "`, replaces spaces with underscores and appends
  `.mp4`. **The leading number is not used** -- `Pre-set 1: Oasis` and
  `Pre-set 8: Oasis` both load `Oasis.mp4`.
- It does **not** check the file exists. `Pre-set 99: Nonsense` cheerfully
  tries `/system/media/video/Nonsense.mp4` and blanks the panel. `preset`
  therefore validates the name against the device's real listing and refuses
  anything else, rather than letting a typo look like a dead screen.
- The `Pre-set <n>: ` prefix is required. A bare `{"id":"Oasis"}` is not
  dispatched at all.

The list is read from the device rather than hardcoded, so it tracks the
firmware: this one ships 15 clips where the vendor app exposes 7. `standby`
is filtered out -- it is the sleep animation, not a preset.

A preset and custom media are mutually exclusive. Selecting a preset is
remembered, `display` clears it, and the daemon re-applies whichever is active.

### Sleep display

```bash
reed-tpse sleep-display on     # standby animation once the host stops handshaking
reed-tpse sleep-display off    # black panel instead
```

The device switches to firmware-drawn content ~60s after the last handshake --
when the PC powers off, or whenever no process is holding the connection.
`sleep-display` chooses what that is.

**The name reads backwards.** `displayInSleep` is the device's own "display
something while the host is asleep", not "blank the display":

| `displayInSleep` | Panel after the ~60s timeout |
|---|---|
| `{"enable":true}`  | the firmware's standby animation |
| `{"enable":false}` | black |
| `{"value":true}`   | silently ignored -- `value` is not read |

This tool passes the value straight through, so it matches the vendor's own
toggle. ⚠ An earlier version of this table said the opposite, and that error
survived a long time: it was "verified" by sampling the panel's mean luminance
over `screencap`, which is the one measurement this device breaks. **Taking a
`screencap` wakes the panel**, so the reading reflects the wake, not the
setting. Confirmed the right way round by watching the panel through a full
undisturbed timeout, in both directions.

⚠ `power {"event":"shutdown"}` does **not** blank the panel either, with or
without this setting -- it shows the standby clip, exactly like `lock-screen`.
The only immediate blank is `reed-tpse screen off`
(`waterBlockScreen {"enable":false}`).

The endpoint returns `200` with an empty body for *any* payload, including a
nonsense endpoint name, so the reply proves nothing -- only the panel does.

The setting lives in controller RAM and is lost whenever USB power drops (which
it does at S5), so `sleep-display` persists it to the state file and the daemon
re-applies it on every connect.

### Raw passthrough

The protocol has two methods: `POST` writes, `STATE` reads. (There is no
`GET` — the firmware ignores it.) `raw` sends either one to any endpoint,
which makes undiscovered endpoints reachable without a code change:

```bash
reed-tpse raw STATE all                        # the only read that returns a body
reed-tpse raw POST conn                        # same data as `reed-tpse info`
reed-tpse raw POST brightness '{"value":60}'
reed-tpse raw POST displayInSleep '{"enable":true}' -v   # -v prints frame hex
```

⚠ **An empty body with status 200 means the endpoint took no action.** It is
not an error, and it is not proof of success — `raw` says so explicitly rather
than reporting success. Endpoints that control cooling hardware
(`fanLCDSet`, `turboPump`) should be read before they are written; verify any
write by re-reading `STATE all` and checking that the value actually moved.

Confirmed payloads, captured from KANALI 1.2.1 on this firmware, are in
[docs/vendor-protocol.md](docs/vendor-protocol.md) -- that file is the
authority where it and this one disagree.

Endpoints seen in the vendor application's own dispatcher:

```
conn · disconn · all · status · spec · brightness · waterBlockScreen
waterBlockScreenId · rotate · recovery · preset · sysinfoDisplay
displayInSleep · waterfallMode · fanLCDSet · turboPump · mediaDelete
power · reboot
```

### HUD (telemetry overlay)

The Panorama's firmware natively renders a system-telemetry overlay on top of
the configured media — no host-side frame compositing is involved. We send it
(1) a layout config (which labels to show, position, color, CPU/GPU badges)
and (2) periodic value pushes; the cooler draws everything itself.

```bash
# Pick up to 3 metrics, place them top-left, show CPU/GPU badges
reed-tpse hud config \
    --metrics "CPU Temperature,GPU Temperature,GPU Usage" \
    --align Left --color FFFFFF \
    --badges cpu,gpu --interval 5

reed-tpse hud status    # show current config
reed-tpse hud clear     # turn the overlay off
```

#### Known labels

Metric labels are defined by the firmware. Unknown labels are rejected:

```
CPU Temperature, CPU Frequency, CPU Usage, CPU Voltage,
GPU Temperature, GPU Frequency, GPU Usage, GPU Voltage,
Motherboard Temperature, Memory Frequency, Memory Utilization,
Hard Disk Temperature, Date&Time
```

⚠ `Date&Time` is **unspaced** on the wire. The spaced `Date & Time` is the
vendor app's UI string, and the firmware silently drops it while accepting
every other label in the same request -- so the clock just never appears.
`reed-tpse` accepts either spelling and normalises it.

#### Layout options

- `--align Left|Center|Right`
- `--color RRGGBB` text colour — **six hex digits, no `#`**. An unquoted
  `#RRGGBB` is a shell comment: the value is silently dropped before the
  program ever sees it. A `#` is still accepted if you quote it, and is added
  back when the value goes on the wire.
- `--badges cpu,gpu` draws CPU/GPU marketing-name chips alongside the values
  (auto-detected from `/proc/cpuinfo` and `nvidia-smi`; override with
  `--cpu-name` / `--gpu-name`)
- `--unit Celsius|Fahrenheit`
- `--interval <sec>` — how often the daemon pushes fresh values (default 5s)

**Firmware limits:** 3 metrics max per screen, and vertical placement is not
adjustable. Metrics render mid-height and badges near the top; only the
horizontal `align` has any effect. There was a `--position Top|Center|Bottom`
flag here, removed after testing: all three values were sent with badges and
metrics enabled and nothing moved on firmware V1.0.11, and KANALI never sends
the field at all. If you need free placement or more than 3 metrics, you'd have
to composite frames host-side (not supported here).

#### Telemetry sources

Values are sampled in the keepalive daemon from:

- CPU: `/sys/class/hwmon` (`k10temp` / `zenpower` / `coretemp`),
  `/proc/stat` (delta-based usage), per-core `scaling_cur_freq`
- GPU: `nvidia-smi` if present (preferred for dGPU), otherwise
  `/sys/class/drm/card*/device` for AMD (`gpu_busy_percent`, hwmon,
  `pp_dpm_sclk`)
- Memory: `/proc/meminfo`

The firmware renders the chosen metrics in its own fixed order, not the order
they are given -- asking for `CPU Temperature,GPU Temperature,Date&Time` renders
GPU first. `--metrics` order is therefore cosmetic.

##### Repurposing a slot

The label set is fixed. A label the firmware does not know is silently dropped:
sending `sysinfoDisplay: ["COOLANT TEMP", "CPU Temperature"]` renders only the
CPU one, so there is no way to add a metric of your own.

What *is* yours:

- **The value.** Whatever you put in a `PcInfo` field is what the panel shows,
  so a slot can carry something else entirely -- coolant temperature in the
  `CPU Temperature` field, say.
- **The badge text.** `--cpu-name` / `--gpu-name` (the `spec` endpoint) render
  as free-text tags, so a repurposed slot can at least be labelled.

The catch is that the badges are separate tags at the top of the overlay, not
captions on the metrics. The metric keeps its firmware caption -- a repurposed
`CPU Temperature` still reads `CPU TEMP` underneath the number, whatever the
badge says. Verified on firmware V1.0.11: badges rendered `COOLANT` and
`AMBIENT` while the metrics below still read `CPU TEMP` and `GPU TEMP`.

Badges also depend on a matching metric being selected -- the CPU badge needs a
CPU metric in the set.

##### Metric availability

The firmware accepts 16 labels, but a Linux host cannot source all of them, and
the overlay only has three slots -- a metric with no source silently renders a
permanent `0`, which reads as a device fault. `hud config` samples the
machine first and warns instead:

```
Warning: "CPU Power" has no data source on this system -- it will render 0.
         RAPL energy_uj is root-only on kernels >= 5.10; the daemon runs unprivileged
```

| Metric | Source |
|---|---|
| CPU Temperature / Usage / Frequency | `coretemp`/`k10temp` hwmon, `/proc/stat`, cpufreq |
| CPU Voltage | super-I/O `in0` (VCore) — needs `nct6775` or similar loaded |
| CPU Power | RAPL `energy_uj` — **root-only** since kernel 5.10 (Platypus), so unavailable to the daemon |
| GPU Temperature / Usage / Frequency / Voltage / Power | `nvidia-smi`, or AMD sysfs |
| Memory Utilization | `/proc/meminfo` |
| Memory Frequency | needs an SPD/DMI read (root) — normally unavailable |
| Memory Temperature | DIMM sensor (`jc42`/`spd5118`) — most desktop boards have none |
| Motherboard Temperature | super-I/O `SYSTIN`, resolved by label |
| Hard Disk Temperature | `nvme` hwmon, or `drivetemp` for SATA |
| Date&Time | drawn from the device's own clock; no host value needed |

The HUD config is persisted in `~/.local/state/reed-tpse/display.json`
alongside the media state, so it survives reboots.

## Configuration

Two files, with different jobs.

**`~/.config/reed-tpse/config.json`** -- things you set once:

```json
{"keepalive_interval":10, "report_lock":true,
 "lock_media":"lockscreen.mp4", "lock_brightness":30}
```

| key | |
|---|---|
| `port` | pin a serial port; omit to auto-detect |
| `keepalive_interval` | seconds between daemon handshakes (1-55, default 10) |
| `report_ac_power` / `report_lock` / `report_shutdown` | mirror host power, lock and shutdown state to the device (all default true) |
| `lock_media` / `lock_brightness` | what to show while the session is locked |

⚠ There is no `brightness` key. There used to be, and it did nothing -- the
value that reached the device was always either `--brightness` or the stored
display state. If your config still has one it is ignored, and the next command
that writes the file drops it. That is deliberate, but it means a key you added
by hand can disappear without comment; brightness lives in the state file, set
with `reed-tpse brightness`.

⚠ The daemon asserts a **complete** device state on every connect, not just
the parts you set. Its post-connect frame carries panel power, sleep
behaviour, temperature unit, brightness, media and the fan curve together --
one frame is what stops the device running half your settings and half the
firmware's during startup. Anything you never configured is sent as this
tool's default rather than left alone, so reed-tpse will overwrite another
program touching the same panel every time it reconnects.

**`~/.local/state/reed-tpse/display.json`** -- what the daemon re-applies on
every connect: media, brightness, ratio, play mode, screen mode, filter and its
opacity, panel power, sleep behaviour, fan tier and duty, and both HUD zones
(`hud` and `hud_right`). Written by the commands, not meant to be hand-edited,
though nothing stops you.

## Architecture

```
reed-tpse/
├── include/reed/      # Public headers (libreed)
│   ├── picojson.h     # JSON parser (header-only, third-party)
│   ├── protocol.hpp   # Frame protocol
│   ├── device.hpp     # Serial device communication
│   ├── adb.hpp        # ADB wrapper
│   ├── media.hpp      # Media type detection, GIF conversion
│   └── config.hpp     # XDG config/state management
├── src/               # Library implementation
├── cli/               # CLI frontend
└── systemd/
    ├── user/          # session-scope unit
    └── system/        # boot-scope unit template (configured by CMake)
```

The core functionality is in `libreed.a` and the CLI links against it, so a GUI
can reuse the same library — [koconnorgit/tryx-panorama](https://github.com/koconnorgit/tryx-panorama)
is a Qt tray front-end that drives the CLI.

## Tested on

| Distro | Kernel | CPU | GPU | Contributor |
|--------|--------|-----|-----|-------------|
| Arch Linux | 6.17.9 | Intel Core Ultra 9 285K | NVIDIA RTX 5080 | [@fadli0029](https://github.com/fadli0029) |
| Bazzite | 6.17.7 | AMD Ryzen 7 9800X3D | Radeon RX 9070XT | [@CRE82DV8](https://github.com/CRE82DV8) |
| CachyOS | 6.19.8-1-cachyos | AMD Ryzen 9 9950X3D | AMD Radeon RX 9070 XT | [@nerddotdad](https://github.com/nerddotdad) |
| Linux Mint | 6.17.0-29-generic | AMD Ryzen 9 9950X3D | AMD Radeon RX 9070 XT | [@chuiden](https://github.com/chuiden) |
| Ubuntu 24.04.4 LTS | 7.0.0-28-generic | Intel Core i9-13900K | NVIDIA RTX 4090 | [@Julien-P41](https://github.com/Julien-P41) |

Rows above the Ubuntu one come from upstream and its forks; if you've tested on
another system, a PR to this fork is welcome.

⚠ The Ubuntu row is a **Panorama 360 ARGB**, not an SE — firmware V1.0.11,
hardware V1.1, `productId` `cm01`. It is the same controller board, so the
protocol is identical, but every measurement in this README (fan RPM figures,
`displayInSleep`, preset ids) was taken on that model. Treat the numbers as
model-specific until someone confirms them on an SE.

## License

MIT

## Contributing

Issues and pull requests for **this fork**:
https://github.com/Julien-P41/reed-tpse

For the original tool and the protocol groundwork, go upstream:
https://github.com/fadli0029/reed-tpse

Changes here are meant to be upstreamable — commits are scoped so the useful
ones can be cherry-picked out, and anything unverified is labelled rather than
presented as fact. If you have a Panorama SE, confirming (or contradicting) the
measurements in the Fan and Sleep-display sections would be the most useful
thing you could add.
