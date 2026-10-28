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

## Follow-up: one input path (2026-10-28)
The guest-pad verbs `tap`, `press`, `release` and `hold` wrote a second mask straight into `Pad` (`repl_hold`/`repl_tap`,
merged into replays), beside the player's controller path. They now press PSX buttons on the control channel's virtual
gamepad through `HostInput::padButtons`, which uses the same `kGamepadBindings` table `applyGamepad` reads controllers
with (L2/R2 as trigger axes). `tap` holds for pad frames the guest is served (`InputInjection::afterPadFrame` from
`Pad::serviceFrame`), as the old drive did, so a paused or stepped run holds it; `gamepad tap` and the other host
verbs keep input turns, which a paused overlay needs. `Pad` lost its drive state, a replay no
longer merges control input (it is the guest-level recording alone), and the SIO savestate section dropped the four
drive fields (`kBusLayoutVersion` 5; older states are refused). `tools/oracle/compare_cores.py` checks the gamepad reply
instead of a `held=` echo; the guest's pad words stay a compared RAM range. Title debug drivers that pressed through `Pad::driveTap` (Crash Bash
`pause`/`arena`, Tomba! 2 autoskip) and the verbs press through `Pad::injectButtons`, the one press entry
(`HostInput::padButtons` is private to it). It refuses a press while a replay owns input or while another Game holds the
player's input: a Tomba! 2 picker panel's autoskip taps confirmed the picker (psx-tomba issue 0044).

Review (2026-10-28): the first cut counted input turns for `tap`, which advance every 15 ms in `DbgServer::idleFrame`
while paused, so a stepped `tap start 6` let go before the guest saw a frame. `TapClock::PadFrames` restores frame
counting. `PSXPORT_PAD_NOPAD` closed the virtual pad with the physical ones, so verbs replied as if pressed while the
guest got nothing; `rescanControllers` now drops only physical pads. While a pad replay owns input the verbs refuse
instead of pressing a pad nobody reads. Mega Man X4's `x4-input-path` line logs `host(buttons=…)`
(`Pad::hostButtons`) and `tools/live_play.py` reads that stage in place of the drive fields.
