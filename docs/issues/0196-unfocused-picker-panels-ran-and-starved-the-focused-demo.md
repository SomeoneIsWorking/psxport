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
title's own attract demo. The logo (`panelLogo`) is hidden while `GameRuntime::showsOwnLogo` says the panel's
picture already shows it, asked of every panel's machine (a paused one still holds its still's state); Spyro
answers from the same title-screen gate its logo snapshot uses (game state 13 and the overlay tick past 1169).
`PanelLayout::logoRect` sizes it at the largest whole multiple of its texels inside the panel's seams.

## Proof
`psx-spyro/scratch/picker/walk3` (hidden, unpaced, sink 3840x2160, ires 0, fps60 1): the `panels:` lines
show Spyro 1 frozen at steps=2397 insns=85724549 for the whole time Spyro 2 is focused and Spyro 3 cold at 0
throughout; `walk_sheet.png`: the title screen with no second logo, the attract demo with the logo over it,
Spyro 1's greyed still and logo beside focused Spyro 2, Spyro 1 resumed in its demo, and the product booted
fresh after a pad `a` press. `presentseq` in `scratch/picker/new2`: one guest field per present (9570 of 10786
presents `fields=2/2` at t 0.5 and 1, 1210 `1/1`). Crash: `psx-crash/scratch/picker/walk1`.

## Review follow-up (2026-11-03)
- The logo was cut to "SPYR" on a 20% panel and magnified past whole texels; it was hidden only on the focused
  panel, so a still left on the title screen would have drawn a second logo, and the title fade-in after a demo
  had none. All four answered by `showsOwnLogo` and `logoRect` above.
- Shown in `psx-spyro/scratch/picker2` (hidden, unpaced, sink 3840x2160, ires 0, fps60 1): `s1/sheet.png` rows:
  demo with the logo (6x texels, 1530 px, inside the panel); the title fading in after the demo with the picker
  logo, then the game's own logo with none over it; Spyro 1 left on its title screen, greyed with only its own
  logo; Spyro 3's boot card; Spyro 1 resumed on its title; Spyro 1 started fresh after `gamepad a`.
  `narrow/narrow_demo_still.png`: the demo still on the 20% panel with the whole logo at 2x. `s1/run.log`:
  Spyro 1 holds steps=3750 insns=189184538 while Spyro 2 and 3 are focused; presentseq has 3786 two-field logic
  frames each with a t=0.5 present and 1900 one-field frames with one, so one present per guest field.
  A `gamepad y` tap during the demo (which ends on any guest button) left it running 4 s later.
- Attract walks: Crash 2 loops title and DEMO levels (`psx-crash/scratch/picker2/crash2_sheet.png`); Crash 3
  hangs in guest code alone as well (psx-crash 0035); Spider-Man 1 reaches its DEMO level, Enter Electro is black
  alone as well (psx-spider-man 0039, `psx-spider-man/scratch/picker2/sheet.png`); Tomba! 2 reaches its attract
  field, Tomba! 1 showed only its opening movie in 21812 steps (`psx-tomba/scratch/picker2/sheet.png`). In every
  walk the unfocused panel's steps and instructions stay fixed.

## Open
- Wall-clock pacing of the focused demo was not judged: host load stayed far above 4 for the whole pass.
- No all-title real-time run since the psxport change (same load).
- Only Spyro 1 has a logo; Spyro 2/3, Crash, Tomba and Spider-Man supply no `panelLogo`.
- A panel never focused shows nothing until it is focused once; the first focus shows the title's boot cards
  before its demo.
- Crash 3, Enter Electro and Tomba! 1 show no attract demo in the selector (above).
