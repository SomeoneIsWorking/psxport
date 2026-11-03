# 0196 — Unfocused picker panels ran and starved the focused demo

Status: fixed (2026-11-03). Owner: `runtime/psx/host/panel_sessions.cpp` (`PanelSessions`).

## Observed
"Spyro's game selector still feels like something hacked together ... it's slowed down and the other titles
play some frames even when not active." The user also wants the attract demos back, with the game logo.

Reproduced on HEAD `f33ee4c6` (Spyro selector, hidden, unpaced, `PSXPORT_NATIVE_FRAMES=1500`, sink 3840x2160,
`psx-spyro/scratch/picker/head`): every panel booted at start; in 1012 of the 1500 selector frames the focused
Spyro 1 panel shared the frame rotation with Spyro 2 and 3 pre-rolling 900 steps each, so it ran on one turn
in three, fast-forwarded and silent, and froze on the others.

## Cause
`PanelSessions::nextPanelNeedingWork` / `advance` gave every unsettled panel pre-roll turns (`PreRollBudget`,
`SessionControl::setPreRolling` read through `Game::fastForwarding`), including panels the player was not on,
and fast-forwarded the focused one during its own pre-roll.

## Fix
Restructured to one rule (`title-host.md`, "The selector"): only the focused panel's session is stepped, one
ordinary paced step per selector iteration; unfocused sessions are paused and execute nothing; a panel boots
the first time it is focused and resumes where it was afterwards. Pre-roll, `PreRollBudget` and
`SessionControl::preRolling` are deleted. The focused panel runs its title's boot path with no input into the
title's own attract demo. The logo (`panelLogo`) is drawn at 0.6 of the panel width, over the focused panel
only while `GameRuntime::playsAttractDemo` says the demo plays (Spyro 1: `g_DemoMode` == `DEMO_MODE_PLAY`).

## Proof
`psx-spyro/scratch/picker/walk3` (hidden, unpaced, sink 3840x2160, ires 0, fps60 1): the `panels:` lines
show Spyro 1 frozen at steps=2397 insns=85724549 for the whole time Spyro 2 is focused and Spyro 3 cold at 0
throughout; `walk_sheet.png`: the title screen with no second logo, the attract demo with the logo over it,
Spyro 1's greyed still and logo beside focused Spyro 2, Spyro 1 resumed in its demo, and the product booted
fresh after a pad `a` press. `presentseq` in `scratch/picker/new2`: one guest field per present (9570 of 10786
presents `fields=2/2` at t 0.5 and 1, 1210 `1/1`). Crash: `psx-crash/scratch/picker/walk1`.

## Open
- Wall-clock pacing of the focused demo was not judged: host load stayed above 4 for the whole pass.
- Only Spyro 1 has a logo; Spyro 2/3, Crash, Tomba and Spider-Man supply no `panelLogo`.
- A panel never focused shows nothing until it is focused once; the first focus shows the title's boot cards
  before its demo.
