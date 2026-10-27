# 0186 — A host-run CD command leaves its interrupt unlatched

Status: fixed (2026-10-28, `d7bbc0d3`). Owner: `runtime/psx/cd/stock_libcd.cpp` `StockLibcd::applyCommand`.

## Observed
C-12 (psx-c12 issue 0006) runs a Pause before each ReadN through the stock `CD_cw`. The INT3/INT2 that Pause raised,
and the first sector's INT1 behind it, reached the guest only at the field end: a guest spinning in libcd's CdReady
never reads I_STAT, and nothing else latched the edge. Each new file read lost the rest of a field
(psx-c12 `scratch/player-bugs-3/cdwait3/excerpt.log`).

## Cause and fix
A guest write to the controller latches I_STAT after the write; `applyCommand` ran the same controller path from the
host without it. It now calls `core.irqStatLatch()` after every command it runs. This changes every title that binds
the stock `CD_cw`/`CdControl`: a response a host-run command makes current is visible to the guest's next interrupt
check instead of the next field boundary.

## Shown
C-12 boot with the guest CdRead: sectors arrive one per drive period inside the field (`cdwait5/excerpt.log`) and
budget-ended boot fields fall from 94 to 89. The all-title 40 s paced runs after the change are in
`scratch/player-bugs-3/realtime_all.log`.
