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

> **ANSWERED 2026-09-26 — yes, partly. See §10.** The original text of this
> box follows, unchanged, because the way the question was framed shaped the
> test.
>
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

## 10. Measured on V1.0.11 — the transport probe (2026-09-26)

Wave 1 of [roadmap-next.md](roadmap-next.md). Daemon stopped, `logcat` captured
*during* each attempt, one payload per attempt, three attempts total. Device
state recorded beforehand and restored afterwards; the two probe files were
deleted from `/sdcard/pcMedia/`.

### The endpoint exists and answers

```
POST transport 1
FileName=reedprobe.png  FileSize=512
{"type":"media","fileSize":512,"fileName":"reedprobe.png"}

1 200  AckNumber=2
{"state":"success","blockMaxSize":888888888}
```

Byte-identical in shape to the vendor capture in `vendor-protocol.md`,
including the nonsense `blockMaxSize` of 888888888.

### The firmware has typed fields for the whole vocabulary

The device's own parse, from `logcat`:

```
DataHeader{SeqNumber=1, AckNumber=-1, ContentLength=58, ContentType='json',
           FileName='reedprobe.png', FileSize=512, ContentRange=-1,
           Counter=-1, Date=1790453916280, msgId=-1}
```

Not "tolerated and ignored" — parsed into named fields, with `-1` sentinels for
the ones we left out. `msgId` is the device's name for the `Id` header.

There is a receive-file state machine too: every inbound chunk is preceded by
`--isReceiverFile--<bool>--fileSize--<n>--fileBytes.length--<n>`, and the
announce is followed by `---开始文件传输---reedprobe.txt--size--11`
("begin file transfer").

### 🔴 `ContentRange` is a single integer here, not a range

KANALI 2.4.0 parses it with `e.split("-")`, i.e. `start-end`. V1.0.11 does not:

```
org.json.JSONException / NumberFormatException: For input string: "0-10"
```

Sending `ContentRange=0` instead is accepted. **A real v1/v2 protocol
difference**, and the kind that would have cost hours to find by guessing.

### The announce creates the file

After each successful announce, `adb shell ls -l /sdcard/pcMedia/` showed a new
**0-byte** file under the announced name. So `transport` is not a no-op
envelope; it allocates the destination.

### ⚠ But no bytes moved, and that qualifies §5

A second framed `transport` message carrying the payload is rejected — the
device tries to parse the body as JSON:

```
---Exception--88---Value hello of type java.lang.String cannot be converted to JSONObject
```

and `--isReceiverFile--` stays **false** after the announce. So the host→device
byte path is *not* a framed message, and **not a single byte has been moved
over serial by this driver.**

**§5 of this document is therefore half right, and the correction in
`vendor-protocol.md` was worded too strongly.** What is now measured is that
the *mechanism is present in V1.0.11* — typed headers, a receive-file state
machine, a "begin file transfer" log line and a real file created. What is not
shown is that bytes can cross. The original claim it replaced ("there is no
serial file transfer to implement") is still wrong; "a block transfer exists"
is more than has been demonstrated. Both go in the record.

The obvious remaining lever is the `type` field — `"media"` was used throughout
and something else may be what flips `isReceiverFile` to true. That is variant
sweeping, which this device's deferred-settings behaviour makes unsafe to do
casually, so it stops here pending a reason to continue.

### Incidental confirmations

- `SeqNumber=1` out came back `AckNumber=2` — wave 0's `AckNumber` correction,
  verified live and independently of the capture that misled the old doc.
- The device self-heals from a malformed exchange: 10 s `MSG_TIMEOUT`, then
  `close` / `open sPort = /dev/ttyGS0`. The `java.io.IOException: Bad file
  descriptor` in the log belongs to that close and is normal.
- Device-side serial port is `/dev/ttyGS0` (USB gadget serial), service
  `com.baiyi.service.serialservice`.

---

## 11. Measured on V1.0.11 — wave 2 (2026-09-26)

### Panel geometry: **2240×1080** (Q3 closed)

```
$ adb shell wm size
Physical size: 2240x1080
$ adb shell dumpsys display | grep -oE 'deviceWidth=[0-9]+, deviceHeight=[0-9]+'
deviceWidth=2240, deviceHeight=1080
```

Three independent readings agree (`wm size`, `dumpsys display` device
dimensions, and its `real` line), density 240.

**The 1760×880 figure carried by the knowledge base is simply wrong.** It is
not a usable-area measurement or a curved-region figure: `1760` and `880`
appear nowhere in `dumpsys display`, nowhere in `getprop`, and nowhere in the
DRM modes. And 2240×1080 is exactly the geometry baked into the v2 firmware's
own default media filenames (`default_01.mp4.h264_2240x1080`), so both
generations use the same panel resolution.

### `cpuStatus`: accepted, does nothing

Both `STATE cpuStatus` and `POST cpuStatus {}` return `1 200` with an **empty
body**. The device parses the frame — `getSerDataByBytes--解析成功` — and it
reaches the generic `---STATE_POST---` dispatch log, then stops. No
handler-specific line follows, where `transport` produced
`---开始文件传输---` at the same point.

So the endpoint is *routable* but has no behaviour behind it on V1.0.11, or
wants a payload shape we do not know. Per this repo's own rule, an empty body
with 200 means the endpoint took no action — it is not an error and not proof
of success. KANALI 2.4.0 declares `cpuStatus` in its request factory but no
call site passes that literal, so it may simply be a v2 endpoint declared for
both generations.

### `recovery`: documented, deliberately not sent

Present in KANALI 2.4.0's factory. **Not fired, and not to be fired casually.**
On a Rockchip device an endpoint named `recovery` most plausibly reboots into
the recovery partition; the v2 flashing code in the same bundle lists
`recovery` as a partition and drives an external Rockchip tool. The downside
risk is a cooler that needs a physical reseat or a reflash, against no
information worth having.

If it is ever worth settling: do it with the panel already in a known-bad
state, not on a working machine.

### `DELETE` and `GET` already reach the wire

The roadmap claimed this needed "one enum value and one branch in the request
builder". **Wrong — there is no enum.** `build_frame` takes the method as a
plain string and writes it verbatim, and `raw` passes its argument straight
through, so `raw DELETE <endpoint>` has always been possible. The gap was
documentation: the usage text said "METHOD is POST (write) or STATE (read)".

Now named in the usage text, and pinned by a test so that tidying the method
into an enum cannot quietly remove the capability.

### A fork-free lock poll is possible — half verified

`/run/systemd/sessions/<id>` is world-readable (`-rw-r--r-- root:root`) and
carries `USER=`, `ACTIVE=`, `STATE=`, `SERVICE=`. Reading it costs no fork,
where `loginctl` costs two processes per poll.

⚠ **Only the unlocked case is verified.** While unlocked the file contains no
`LOCKED_HINT` line at all, so the locked case presumably adds
`LOCKED_HINT=1` — presumably, because confirming it means flipping the hint,
which makes the running daemon drive the panel. Not assumed, not implemented.

Settling it costs one command and one panel transition:
`busctl call org.freedesktop.login1 /org/freedesktop/login1/session/_3<id> org.freedesktop.login1.Session SetLockedHint b true`
sets the hint **without** locking the screen. Still to do — reading the file
while the hint is set is the whole of it.

### `lock-screen` does change the panel, and it goes black

The `SetLockedHint` trick above was run (by Julien, since service control here
needs his password):

| | Panel |
|---|---|
| `SetLockedHint b true` | **black** |
| `SetLockedHint b false` | media returns |

Two things follow.

**The lock branch works.** Detection, the power event, and the reverse all fire.
Which leaves the **10 s poll** as the explanation for the original report — "the
lock screen does not change the aio display, but the unlock restart the video":
lock, look, see nothing because the deadline has not arrived, unlock, and the
daemon catches up. A latency complaint, not a broken branch. `lock-display` now
states that window.

**`--showStandby--` does not mean "standby animation".** The panel went *black*,
not to the standby clip, with `display_in_sleep: false` and the daemon still
connected and handshaking. `firmware-notes.md` documented `displayInSleep: false`
as giving black **after the ~60s disconnect timeout**; it also governs what a
`lock-screen` event renders while the host is fully connected. So the log line
means "entered the standby state" and `displayInSleep` decides whether that
state shows the animation or nothing. Corrected there.

⚠ This does **not** re-open the earlier `shutdown` finding. That was retested
with `displayInSleep` applied fresh and produced the animation; the variable is
`displayInSleep`, not which of the three events was sent.

---

## Open questions

| # | Question | How to settle it |
|---|---|---|
| ~~Q1~~ | ~~Does V1.0.11 implement `transport`/`transported` block transfer?~~ | **Answered §10: the endpoint and the headers yes, moving bytes no.** |
| Q1b | What `type` value flips `isReceiverFile` to true? | Variant sweep — unsafe here without a reason. Look for `type` in a v1 capture first. |
| Q2 | Does V1.0.11 accept `ContentType: png`? | Only after Q1 is yes. |
| ~~Q3~~ | ~~What is our panel's actual framebuffer geometry?~~ | **Answered §11: 2240×1080.** The KB's 1760×880 is wrong. |
| Q4 | Do `cpuStatus` and `recovery` exist in V1.0.11? | **Partly answered §11.** `cpuStatus` routes and does nothing. `recovery` deliberately not sent. |
| Q7 | Does `/run/systemd/sessions/<id>` gain `LOCKED_HINT=1` when locked? | §11 — set the hint, then read the file. Worth it only if the 10 s latency is to be reduced. |
| Q5 | Is there a v2 firmware for `cm01` hardware, or is v2 a new board? | Nothing in hand answers this. The SoC matches; the USB identity does not. |
| Q6 | Does KANALI 2.4.0 actually drive a v1 device, or only enumerate it? | Would need 2.4.0 running against our cooler with a capture. |

## What this does **not** say

- That our cooler can be upgraded to v2. Nothing here supports that, the USB
  identity differs, and a bad flash bricks the panel.
- That any v2 payload shape works on V1.0.11.
- That the transport protocol is usable for live overlays. It is not — see §5.
