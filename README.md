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
- **Sleep behaviour** (`sleep-display`) — black panel instead of the firmware's
  sleep animation when the host stops talking.
- **Firmware presets** (`preset`) — the clips shipped in the device's own system
  image, no upload required.
- **`raw` passthrough** — any method/endpoint/body, so an undiscovered endpoint
  is reachable without a code change.
- **Exclusive port locking** and a system/user systemd unit split, fixing a
  packaging arrangement that let two daemons hold the same tty and corrupt each
  other's replies.
- **HUD metric coverage** completed against the firmware's label set, with a
  warning for any metric the host cannot actually source instead of a silent 0.

Everything above was verified against real hardware (Panorama 360 ARGB,
firmware V1.0.11); where something is only inferred, the text says so.

## Currently supported features

- Upload images, videos, and GIFs (auto-converts to MP4)
- Set display content and brightness
- Read device status: fan/pump RPM, health warnings, free storage
- LCD fan speed control (named tiers or any duty 0-100%)
- Black screen instead of the demo loop when the host is off (`sleep-display`)
- Firmware presets, played from the device's own storage (`preset`)
- Host power events -- lock / unlock / shutdown -- manually or mirrored
  automatically by the daemon (`power`)
- Raw protocol passthrough for reaching any endpoint
- List and delete media files on device
- On-device system telemetry overlay (CPU/GPU temp, usage, frequency, voltage, power; RAM; motherboard/disk temps; date & time), with a warning for any metric this host cannot source
- systemd service (user or system scope) for persistent display across reboots
- Exclusive port locking, so a second instance can't corrupt the first's replies
- Auto-detects device (scans /dev/ttyACM*)
- Minimal dependencies (picojson header-only)

## TODO

Open:

- [ ] Fan `smartMode` curve values -- shape known, values need a captured
      vendor payload
- [ ] Screen Splitting mode (6-metric, two-zone layout)
- [ ] Network throughput (the `PcInfo` blob already carries it; nothing
      collects it host-side)
- [ ] Custom overlay layouts, beyond the firmware's 9 anchor points and
      3-metric cap

Done in this fork: `status`, `raw`, `fan`, `preset`, `sleep-display`, `power`
(+ daemon auto mode), the full HUD metric set, frame CRC/length validation,
protocol and config tests, exclusive port locking, the system/user unit split
and a udev rule.

Not possible on firmware V1.0.11, established rather than assumed: `rotate`
(unimplemented) and `waterfallMode` (present but SE-only). Details below.

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

## Build

```bash
cd reed-tpse
mkdir build && cd build
cmake ..
make
sudo make install
```

### Installing the systemd unit

`make install` deliberately installs **no** unit by default, so it never
silently enables a daemon. Pick exactly one scope:

```bash
cmake .. -DREED_SYSTEMD_SCOPE=user      # session-scoped, WantedBy=default.target
cmake .. -DREED_SYSTEMD_SCOPE=system    # boot-scoped,  WantedBy=multi-user.target
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
reed-tpse upload <file>          # Upload media file
reed-tpse display <file...>      # Set display content (playlist if >1 file)
reed-tpse brightness <0-100>     # Adjust brightness
reed-tpse sleep-display <on|off> # Black screen vs sleep animation when host is off
reed-tpse preset <name|list>     # Show a firmware-bundled preset
reed-tpse power <event>          # shutdown|lock|unlock|ac|battery
reed-tpse lock-display <file>    # custom screen while the session is locked
reed-tpse fan [low|mid|high|full] # LCD fan RPM, or set a tier (--speed N for 0-100)
reed-tpse list                   # List files on device
reed-tpse delete <file>          # Delete file from device
reed-tpse daemon start           # Start background keepalive
reed-tpse daemon stop            # Stop daemon
reed-tpse daemon status          # Check daemon status
reed-tpse hud configure ...      # Configure on-device telemetry overlay
reed-tpse hud clear              # Disable the telemetry overlay
reed-tpse hud status             # Show current HUD configuration
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
reed-tpse fan --reset      # hand back to the firmware's own curve
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
| `--reset` | 40% | the `low` curve, in Smart Mode -- the vendor default |

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
`--reset` sends it empty, which yields the firmware default. Recovering the real
curve needs a captured vendor payload.

#### `--profile` and `--force`

`fan --profile <file.json>` sends a raw profile but **refuses** any file
containing non-empty nested curve arrays unless `--force` is passed. A correct
curve has to come from a capture, not from inference.

### Host power events

```bash
reed-tpse power shutdown    # blanks the panel (with sleep-display on)
reed-tpse power lock        # standby clip
reed-tpse power unlock      # back to the media
reed-tpse power ac          # host on mains
reed-tpse power battery     # host on battery
```

`power` tells the device what the *host* is doing; it is not a screen switch.
The wire details are under [What the firmware can actually act
on](#what-the-firmware-can-actually-act-on).

⚠ The panel wakes again the moment anything reopens the serial port -- the
device treats a reconnect as a wake (`--onReConnect--` then `hindStandby`). So
stop the daemon before sending `shutdown` or `lock` if the panel is meant to
stay dark, or use auto mode below, which sends it from inside the daemon as it
exits.

#### Auto mode

```json
// ~/.config/reed-tpse/config.json
{"port": "/dev/tryx-panorama", "power_auto": true}
```

With `power_auto`, the daemon mirrors the host state:

- **on connect** -- announces mains/battery and the current lock state
- **while running** -- watches the session's `LockedHint` via `loginctl` on the
  keepalive cadence and sends `lock-screen` / `unlock-screen` as it changes
- **on exit** -- sends `shutdown`

That last one is the useful one. The daemon is stopped as part of the host
shutting down, so with `sleep-display on` the panel blanks right then, instead
of waiting out the ~60s keepalive timeout. A boot/shutdown script no longer has
to hold a keepalive alive to keep the screen from reverting.

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

The setting lives in `config.json` alongside `power_auto`, not in the state
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
reed-tpse sleep-display on     # panel goes black when the host stops handshaking
reed-tpse sleep-display off    # panel falls back to the firmware's sleep animation
```

The device reverts to firmware-drawn content ~60s after the last handshake --
when the PC powers off, or whenever no process is holding the connection.
`sleep-display on` makes that fallback a black screen instead of the sleep
animation.

Verified on firmware V1.0.11 by measuring the panel's mean luminance over adb
`screencap`, 150s after the last handshake:

| `displayInSleep` | Panel after the keepalive timeout |
|---|---|
| `{"enable":true}`  | black (luminance 0) |
| `{"enable":false}` | sleep animation (luminance ~58-63) |
| `{"value":true}`   | silently ignored -- behaves as disabled |

⚠ That table describes the **keepalive-timeout** path only. `displayInSleep`
does not blank everything: with it enabled, a `power {"event":"lock-screen"}`
still rolls the standby clip, while `power {"event":"shutdown"}` goes fully
black. The two paths are separate.

So the payload field is `enable`; `value` is not read. Note the endpoint
returns `200` with an empty body for *any* payload, including a nonsense
endpoint name, so the reply proves nothing -- only the panel does.

⚠ Taking a `screencap` wakes the panel out of the black state for a few
seconds. If you are measuring this, sample once after a long undisturbed wait
rather than polling, or the wake-ups look like the setting not working.

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
reed-tpse hud configure \
    --metrics "CPU Temperature,GPU Temperature,GPU Usage" \
    --position Top --align Left --color "#FFFFFF" \
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
Hard Disk Temperature, Date & Time
```

#### Layout options

- `--position Top|Center|Bottom` × `--align Left|Center|Right` → 9 anchor points
- `--color "#RRGGBB"` text color
- `--badges cpu,gpu` draws CPU/GPU marketing-name chips alongside the values
  (auto-detected from `/proc/cpuinfo` and `nvidia-smi`; override with
  `--cpu-name` / `--gpu-name`)
- `--unit Celsius|Fahrenheit`
- `--interval <sec>` — how often the daemon pushes fresh values (default 5s)

**Firmware limits:** 3 metrics max per screen; placement is the 9-anchor grid,
not arbitrary pixel coordinates. If you need free placement or more than 3
metrics, you'd have to composite frames host-side (not supported here).

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
permanent `0`, which reads as a device fault. `hud configure` samples the
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

## Startup race

The device accepts a serial connection before its UI app is ready -- adbd and
the CDC-ACM link come up around 12s before HomeUI does. Anything pushed into
that window is lost: the media never appears (black panel) and the fan is left
in Smart Mode with a null curve, which the firmware evaluates as **0 RPM** and
re-evaluates as 0 on every telemetry push.

The daemon therefore re-applies its settings once, 20s after connecting. If you
script around a device restart, wait for the app rather than the transport:

```bash
adb wait-for-device                                    # adbd only -- not enough
until adb shell pidof com.baiyi.homeui.tkcfanhomeui >/dev/null 2>&1; do sleep 2; done
```

## What persists

Every device-side setting is volatile -- the controller forgets it whenever USB
power drops, which happens at S5 -- so persistence means "stored on the host and
re-applied by the daemon on every connect". Nothing sticks without the daemon
running.

| Setting | Stored in | Re-applied |
|---|---|---|
| media / ratio / screen mode / play mode | `display.json` | yes |
| brightness | `display.json` | yes |
| `sleep-display` | `display.json` | yes |
| `preset` | `display.json` | yes |
| `fan` tier or duty | `display.json` | yes |
| HUD metrics, layout, badges, unit | `display.json` | yes |
| `port`, `keepalive_interval` | `config.json` | n/a |
| `power_auto` | `config.json` | yes |
| `lock-display` media + brightness | `config.json` | yes |

⚠ There are two `brightness` values and they are not the same thing.
`config.json` → `brightness` is only the **default for `display --brightness`**
when the flag is omitted; the daemon never applies it. What actually reaches
the panel is `display.json` → `brightness`, set by `reed-tpse brightness N` or
by `display --brightness N`.

`display` without `--brightness` leaves the stored brightness alone. It used to
overwrite it with the config default, so any `brightness N` was quietly undone
the next time the media changed.

Not persisted, by design: `raw`, and one-shot `power` events -- those describe a
moment, not a state.

The daemon re-reads both files when either changes, so edits apply without a
restart.

## Configuration

Config: `~/.config/reed-tpse/config.json`

```json
{"brightness":100,"keepalive_interval":10}
```

Port is auto-detected by default. To pin a specific port:
```json
{"port":"/dev/ttyACM1","brightness":100,"keepalive_interval":10}
```

Display state (for daemon): `~/.local/state/reed-tpse/display.json`

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

## How it works

The Tryx Panorama exposes:
1. **USB CDC ACM** (`/dev/ttyACM0`): Serial interface for display commands
2. **ADB**: Android Debug Bridge for file transfer to `/sdcard/pcMedia/`

The device needs a handshake every ~60s or it reverts to firmware-drawn
content; the daemon exists to supply that, at about 1MB RAM and negligible CPU.

The wire format is essentially HTTP-over-serial carrying JSON, at 115200 8N1:

```
0x5A │ escape( LEN_HI LEN_LO
                "<METHOD> <ENDPOINT> <VERSION>\r\n"
                "ContentType=json\r\n"
                "ContentLength=<n>\r\n"
                "AckNumber=<seq>\r\n"
                "\r\n" <json body>
                CRC ) │ 0x5A

LEN    = len(message) + 5   (uint16, big-endian)
CRC    = sum(LEN bytes + message bytes) & 0xFF
escape = 0x5A -> 0x5B 0x01 ,  0x5B -> 0x5B 0x02
```

Opening the port asserts DTR and the device replies with an unprompted info
frame, so the first read after connecting has to be drained or it is mistaken
for the answer to the first command.

Incoming frames are validated: the first complete `0x5A..0x5A` frame is taken
(a single read can return two, and slicing to the end of the buffer parses them
as one), then the declared length and the checksum are both checked and a
failing frame is discarded with a message rather than parsed as if valid. That
is what two daemons sharing the tty used to produce.

```bash
cmake .. -DREED_BUILD_TESTS=ON && make && ./reed-protocol-test
```

runs the hardware-free checks: `reed-protocol-test` covers framing (real
captured frames, plus corrupted-CRC, wrong-length, unterminated and
two-frames-in-one-buffer cases), and `reed-config-test` covers the config/state
round-trip -- every field, optionals staying unset, `false` being
distinguishable from unset, and a load/mutate/save cycle preserving the
settings that share the state file.

### What the firmware can actually act on

The device's dispatcher reaches its UI through exactly one interface,
`IOnPcControlCallBack`. Its methods are the complete set of things an incoming
command can cause:

```
onConnected  onDisConnect  onReConnect   onRefreshUI    refreshShowData
onBlockScreen              screenConfigChange           presetConfigChange
onDoBrightness             onDoPower     onWaterfallModeChange
onFileUpload               nameTitleChange
```

That list looked like a useful predictor: an endpoint with no corresponding
callback should not be able to do anything however well-formed the payload is.
**It has now mispredicted twice** -- `waterfallMode` and `rotate` both work,
and both apply only at the next device restart, so a live check sees nothing.
Treat it as a hint about the live path, not as evidence an endpoint is inert.

**`rotate` is "Mirror Mode", and it works.** Captured from KANALI 1.2.1 on
this firmware:

```
POST rotate {"degree":90}   -> 200. Nothing happens yet; the value is
                               stored and applies at the next restart.
```

Only two values are ever used, and they are 180° apart: **270 is this unit's
baseline and 90 is the mirrored state**, which fits the vendor's own wording --
*"a mirror image of PANORAMA screen for users with a left-mounted chassis"*.
The value is also a field inside the `config` blob
(`waterBlockScreen.rotate`), re-asserted on every connect.

**It does not restart the device.** Sending it changes nothing visible; the
panel turns at the cooler's next start. Verified by sending `90`, seeing
`rotate--90` dispatched with no change, then rebooting and finding the panel
180° over. KANALI appears to restart on confirm only because it bundles
`adb.exe` -- no `reboot` frame exists in any capture. `reed-tpse rotate` does
the `adb reboot` for you, or the command would look inert.

⚠ **This corrects an earlier conclusion here that `rotate` was unimplemented on
V1.0.11.** That was measured by watching Android's `mRotation` for a live
transform across `degree` values and after several plausible "latches". The
setting is stored and applied at the next restart, so nothing was ever going to
show up in the window being measured -- the same trap as `waterfallMode`, and
the second time the missing-callback heuristic below mispredicted.

⚠ Because the baseline is 270, sending a neutral-looking `degree: 0` or `180`
leaves the panel **90° out**, which is indistinguishable at a glance from
waterfall mode.

`screenFlip` is not a device endpoint at all; the vendor app implements it by
mapping a boolean onto `rotate`'s `degree`, so it is inert here for the same
reason.

Nor does the app rotate media host-side before upload: there is no `sharp`
rotate, no ffmpeg transpose and no EXIF handling anywhere in its main process.

`waterfallMode` and `power` are different -- both have callbacks *and* device
implementations.

**`waterfallMode`** is a 90° re-layout of the **parameter display**, for people
who mount the cooler rotated: *"a more reasonable parameter display for users
who rotate the watercooler by 90 degrees"*. It moves the sysinfo overlay, not
the media. The firmware carries the implementation
(`MainActivity.onWaterfallModeChange`, `doWaterfallMode`,
`changeWaterModelPosition`, `getWaterfallInsets`, `waterfallModePosition`).

**It works, but only after the device is power-cycled.** An earlier round here
concluded it did nothing: the layout was pixel-identical before and after, and
`MainActivity`'s own `--onWaterfallModeChange--` never fired across seven
payload shapes. That was measuring the wrong window. `POST waterfallMode
{"enable":true}` is stored and applied when the controller next starts -- the
same "set now, take effect on restart" shape as Mirror Mode, whose dialog warns
that the cooler restarts on confirmation.

It surfaced by accident: after the AIO was unplugged during cable management,
the panel came back rotated 90° clockwise, from an `enable:true` sent during
that earlier testing. So the absence of a live handler call proves nothing here
-- the same trap as `rotate`, and the second time it caught me.

To turn it off, send the `false` form and restart the device. `adb reboot`
restarts the AIO alone -- no PC shutdown needed, and this hardware records
`reboot,shell` in `persist.sys.boot.reason.history` from previous ones. It is an
ordinary Android reboot, not the `adb root` that is known to drop the USB link.

⚠ The payload key is **not known**. `{"enable":false}` alone did not clear it;
what worked was sending `enable`/`value`/`mode`/`waterfallMode`/`open` as
`false` and `0` together, then rebooting. Unlike `power`, this handler does not
validate, so it never names its field in an exception, and the setting is
invisible from the host: `screencap` shows normal orientation, Android's
`mRotation` stays `0`, and nothing is logged at boot -- it is applied below the
compositor and stored in the app's own `/data`, which needs root to read.

⚠⚠ **Do not sweep payload variants at this device.** Settings here can be
stored and applied only at the next restart, so a sweep that appears to do
nothing can leave a change armed -- that is exactly how waterfall mode ended up
enabled, discovered only when the AIO was next unplugged. The rule the vendor's
own UI states for Mirror Mode ("the cooler will restart upon confirming")
applies more widely than to Mirror Mode.

**`power` is a host power-event notification, not a screen switch.** The field
is `event`, and sending anything else throws `---Exception---No value for event`
on the device -- which is how the field was found, after seven payload shapes
built around `enable` had quietly failed. The vocabulary comes from the vendor
app's own strings:

```
ac-power  on-battery  shutdown  lock-screen  unlock-screen
```

KANALI uses it to tell the cooler what the host is doing. Verified on firmware
V1.0.11 (`--onDoPower--<event>` then `--showStandby--` / `hindStandby` in the
device log):

| Event | With `sleep-display on` | Effect |
|---|---|---|
| `lock-screen` | plays the standby clip | panel leaves the media |
| `shutdown` | **full black** | panel blanks |
| `unlock-screen` | -- | hides standby, restores the media |

So `shutdown` and `lock-screen` are not the same despite both logging
`--showStandby--`: only `shutdown` blanks completely. That makes
`power {"event":"shutdown"}` plus `sleep-display on` the deterministic way to
darken the panel at host shutdown, rather than waiting out the ~60s keepalive
timeout.

⚠ Measuring this needs the serial port held **open**. Every one-shot CLI
invocation closes the port on exit, and the device treats the following
reconnect as a wake -- `--onReConnect--` then `hindStandby` -- so the panel
comes straight back and the command looks inert. Several rounds of "power does
nothing" were this artefact.

The stats overlays never needed host-side image generation: the device renders
them itself. The screen config object carries a `sysinfoDisplay` array and a
`settings{position,color,align,badges}` block, and the host only pushes values.

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
