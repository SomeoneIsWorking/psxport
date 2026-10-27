# 0181 — The selected picker panel shares its boot with the others

Status: closed (2026-10-27). Owners: `runtime/psx/host/panel_sessions.cpp` (`PanelSessions::nextPanelNeedingWork`),
`runtime/psx/host/title_session.cpp` (`TitleSession::step`, `showsTitlePicture`).

## Observed
Spyro player report `20261023-181726-spyro-1-loads-very-late`: in the selector, the Spyro 1 panel (selected, panel 0)
stays black while Spyro 2 and 3 already show their Insomniac logos.

## Cause
Two delays, both before the panel's first picture:
- `nextPanelNeedingWork` gave every unsettled panel one pre-roll slice in turn, so the selected panel ran Spyro 1's
  436-field boot prefix at a third of the rate with three panels.
- `probePicture` accepted a picture only when two thirds of its neighbour samples were within 24 luma. Spyro 1's lit
  intro flyover sits at 53-67%, so at fps60=0 the panel was refused for 176 more fields (`probe` stats in
  `psx-spyro/scratch/player-bugs/picker-fix/pk_f0c`); at fps60=1 a lucky in-between passed after 8.

## Fix
The selected panel takes every slice until it shows a picture; then the unsettled panels take turns as before.
The probe's coherence bar is two fifths: uninitialised memory sits near a fifth, a lit scene near half.

## Proof
Spyro selector, paced, hidden, `PSXPORT_DEBUG=picker`, sink 3840x2160, ires 0, `aspect=1`
(`psx-spyro/scratch/player-bugs/picker-fix/fx*`). Spyro 1's picture now lands 1 field after its boot prefix at
fps60=0 and 1 (before: 176 and 8). On a host at load 7-9 that is 0.80-0.83 s after the panels are made (before:
2.84 s round-robin, 3.4-3.7 s in the review's fps60=0 runs); with the host saturated by other builds (load 12-18)
the unpaced boot prefix itself takes 2.8-3.0 s.

Cost: Spyro 2 and 3 start their boots only once Spyro 1 shows, so they show later by about that time (5.8-5.9 s and
8.3-8.6 s at load 7-9, against 5.3 s and 8.2 s round-robin). At load 12-18 they did not show within 14 s.

All-title real-time run on psxport `79dcd0e6` (40 s paced, hidden, `scratch/player-bugs/realtime_all.py`, load 4.8-10.9): 0 error lines in all 11 titles; Spyro, Crash, X4 and Tekken 3 0 missed, CTR, Tomba, Spider-Man, Vagrant and Crash Bash at most 0.19%, Toy Story 2 0.74% (its boot hold, issue 0057), C-12 12.5% (psx-c12 issue 0006).

## Readiness from guest state, 2026-10-27
The luma probe answered a question the guest state already answers. `RenderState::probePicture`,
`retainFilledPresentImage`, `newestPresentedPicture` and the held copy are deleted. A panel is ready once its title is
past its boot prefix (`FrameDriver::pastBootPrefix`) and a step presented a field while the guest display was enabled
(GP1(03), `GpuDevice::displayArea().enabled`). A panel shows its newest present, so one deselected mid-fade holds the
faded frame.

Proof: Spyro selector, paced, hidden, `PSXPORT_DEBUG=picker`, sink 3840x2160, `PSXPORT_NATIVE_FRAMES=1200`, load 8
(`psx-spyro/scratch/player-bugs-2/picker-ready3`). From the panels being made, Spyro 1 shows a picture after 0.55 s,
Spyro 2 after 5.9 s and Spyro 3 after 9.2 s; all three reach their demos.

The Spyro 2/3 wait is accepted: it is their own boot prefixes, run unpaced inside each selector frame's pre-roll
budget, after the selected panel. Round-robin bought them about 0.6 s and cost the panel in focus about 2 s.

What a panel shows when it turns ready (psxport working tree after `2b3cb3bb`, Spyro selector, paced, hidden, sink
960x540 so every present could be shot, `PSXPORT_PRESENT_SHOT_AT=1-900`; `psx-spyro/scratch/player-bugs-2/picker-shots`
and `picker-shots2`, contact sheets `spyro1_ready.png` and `flips.png`):
- Spyro 1 turns ready on present 438, the first frame of its title flyover fading in from black (mean luma 0-2 at
  presents 436-439, 4 at 440, 7 at 441). It is the selected panel and keeps running, so the pane shows the live
  fade-in; with the deleted hold it showed its cover until a lit frame passed the probe.
- Spyro 2 and Spyro 3 turn ready on their "Developed by Insomniac" card (mean luma about 4), as in the original report.
- Not shown: a panel deselected in the middle of a fade. That needs a selector input injected while a panel fades;
  a deselected panel then holds whatever it last presented, black included.
