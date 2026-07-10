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

## Sequencer (libsnd SsSeqCalled) cluster wiring — 5 bugs found at §9 re-verify; 8 addresses wired 0-diff, 5 left unwired-honest (2026-07-10)

- **symptom / task:** promote the banked wide-RE Sequencer drafts (game/audio/sequencer.{h,cpp} —
  0x800909C0 frameTick, 0x80090BD0 SsSeqCalled, and 11 leaves) from draft to VERIFIED ownership.
- **status: DONE for 8 addresses (wired via `engine_set_override_main`, SBS-full 0-diff through
  f9030 + 23k-invocation MIRROR_VERIFY byte-compare); 5 addresses DELIBERATELY UNWIRED** (never
  fired in any run — honest-gate rule; drafts stay banked in sequencer.cpp).
- **bugs found at re-verify (fleet-workflow §9 predicted "multiple bugs even in high-confidence
  drafts" — 5 real ones):**
  1. **`channelKeyEventScan` (0x80095B90) stamped the WRONG VALUE to the 0x80105D10 scratch:** the
     draft wrote `tableVal` (the matched pitch); the gen's delay-slot value is `i & 255` — the
     matched **voice index**. `channelKeyRegisterMerge` reads it back as `value*56` (the same
     stride the scan just used for `i*56`), so pitch-vs-index corrupts every downstream KON bit.
  2. **`channelKeyEventScan` omitted the s0(r16) spill/restore entirely** (gen frame: sp-32,
     spills ra/s0/s1/s2 — draft spilled only ra/s1/s2). Root-caused via SBS bisect: f154 diff at
     0x801FE928, A=stale 0x1F800000 vs B=channelBase 0x800BE698.
  3. **Register-lifetime class bug in every structured (non-register-literal) body:** gen keeps
     live values in callee-save regs across calls, and CALLEES spill those regs into their own
     frames — so a draft that keeps the value only in a C++ local leaves stale bytes in the
     callee's spill slots. Instances fixed: `channelNoteInit` (r16=channelBase), `channelReleaseClear`
     (r16=chanStride, r17=channelBase, r18=seqPtrSlot), `frameTick` (r16=0x800AC430 cb-slot base,
     + its whole sp-24 frame was missing), `seqChannelDispatch` (whole loop state r16-r23/r30 —
     rewritten register-literal).
  4. **`seqChannelDispatch` control-flow bug:** gen only tests flag bits 0x10/0x20/0x40/0x80 when
     bit 0x01 was SET (the bit0-clear branch jumps straight to the 0x02 test at L_80090D48); the
     structured draft tested all 8 bits unconditionally.
  5. **Missing `ra` return-site constants:** every call site in the cluster sets r31 to the RE'd
     guest return address before the jal (0x80090C1C/CA8/CD0/CF8/D20/D48/D70/D98/DC0, 0x800909EC/FC,
     0x8009110C, 0x800910B0, 0x80091A38/40, 0x80095C0C, 0x8009567C); the drafts set none of them.
- **wired + how each was verified** (SBS gate = `PSXPORT_SBS_MODE=full` autonav, NOAUDIO; MV =
  `PSXPORT_MIRROR_VERIFY=0x800909C0` single-core run — SBS diff_mode SKIPS the whole per-vblank
  audio block on BOTH cores (game_tomba2.cpp), so NO SBS config can exercise the tick path; the
  strict mirror gate is the byte-verifier for the tick subtree):
  - 0x800909C0 frameTick + 0x80090BD0 seqChannelDispatch — MV: 3000 armed invocations per run
    (23,080 total in the input-driven run), full RAM+spad+ABI-reg compare each, 0 mismatches.
  - 0x800910F0 channelPitchSelectDispatch — MV subtree (212 firings), 0 mismatches.
  - 0x80091970 channelNoteInit — SBS 1/1 hit 0-diff + MV subtree (7 firings).
  - 0x80095B90 channelKeyEventScan — SBS (B-side 1 hit, A ran it natively inside noteInit) 0-diff
    + MV subtree (10 firings).
  - 0x80094B50 channelKeyRegisterMerge — SBS 24/24 0-diff + MV subtree (195 firings).
  - 0x80095530 channelVoiceRegisterWrite — SBS 2/2 0-diff (LOW-confidence draft: survived).
  - 0x800962B0 channelVoiceSelectPrep — SBS 9806/9806 0-diff (heavily exercised by substrate SFX
    callers outside the tick; LOW-confidence draft: survived).
- **deliberately UNWIRED (never fired in ANY run — SBS autonav 9k frames, 12k-frame free-roam MV
  run, 23k-tick input-driven run with jumps + pause menu):** 0x80091050 channelReleaseClear,
  0x80091910 channelStopFlagSet, 0x80090E40 channelPitchSlideTick, 0x80092080
  channelEnvelopeRampTick, 0x80095A9C channelVolumeSnapshot. Their flag bits
  (0x02/0x08/0x10/0x20/0x40/0x80) never came up in reachable content. §9-line-verified drafts stay
  banked; native seqChannelDispatch routes those bits via `rec_dispatch` to the substrate body. A
  future session reaching SEQ content with note-releases/pitch-slides/envelope-ramps should
  exercise, wire, and gate them (nat_ trampolines already exist in registerOverrides).
- **tooling added:** `PSXPORT_SBS_EXIT_FRAME=<n>` (sbs.cpp, docs/config.md) — clean `exit(0)` at
  frame n so atexit hit-count dumps print (a `timeout`-killed gate dies via the watchdog's SIGTERM
  `_exit(130)`, skipping atexit — hit counts are how a wiring pass proves addresses FIRED).
- **oracle-integrity fix:** engine_override_thunk.cpp now consults `verify.inSubstrateLeg` — before
  this, MV_CHECK's "substrate replay" leg on a thunk-wired address ran the NATIVE body and compared
  native-vs-native (fake pass). overlay_router already had the gate; the thunk didn't.
- **dead end recorded:** tried exercising the 5 dormant leaves with an input-driven REPL run
  (jump SFX, pause-menu open/close, walking) — bits never set. Tomba!2's field BGM is XA, not SEQ;
  SEQ appears to be used for jingles/SFX whose reachable content only uses bit0 (pitch select) +
  bit2 (note init) here.
- **refs:** game/audio/sequencer.{h,cpp}, runtime/recomp/engine_override_thunk.cpp,
  runtime/recomp/sbs.cpp, docs/engine_re.md (SsSeqCalled cluster entries), scratch/logs/sbs_gate.log
  + mv_final.log (regenerable).

## `channelNoteInit` (0x80091970) flagged by the new generalized `MIRROR_VERIFY=all` gate — OPEN, needs re-check (2026-07-10)

- Earlier entry above certified `0x80091970` 0-diff via a narrow `PSXPORT_MIRROR_VERIFY=0x800909C0`
  subtree run (7 firings, all reached through the `frameTick` tick subtree). The new generalized
  `=all` gate (docs/config.md "Mirror TDD gate", docs/fleet-workflow.md §9a; hooks
  `engine_override_thunk`/`EngineOverrides::run` centrally instead of per-call-site) reaches EVERY
  caller path, not just the one the earlier targeted run happened to exercise, and flags a v0/v1-
  only ABI-register mismatch on invocation #1 (`ra=0x80091B08`, a caller context outside the
  frameTick subtree — likely the boot-time `SEQ_PREP_FN` (0x800931C0) init path). No RAM/scratchpad
  diff observed, only `v0`/`v1` (native leaves pointer values `0x800BE368`/`0x800BE388`, substrate
  leaves small ints `0x7F`/`0x82` — looks like the same v0/v1-dead-scratch-vs-live-pointer pattern
  as the render cluster, see docs/findings/render.md's `MIRROR_VERIFY=all` entry, but NOT
  individually re-verified here). **OPEN** — re-RE `channelNoteInit`'s callers from
  `gen_func_80093650`/`SEQ_PREP_FN` to confirm whether v0/v1 are genuinely dead at this call site or
  whether the draft is missing a real return value a caller consumes.

## pc_skip vs oracle: SPU register stream divergences — MODE=skip (2026-07-10, OPEN)

- **how found**: operator oracle-compare session, `PSXPORT_SBS_MODE=skip PSXPORT_SBS_AUTONAV=combat`
  (95s): 54 sbs-div lines, ALL `[AUDIO spu_reg]` — guest RAM otherwise clean.
- **shape**: (a) f2–f9 one-sided writes (regs 0x1A6/0x1AA/0x1B0/0x1B2 present on one core only,
  alternating only-A/only-B — boot-order skew of the first SPU reg programming); (b) f890–f894 a
  sustained value lag on 0x1B0/0x1B2 (main-volume pair): A=0x32E8 B=0x32E9 … A=0x330B B=0x330C — the
  pc_skip music volume RAMP runs 1–2 steps behind the oracle's.
- **why it matters**: pc_skip scratch diverges are by design, but SPU registers are observable/
  consumable state (audible) — the skip path must produce the same audio programming as the faithful
  path. Related history: issue #29 (SFX divergence class).
- **repro**: scratch/logs/oc_skip.log.
## pc_skip vs oracle: SPU register stream divergences — MODE=skip (2026-07-10, root-caused)

- **how found**: operator oracle-compare session, `PSXPORT_SBS_MODE=skip PSXPORT_SBS_AUTONAV=combat`
  (95s): 54 sbs-div lines, ALL `[AUDIO spu_reg]`, guest RAM otherwise clean. Full repro (this session,
  same binary/build) reproduces the identical 54-line sequence — see `scratch/logs/final_skip.log`.
  Register offsets (relative to SPU base 0x1F801C00): 0x1A6 = Sound RAM xfer address, 0x1AA = SPUCNT,
  0x1B0/0x1B2 = CD-input volume L/R (`MusicCoord`'s "CDVOL" — game/audio/music_coord.cpp), 0x18C/0x18E
  = voice KOFF, 0x180/0x182 = main volume L/R.
- **mechanism (root cause)**: under `MODE=skip`, core A runs with `pc_skip=true` (the real
  `./run.sh` default config) and core B is the pure oracle (`pc_skip=false`, and — this is the key
  fact the earlier symptom writeup didn't establish — **B ALSO never takes any `EngineOverrides`
  entry under SBS**: `engine_override_thunk`'s `verify.inSubstrateLeg` gate forces the oracle leg to
  stay pure substrate for every wired address, so B always executes the literal recompiled
  `gen_func_80075824`/`gen_func_80075D24` (voiceMixTick/setGain2), never the native C++ port, even
  though the port is globally registered). Confirmed by instrumenting both `MusicCoord::voiceMixTick`
  and `MusicCoord::setGain2` (new `PSXPORT_DEBUG=vmt` channel, `[vmt]`/`[gain2]` traces,
  game/audio/music_coord.cpp) — under the SBS-skip repro, **every** `[vmt]`/`[gain2]` line tags
  `A(skip)`; core B never fires the native method at all (by design — oracle purity), so its
  equivalent state can only be observed indirectly via the SPU register write log.
  - **(a) f2–f9 boot-time one-sided writes (0x1A6/0x1AA/0x1B0/0x1B2)**: value-for-value IDENTICAL
    writes appear on the two cores a few frames apart (e.g. f2 B writes 0x1A6=0x5C22 / 0x1AA=0xC0A0;
    f3 A writes the SAME two values). This is a pure **frame-cadence phase-skew of one-time boot SPU
    programming** — pc_skip's collapsed init reaches the same SPU-init call a few frames earlier/later
    than the oracle's multi-step boot. **Verdict: phase-skew, not a real bug** — confirmed by exact
    value equality across the shifted frames (the diagnostic the new alignment work should use: same
    value, different frame == skew; different value == real).
  - **(b) f890–f894 CD-volume ramp (0x1B0/0x1B2) "1-2 steps behind"**: NOT a simple constant frame
    shift. Tested directly: comparing `A[f]` vs `B[f-k]` for k ∈ {-1,0,+1} over the f882-894 window,
    `k=0` (no shift) gives the SMALLEST **and monotonically shrinking** residual (5,5,5,3,3,4,3,1,1,
    2,2,1,1); k=±1 make it worse or bigger. A pure frame-shift of an otherwise-identical value
    sequence would produce a residual that's either ~0 at the right k or roughly CONSTANT at the
    wrong k's — not a decaying one. The decay pattern is the signature of `voiceMixTick`'s g2
    **exponential smoother** (`g2 += (g2_target - g2) >> 3`, ~12.5%/frame decay) chasing a
    `setGain2`-supplied target (`g2_target`, V+0x2E) whose *call timing* differs by a frame or two
    between the two cores (native override calls for A vs the substrate's own internal call graph for
    B) — i.e. the ramp's OUTPUT looks time-shifted-and-blended rather than plain time-shifted, because
    an IIR filter mixes history across the very call-timing skew that's the real cause. Root mechanism
    is the SAME class of bug as (a) — pc_skip's collapsed load/area-transition cadence firing the
    `setGain2`/`voiceMixTick` chain 1-2 frames off from the oracle's real (uncollapsed) timing — just
    observed through a filter that blends it instead of passing it through cleanly.
    **Verdict: phase-skew propagated through an IIR smoother, not a port-accuracy bug.** Per USER
    directive (2026-07-10): do NOT patch the ramp/smoother to "compensate" for this — that would be
    exactly the kind of magic-constant bandaid CLAUDE.md bans. The correct fix is the frame-rendezvous
    alignment a separate harness agent is adding to `PSXPORT_SBS_MODE=skip` (hold pc_skip's frame
    counter in lockstep with the oracle's real multi-step cost) — once landed, `setGain2`/
    `voiceMixTick` will fire on the same absolute frame on both cores and this residual should vanish
    on its own. Re-verify after that lands; if it does NOT vanish, that's the signal the port itself
    (not the cadence) has a real bug.
  - **f115–f171 (NOT in original symptom scope, present in every repro of this run)**: a third
    cluster around the game's FIRST BGM start (title/menu music) — same register set (0x1AA/0x1B0/
    0x1B2/0x180/0x182) plus voice KOFF (0x18C/0x18E) with genuinely large value deltas (e.g. f157
    `0x18C A=0x0080 B=0xFF00`, f117 `0x1B0 A=0x638C B=0x00C6`) that do NOT look like small-residual
    smoother convergence — this looks like a materially different KOFF voice-mask / CDVOL-snap
    sequence between the two cores at the very first music start, the single most collapsed-load-
    sensitive point in the run (first-ever CD/area load). **OPEN, not analyzed to completion this
    session** — same phase-skew-vs-real-bug question as (b), needs the same k-shift/timing analysis
    once the ramp harness lands; flagging so it isn't silently dropped from the 54-line symptom set.
- **workflow fix (tooling defect found + fixed while root-causing)**: `Timing::logicFrame`
  (`runtime/recomp/timing.h`) was **silently 0 for the entire run under SBS**. It was only ever
  written by the standalone `native_boot.cpp` frame loop (`c->game->timing.logicFrame = f;` right
  before calling `native_step_frame`); SBS reaches `native_step_frame` via `dc_step_frame()` /
  `stepCore()`, which never runs that loop, so every consumer (`Cd::audioTrace`'s `[xa f%u ...]` tag,
  the new `[vmt]`/`[gain2]` traces, any future frame-tagged debug print) silently printed `f0` forever
  under SBS. Fixed by moving the assignment into `native_step_frame()` itself (the single per-frame
  entry point both the standalone loop and `dc_step_frame` funnel through) — `runtime/recomp/
  native_boot.cpp`. This is exactly the workflow-first class of bug CLAUDE.md flags: a diagnostic that
  looked like it worked (never errored, printed a plausible-looking `f0`) but was quietly useless for
  its actual purpose under the one mode (SBS) most debugging happens in.
- **new tooling**: `PSXPORT_DEBUG=vmt` — `[vmt]` traces `MusicCoord::voiceMixTick`'s ramp/smoother
  state every call (cur/tgt/g2cur/g2tgt/base, tagged `A(skip)`/`B(oracle)` + `Timing::logicFrame`);
  `[gain2]` traces every `MusicCoord::setGain2` call the same way. Registered in docs/config.md.
- **repro**: `scratch/logs/oc_skip.log` (original), `scratch/logs/final_skip.log` (this session,
  identical 54-line reproduction), `scratch/logs/vmt_dbg4.log` / `gain2_dbg.log` (per-core internal
  state traces used for the k-shift analysis above).
- **gates**: `MODE=skip` still shows the same 54 `[AUDIO spu_reg]` lines (expected — no ramp code was
  patched, per the no-bandaid directive above; the fix is upstream in the alignment harness, owned by
  a separate agent). `MODE=full PSXPORT_SBS_AUTONAV=combat` (95s) — **0-diff**, unaffected by the
  `Timing::logicFrame` fix or the new debug channel (both are diagnostic-only, no behavior change).