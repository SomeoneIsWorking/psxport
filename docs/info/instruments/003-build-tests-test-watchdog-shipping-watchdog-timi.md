---
id: I003
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

build/tests/test_watchdog — shipping watchdog timing and completed-present contract

## Validated by

2026-08-21: showed both answers through the production API: bootstrap progress survived beyond the 1 s steady timeout without ending the 3 s boot phase, while stalls after `watchdog_main_present_complete` exited 134 and emitted STUCK. A second negative proved that later generic progress resets the steady alarm without restoring boot grace. The source contract also rejects entry-time arming.

## Known failure modes

(none recorded yet)
