---
id: I018
kind: instrument
status: trusted
created: 2026-08-22
---

## Instrument

test_hsync_counter — shipping root-counter/VSync phase gate

## Validated by

The Clang-built test drives the shipping Core::mem_r16(0x1F801110) and Timing::vsync paths. A fresh counter must answer zero, one NTSC/PAL field must answer 263/314, a line-248 instruction advance must cross 248 without reaching 263, a wait must reset only the VSync delta, and zero cadence inputs must retain the opposite zero answer. The pre-change framework explicitly returned zero for Timing::vsync(1) and left the MMIO address unmapped, which the positive assertions reject.

## Known failure modes

(none recorded yet)
