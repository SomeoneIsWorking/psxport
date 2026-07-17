# Scene checklist — pc_render visual verification spine

The ordered walkthrough of every scene from boot onward, each checked **default config**
(pc_skip + pc_render, plain `./run.sh`) against the oracle (`PSXPORT_ORACLE=1` = pure recomp +
pure PSX render). This is the burndown order for visual bugs: work TOP-DOWN, first unfixed scene
first (USER 2026-07-14: "start from the narration cutscene"). Update the status column in the
same commit as a fix; a scene is `OK` only after the USER eyeballs it (no visual self-verify).

Statuses: `OK` (user-verified) · `BUG #N` (tracked on the kanban, tools/kanban.py) ·
`UNCHECKED` (nobody has compared it yet) · `PARTIAL` (some elements wrong, see notes).

Reach recipes assume the REPL (`PSXPORT_REPL=1`, headless add `PSXPORT_VK_HEADLESS=1
PSXPORT_NOAUDIO=1`); drive by GAME STATE latches, not frame numbers (docs/driving-the-game.md).
Compare shots: `shot scratch/screenshots/scene-<n>-pc.png` on the default build vs the same state
under `PSXPORT_ORACLE=1`.

| # | scene | reach | status | notes |
|---|-------|-------|--------|-------|
| 1 | SCEA license screen | boot (automatic) | UNCHECKED | native splash (native_scea_splash), baked asset |
| 2 | OP.FMV opening movie | boot, wait past SCEA | UNCHECKED | recomp+psx_render SKIPS it (deferred); check pc path plays it |
| 3 | Title / main menu + attract demo | boot, wait | UNCHECKED | Demo stage; cursor sub-machine native (0x80106AC4) |
| 4 | **Narration cutscene** (story slides after New Game) | `newgame`, watch | PARTIAL | 2026-07-14: MODE=skip pane sync FIXED (demo_start_game rendezvous + B-first stepping, 2acc746); field-slide beats pixel-near-identical (~4% text-edge residual). BUG #43 FIXED (Sop::fieldUpdate restored the substrate's unconditional 0x80109FE0 + 0x8003C048 render dispatches — vortex+Tabby+coin now draw on pc_skip; findings/scene.md). Cutscene-wide: #35 darkening, #27 fadeouts — awaiting user eyeball for OK |
| 5 | Narration-end transition (was "loading screen") | after 4 | PARTIAL | 2026-07-14 re-verify: NO black+"Loading" card exists in the current build — BOTH default and oracle hold the frozen last-narration frame for the 2-6 frame beat-reset→scene-flip gap (f1124-1128) and roll straight into the fisherman scene. Default matches oracle; the 2026-07-08 "garbage vs black-hold" premise is not reproducible — user eyeball to confirm OK |
| 6 | Fisherman intro cutscene | after 5 | BUG #34 | 2026-07-14 checked f1130-1830: NO #35 darkening here (daytime segment; luminance tracks oracle within ~1pt — the f1520-1640 gap was the missing dialog box). #44 "upside-down Tomba" was a FALSE ALARM (raw-frame-matched comparison across a 12-14 frame exec lag; exec-aligned A/B identical, eprojv identity-fit sub-pixel). #34 panel FIXED (9178563, UI-span provenance) — row awaits user eyeball |
| 7 | Prologue interactive (pig bag, first field) | after 6, or `newgame` + AUTO_SKIP free-roam | UNCHECKED | |
| 8 | Seaside field free-roam | `PSXPORT_AUTO_SKIP=1` (~f216) | PARTIAL | z-fighting on barrels PARTIAL-fixed; #40 door arrow doesn't clear; #41 fps60 2D grouping |
| 9 | Dialog / signpost text | field, `press up`+O at sign | PARTIAL | #34 panel fix landed (9178563) — same emitter chain as the fisherman box; signpost instance not yet eyeballed |
| 10 | Weapon attack (chain + impact) | field, attack input | BUG #39 | weapon chain + impact effect missing |
| 11 | Hut door transition (fade + swap) | `replays/scene-transitions/hut-entry-door-freeze.pad` | UNCHECKED | exec side now 0-diff (voiceMixTick fix bb29e0a); check the VISUAL transition; #27 fade family |
| 12 | Fisherman's-hut interior | after 11 (quest-gated; see findings/render.md repro-BLOCKED note) | BUG #36 | "much different than oracle"; transition bug (overlay not loaded) — first fix attempt reverted (37c7953) |
| 13 | Inventory / weapon carousel HUD | field, open menu | BUG #38 | widescreen: HUD bleeds to screen edges (safe-area crop) |
| 14 | Save/load screens (memory card) | field, save point | UNCHECKED | |
| 15 | Combat encounter (Koma pigs) | field, engage enemy | UNCHECKED | AUTONAV=combat leg reaches it headless |
| 16 | Widescreen general | any scene, wide mod on | BUG #42 | expands only to the right; should be symmetric around the 4:3 center |

## Cross-scene bug families (fix once, re-verify the affected rows)

- **Fades (#27, #35):** ScreenFade held-latch + cutscene darkening — affects rows 4, 5, 6, 11.
- **Widescreen (#38, #42):** present/crop geometry — affects rows 13, 16 (and any HUD scene).
- **fps60 (#41):** per-object interpolation grouping — field scenes (row 8).

## How to add a row

New scene encountered (user report or playthrough) → add the row where it falls in game order,
with a reach recipe the next session can replay headless (prefer a `replays/` capture for
anything needing real navigation), and file it on the bug board if it's broken.
