---
id: 149
title: Guest time advances at host execution rate, not field rate, so drive-paced CD delivery runs fast
status: open
symptom: A title whose Setmode selects 150 sectors/s receives its streaming sectors at 304.8 sectors/s, so XA audio is produced twice as fast as it is consumed and the stream overruns its own video
tags: timing,cdrom,xa,accuracy,emulated-time
created: 2026-08-23
updated: 2026-08-23
---

## Measurement (Toy Story 2, `toy2fmv` STR streams)

Toy Story 2's guest FMV overlay issues `Setmode 0xC0` — 2x speed plus XA-ADPCM — so the drive
should deliver **150 sectors/s**, one audio sector per 16 LBAs. Measuring the delivered stream:

- drive sector period: **225,792 CPU ticks** — exactly `33,868,800 / 150`, the correct 2x period;
- measured delivery: **304.8 sectors/s**, i.e. almost exactly 2x the selected rate;
- audio interleave was correct (16 LBAs) and the per-sector frame count was correct (2016 frames at
  18900 Hz), so routing and decode were not at fault.

## Root cause

`EmulatedTime` is advanced by executed guest instructions, and a display-field boundary only ever
pulls it *forward to* the next boundary (issue 7). When the host executes a great deal of guest
code between fields — as a title's unadapted streaming loop does while polling a status register —
instructions are counted at the host's execution rate, which runs well ahead of real time. The
emulated clock therefore passes the next field boundary long before the display has presented that
field, and the sector deadline is compared against a clock that has already overshot.

The consequence is a production/consumption imbalance rather than a visible stall: the SPU's audio
pull stays field-paced while the drive keeps producing, so the XA ring saturates and overflows while
the video sectors the guest is waiting for arrive at twice their nominal spacing. The guest's
"wait for the next sector" loop still completes, because it completes sooner, which is what hid the
defect behind a plausible-looking picture.

## Why it is not fixed here

Toy Story 2's issue 22 was fixed at the title's own owner (the guest's unadapted streaming player
replaced by psxport's native movie owner, `runtime/psx/movie/native_fmv.cpp`), which is the correct place
for it: the guest loop is title code and instant CD is a standing product design. Clamping the
emulated clock inside this change was tried and reverted, because it restored drive timing — the
thing the product deliberately does not do.

The framework defect is real and title-independent: **guest time must not run ahead of the display
field rate.** A fix belongs in `EmulatedTime` (issue 7's owner) as a bound on how far instructions
may advance the clock within one field, and it will change drive pacing for every title, so it needs
its own change with its own measurement. Acceptance: a title selecting 150 sectors/s measures
150 sectors/s delivered, with the XA ring steady and not pinned at its ceiling.

## Also still open

A clock that never runs ahead makes wall-clock measurements meaningful; until then every headless
timing and A/V-sync figure for a streaming title is a statement about the host, not the console.
