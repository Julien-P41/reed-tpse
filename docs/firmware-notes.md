# Firmware notes

Measured behaviour of the Panorama's own firmware -- what it does, what it
ignores, and what had to be worked around. Payload shapes live in
[vendor-protocol.md](vendor-protocol.md); this file is about how the device
behaves once those payloads arrive.

Everything here was measured on a **Panorama 360 ARGB, firmware V1.0.11,
hardware V1.1** (`productId cm01`). Treat the numbers as model-specific.

---

## Startup race

The device accepts a serial connection before its UI app is ready -- adbd and
the CDC-ACM link come up around 12s before HomeUI does. Anything pushed into
that window is lost: the media never appears (black panel) and the fan is left
in Smart Mode with a null curve, which the firmware evaluates as **0 RPM** and
re-evaluates as 0 on every telemetry push.

The daemon therefore waits for the UI app before applying anything, polling
`pidof` over adb for up to 45s after each connect. Where adb is unavailable
there is nothing to poll, and it falls back to re-applying once, 20s after
connecting.

If you script around a device restart, wait for the app rather than the
transport -- `adb wait-for-device` returns when adbd is up, which is about 12s
too early:

```bash
until adb shell pidof com.baiyi.homeui.tkcfanhomeui >/dev/null 2>&1; do sleep 2; done
```

### If you script your own boot or shutdown sequence

Playing a clip of your own around boot or shutdown means taking the serial port
from the daemon, since it holds the port exclusively. Two consequences are easy
to miss:

- **Settle the fan yourself.** A cold boot leaves the controller on its
  firmware default, which runs full speed. The daemon normally fixes that on
  connect -- but if you have stopped it to take the port, nothing else will for
  the length of your clip. Send `reed-tpse fan <tier>` at the start.
- **Hold the connection for the whole clip.** The device falls back to its own
  standby animation about 60s after the last handshake, so a one-shot
  `display` will be replaced part-way through.

Hand the port back when you are done, and remember that a system-scope daemon
reads root's config, not yours.

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
**`rotate` breaks it** -- there is no rotation callback, yet the endpoint
stores a value that turns the panel at the next start. A live check cannot see
a deferred setting, so treat the list as a hint about the live path, not as
evidence that an endpoint is inert.

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
show up in the window being measured. Deferred settings are the blind spot of
every live measurement in this document.

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

**Whether it does anything is unverified.** Across seven payload shapes the
layout was pixel-identical before and after, and `MainActivity`'s own
`--onWaterfallModeChange--` never fired.

That result was once read as a measurement artefact, on the strength of a panel
that came back rotated 90° clockwise after the cooler lost power. That evidence
does not hold: `rotate` is now known to store a value and apply it at the next
start, it was sent during the same testing, and 0 or 180 leaves this panel 90°
off upright. Waterfall mode moves the sysinfo overlay, not the media -- a whole
rotated picture is `rotate`'s signature, not this one's.

So what is known is only what the firmware contains, and the payload key
remains unknown. `{"enable":false}` alone changed nothing; `enable`, `value`,
`mode`, `waterfallMode` and `open` sent together as `false` and `0`, followed
by a reboot, left the panel upright -- but so would a `rotate` correction sent
in the same batch. This handler does not validate its input, so it never names
its field in an exception, and the setting cannot be read back: `screencap`
shows normal orientation, Android's `mRotation` stays `0`, nothing is logged at
boot, and the value lives in the app's own `/data`, which needs root to read.

KANALI 1.2.1 has no UI control for it, so no capture of the vendor sending it
exists either. Settling this needs a build of the app that exposes the toggle.

`adb reboot` restarts the AIO alone -- no PC shutdown needed, and this hardware
records `reboot,shell` in `persist.sys.boot.reason.history` from previous ones.
It is an ordinary Android reboot, not the `adb root` that is known to drop the
USB link.

⚠⚠ **Do not sweep payload variants at this device.** Several settings here are
stored and applied only at the next restart, so a sweep that appears to do
nothing can leave a change armed, to surface whenever the cooler is next
power-cycled -- and with no way to read any of them back, the armed value is
invisible until it takes effect. The rule the vendor's own UI states for Mirror
Mode ("the cooler will restart upon confirming")
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

| Event | Effect |
|---|---|
| `lock-screen` | panel leaves the media for the standby clip |
| `shutdown` | same -- standby clip |
| `suspend` | same -- standby clip |
| `unlock-screen` / `resume` | hides standby, restores the media |

⚠ **`shutdown` does not blank the panel.** This table used to claim it did,
"with `sleep-display on`", and that a black screen was what distinguished it
from `lock-screen`. Retested end to end -- `displayInSleep` applied fresh and
confirmed on the wire, then `shutdown` sent -- and the panel ran the standby
clip, exactly like `lock-screen`. All three events log `--showStandby--` and
all three do the same thing.

Two ways to actually get a black panel:

- `waterBlockScreen {"enable":false}` (`reed-tpse screen off`) blanks it
  immediately and unconditionally.
- `displayInSleep` set to **false**, then let the ~60s disconnect timeout
  expire. See below -- the field reads backwards.

### `displayInSleep` reads backwards

It is the device's own "display something while the host is asleep", not
"blank the display":

| `displayInSleep` | after the host stops handshaking |
|---|---|
| `true` | the firmware's standby animation |
| `false` | black |

Measured both ways through a full disconnect timeout. It was documented as the
inverse for months, which is why a tool setting it to `true` in order to get a
black panel produced the animation instead. `reed-tpse sleep-display` passes
the value straight through, matching the vendor's own toggle: `on` gives the
animation, `off` gives black.

Either way the switch takes about 60s -- the device waits out its own
disconnect timeout (`--onDisConnect--`) before changing what it shows.

⚠ **Standby is sticky.** Once `shutdown`, `lock-screen` or `suspend` has put
the panel there, only `unlock-screen` or `resume` takes it off. Re-sending
media does not: the device accepts the new path, logs `setLayout1Path`, and
carries on showing standby over it -- so the media looks applied in every log
while the panel says otherwise. That makes
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
