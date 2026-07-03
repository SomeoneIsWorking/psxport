# Findings — audio subsystem

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
