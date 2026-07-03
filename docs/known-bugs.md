# Known bugs — PC-port symptoms tracker

Live, durable list of KNOWN PORT BUGS — the things the user has reported or the diagnostic
tools have surfaced that don't yet have a root cause + fix. Once a bug is understood +
fixed, PROMOTE it to `docs/findings/<subsystem>.md` and mark the entry here `FIXED` +
link the finding. This file is the LIVE tracker; the findings registry is the distilled
long-term memory.

Search: `tools/bugs.py [words]`. See the header of that script for the parser and the
block format. Add a new bug as a `## BUG-<n>: <title>` block with the fields below.

Statuses: `OPEN` (not yet actively worked) · `INVESTIGATING` (someone is chasing it now) ·
`PORTED-BUT-UNVERIFIED` (a native port that plausibly fixes it landed; awaiting user
eyeball) · `FIXED` (verified fixed — move the write-up to `docs/findings/`).

## BUG-1: Cutscene fadeouts leave the screen dark
- **status:** INVESTIGATING
- **area:** render / fade
- **symptom:** cutscene fade-out completes, screen stays fully black, never fades back in
- **hypothesis:** any still-recomp caller of `FUN_8007E9C8` writes guest OT data our renderer no longer draws (see `game/render/screen_fade/screen_fade.h` design note). Frame-scoped ScreenFade stays NONE, and the HOLD latch at full-black never releases. Fix path: port each remaining shard caller of `func_8007E9C8` so it lands on `c->screenFade.applyLeafCall(color, a1)`.
- **investigation 2026-07-03 (in-progress, next session pick up here):** Surveyed all 36 shard callsites of `func_8007E9C8`. The MAJOR native SMs that already reach `ScreenFade` correctly: `Engine::transitionMain` (FUN_80107AFC — sm[0x4a]==5 door/area transition, 3 fade calls), `Engine::fieldRun` case 10 (FUN_80106B98's fade — area-transition subtractive ramp), `Engine::fadeSequencer` (FUN_8010957C — a0l fade SM), `Sop::fieldMode` (FUN_80109450 — SOP intro cutscene, 3 fade calls), `bg_scene_transition_sm` (FUN_8002655C). The un-owned cluster identified is 5 sibling handlers in the game.bin overlay — 0x80108CAC / 0x80108D80 / 0x80108DF4 / 0x80108E58 (subtractive fade-out step) / 0x80108EBC (additive fade-in step) — all read `sm[+0x4E]` state + `sm[+0x6B]` fade counter. THEIR PARENT SM IS NOT YET IDENTIFIED: none of the 5 addresses appears as a raw pointer in any overlay/MAIN.EXE, none is reached by an existing native `d0(c, 0xXXX)` or `rec_dispatch(c, 0x80108...)` call. The parent is a still-substrate switch built via `lui/addiu` (splits addresses across immediates, so a byte-pointer search misses it). Next step: RE the game.bin overlay's function-entry list to find the fn that dispatches on `sm[+0x4E]` with these 5 as target handlers — likely one of 0x80108D64 / 0x80108DA4 / 0x80108F24 or another nearby fn. Ghidra decomp of the parent then port + migrate ownership of the whole SM native (including the 5 leaves) so the fades land on `ScreenFade`. Do NOT port the leaves in isolation — their `rec_dispatch(0x8007E9C8)` calls in the shard body are never reached from native code (top-down PC-driven, no overrides) so an isolated leaf port would be dead code.
- **refs:** user report 2026-07-03; `game/render/screen_fade/screen_fade.h`; `game/scene/engine_stage.cpp` fieldTransition/transitionMain/fieldRun; Ghidra decomps in commit history

## BUG-2: Score gems (AP pickups) render wrong in 3D world
- **status:** OPEN
- **area:** render / entity
- **symptom:** gems dropped by enemies (100/200/500/1000/5000/10000/20000/100000 AP denominations) don't render at the source position — they appear at the wrong world location or origin
- **hypothesis:** the gem-spawner `FUN_80071B44` (still substrate — only its thin wrapper `Spawn::dropScoreGem` is native) stamps `+0x54/56/58` from `sourceNode+0x2E/32/36` ONLY inside its `if (param_3 == '\x01')` branch. Every native callsite passes `param_3=0`, so the position stamp is dormant and the gem inherits whatever pool-slot state the previous occupant left behind. Per-frame render lives in FUN_80071DFC (small) / FUN_80072308 (large) via `FUN_8007e1b8` with scratchpad billboard at `0x1F8000C0..C4` (world pos, Y-200). Verify by porting `FUN_80071B44` native with unconditional position stamp.
- **refs:** user report 2026-07-03; Ghidra decomp of `FUN_80071b44` in commit history; `game/world/spawn.cpp:Spawn::dropScoreGem`

## BUG-3: Wrong SFX plays at some events
- **status:** OPEN
- **area:** audio
- **symptom:** wrong sound effect fires at gameplay events — SFX id mismatch
- **hypothesis:** `Sfx::trigger` (FUN_80074590, native) itself is correct — under `PSXPORT_GATE=1` (all-gameplay recomp) SFX play correctly (user confirmation 2026-07-02). So the wrong-SFX bug is a NATIVE gameplay caller passing the wrong id. Next diagnostic: SBS in gameplay/full mode with an id-parameter watch on the Sfx::trigger boundary — attribute the bad id to a specific native caller.
- **refs:** user 2026-07-02 GATE-mode confirmation; `game/audio/sfx.cpp`

## Tooling gaps (feature requests on the debugging tools, not port bugs)

## GAP-1: SBS divergence report lacks a call stack
- **status:** OPEN
- **area:** tooling / SBS
- **symptom:** `[sbs] *** DIVERGENCE at lockstep frame N: 0xADDR..0xADDR (mode=X) ***` reports the divergent byte range but no attribution — no MIPS caller `ra`, no guest function name, no C++ native call stack.
- **hypothesis:** two orthogonal add-ons — (a) keep a per-Core ring of the last N `jal ra` values captured in the interpreter and dump them on divergence; (b) walk the guest sp for a saved-ra backtrace. For native callers, hook `mem_set_store_watch_cb` to also snapshot `backtrace()` at the moment of the divergent store.
- **refs:** user request 2026-07-03; `runtime/recomp/sbs.cpp` divergence path
