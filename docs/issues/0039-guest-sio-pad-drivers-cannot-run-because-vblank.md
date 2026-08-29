---
id: 39
title: Guest SIO pad drivers cannot run because VBlank, SIO0, and root counter 2 are absent
status: resolved
symptom: A title that registers a VBlank interrupt element and drives SIO0 directly receives no controller bytes: display fields do not raise I_STAT bit 0, SIO0 MMIO is unmapped, and timer-2 stopwatch reads zero.
tags: timing,vblank,sio,pad,input,root-counter,interrupt,crashbash
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The framework had three separate omissions on one real guest path: Timing advanced fields without
latching VBlank I_STAT bit 0, Core hardware decode had no SIO0 controller device, and root counter 2
was unmapped. Crash Bash issue 0019 supplied the retail handler/verifier and timeout evidence.

## Fix

Timing now raises one VBlank per completed physical field and owns timer-2 stopwatch semantics;
io_peripherals owns MMIO decode; Sio0 owns a single digital pad on physical port 1 with baud-derived
transfers and oracle-derived ACK deadlines.

## Verification

Framework focused tests and the full Clang CTest gate cover positive and negative hardware answers.
A fresh Crash Bash frame-200 idle/START A/B proves packet, parsed, and game-facing P1 propagation
while P2 stays absent.

### Resolution (2026-08-30)
Implemented the missing shared hardware owners at their real boundaries: completed physical fields
latch VBlank I_STAT bit 0, Timing models root-counter-2 stopwatch registers, io_peripherals owns
decode, and Sio0 models one port-1 digital pad with timed transfer/ACK and enabled I_STAT bit 7.
The clean Clang framework suite passes 125/125, including clang-format and clang-tidy. Fresh Crash
Bash 210-frame idle/START runs prove packet `41 5A FF FF -> 41 5A F7 FF`, parsed
`FFFF -> FFF7`, game P1 `0 -> 8`, and P2 remains zero.
