---
id: C043
kind: claim
status: holds
created: 2026-08-30
tags: input,sio,vblank,crashbash
depends: runtime/psx/sio_pad.cpp#Sio0::dataWrite, runtime/psx/timing.cpp#Timing::advanceDisplayFields, runtime/psx/timing.cpp#Timing::rootCounter2, runtime/psx/io_peripherals.cpp#io_peripheral_read
---

## Claim

The framework delivers a port-1 digital pad through guest-owned VBlank and SIO0 interrupt code without a title-local input injection path.

## Evidence

The full Clang framework suite passes 125/125, including `test_vblank_irq`, `test_sio_pad`,
`test_root_counter2`, clang-format, and clang-tidy. Fresh Crash Bash frame-200 RAM A/B propagates
START through packet `0x80077FBC` (`41 5A FF FF -> 41 5A F7 FF`), parsed P1 `0x80063A92`
(`FFFF -> FFF7`), and game P1 `0x8005133C` (`0 -> 8`), while game P2 `0x80051394` remains 0.

## What would falsify it

A clean exact-pin Crash Bash build fails any packet/parsed/game-facing value, port 2 answers the port-1 exchange, a fractional half-field raises VBlank early, or a guest timer-2 delay cannot terminate.
