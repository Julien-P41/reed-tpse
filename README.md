# reed-tpse

Linux CLI for Tryx Panorama SE AIO cooler display, reverse-engineered protocol, not affiliated with Tryx.

https://github.com/user-attachments/assets/1bc87fa9-cde9-4fd5-ab35-a1a15152c467

> **Fork note:** This is a fork of [fadli0029/reed-tpse](https://github.com/fadli0029/reed-tpse) — all upstream credit to [@fadli0029](https://github.com/fadli0029) for the reverse-engineering work and the original implementation. Changes in this fork:
> - Keepalive daemon now reconnects (and re-applies the saved display state) when `handshake()` fails, instead of silently writing to a dead fd after USB suspend/resume or `/dev/ttyACM*` renumbering. Reconnect events are logged to stderr so they show up in `journalctl` in real time.
> - On-device system telemetry HUD. The cooler firmware renders the overlay natively (up to 3 metrics with position/alignment/color and CPU/GPU marketing badges) — no host-side frame compositing required. CPU/GPU/memory samples are collected from `/proc`, `/sys`, and `nvidia-smi`, then pushed by the keepalive daemon at a configurable cadence (default 5s). See the [HUD](#hud-telemetry-overlay) section below.

## Currently supported features

- Upload images, videos, and GIFs (auto-converts to MP4)
- Set display content and brightness
- Read device status: fan/pump RPM, health warnings, free storage
- Raw protocol passthrough for reaching any endpoint
- List and delete media files on device
- On-device system telemetry overlay (CPU/GPU temp, usage, frequency, voltage; RAM utilization; motherboard/disk temps; date & time)
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
- [ ] Screen Splitting mode support (6-metric layout)
- [ ] Fan curve / pump control (`fanLCDSet`, `turboPump`)
- [ ] Screen behaviour: `displayInSleep`, `rotate`, `waterfallMode`, `power`

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

The core functionality is in `libreed.a`. The CLI links against it. Future GUI (maybe in v2.0.0?) will also link against the same library.

## How it works

This is a TLDR on how it works. I plan to write a blog post on this some time in the future. Will update this section once the blog post is live.

The Tryx Panorama SE exposes:
1. **USB CDC ACM** (`/dev/ttyACM0`): Serial interface for display commands
2. **ADB**: Android Debug Bridge for file transfer to `/sdcard/pcMedia/`

The device requires periodic keepalive (~60s timeout) or it reverts to the default screen. The daemon runs in the background (~1MB RAM, negligible CPU, I bet you could run this on a potato and not notice it) and handles this automatically.

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

## Tested on

| Distro | Kernel | CPU | GPU | Contributor |
|--------|--------|-----|-----|-------------|
| Arch Linux | 6.17.9 | Intel Core Ultra 9 285K | NVIDIA RTX 5080 | [@fadli0029](https://github.com/fadli0029) |
| Bazzite | 6.17.7 | AMD Ryzen 7 9800X3D | Radeon RX 9070XT | [@CRE82DV8](https://github.com/CRE82DV8) |
| CachyOS | 6.19.8-1-cachyos | AMD Ryzen 9 9950X3D | AMD Radeon RX 9070 XT | [@nerddotdad](https://github.com/nerddotdad) |

If you've tested on a different system, feel free to add yours via PR.

## License

MIT

## Contributing

Issues and pull requests welcome at https://github.com/fadli0029/reed-tpse
