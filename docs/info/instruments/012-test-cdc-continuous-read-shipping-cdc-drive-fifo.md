---
id: I012
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

test_cdc_continuous_read shipping CDC drive/FIFO independence test

## Validated by

On the pre-fix sector_consumed-owned model, the partial-FIFO and full-drain cases both failed at their first following-sector event check (pending IRQ type 0 instead of 1), while the original stopped-controller opposite case passed 3 checks. With separate drive-side following-sector availability, the strengthened suite passes 3/3 tests and 31 checks: partial FIFO retains LBA/cursor and gets exactly one INT1, a Pause command produces its INT3/INT2 responses then no data event, and full drain does not itself advance but rearms BFRD for the announced sector.

## Known failure modes

(none recorded yet)
