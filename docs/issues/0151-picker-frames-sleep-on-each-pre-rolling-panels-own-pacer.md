# 0151: picker frames sleep on each pre-rolling panel's own pacer

Status: fixed

## Observed
The title picker ran at a mean of 55 ms per frame with 190 ms frames every third frame in the
player's own run (`run.sh`, Spyro trilogy, 2026-10-10), and no panel finished its 900-step pre-roll in
the 15 s it was up. A paced headless run of the same binary (`PSXPORT_DEBUG=picker,pacer`, 20 s of
picker) measured mean 64.4 ms, p50 23, p95 194, max 209, up to 13 pacer sleeps in one picker frame,
and every panel still pre-rolling at 618-624 steps.

## Cause
`PanelSessions::advance` ran six steps of a pre-rolling panel per picker frame, and every presented
field inside those steps slept on that panel's own `FramePacer` (`waitPresentedFields`), so one picker
frame lasted 6-12 guest fields of wall time. Two further defects in the same owner: a panel that never
drew a picture never settled (`hasArrived` required `panelReady`), so it took every frame from the
selected panel forever; and panel 0 was never made audible, because `select(0)` on the initial
selection was a no-op.

## Resolution
A pre-roll is a fast-forward: `SessionControl::setPreRolling`, read with the resume replay through
`Game::fastForwarding`, so the pacer does not sleep, movies run uncapped and audio is dropped.
`PreRollBudget` gives a pre-rolling panel as many steps as fit in one NTSC field. A panel settles after
900 steps whether or not it has a picture. Panel 0 is set audible at construction.

Same headless measurement after: pre-roll frames mean 19.2 ms, p50 17, p95 34, max 99, one pacer
sleep per frame, all three panels settled in 15 s; settled frames mean 33.5 ms (stdev 2.3, Spyro 1's
30 Hz step). Windowed (MAILBOX): settled picker audio 44100 samples/s, drops 0.

## Open
- A panel's picture is the held copy, refreshed once per 24 steps (`TitleSession::refreshPicture`'s
  12-step gate compounded with `GpuVkState::retainFilledPresentImage`'s 15-present gate), so the
  selected demo updates about every 0.8 s.
- Spyro 3's panel passes its boot prefix but no probe finds a filled picture, so it stays empty.
