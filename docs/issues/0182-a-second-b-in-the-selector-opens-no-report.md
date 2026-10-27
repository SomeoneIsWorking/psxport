# 0182 — A second B in the selector opens no report

Status: closed (2026-10-27). Owners: `runtime/psx/input/host_input.cpp` (`HostInput::drainEvents`),
`runtime/psx/host/product_host.h` (`ProductHost::Process`).

## Observed
Spyro selector, hidden, input injected on the control channel: `key B`, `key Escape`, `key B`. The first B opens the
report form and Escape cancels it; the second B opens nothing. An explicit `key B up` before it makes it work again.

## Cause
Every `Game` owned its own `HostInput`, and the selector runs four Games (its host and three panels) over the one
window's SDL queue. Whichever Game drained next took an event, so B's release often went to a panel Game, and
the selector's key state kept B held, so the next press made no edge.

## Fix
`ProductHost::Process` owns one `HostInput` for the process, and every Game it builds reads it. Events go to the overlay of
the Game holding player input (`Pad::setPlayerInputSuppressed` marks it live). The P / '.' / B edges latch until
that Game takes them. A standalone `Game` still owns its own.

## Proof
`psx-spyro/scratch/input-injection/p4-sequence.png`: the selector, B (the form), Escape (closed), B (the form again),
Escape. The log shows four `report cancelled` lines for these presses and `picker3-reports/` stays empty. After
`key Return` picks Spyro 1, ESC opens the title's menu, `gamepad start` reaches the Select Memory Card screen,
and B then Escape opens and closes the title's form twice (`p4-title-seq.png`, `p4-title-b-seq.png`).
