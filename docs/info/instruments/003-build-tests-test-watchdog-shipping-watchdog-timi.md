---
id: I003
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

build/tests/test_watchdog plus test_gpu_clear_watchdog — shipping watchdog phase and typed present-completion contract

## Validated by

2026-08-21: showed both answers through the production API: bootstrap progress survived beyond the 1 s steady timeout without ending the 3 s boot phase, while stalls after `watchdog_main_present_complete` exited 134 and emitted STUCK. A second negative proved that later generic progress resets the steady alarm without restoring boot grace. The source contract also rejects entry-time arming.

2026-08-27: `test_gpu_clear_watchdog` extended that coverage through the real
`GpuState::gpu_clear_display` caller. The pre-fix control exited 134 after the transition clear
selected the one-second steady phase; the corrected shipping path survives 1.3 seconds under the
three-second boot budget. Full Clang CTest passes 107/107.

## Known failure modes

Before `test_gpu_clear_watchdog`, the instrument covered direct watchdog APIs and FMV presenters but
not the transitive bootstrap-black-clear caller. That blind spot allowed a semantic transition frame
to use the main-frame completion signal while the lower-level watchdog tests remained green.
