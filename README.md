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
- Raw protocol passthrough for reaching any endpoint
- List and delete media files on device
- On-device system telemetry overlay (CPU/GPU temp, usage, frequency, voltage, power; RAM; motherboard/disk temps; date & time), with a warning for any metric this host cannot source
- systemd service (user or system scope) for persistent display across reboots
- Exclusive port locking, so a second instance can't corrupt the first's replies
- Auto-detects device (scans /dev/ttyACM*)
- Minimal dependencies (picojson header-only)

## TODO

- [x] CPU stats overlay (temperature, usage, clock speed) — `reed-tpse hud`
- [x] GPU stats (temperature, usage, clock speed) — `reed-tpse hud`
- [x] RAM usage — `reed-tpse hud`
- [x] Fan/pump RPM display — `reed-tpse status`
- [ ] Custom overlay layouts (beyond the firmware's 9 anchor points and 3-metric cap)
- [ ] Network throughput
- [x] Frame CRC/length validation + protocol tests
- [ ] Screen Splitting mode support (6-metric layout)
- [x] Fan control -- `reed-tpse fan low|mid|high|full` / `--speed 0-100`
- [ ] Fan `smartMode` curve values (need a captured vendor payload)
- [x] Screen behaviour: `displayInSleep` -- `reed-tpse sleep-display`
- [x] Firmware presets -- `reed-tpse preset`
- [ ] Screen behaviour: `rotate`, `waterfallMode`, `power` (see note below)

`rotate` is dispatched by the firmware -- `POST rotate {"degree":180}` produces
a handler-specific `rotate--180` in the device log, which a bogus endpoint does
not -- but no rotation is visible in an `adb screencap`, which cannot see a
transform applied below the compositor. Whether the panel physically rotates is
unconfirmed, so it is not exposed as a command yet. `screenFlip` is not a device
endpoint at all; the vendor app implements it by mapping a boolean onto
`rotate`'s `degree`.

The stats overlays never needed host-side image generation: the device renders
them itself. The screen config object carries a `sysinfoDisplay` array and a
`settings{position,color,align,badges}` block, and the host only pushes values.

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
reed-tpse display <file>         # Set display content
reed-tpse brightness <0-100>     # Adjust brightness
reed-tpse sleep-display <on|off> # Black screen vs sleep animation when host is off
reed-tpse preset <name|list>     # Show a firmware-bundled preset
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

The named tiers start at the firmware's own default and climb linearly to
full. Measured on firmware V1.0.11 (Panorama 360 ARGB):

| Tier | Duty | RPM |
|---|---|---|
| `low` | 35% | ~2010 |
| `mid` | 57% | ~2880 |
| `high` | 78% | ~3570 |
| `full` | 100% | 4170 |
| `--reset` | — | ~2040 (firmware curve) |

`--speed <0-100>` sets any duty directly; the response is monotonic but not
linear in RPM (45% ≈ 2460, 65% ≈ 3150). The tier *name* sent to the device is
decorative on this firmware -- with empty curve arrays all four tiers measured
identical RPM -- so the duty is what actually matters, and `--speed` sends the
nearest tier name purely so the device's model carries a sensible label.

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
POST fanLCDSet {"speed":"Low Speed","mode":"Fixed Mode","fixedMode":35,"smartMode":[]}
```

⚠ **`fixedMode` is an `int`.** Sending it as an array (an earlier inference from
the vendor app's chart config read it as `[[tempC, dutyPercent], ...]`) coerces
to 0 and **stops the fan dead**. It stayed at 0 RPM through every telemetry
value tried, including an all-100% profile; only an empty-curve profile restored
it. `fan --profile` refuses that shape for exactly this reason -- both the flat
form and the vendor's nested per-tier form.

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

| `displayInSleep` | Panel |
|---|---|
| `{"enable":true}`  | black (luminance 0) |
| `{"enable":false}` | sleep animation (luminance ~58-63) |
| `{"value":true}`   | silently ignored -- behaves as disabled |

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

Endpoints seen in the vendor application's own dispatcher:

```
conn · disconn · all · status · spec · brightness · waterBlockScreen
waterBlockScreenId · rotate · recovery · preset · sysinfoDisplay
displayInSleep · waterfallMode · fanLCDSet · turboPump · mediaDelete
power · reboot
```

## HUD (telemetry overlay)

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

### Known labels

Metric labels are defined by the firmware. Unknown labels are rejected:

```
CPU Temperature, CPU Frequency, CPU Usage, CPU Voltage,
GPU Temperature, GPU Frequency, GPU Usage, GPU Voltage,
Motherboard Temperature, Memory Frequency, Memory Utilization,
Hard Disk Temperature, Date & Time
```

### Layout options

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

### Telemetry sources

Values are sampled in the keepalive daemon from:

- CPU: `/sys/class/hwmon` (`k10temp` / `zenpower` / `coretemp`),
  `/proc/stat` (delta-based usage), per-core `scaling_cur_freq`
- GPU: `nvidia-smi` if present (preferred for dGPU), otherwise
  `/sys/class/drm/card*/device` for AMD (`gpu_busy_percent`, hwmon,
  `pp_dpm_sclk`)
- Memory: `/proc/meminfo`

#### Metric availability

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

runs the hardware-free checks over that layer -- real captured frames, plus
corrupted-CRC, wrong-length, unterminated and two-frames-in-one-buffer cases.

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
