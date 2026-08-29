# Two coolers on one machine

reed-tpse assumes exactly one Panorama. Nothing enforces that assumption, and
nothing warns about it, so this is what would actually happen on a dual-socket
board with a cooler on each CPU — and what it would take to support properly.

Written down because the question came up, not because anyone has tried it.
None of the below is tested; there is only one cooler here.

## What already works

**adb and the serial port address the same physical device.** The two halves —
the CDC-ACM tty and the ADB interface — are separate endpoints on one USB
device, and until recently nothing tied them together: the tty could be pinned
with `--port` while adb quietly drove the other panel, because both coolers
report `product:cm01` and selection took the first match.

They are paired by USB port now:

```
tty  /sys/class/tty/ttyACM0/device  →  .../usb1/1-11/1-11:1.0
adb  adb devices -l                 →  usb:1-11
```

`Adb::bind_to_port()` derives the port from whichever tty is in use and selects
the adb device on the same one. So *if* you pin a port, both halves follow it.

## What would break

**Auto-detection picks an arbitrary one.** `find_device` returns the first tty
that answers, and tries `/dev/tryx-panorama` first — a symlink both coolers
match, so it resolves to whichever udev handled last. Pinning `port` in
`config.json` avoids this, and with the pairing above that pins adb too.

**One state file for both.** `display.json` holds one media list, one
brightness, one fan tier, one HUD. Two coolers would share it, so configuring
the second overwrites the first, and a daemon would apply one panel's settings
to whichever device it connected to.

**One daemon, and the units are mutually exclusive by design.** The system and
user units must not both run, because two readers on one tty split incoming
frames at random. Two coolers need two daemons on two ports — which the current
unit files cannot express.

**The udev symlink collides.** `SYMLINK+="tryx-panorama"` is matched by every
cooler, so the stable name is only stable with one of them.

## What supporting it would take

Roughly, in dependency order:

1. **Per-device state.** Key the state file on the device serial — the
   `BYZL…` string `adb devices` prints, also readable over serial from the
   handshake. `display.json` becomes `display-<serial>.json`.
2. **Per-device udev symlinks.** `SYMLINK+="tryx-panorama-$attr{serial}"`
   alongside the existing one, so each cooler has a stable name.
3. **A templated unit.** `reed-tpse@.service` taking the serial or port as the
   instance name, so `systemctl start reed-tpse@BYZL….service` runs one daemon
   per cooler.
4. **A device selector on the CLI.** `--device <serial>` resolving to a port
   and a state file, with the existing `--port` kept as the low-level escape.
5. **Making `find_device` refuse to guess** when several coolers are present,
   rather than silently picking one.

Steps 1 and 2 are the load-bearing ones; 3–5 are mechanical once state is keyed
per device.

## Why it is not built

No one has reported two of these, dual-socket desktop boards are rare, and
every part of the above would ship untested — this project has been burned
several times by changes made on plausible reasoning without a device to check
them against. The single-cooler path is correct and now provably so; the
multi-cooler path would be speculation with a config-file migration attached.

If you do get a second one, start at step 1 and pin `port` per daemon in the
meantime — that alone makes two coolers usable, just not convenient.
