# 0195 — The save-state clock truncates after 127 seconds

Status: open (2026-10-28). Owner: `frame/emulated_time.h:EmulatedTime::q32`.

## Observed
`EmulatedTimeQ32` holds the Q32 clock and display boundary as `uint64_t`, so `q32()` drops every bit above 2^64: a
Q32 value passes 2^64 at 2^32 guest ticks, 126.8 s of guest time. A field digest that read the boundary through
`Timing::clockSnapshot()` wrapped at C-12 step 3838 (`scratch/dispatch-cost/c12-replay-grid2/`); the digest now reads
`Timing::displayBoundaryTicks()`.

## Why it matters
A save state taken past two minutes restores `now` and the display boundary modulo 2^32 ticks
(`state/device_bus.cpp` writes them as `u64`), so the restored clock, HSync count and every device deadline jump back.

## Fix
Serialise the two 128-bit values in full (two `u64` each) in `device_bus.cpp`, with the save-state version bumped.
