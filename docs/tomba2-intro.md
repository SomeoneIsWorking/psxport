# Tomba! 2 (SCUS-944.54) — intro / logo sequence, toward a NATIVE skip

Goal (user, 2026-06-13): make the SCEA + Whoopee Camp logos **natively skippable**
(instant, via RE'd game logic) — NOT emulator fast-forward. The shipped
`Tomba2_WantTurbo` Start-held fast-forward is explicitly rejected: "I want native
skip via RE + port." Methodology priority the user set: **tooling → reverse
engineering → native port → patch the native port**.

All addresses KSEG0 virtual. EXE `SCUS_944.54`: T_ADDR `0x80010000`,
**T_SIZE `0x28800`** (167936 − 0x800 header), entry `0x80018b6c`. NOTE: the
older scene-orchestrator doc says size `0x69000` — that is WRONG (a Crash Bash
copy-paste); the real Tomba2 EXE is 0x28800, spanning 0x80010000–0x80038800.

## Verified intro timeline (headless, instant CD default, no input)

Captured with `tools/frames.py` (contact sheet) — see
`scratch/screenshots/introseq/sheet_native.png`:

| frames        | screen | nature |
|---------------|--------|--------|
| ~0–200        | black  | boot (EXEC at f82, card/GTE patches f1361) |
| ~200–600      | "Sony Computer Entertainment America Presents" | SCEA license (load mask) |
| ~600–850      | black  | transition / load |
| ~850–1700     | Whoopee Camp logo (colour on white) | developer logo (load mask) |
| ~1800+        | opening movie (Tomba face, village…) | **MDEC FMV stream** |
| (after FMV)   | "TOMBA 2 The Evil Swine Returns" | title screen |

Frame counter here = emulator presented frames (g_frame), ~60/s.

## Skippability — measured on the running system (REPL `shot`)

- **The opening FMV IS natively Start-skippable.** Driven via REPL: reach f1900
  (FMV), `press Start`, run 90 → title screen by **f1990**, whereas a no-input run
  is still mid-FMV at f2160+. So the movie player polls the pad and aborts on Start.
- **SCEA + Whoopee are NOT natively skippable.** Holding Start from boot still shows
  SCEA at f300 and Whoopee through f1500 (`scratch/screenshots/repl/hold_sheet.png`).
  They are load masks with display-hold timers; the pad is not polled to skip them.
- The gameplay render heartbeat `0x8003CCA4` is **dormant through the entire intro**
  (render-hits = 0, GAMELOG). Expected: SCEA/Whoopee/FMV don't use the in-engine
  object draw path. Do NOT treat render-hits=0 during the intro as a hang.

## FALSIFIED prior RE (do not reuse for the intro)

1. **`0x800675CC` "scene phase" (docs/tomba2-scene-orchestrator.md) is NOT written
   during SCEA/Whoopee.** Write-watchpoint `PSXPORT_WATCHW=800675CC` over f0–1800
   logs exactly ONE write: the BIOS RAM-clear at f25 (value 0). The orchestrator
   that doc describes does not drive the actual intro logos on this disc. Same
   failure mode as the earlier falsified `0x800253EC` note.
2. **The `0x80011B4 → 0x80018C10` "intro driver" chain is a stale stack-scan
   artifact.** `bt`'s heuristic scan picked up stale return addresses. Tracing
   `0x8001e0cc` (a function in that chain) over f0–1900 → **zero hits**: that code
   does not run during the intro. The `0x80011xxx` straight-line "play intro"
   function is post-intro **title/menu** code, not the logo driver. Do not chase it
   for the logo skip.

## Ground truth: what actually drives the logos (PCCOV)

`PSXPORT_PCCOV` executed-PC bitmaps, intersection of SCEA (f250–380) and Whoopee
(f900–1000) = the per-frame intro driver. Common code regions (game-code area):

```
80013A78-80013B3C   80014FE0-80015100   80017C00-80017CA4
80017E4C-80018058   800181E8-80018484   8001874C-8001883C
```

- **`0x80017E4C` = VSync / elapsed-frame timing primitive.** Reads global frame
  counter **`0x800267B4`**; markers `0x80025684` (start) / `0x80025688` (prev);
  computes `elapsed16 = (counter - 0x80025684) & 0xffff`. Sub-fn `0x80017FC4` =
  "wait until counter ≥ a0" (with a timeout that fires `0x8001effc`/`0x8001799c`).
  The display-holds are frame counts against this. SHARED by all timing (animation
  included) → NOT a clean blanket override target.
- `0x800181E8-0x80018484` (168 words) = next region to disassemble; likely the
  stage state machine / dispatcher. **TODO next session.**

## Candidate stage / progress variables (SCEA f400 vs Whoopee f1000 RAM diff)

Small-int words that changed between the two logo phases (strongest = a stage index):

```
80025454: 5 -> 0      80025458: 3 -> 0      8002667C: 1 -> 0
80026620: 0 -> 8      80026628: 0 -> 8      80026BE8: 0 -> 4
80026BF8: 0 -> 1      80026D00: 0 -> 10     80038474: 0 -> 1
```

These are NOT yet confirmed as the stage selector. **NEXT: watchpoint the most
promising (e.g. 0x80025454/0x8002667C) across the intro to find who writes them and
the dispatch that reads them.** The timing globals cluster at 0x8002567C–0x80025688
and the candidate vars at 0x80025454/58 are near each other — the intro state likely
lives in the 0x80025xxx block.

## Design intent for the native skip (not yet implemented)

A true native skip (not fast-forward) means making the GAME's own logic advance the
logo as if its display-hold elapsed, while **leaving the asset-load wait intact** (so
no white-screen race — that was the failure of forcing scene advance previously). The
plan once the stage machine + per-stage hold are identified:
- Detect SCEA / Whoopee stage from the (to-be-confirmed) stage variable.
- On Start press, force the current stage's display-hold to "elapsed" (e.g. set the
  elapsed marker `0x80025684` back, or satisfy the stage's hold compare) — but only
  after that stage's load-complete condition is true.
- Implement as a native override hook (KEEP the original code resident/diffable, per
  recomp-overrides discipline), not a RAM poke.

## Tooling added this session
- **REPL `shot <path>`** (runtime/main.cpp): dump the current presented framebuffer
  to a PPM on demand while driving via `-repl`. VideoCb caches the last frame
  (tightly packed) into `g_last_fb`. This is the interactive analogue of
  `PSXPORT_FRAMEDUMP` and is what made the skippability tests above possible.
- Reused: `tools/frames.py` (contact sheets), `PSXPORT_WATCHW`, `PSXPORT_PCCOV`,
  `PSXPORT_RAMDUMP`, `tools/disasm.py`. KEY: dump RAM **during the logo phase**
  (f400/f1000), not during the FMV — the FMV stream overwrites the intro driver code
  at 0x80011xxx/0x80018xxx.
