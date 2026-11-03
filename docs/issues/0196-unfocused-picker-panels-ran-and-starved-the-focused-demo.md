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

## Follow-up (2026-11-03, picker2)
- `rw` on the selector's channel returned 0: the selector's Game holds the channel and has no guest, so every
  shared verb read its empty machine. Cause: `DbgServer::runCommand` ran every verb against the Core of the Game
  that claimed the endpoint. Fix: `DebugCommands::reachesMachine` classes each shared verb (machine, or window /
  host input / configuration), and the selector routes machine verbs and title verbs to the focused panel's
  booted machine (`DbgServer::routeMachineVerbsToPanel`, set each selector frame from `PanelSessions::focusedMachine`);
  before that panel boots a machine verb is refused (`focused panel not booted`) instead of reading the selector's
  empty machine.
  Shown in `psx-spyro/scratch/picker2b/run1`: `rw 800757D8` reads 0x0000000D (Spyro 1's title state) with Spyro 1
  focused, Spyro 2's word 0x30420006 with Spyro 2 focused, 0x0000000D again back on Spyro 1; `pshot` still
  shoots the composed selector (`run2/sheet.png`).
- A panel never focused had no logo. Decision: the logo read from the running title is saved in the title's
  user-data directory (`SavedLogo`, `picker-logo.rgba`) and loaded when the selector creates its panels, so the
  panel shows it from the first frame with zero guest instructions; it is derived from the player's own disc at
  run time and never committed. Rejected: extracting each title's logo from disc files at session creation, which
  needs each title's archive and texture formats reverse-engineered for no visible difference after the first
  launch. Shown in `run2` (logo snapshotted 255x128 at 29 s, saved) then `run3/sheet.png`: the next process
  draws Spyro 1's logo over its black panel in the first selector frame, before the title has presented.
- `SavedLogo` repeated `SavedDisc`'s user-data path and `.pending` rename; both now keep their file through
  `TitleUserFile`, the one owner of a title user-data file and its whole-file replace.

## Follow-up 2 (2026-11-03, picker2 review)
- Before the focused panel boots, a machine verb is refused (`focused panel not booted`) instead of reading the
  selector's empty machine: `DbgServer::routeMachineVerbsToPanel` is set when the channel starts and every
  selector frame. Not reproduced in a run: panel 0 boots in the selector's first frame, before a client's first
  command arrives.
- `SavedLogo` and `SavedDisc` keep their files through `TitleUserFile` (one user-data path and `.pending` rename).
- Logos come from the title screen's draws (`psx::present::drawnLogo`), taken once two steps in a row read the same
  size. Spyro 1, 2 and 3 supply them (psx-spyro 0201). `psx-spyro/scratch/picker2/final1`: each is read on
  its title screen, unfocused counters stay fixed (Spyro 3 `steps=1764` from 49 s to the pick), `pick spyro1`
  starts Spyro the Dragon and `rw 800757D8` reads its machine (0x0D). `final2/sheet.png`: the next process draws
  all three saved logos in its first frame, Spyro 2 and 3 never focused (`steps=0 insns=0`).

## Follow-up 3 (2026-11-03, picker3)
- Spyro (`psx-spyro/scratch/picker3/w1`, `w2`): each logo is read on its title screen and saved; the next process
  shows all three from its first frame with `steps=0`; unfocused counters stay fixed; `pick spyro1` enters the game
  (`rw 800757D8` 0x0D). The `focused panel not booted` refusal is unreachable in a walk: the focused panel boots in
  the selector frame before the channel serves its first command, so it guards only a failed boot.
- Crash 3's attract hang is fixed (psx-crash 0035): the loader's VBlank-counter poll ends the host frame.
- Enter Electro now shows its intro movie, title and DEMO (psx-spider-man 0039: the libapi DMA callback table and
  the heap-1 overlay window); in the DEMO only the actors draw and the city is black, not traced.
- Tomba! 1 is not stuck: its attract loop is the intro movie whose last segment is the PRESS START title, held
  about 890 guest fields, then the movie restarts (psx-tomba `titles/tomba1/docs/project-state.md` S004).
- Crash, Tomba and Spider-Man pickers walked hidden (each repo's `scratch/picker3/pw/sheet.png` and `run.log`): cold
  panels black with `steps=0`, every unfocused panel's step count unchanged in all 3335/3293/5105 `panels:` lines,
  and `pick` starts the title full-window (Crash 1 to its title and story intro, `psx-crash/scratch/picker3/pw2`).
  Tomba! 2's focused panel was black there (Follow-up 4).

## Follow-up 4 (2026-11-03, picker3 review)
- `drawnLogo` no longer rasterizes: it replays the logo's recorded primitives through gpu.c on a scratch
  `GpuDevice` over the live VRAM, with the E6 mask bit forced so the written pixels are the alpha; the bounds
  follow gpu.c's exclusive right column and bottom row. The "1 visible texel in 50" test is gone.
- A logo is taken on the second step in a row with the title's gate open (`PanelSessions::takeLogo`), not after
  two equal sizes; Spyro's gates are each title screen's resting-logo state (psx-spyro 0201). `logoRect` draws
  every panel's logo at the whole texel scale at which the largest one fits, so Spyro 1/2/3 share one scale
  (`psx-spyro/scratch/picker3/r4/sheet.png`).
- Tomba! 2's panel stayed black while focused: its native boot movie and SCEA card present through
  `RenderState::buildImage`, which never told the pane host, so the selector never composited them (also in the
  earlier `psx-tomba/scratch/picker3/pw`, where Follow-up 3 called it fine). Every build now ends in
  `RenderState::finishBuild`. `psx-tomba/scratch/picker3/pw4/sheet.png`: Tomba! 2's movie in its panel beside
  Tomba! 1's greyed still, then picked full-window.
- Walks with 40-45 s holds (hidden, unpaced, sink 3840x2160, ires 0, fps60 1), each repo's `scratch/picker3/pw3`
  (Tomba `pw4`): Crash 1/2 titles and Crash 3 through four DEMO levels and `pick crash3` to its title; Spider-Man 1
  menu and demo, Enter Electro title, movie and DEMO; Tomba! 1 movie, Tomba! 2 movie. Unfocused step counts
  never change in any `panels:` line. Tomba! 1's title is reached at guest field ~14,390 (`t1g/title_loop.png`).
- The `focused panel not booted` refusal stays unexercised: the focused panel boots before the channel serves
  its first command.

## Open
- Crash 1/2/3, Tomba! 1/2 and Spider-Man 1/2 supply no logo yet and answer `showsOwnLogo` false. Crash has no
  global menu word for Crash 1 or 3, and a gate on the menu-text pages read the wrong art because the wordmark is a
  textured model, not the named pages (psx-crash 0036, third pass). Tomba! 1's logo is part of its movie frame.
  Tomba! 2 and Spider-Man were not attempted. A title never focused on this machine has no logo until its title
  screen shows once.
- Wall-clock pacing of the focused demo was not judged and no all-title real-time run was made: host load stayed
  between 35 and 57 for the whole pass.
