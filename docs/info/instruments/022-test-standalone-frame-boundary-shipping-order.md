---
id: I022
kind: instrument
status: trusted
created: 2026-08-25
---

## Instrument

`test_standalone_frame_boundary` shipping standalone frame-order gate

## Validated by

The same callback trace produced both answers. Against the extracted old loop, both cases failed at
event two because warp service came before pending-frame presentation. Against the shared boundary
helper, armed and unarmed cases each report the required four-phase order and pass 10 checks total.

## Known failure modes

The callback test proves the shared ordering owner, but it cannot by itself prove `native_boot.cpp`
wires every shipping operation into the correct callback. The Clang build covers that wiring
statically, while a bounded consumer trace is required to show the old scene was actually presented
before game-owned guest state was replaced.
