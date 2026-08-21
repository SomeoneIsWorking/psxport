---
id: I007
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

ctest -R ^test_bios_interrupt$

## Validated by

Trusted for the BIOS custom-exit context/control contract: it restores all 12 measured jmp_buf fields and reaches shipping B0:0x19/0x18; forced other answers refuse zero buffer and zero RA, prove B0:0x17 skips the line after it, and report an ordinary dispatch return as FellThrough.

## Known failure modes

(none recorded yet)
