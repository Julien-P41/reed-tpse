# The vendor protocol, as captured

Every payload below was read off the wire from **KANALI 1.2.1** talking to a
**Panorama 360 ARGB on firmware V1.0.11** — 20 USBPcap captures, one UI action
each. Where this contradicts something worked out by reading the firmware, the
capture wins and the old conclusion is marked.

Anything *not* in this file was not captured, and several such endpoints
(`waterfallMode`, `reboot`, `recovery`, `turboPump` as a write) have no UI
control in 1.2.1 to trigger them.

---

## Framing and headers

```
0x5A | escape(LEN_HI LEN_LO + message + CRC) | 0x5A
LEN  = len(message) + 5      CRC = sum(LEN + message) & 0xFF
escape: 0x5A -> 0x5B 0x01 , 0x5B -> 0x5B 0x02
```

Requests and responses differ in their headers, which we had wrong:

```
host   ->  POST brightness 1\r\nSeqNumber=463\r\nDate=1787078658098\r\n
           ContentType=json\r\nContentLength=12\r\n\r\n{"value":72}
device ->  1 200\r\nAckNumber=463\r\nContentLength=0\r\nContentType=json\r\n\r\n
```

- The host numbers its own frames with **`SeqNumber`** and stamps **`Date`**
  (epoch ms). Only the device replies with `AckNumber`, echoing the SeqNumber.
  We send `AckNumber` on requests — i.e. we format requests like responses. It
  works, but it is not what the vendor does.
- The version token is `1`, and a response's first line is `1 <status>`.
- `conn` and `config` get **no response at all**. The first ACK in every
  capture belongs to the frame after them.

## Telemetry: `STATE all`, not `POST all`

The host pushes its PcInfo blob once a second as a **`STATE all`** *request
body*, and the device answers the same exchange with its status:

```
>>> STATE all 1   {"network":{...},"memory":{...},"cpu":{...},"gpu":{...},
                   "disk":{...},"fans":[...]}
<<< 1 200         {"status":{"fanLCD":"2040","turboPump":"2940"},
                   "warning":"[{\"description\":\"No ERROR\",\"type\":\"Fan LCD\"}]",
                   "availableStorage":2872836096}
```

One call both feeds the HUD and reads fan/pump RPM. We send `POST all` for the
push and `STATE all` for the read as two separate calls; both work, but this is
why the vendor never needs a separate "latch" after changing a fan setting —
telemetry is always in flight.

---

## `config` — the whole device in one frame

Sent immediately after `conn`, on every connect:

```json
{"temperature":"Celsius",
 "waterBlockScreen":{
   "enable":true, "displayInSleep":false, "brightness":100, "rotate":270,
   "id":{"id":"Customization","screenMode":"Full Screen","playMode":"Shuffle",
         "ratio":"2:1","media":["a.mp4","b.mp4"],
         "settings":{"color":"#dcdcdc","align":"Left",
                     "filter":{"value":null,"opacity":25},"badges":[]},
         "sysinfoDisplay":[]},
   "fanLCD":{"mode":"Smart Mode","smartMode":[[0,10],[10,20],[30,30],[50,40],
             [65,55],[80,70],[90,100],[100,100]],"fixedMode":40}},
 "spec":{"cpu":"Intel Core i9-13900K","gpu":"NVIDIA GeForce RTX 4090"},
 "turboPump":false}
```

Brightness, sleep, rotation, media, play mode, HUD, filter, fan, CPU/GPU names
and pump — atomically. `spec` and `temperature`, which we send as standalone
commands, are only ever seen folded in here.

This is the clean fix for the post-connect startup race: one frame instead of
five, applied before anything can race it. **Not yet implemented.**

## `rotate` — Mirror Mode, and it works

```
POST rotate {"degree":90}    ->  200. Nothing happens yet.
                                 The value is STORED.
   ... at the next device restart, the panel comes up rotated.
```

Only two values are used: **270 is this unit's baseline, 90 is mirrored** —
180° apart, matching the vendor's description ("a mirror image of PANORAMA
screen for users with a left-mounted chassis").

**`rotate` does not restart the device.** Tested directly on V1.0.11:
`rotate--90` is dispatched, the panel does not move, USB does not drop — and
after an `adb reboot` the panel comes up 180° over. The capture *looks* like
the device restarts on receipt because KANALI re-enumerates right afterwards,
but no `reboot` command appears anywhere in the traffic; the app bundles
`adb.exe`, so it is almost certainly rebooting the cooler itself. That is what
its "PANORAMA water cooling will restart upon confirming" dialog is doing.

⚠ **This still supersedes the original conclusion that `rotate` is
unimplemented.** It works; it is just deferred. Watching Android's `mRotation`
for a live transform could never have seen it.

⚠ Because 270 is baseline, a "neutral-looking" `degree: 0` or `180` leaves the
panel **90° out**. A panel found rotated after a power cycle is far more likely
to be a stored `rotate` value than waterfall mode, which moves only the sysinfo
overlay.

## `fanLCDSet` — tiers are (duty, curve) pairs

```json
{"mode":"Smart Mode"|"Fixed Mode",
 "smartMode":[[0,10],[10,20],[30,30],[50,40],[65,55],[80,70],[90,100],[100,100]],
 "fixedMode":40}
```

- No `speed` field. The device's model carries one; KANALI never sends it.
- **Both** the curve and a numeric `fixedMode` go out every time, in either
  mode. A non-numeric `fixedMode` coerces to 0 and stops the fan.
- `smartMode` is 8 `[°C, duty%]` points, ascending in both axes, first at
  temp 0, last at `[100,100]`.

| Tier | `fixedMode` | curve |
|---|---|---|
| Low Speed | 40 | `[0,10] [10,20] [30,30] [50,40] [65,55] [80,70] [90,100] [100,100]` |
| Mid Speed | 60 | `[0,10] [10,20] [30,35] [50,50] [65,75] [80,80] [90,100] [100,100]` |
| High Speed | 80 | `[0,10] [10,20] [30,50] [40,70] [55,85] [70,90] [90,100] [100,100]` |
| Full Speed | 100 | `[0,10] [10,20] [30,70] [40,100] [65,100] [80,100] [90,100] [100,100]` |

A user-dragged curve came out as
`[[0,10],[10,17],[29,24],[52,36],[69,49],[82,67],[94,91],[100,100]]`, so the
points are freely editable within those constraints.

## `waterBlockScreenId` — media, presets and splitting

Custom media, full screen:

```json
{"id":"Customization","screenMode":"Full Screen","playMode":"Shuffle",
 "ratio":"2:1","media":["a.mp4","b.mp4"],
 "settings":{"color":"#dcdcdc","align":"Left",
             "filter":{"value":null,"opacity":25},"badges":[]},
 "sysinfoDisplay":[]}
```

A preset — same command, `id` swapped, no `ratio`, no `media`:

```json
{"id":"Pre-set 12: Cyber Bunker",
 "settings":{"color":"#000000","align":"Left",
             "filter":{"value":"Rain","opacity":50},"badges":[]},
 "sysinfoDisplay":[]}
```

Screen Splitting — two zones as parallel arrays:

```json
{"id":"Customization","screenMode":"Screen Splitting","playMode":"Single",
 "media":["left.png","right.png"],
 "settings":[{...},{...}],
 "sysinfoDisplay":[[],[]]}
```

Notes:
- There is **no `Type` key**. We were sending `"Type":"Custom"`; it is ours.
- `settings.position` is also ours — the vendor sends only `align`.
- KANALI sends this **once**, not twice. It does send a `mediaDelete` sweep
  immediately before, every time.

### `playMode`

Three values — `"Single"`, `"Shuffle"`, `"Loop"` — top-level, beside `media`:

| Capture | playMode | media |
|---|---|---|
| shuffle | `Shuffle` | 2 files |
| single | `Single` | 1 file |
| loop | `Loop` | 2 files |

`media` is a flat array, so a playlist is simply more than one entry. The
device's own filename rules accept `mp4`, `png` and `gif`, and the splitting
capture used two `.png` files, so a playlist is not video-only.

### The 14 presets

Cooling delivery · Migration · Quantum time capsule · Exo-Ecologies · Racing ·
Shuttle · Gift of TRYX · Sweet Cool Acrylic · Catch Me If You Can ·
The Battle of Ice and Fire · Oasis · Cyber Bunker · Edge Of Dream ·
Thermal Energy Prohibited

## `preset` — the HUD command

Not the screen presets above, despite the name. This is how the overlay is
configured, carrying styling and metrics together:

```json
{"settings":{"color":"#dcdcdc","align":"Left",
             "filter":{"value":null,"opacity":50},
             "badges":["CPU Badge","GPU Badge"]},
 "sysinfoDisplay":["CPU Temperature","GPU Temperature","Memory Frequency"]}
```

`POST sysinfoDisplay {"items":[...]}`, which we use, is never sent by KANALI.
Ours works, but it cannot carry colour and badges in the same frame.

Filters seen: `"Rain"`, `"Smoke"`, `null`, with `opacity` 0-100.

## `mediaDelete` — two opposite modes

```json
{"type":"custom","include":["one.png"]}          // delete these
{"type":"custom","exclude":["a","b","c","d"]}    // delete everything else
```

`exclude` carries the app's whole media library and is sent after every upload
and before every screen-config change. `type` is required.

## Media upload — the vendor uses adb too

```
POST transport   {"type":"media","fileSize":3329147,"fileName":"....png"}
   ... file bytes, NOT on this endpoint ...
POST transported {"md5":"todo","fileName":"....png"}
<<< {"state":"success","blockMaxSize":888888888}
```

The two JSON frames are 0.5 s apart, which is nowhere near long enough to move
3.3 MB at 115200 baud — and the serial endpoint carries only ~60 KB across the
whole capture. The file goes out on a **second bulk endpoint on the same USB
device**, and its first packets are:

```
OPEN....  sync:  WRTE....  STA2+.../sdcard/pcMedia/2026-08-...  OKAY....
```

That is the **adb wire protocol**, pushing to `/sdcard/pcMedia/` — the same
path and the same mechanism `reed-tpse upload` already uses. KANALI bundles
`adb.exe` for exactly this.

So there is no serial file transfer to implement, and the adb dependency is not
something the vendor avoids. `transport`/`transported` are announce/confirm
envelopes around an adb push; we skip them deliberately, because sending them
would make `upload` need the serial port, which the daemon holds with
TIOCEXCL. (KANALI does not compute the MD5 either — it sends the literal
string `todo`.)

## Everything else, verbatim

```json
POST conn              (empty body, no response)
POST waterBlockScreen  {"enable":true|false}      // panel power
POST brightness        {"value":0-100}
POST displayInSleep    {"enable":true|false}
POST power             {"event":"lock-screen"|"unlock-screen"|...}
```

## Command frequency across all 20 captures

| n | command | | n | command |
|---|---|---|---|---|
| 420 | `STATE all` | | 6 | `conn`, `waterBlockScreen`, `mediaDelete` |
| 18 | `waterBlockScreenId` | | 5 | `brightness` |
| 16 | `preset` | | 3 | `config` |
| 9 | `fanLCDSet` | | 2 | `power`, `rotate`, `displayInSleep`, `transport` |

Never sent: `sysinfoDisplay`, `spec`, `temperature`, `turboPump`,
`waterfallMode`, `reboot`, `recovery`, `disconn`, `status`, `fanLCD`.
