# Dissecting the v2 stack — working notes

A living record of pulling apart the newer Tryx firmware and host app, and what
it means for this driver. **Append as things are learned; correct in place and
say so.** Everything here is either *measured* (a command was run, output
quoted) or explicitly labelled *unverified*.

Status: **first pass, 2026-09-22.** No v2 hardware in hand. Nothing here has
been tested against the cooler on this machine.

---

## What was examined

| Artifact | Path | Provenance |
|---|---|---|
| v2 firmware images | `~/Downloads/KANALISetup/panoRKUpdate/*.img` | extracted from `panoRKFirmware/firmware.zip` (identical contents) |
| v2 partition manifest | `panoRKUpdate/parameter.txt`, `package-file` | |
| KANALI 2.4.0 host app | `~/Downloads/KANALISetup/KANALI_2.4.0.exe` | NSIS installer, 377 MB |
| KANALI 2.4.0 main bundle | carved `out/main/index-3780cf4b.js`, 1.13 MB | see *Method* |
| Our device, for comparison | live, `reed-tpse info` | Firmware V1.0.11, app 1.4, hardware V1.1 |

### Method

The `.img` files are ext4 and this machine has no `sudo`, so they were read
with **`debugfs`** — read-only, unprivileged, no mount:

```bash
debugfs -R "ls -l /etc/init.d" panoRKUpdate/rootfs.img
debugfs -R "dump /usr/bin/usbdevice out/usbdevice" panoRKUpdate/rootfs.img
```

7-Zip 23.01 no longer extracts NSIS, so KANALI 2.4.0 was opened by hand: the
outer PE yields one `[0]` blob whose header is `NullsoftInst`, with a standard
LZMA properties byte (`5d 00 00 80 00`) at offset `0x2c`. Decompressing from
there with `lzma.FORMAT_ALONE` and a `0xff`-filled size field gives the
installer script; scanning the whole blob for the same signature finds **291
candidate blocks, 143 of which decompress to recognisable files** — 127 PE/DLL,
12 ELF, 1 PNG, 1 ZIP and one **ASAR** at offset `159318977`. The ASAR is the
Electron bundle: 10,745 files, index parsed from its JSON header.

Scripts are in the session scratchpad, not the repo. Re-deriving them takes
about ten minutes.

---

## 1. It is the same SoC — the previous note was wrong

**Correction.** The KB note `aio-lcd-setup.md` and this repo's earlier reading
both say the panoRK stack targets **RK3568, a different product generation**.
It does not. From `/info/rockchip_config` inside `rootfs.img`:

```
RK_CHIP_FAMILY="rk3566_rk3568"
RK_CHIP="rk3566"
RK_KERNEL_DTS_NAME="rk3566-evb2-lp4x-v10-linux"
```

and `/etc/os-release`:

```
RK_BUILD_INFO="root@mo Fri Jun 12 14:41:08 CST 2026 - rockchip_rk3566"
```

**RK3566 — the same SoC as our unit.** `parameter.txt` says
`MACHINE_MODEL: RK3568` because that is the SDK's chip *family* name, which is
where the error came from. A commented-out line in `/usr/bin/usbdevice` reads
`MFG:RK;MDL:RK3566-PRN;CMD:RAW;`, confirming it independently.

This matters: the v2 stack is not a different device class that can be
dismissed. It is a **platform replacement for the same silicon**.

### What it replaces

| | Ours (V1.0.11) | v2 (2.0.5 / `v2.0.1.20260611`) |
|---|---|---|
| OS | Android 11 | **Buildroot 2021.11**, kernel 5.10 |
| UI | `com.baiyi.homeui.tkcfanhomeui` 1.4 | Weston/Wayland |
| Media player | Android stack | `mpv`, `gstd` |
| Packaging | Android OTA (`system.new.dat.br`, SignApk) | raw partition images |
| Partitions | Android set | uboot, misc, boot, recovery, rootfs, oem, userdata |
| Build date | 2025-11-15 (archive) | **2026-06-12** |

Not an OTA. Flashing it would replace the whole operating system.

## 2. USB identity changes completely

From `/usr/bin/usbdevice` in the v2 rootfs:

```
echo 0x391A > idVendor          # 0x38C1 commented out above it
printer)        echo 0x1011;;   #PANO
# printer)      echo 0x1021;;   #PASE
# printer)      echo 0x1031;;   #PAWB
adb-printer)    echo 0x1002;;
acm)            echo 0x1005;;
*)              echo 0x0066;;
```

- **VID `0x391A`**, not `0x18d1` (ours) and not `0x6666`.
- **PID `0x1011`** for the Panorama, via the USB **printer** class with
  `pnp_string = "MFG:RK;MDL:PANO;CMD:RAW;"` — a raw bulk pipe, not CDC-ACM.
- USB product string becomes `PANO` (manufacturer `RK`).

**Correction.** The earlier note records the v2 USB ID as `0x6666:0x0066`.
`0x0066` is the *fallback* branch of `usb_pid()` — the value used when no known
function combination matches. Someone read the default case. The real pair is
`0x391A:0x1011`.

> ⚠ **Consequence for this driver:** on v2 there is no `/dev/ttyACM*`. Our
> entire transport — open the CDC-ACM node, `TIOCEXCL`, 115200 8N1 — does not
> apply. A v2 port means talking to a USB printer endpoint. `acm` (`0x1005`)
> still exists as a selectable function, so a v2 device *could* be configured
> to expose serial, but the shipped Panorama profile is `printer`.

## 3. The v2 firmware carries no vendor application

`rootfs.img` is a stock Rockchip SDK Buildroot image: `/opt` holds only
`unixbench`, `/oem` is empty, `/etc/init.d` has nothing product-specific, and
`oem.img` contains Rockchip's demo media (`SampleVideo_1280x720_5mb.mp4`,
`game_test.gba`). The Tryx application is **not in the firmware package.**

Where the product identity *does* live is `userdata.img`:

```
/default/default_01..06.mp4.h264_2240x1080
/default/screensaver.mp4.h264_2240x1080
/default/start.mp4.h264_2240x1080
/filter        (empty)
/keyboard      (empty)
/user          (empty)
/pwm_backup.ko
```

Two things follow.

- **Media is stored pre-decoded, as raw H.264 with the geometry in the
  filename** — `.mp4.h264_2240x1080`. The device does not hold MP4s; it holds
  elementary streams sized to the panel. This is a strong hint about why our v1
  device caches by *filename*: the name is the cache key because it encodes the
  decode parameters.
- **The panel is 2240×1080** on v2. Our KB note says 1760×880 for v1 and I
  still have not measured ours, so this does **not** settle that question — but
  it does make 1760×880 look more like a usable-area figure than a framebuffer
  size. *Unverified for our hardware.*

## 4. KANALI 2.4.0 still speaks our protocol

The app is Electron, `"author": "TRYX"`, and still depends on `serialport`
(`baudRate: 115200`) alongside `appium-adb`. Its request factory dispatches
thirteen commands, all in the shape we already know:

```
POST  conn          STATE all           POST waterBlockScreen
POST  cpuStatus     POST waterBlockScreenId
POST  brightness    POST rotate         POST recovery
POST  sysinfoDisplay POST displayInSleep
POST  fanLCDSet     POST mediaDelete    POST config
```

plus `POST power`, `POST transport` and `POST transported` constructed
directly rather than through the factory.

Against what this driver implements, the gaps are **`cpuStatus`**,
**`recovery`**, **`transported`** and the `DELETE` method.

> Call sites pass a variable to the factory, so I could not determine from the
> bundle *which* of these are actually sent at runtime — only which are
> declared. Do not read the list as "all of these work".

### The header set is larger than we thought

```
SeqNumber  AckNumber  ContentLength  ContentType
FileName   FileBlockId  FileSize  ContentRange
Counter    Date  Id  Option
```

and the method enum is `GET | POST | STATE | DELETE` — **four**, where our
`vendor-protocol.md` says two.

`ContentType` is an enum: `text, json, xml, png, jpg, gif, mp4, avi, pdf`.

## 5. 🔴 There is a file-transfer protocol over the serial link

This is the biggest finding, and it contradicts a conclusion this project has
been operating on for months.

`vendor-protocol.md` states that media moves over adb because the
`transport`/`transported` frames are half a second apart, far too fast for
megabytes at 115200. **The timing observation stands. The conclusion drawn from
it does not.** A complete block-wise file transfer exists:

```js
// phase 1 — announce
new as(Is.POST,"transport")
  .setBodyContent(kn.Json,{ type, fileSize: stat.size, fileName: basename(path) })
// 300 ms timeout waiting for the reply

// phase 2 — stream
const r = ES.maxFileBlockSize;              // = 1024
const blockCount = Math.ceil(stat.size / r);

// phase 3 — finish
POST "transported"
```

with a `Transport → Transporting → Transported → Idle` state machine (plus
`Error`), and the `FileName` / `FileBlockId` / `FileSize` / `ContentRange`
headers to carry the blocks.

**1 KB blocks at 115200.** Real throughput is roughly 11 KB/s, so ~90 s per
megabyte. That is why video goes over adb — not because the serial path does
not exist, but because it would be unusable for video. For something small it
is entirely practical.

> **Open, and worth a careful test:** does V1.0.11 implement this? If it does,
> `ContentType: png` plus a filename we control is a way to put host-rendered
> pixels on the panel without adb. It does **not** revive the live-overlay
> feature — a full-panel PNG is hundreds of KB, i.e. tens of seconds, and the
> filename cache still forces a black frame on every new name — but it changes
> what "we cannot send pixels" means.
>
> Test it read-only first: announce a tiny file and see whether the device
> replies at all. **Do not sweep variants at this device** — deferred settings
> make blind sweeps dangerous (see `firmware-notes.md`).

## 6. `waterfallMode` is answered — it is a host-side rotation

Open in the KB since the beginning: the firmware has
`onWaterfallModeChange`/`doWaterfallMode` but no payload key was ever found,
and KANALI 1.2.1 has no UI for it.

KANALI 2.4.0 has it, and it is **not a device flag at all**. The host computes
two rotations from two booleans:

```js
displayConfig = { backlightBrightness, backlightEnable,
                  mirror:false, uiRotation:0, mediaRotation:0 }

mirrorMode && waterfallMode → uiRotation=90,  mediaRotation=180
mirrorMode                  → uiRotation=0,   mediaRotation=180
waterfallMode               → uiRotation=90,  mediaRotation=0
```

So **waterfall rotates the overlay 90° and leaves the media alone**; mirror
rotates the media 180°. That matches the KB's own reasoning — *"waterfall moves
only the sysinfo overlay, not the media"* — which was recorded as a correction
to an earlier wrong claim. It now has vendor code behind it.

⚠ This is the **v2** config schema (`rkConfig` / `displayConfig` /
`workConfig` / `userConfig` / `lightConfig`), not ours. It explains the
*concept*; it is not a payload we can send to V1.0.11. Whether v1 exposes the
same thing through some key remains unknown.

The v2 layout engine also exposes `kaleidoscopeSource` / `kaleidoscopeMediaFile`
and a text-layout model (`groupId`, `labelId`, `groupX/Y/Width/Height`,
`BackGround_GardientHorizontal`, `textFont: "roboto-regular"`) — a far richer
overlay system than v1's fixed badge/metric slots.

## 7. The product line is wider than one cooler

Model codes in the bundle: `PANO`, `PASE`, `PAWB`, each with a `…V2` twin, plus
`HICU`, `HOLO`, `STIGI`, `ROTA`, `TURR`. Settings namespaces exist for
`panorama`, `panoramaSE`, `panoramaWB` and the three V2 variants.

So: Panorama / Panorama SE / Panorama WB, two hardware generations each. Ours
is `PANO` v1. Useful mainly as a reminder that payload differences between
sibling products are expected, and that a capture from one is not evidence
about another.

## 8. Firmware flashing is implemented in the app

```js
static DEFAULT_MARKER_ADDR = "0x077ff8";
static PARTITION_ORDER = ["uboot","trust","misc","boot","recovery",
                          "rootfs","oem","userdata"];
```

driven by an external tool path with start/success marker files. Standard
Rockchip `upgrade_tool`/`rkdeveloptool` flow. Note `trust` appears here but not
in `package-file`'s list.

## 9. The new app phones home

Not protocol, but it belongs in the record.

- Vendor API: `https://kanali2-api.tryxzone.com/api`, with routes including
  `/app-firmware/getVersion`, `/app-firmware/getUrl`, `/app-material/query`,
  `/app-software/getVersion` and **`/app-device-usage-logs/save`**.
- Key exchange endpoints `/app-sm/get-sm4-key` and `/app-sm/rsa/get-sm4-key`;
  the bundle depends on `sm-crypto` (SM2/SM3/SM4).
- It calls **`https://ipinfo.io/json`** — public-IP geolocation.
- `node-hid` and `ws` are new dependencies; neither appears in 1.2.1's set.

A device-usage-log upload and an IP geolocation lookup are worth knowing about
before anyone installs 2.4.0 to capture traffic.

---

## Open questions

| # | Question | How to settle it |
|---|---|---|
| Q1 | Does V1.0.11 implement `transport`/`transported` block transfer? | Announce a tiny file, watch for a reply. Read-only, low risk. |
| Q2 | Does V1.0.11 accept `ContentType: png`? | Only after Q1 is yes. |
| Q3 | What is our panel's actual framebuffer geometry? | Unresolved; 1760×880 (KB) vs 2240×1080 (v2 media). Needs a measurement on our unit. |
| Q4 | Do `cpuStatus` and `recovery` exist in V1.0.11? | Send and observe. `recovery` is **not** safe to fire blind. |
| Q5 | Is there a v2 firmware for `cm01` hardware, or is v2 a new board? | Nothing in hand answers this. The SoC matches; the USB identity does not. |
| Q6 | Does KANALI 2.4.0 actually drive a v1 device, or only enumerate it? | Would need 2.4.0 running against our cooler with a capture. |

## What this does **not** say

- That our cooler can be upgraded to v2. Nothing here supports that, the USB
  identity differs, and a bad flash bricks the panel.
- That any v2 payload shape works on V1.0.11.
- That the transport protocol is usable for live overlays. It is not — see §5.
