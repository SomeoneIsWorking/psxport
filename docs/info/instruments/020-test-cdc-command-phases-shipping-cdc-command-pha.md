---
id: I020
kind: instrument
status: trusted
created: 2026-08-24
---

## Instrument

test_cdc_command_phases shipping CDC command phase gate

## Validated by

The suite produces both early/no-response and exactly-due/response answers, observes three distinct parameter-FIFO counts during transfer, distinguishes pre/post Setloc and Setmode effects, forces INT3 then delayed INT2, replaces a pending command, rejects an invalid argument count, and forces an exact INT1/INT3 deadline tie.

## Known failure modes

The suite fixes the oracle's random command-write and seek jitter at their deterministic zero floor.
It proves the shared controller boundary, not that a specific consumer reaches or advances past its
poll state; that requires a bounded title trace.
