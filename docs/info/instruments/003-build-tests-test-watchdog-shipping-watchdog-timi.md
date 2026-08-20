---
id: I003
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

build/tests/test_watchdog — shipping watchdog timing and completed-present contract

## Validated by

2026-08-21: showed both answers through the production API: first-present work survived beyond the 1 s steady timeout before completion, while a stall after watchdog_present_complete exited 134 and emitted STUCK; source contract also rejects entry-time arming.

## Known failure modes

(none recorded yet)
