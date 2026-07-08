# Findings — audio subsystem

## Ownership sweep of 0x800A0000-0x800BFFFF found ZERO owned/ownable dispatch-target code — the whole band is DATA (2026-07-08)

- **task:** assigned band 0x800A0000-0x800BFFFF ("audio (SFX/sequencer) tables and various mid-engine
  leaves") — find the busiest still-substrate dispatch targets via `PSXPORT_DEBUG=recdep` on a free-roam
  session, confirm unowned via `tools/codemap.py --addr`, port a cohesive 3-8-address cluster.
- **finding: there is no such cluster — the band is (almost) pure DATA, not code.** Verified three
  independent ways:
  1. **Static function census.** Every `func_XXXXXXXX` the recompiler ever declared
     (`generated/rec_decls.h`, 2214 total) whose entry falls in `[0x800A0000, 0x800BFFFF]`: exactly
     **7** — `0x800A0C58 0x800A0D4C 0x800A44F4 0x800A44FC 0x800A4504 0x800A4514 0x800A5958`. All 7 have
     **zero callers anywhere in generated code** (`grep` for the address as a call target — direct or via
     `rec_dispatch` — turns up nothing) and their emitted bodies are visibly garbage: strings of
     `/* UNHANDLED special/regimm */` comments over nonsense conditions (`c->r[0] == c->r[0]`,
     `(int32_t)c->r[25] <= 0` with no prior use of r25) — the classic signature of the recompiler's
     linear scanner decoding a stretch of **rodata/data bytes as if they were MIPS instructions**. No
     overlay (`ov_*_decls.h`, all 30) places any function in this range either.
  2. **Live `recdep` histogram** (a new `PSXPORT_DEBUG=recdep,recdep-all` full-dump mode was added this
     session — see `runtime/recomp/overlay_router.cpp` / `docs/config.md` — because the default top-40
     cap was hiding rare targets). Ran `PSXPORT_AUTO_SKIP=1` free-roam sessions covering: plain walking
     (8000 frames), continuous attack/Square-hold from f220 (4000 frames), and the original REPL-driven
     newgame/skip/run path (100k+ frames cumulative) — **zero dispatch hits land in
     `[0x800A0000,0x800BFFFF]`** in any of them (full histograms in `scratch/logs/recdep_r{D,E,F}.log`).
  3. **Ghidra headless auto-analysis** (independent disassembler, not the psxport recompiler) on a
     free-roam RAM dump: `tools/decomp.sh decomp <proj> out.c 0x800A0000 0x800BE800` — Ghidra's own
     function-discovery pass (after full `-analyze`) finds **0 functions** in the entire range.
     (`text 0xAE800` loaded at `0x80010000` means the band sits inside the loaded EXE image — it's
     just the rodata/data tail of that image, not a second code region.)
- **what IS there:** heavily-referenced flat state bytes (e.g. `0x800BF870`/`0x800BF816` fade/pause
  bytes, `0x800BE1F8` the voice-mixer struct owned by `MusicCoord::voiceMixTick`, `0x800AC2D4` the
  libcd dir-cache already ported — see fmv-cd.md) and audio/sequencer TABLES read by code that lives in
  OTHER bands (already owned there). None of it is a dispatch target; owning it would mean rewriting
  other subsystems' already-owned readers, which is out of this band's scope.
- **status:** no code ported this session (nothing legitimate to port). Do NOT re-run the recdep hunt on
  this band expecting a cluster to appear — it won't; the negative result is stable across combat/walk/
  long-run sampling and confirmed statically by two independent disassemblers.
- **workflow win:** `PSXPORT_DEBUG=recdep-all` (full histogram, no top-40 cap) is a durable, low-risk
  diagnostic addition — keep it for future band assignments so "zero hits" is a real signal, not just
  an artifact of the top-40 truncation hiding rare/no hits.
- **recommendation:** reassign this band, or narrow future recdep-based band assignments to ranges
  Ghidra confirms actually contain code before handing them out.

## FUN_80075824 was WRONGLY wired to MusicCoord::musicFadeIn — real fn = per-voice volume mixer tick (2026-07-03)

- **symptom (SBS gameplay mode, HEAD before fix):** first divergence at f217, `0x800BE208..0x800BE226`.
  WRITE SITE at f218, mask=2 (B only): core B (PSX gameplay) writes `0x00003037` to `0x800BE208`
  via `pc=0x80075824`. Native (A) never writes. The address is inside the voice-channel struct at
  `0x800BE1F8` (voice[+0x10] = packed volume word).
- **status:** FIXED — `MusicCoord::voiceMixTick(uint32_t voice_base)` ported (game/audio/music_coord.cpp
  + music_coord.h), wired into `Engine::areaUpdateTail` at game/scene/engine_stage.cpp:1329, SBS
  gameplay mode confirms 0x800BE208 no longer diverges (next surfaced divergence is at 0x800EE0DC,
  unrelated).
- **cause:** the native `Engine::areaUpdateTail` had the line
  `c->engine.musicCoord.musicFadeIn();               // FUN_80075824 (native)` — treating FUN_80075824
  as the "music fade in" routine. It is NOT. Ghidra RE (via new `ghidra-re` skill) shows FUN_80075824
  is the per-frame VOICE-CHANNEL VOLUME MIXER for the ambient/XA voice at `0x800BE1F8`:
  ramps voice[+0x2C] toward voice[+0x2A] by ±0x100 (or ±0x400 in cut mode), applies a boost, folds
  in the second-stage smoother (voice[+0x30] ↔ voice[+0x2E]), writes packed volume to voice[+0x10]
  and voice[+0x12], pans to 0x3FFF at voice[+0x04]/[+0x06], and OR's 0xC0 into voice[+0x00] (dirty
  flag). `musicFadeIn` was a PC-added helper that merely writes `0x800BE224 = 0` + `SPU CDVOL = 0` —
  a completely different behavior. The mis-wiring silently deleted every audio-mixer state update
  on the native path.
- **fix:**
  1. New method `MusicCoord::voiceMixTick(uint32_t voice_base)` in music_coord.{h,cpp} — full port of
     FUN_80075824's three branches (silent state / dialog mode / ramp path), the low-vol arm hook
     (`FUN_80075CEC(0x47FF)` inlined, `FUN_800750D8` still rec_dispatched), and the common tail.
  2. `engine_stage.cpp:1329` swapped `musicFadeIn()` → `voiceMixTick(0x800BE1F8)`.
  3. `MusicCoord::musicFadeIn`'s header comment rewritten to stop claiming it's the FUN_80075824
     port (it's an original PC helper for the "music starts too loud on instant-CD" mod).
  4. `tools/codemap.py` regenerated — `docs/code-map.md` now shows FUN_80075824 →
     `MusicCoord::voiceMixTick` (previously wrong).
- **workflow lesson:** the codemap tag for FUN_80075824 (`MusicCoord::musicFadeIn`) was WRONG —
  written from a speculative comment in music_coord.cpp, never verified against the disas. Codemap
  entries derived from `// FUN_XXXX` mentions in comments are UNTRUSTED until the disas confirms
  them. `ghidra-re` skill created this session to make the RE step cheap enough that this bug
  class doesn't recur.
- **refs:** RE output for the port: `$CLAUDE_JOB_DIR/tmp/f80075824.c`, `$CLAUDE_JOB_DIR/tmp/audio_parent.c`
  (parent FUN_80075A80 = `Engine::areaUpdateTail`); SBS extension that pinpointed the write site:
  runtime/recomp/sbs.cpp `recordDivergence` + wwatch-hit block; skills: `sbs-diverge` (find),
  `ghidra-re` (RE), this file (record).
