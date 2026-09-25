# Plan for the next version

Written 2026-09-22, from the findings in
[firmware-v2-dissection.md](firmware-v2-dissection.md).

The current version is finished in the sense that matters: every finding from
the audit is closed, the suite is 182 checks across six binaries, CI runs on
every push, and the daemon has been running against the cooler for weeks. This
plan is about what the v2 dissection *opened*, not about unfinished business.

**Read this first: most of the interesting work is gated on one cheap
experiment.** Wave 1 is that experiment. If it comes back negative, waves 3 and
4 evaporate and the plan is much shorter. Do not build anything in wave 3 until
wave 1 has answered.

---

## Wave 0 — corrections (no device, no risk)

Three documented facts are wrong. They cost nothing to fix and they are
actively misleading anyone who reads the docs, including future me.

**Status: done, 2026-09-25.**

| # | Change | File | Evidence |
|---|---|---|---|
| 0.1 | The panoRK stack is **RK3566**, not RK3568 — the same SoC as ours | *not in this repo* | `/info/rockchip_config`: `RK_CHIP="rk3566"` |
| 0.2 | v2 USB identity is **`0x391A:0x1011`** (printer class), not `0x6666:0x0066` | *not in this repo* | `/usr/bin/usbdevice`; `0x0066` is the fallback branch |
| 0.3 | The protocol has **four** methods (`GET/POST/STATE/DELETE`) and twelve headers, not two and four | `vendor-protocol.md` | KANALI 2.4.0 enums |
| 0.6 | **`AckNumber` is not an echo** — doc said it was; the code already knew better | `vendor-protocol.md` | measured: `SeqNumber=1` out, `AckNumber=2` back |

⚠ **0.1 and 0.2 were mis-filed when this plan was written.** Both wrong claims
live in the knowledge base's `aio-lcd-setup.md`, not in this repository — a
search here for `RK3568` or `0x6666` returns nothing outside the dissection
doc. They are handover items for the KB steward, not commits. The plan asserted
a file list without checking it; that is the same failure mode the dissection
doc is full of corrections for.

**0.6 was not in the original plan.** `vendor-protocol.md` said the device
replies with `AckNumber` "echoing the SeqNumber", on the strength of a capture
where both read `463`. The code contradicts it in three places with a
measurement — `SeqNumber=1` out, `AckNumber=2` back on a fresh connection — and
warns that correlating on it "was tried and broke every command". The document
was the stale copy. Doc/code drift of exactly the kind an audit is supposed to
catch, and the last one did not.

**0.4 — restate the media-transfer conclusion.** `vendor-protocol.md` says
media moves over adb *because no serial file transfer exists*. The reasoning
was timing-based and the timing was right; the conclusion was not. Rewrite it
as: a block transfer exists (§5 of the dissection), it runs at 1 KB blocks over
115200, and that is why the vendor uses adb for video. The correction goes in
the record — the wrong version has been load-bearing for months.

**0.5 — record `waterfallMode`'s answer** (dissection §6) and close the README
TODO item that has been open since the beginning. It is a host-side rotation
pair, not a device flag. Note explicitly that this is v2's schema and has not
been shown to apply to V1.0.11, so the TODO closes as *"answered in principle,
not reproducible on our firmware"* rather than *done*.

Verification: docs only. `git diff` review, no build needed.

---

## Wave 1 — the one experiment that decides the rest

**Question:** does V1.0.11 implement `POST transport` / `POST transported`?

This is the hinge. Everything in waves 3 and 4 depends on the answer, and the
test is small.

### 1.1 Add `raw`-level support for the transfer headers

`raw` already sends arbitrary method/endpoint/JSON. It cannot set `FileName`,
`FileSize`, `FileBlockId` or `ContentRange`, so extend the header writer to
carry them. Library-only change, no new command surface.

*Verify:* a protocol test asserting the framed bytes for a request carrying all
four headers. Sabotage it — drop a header from the writer, confirm the test
fails.

### 1.2 Announce a tiny file and listen

```
POST transport  {"type":<n>,"fileSize":<small>,"fileName":"probe.png"}
```

Three outcomes, all informative:

| Device response | Meaning |
|---|---|
| A success reply | The transfer state machine exists → proceed to 1.3 |
| An error reply | The endpoint is known but rejects us → payload shape is wrong, keep probing carefully |
| Nothing | Endpoint unimplemented → **stop; waves 3 and 4 are dead** |

The vendor waits **300 ms** for this reply, so a no-answer verdict is quick.

⚠ **Constraints that are not negotiable.** `firmware-notes.md` records that
some settings *store now and apply at next start* — a blind sweep can leave a
change armed and invisible. So: one payload at a time, `logcat` captured
**during** the attempt (the device's log buffer holds as few as 7 frames), and
no variant sweeping. `type` is an unknown integer; find its meaning before
guessing at it.

### 1.3 Only if 1.2 succeeds — send one small file

A few hundred bytes, in 1 KB blocks, then `transported`. Confirm it lands (adb
`ls` of the media directory is the cheap check). Do **not** reuse the name of
anything the running playlist holds — pushing bytes over a file in use was a
test-design mistake once already and produced a black panel.

*Deliverable either way:* the answer written into the dissection doc under Q1,
with the exact frames sent and received. A negative result is a real result and
should be recorded as loudly as a positive one.

---

## Wave 2 — small wins that stand on their own

Independent of wave 1. Do these whatever the answer.

### 2.1 `cpuStatus` and `recovery` — declare, do not fire

Both are in KANALI 2.4.0's dispatch factory and neither is in this driver.

- `cpuStatus` — probe it the way 1.2 probes `transport`. Low risk; it reads as
  a telemetry variant.
- `recovery` — **do not send it.** On a Rockchip device `recovery` plausibly
  reboots into the recovery partition. Document it as known-to-exist and
  deliberately untried. Add it to `raw`'s accepted endpoint list if that list
  is restrictive, so a future investigator can fire it on purpose rather than
  by accident.

### 2.2 `DELETE` as a method

One enum value and one branch in the request builder. Cheap, and it stops
`raw` from being unable to express a quarter of the protocol. No endpoint is
known to accept it yet — ship the capability, not a command.

### 2.3 Measure the panel

Q3 in the dissection: the KB says 1760×880, v2's own media says 2240×1080, and
nobody has measured ours. `adb shell wm size` or the equivalent on this
firmware settles it in one command. It has been wrong-or-unverified in the
documentation for a year.

### 2.4 Make `lock_media` discoverable

The lock-screen "bug" chased last session was a missing config key: without
`lock_media` the daemon sends bare power events, so locking appears to do
nothing and unlocking restarts the video. That is correct behaviour and
terrible discoverability. Either `lock-display --help` should say what happens
when it is unset, or `daemon status` should report which lock mode is active.

Also reconsider the lock poll. It runs on `keepalive_interval` (10 s here);
a lock-unlock cycle shorter than that is invisible. The comment already
describes the fix — cache the answer and re-run `loginctl` on the deadline
rather than reverting to the 1 s poll that forked 170k processes a day.

---

## Wave 3 — *conditional on wave 1 succeeding*

If `transport` works on V1.0.11:

### 3.1 `upload --serial`

A second upload path that does not need adb at all. Value is not speed — it is
**dependency removal**: adb is the fragile half of this driver (server dies,
device handle lost, multi-device ambiguity, the `adb root` landmine). A serial
path for small files means presets, lock screens and still images work on a
machine with no `adb` installed.

Gate it on size. Above ~200 KB the wait is absurd; refuse and point at adb.

### 3.2 Reopen the still-image question, honestly

With `ContentType: png` available, a host-rendered still could reach the panel
without adb. This does **not** revive the live-overlay feature — the filename
cache still costs a black frame per new name, and a full-panel PNG at 11 KB/s
is tens of seconds. Anything that changes is still out.

What it might enable is a *composed static screen*: render once on the host,
send once, show it. Julien's argument against the feature stands for anything
an image editor can do — so the only version worth building is one that
composes something the user cannot easily draw by hand, and even then it is
marginal. **Evaluate, do not assume.** If it does not clear that bar, write
that down and close it for the second time.

---

## Wave 4 — v2 support, and why it is probably not worth starting

This is the part to be sceptical about, so it is stated plainly.

Supporting a v2 cooler would mean: a new transport (USB printer bulk, not
CDC-ACM), a new device-discovery path (`0x391A:0x1011`, not a tty), and a new
payload schema (`rkConfig`/`displayConfig`/`workConfig`, not
`waterBlockScreen`). That is not an extension of this driver. It is a second
driver sharing a CLI.

Three reasons to hold off:

1. **No v2 hardware.** Every line would be written blind against a JS bundle.
   This project's own history says what that produces — the dissection doc
   opens with three corrections to conclusions drawn exactly that way.
2. **No evidence our cooler can become one.** Same SoC, different USB identity,
   different OS, and a bad flash bricks the panel.
3. **Nothing asked for it.** There is one cooler on this machine and it is v1.

**Recommended position: do not build v2 support.** Keep the dissection doc
current so that if a v2 device ever appears the groundwork is there, and spend
the effort on waves 0–2, which improve the thing that actually runs.

If that changes, the first step is not code — it is one USB capture of KANALI
2.4.0 driving a real v2 device. Without it, everything after is guesswork.

---

## Sequencing

```
Wave 0  docs corrections          ──┐ no device, do anytime
Wave 2  small wins                ──┤ independent
                                    │
Wave 1  transport probe  ───────────┴──► decides:
                                            ├─ yes → Wave 3
                                            └─ no  → stop, record it
Wave 4  v2 support        not recommended; revisit only with hardware
```

Order: **0 → 1 → 2 → (3)**. Wave 0 first because it is free and stops the wrong
facts propagating. Wave 1 next because it is the only thing blocking a
decision. Wave 2 can fill any gap.

## Definition of done, per wave

| Wave | Done when |
|---|---|
| 0 | The three corrections and the transfer restatement are in the docs, and the `waterfallMode` TODO is closed with its answer |
| 1 | Q1 in the dissection doc has a measured yes or no, with the frames quoted |
| 2 | `cpuStatus` probed, `recovery` documented-not-fired, `DELETE` buildable, panel geometry measured, lock mode discoverable |
| 3 | `upload --serial` works for a small file with adb uninstalled, or the wave is closed with a reason |
| 4 | Not started |

Every code change keeps the existing bar: builds standalone with zero warnings,
tests added for anything with a rule in it, and each test sabotage-verified —
*if a test cannot fail, it is not a test*.
