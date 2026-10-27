# 0181 — The selected picker panel shares its boot with the others

Status: closed (2026-10-27). Owner: `runtime/psx/host/panel_sessions.cpp` (`PanelSessions::nextPanelNeedingWork`).

## Observed
Spyro player report `20261023-181726-spyro-1-loads-very-late`: in the selector, the Spyro 1 panel (selected, panel 0)
stays black while the other panels boot.

## Cause
`nextPanelNeedingWork` gave every unsettled panel one pre-roll slice in turn, so the selected panel ran its boot prefix
(Spyro 1's two publisher logos, hidden until `pastBootPrefix`) at a third of the rate with three panels.

## Fix
The selected panel takes every slice until it shows a picture; then the unsettled panels take turns as before.

## Proof
Spyro selector, paced, `PSXPORT_DEBUG=picker` (`psx-spyro/scratch/player-bugs/late` before, `late3` after): Spyro 1
shows its picture 2.6 s after the panels are made before, 0.9 s after. Spyro 2 and 3 still show at 5-8 s (host load
about 9).
