# Findings — SBS dual-core compare harness (PSXPORT_SBS)

**Vocabulary — see CLAUDE.md "The 5 paths":** SBS runs core A = pc_faithful against core B =
recomp_path (substrate). Both cores get `pc_skip=false` (faithful branch of every fork).
Divergences are FATAL — no residual allowlist. Older notes below refer to the pre-rename
`mIsFaithful` flag; that's `!pc_skip`.

## LIVE-REGISTER LAW: callees spill the caller's caller-saved regs — mirror the full register file at every dispatch boundary (2026-07-16)

- **The class (3rd instance; treat as LAW when porting):** a native port that calls a still-substrate
  fn (rec_dispatch/guest_fn) must reproduce EVERY register gen holds live at that jal — not just the
  documented args — because the callee's prologue/body SPILLS caller-saved registers to the guest
  stack, and SBS full-RAM compare sees the spilled bytes. Instances: MusicCoord::voiceMixTick (the
  original frame-mirror finding below), Engine::walkStart's early-exit ra (above), and
  Engine::padEdgeFence (wave-3: f12 two 2-byte diffs at 0x801FFFAA/CA = spilled upper halves of
  gen's r2=0x800F0000 at the FUN_8005229C tail call; also gen's s0=0x800F0000 held across both
  dispatches, {r2=1, r3=pollFlag} at the sample call).
- **How to derive the live set:** read the gen body at each jal — every register assigned since the
  last call that isn't dead by the jal is live; delay-slot literals count. `abi_extract.py --contract`
  gives the callee-saved frame; the CALLER-saved live set is hand-read (tooling gap — a
  `--live-at-call <addr>` mode would mechanize this).
- **Return side too:** mirror gen's exit clobbers of arg registers (guestMemset: r4=dst+n, r6=0) —
  callers may spill them before reassigning.
- **refs:** game/input/pad_edge_fence.cpp (the annotated fix), runtime/recomp/mem.cpp ov_guestMemset,
  commit 7db286a8.

## walkStart early-exit spill gap @0x801FE8CC — watch-cut f747 divergence (2026-07-16, RESOLVED same day)

- **Symptom**: watch-cut SBS-full leg diverged f747–f779 on ONE word, guest stack 0x801FE8CC:
  A=0x00000040 (stale) vs B=0x8010A990 (an SOP-overlay return address). Self-healed at f780 (slot
  overwritten identically), so a short gate window could MISS it. Combat leg never showed it (the
  triggering call chain is the SOP intro cutscene: Engine::s48_2 → … → ScriptInterp::callFnptr →
  substrate ov_sop 0x8010A900 → FUN_80054D14 with ra=0x8010A990).
- **Root cause**: `Engine::walkStart` (FUN_80054D14, game/core/engine.cpp) placed its
  `GuestFrame<32,4>` mirror AFTER the `cur == mode` early-exit test, with a comment claiming the
  early exit "performs no calls" so skipping the mirror is unobservable. WRONG for full-RAM
  compare: `gen_func_80054D14` descends sp and spills r16@+16/r17@+20/r18@+24/ra@+28 BEFORE the
  compare, so B's early-exit path still writes the caller's ra at sp+28. Only the ra word surfaced
  because A's stale bytes at +16/+20/+24 happened to match B's spills at that sp. Same class as
  the voiceMixTick finding below: a native body that doesn't mirror the frame the gen body pushes.
- **NOT the cause (ruled out)**: the ScreenFade FUN_8007E9C8 leaf tap installed the same day —
  proven via `PSXPORT_THUNK_FORCE_GEN=0x8007E9C8` (divergence persisted with the tap forced to
  pure gen, same f747–f779 window). Divergence pre-existed at HEAD 80d02145; the watch-cut leg
  simply hadn't been re-run across the SOP-touching commits.
- **Fix**: hoist the GuestFrame construction + r16/r17/r18 loads ABOVE the early-exit test,
  matching gen spill order. Early exit now returns through the RAII epilogue like the gen path.
- **Diagnosis tooling path (worked exactly as designed)**: `PSXPORT_SBS_PREWATCH=0x801FE8CC` on
  the watch-cut leg → asymmetric-store class + core-B host backtrace naming the full substrate
  chain + `tools/codemap.py --addr` on each chain node → the one native owner with a frame
  (walkStart) → gen prologue read confirmed unconditional spills.
- **Verification**: watch-cut leg (`PSXPORT_SBS_WATCH_CUT=1`) 0-diff through f16560+ (old
  divergence window f747 passed clean); combat leg 0-diff. (commit this block lands in)
- **refs**: game/core/engine.cpp `Engine::walkStart`; oracle generated/shard_7.c
  `gen_func_80054D14` (L7290); scratch/logs/sbs_cut_prewatch.log (write-site capture).

## spawn-leaf frame residual @0x801FE918 — natural free-roam divergence (2026-07-11, RESOLVED 2026-07-14)

- **RESOLVED (2026-07-14): the diverging 9 bytes are `FUN_80075824` (voiceMixTick)'s guest-frame
  spills, and the fix is the frame mirror in `MusicCoord::voiceMixTick`** (game/audio/
  music_coord.cpp). gen_func_80075824 descends 32 B from sp=0x801FE928 → 0x801FE908 and spills
  r16@+16=0x801FE918, r17@+20=0x801FE91C, r31@+24=0x801FE920 — exactly the diverging words. The
  native body (called from native `AreaSlots::updateTail`, which only runs on the sm[0x4c]==3
  transition path — hence first firing at f389, the door-transition frame of the hut-entry replay)
  had NO frame, so core A's slots kept earlier scratch (render-chain leftovers: 0x8003CD08 =
  cmdListDispatch jal-site) while core B wrote updateTail-context values (r16=0x800C0000,
  r17=0x800BE358 slot cursor, ra=0x80075C58 voiceMixTick jal-site). Fix: GuestFrame<32,3>
  {r17@20, r31@24, r16@16} + r17=channel base and r16=running vol kept live + hi/lo written at
  every gen `mult` + the low-volume leaves dispatched for real (FUN_80075CEC was INLINED as a bare
  store — now `guest_fn(kFnSetFadeTarget, 0x800759B4, 0x47FF)`) + g2 smoother signed/unsigned read
  mirrored + v0/v1 outputs. **Gates: hut-entry replay SBS-full 0-diff through f47850 (was diverge
  @f389 persisting 23k+ frames); AUTONAV SBS-full 0-diff @f2000; default-config boot renders
  field.** (commit this block lands in)
- **Attribution corrections (why 3 prior diagnoses in this finding were wrong):**
  - "spawn-leaf (FUN_80092660) frame spills of r22/r23/r30" — WRONG function. Same address range,
    but the SETTLE values come from the LAST writer of the frame, voiceMixTick's prologue
    (sp=0x801FE908, spills at +16/20/24), not the spawn leaf (sp=0x801FE8E8, spills at +48/52/56 —
    same absolute addresses, different function). PREWATCH with the new s-regs line named it: A
    wrote the slot 16× vs B 17× in f389; B's missing 17th write was voiceMixTick's spill.
  - "render-walk recursion-depth divergence" — red herring (last-writer map of an earlier probe).
  - "beh_id_routed_dispatch runs 554× on A, 0× on B — the A-only gap IS the bug" (FRAMEPROF,
    f302ad5) — FALSE ALARM, same attribution class as gpuDmaSend: B runs the handler's whole chain
    (pc=800519E0/80075FF8 with ra=801219EC/80121A0C on B), but B's stores attribute to callee pcs
    while A's native body attributes to the entry pc. Totals match (e.g. pc=80075FF8: A=733,
    B=733 split across ras). FRAMEPROF one-sided entries need a paired-total check before being
    believed (now documented in docs/config.md).
- **New tooling from this hunt:** `PSXPORT_SBS_REGDIFF=1` (per-frame register-file compare — showed
  frame-boundary regs EQUAL, proving the gap opens+closes intra-frame) and the PREWATCH per-store
  `s:`-regs line (s0..s7+fp). Both in docs/config.md.

### (original investigation log below — attributions corrected above)

- **Symptom:** SBS-full, normal AUTO-NAV area-0 free-roam (no forcing), diverged @f491 at
  `0x801FE91A..0x801FE923` (9 B) and the residual **persisted/wandered** through f1380+ (never
  converged). detection bytes: A=`00 00 00 00 00 00 08 CD 03` B=`0C 80 58 E3 0B 80 58 5C 07`.
  **HEADLESS REPRO (2026-07-11):** `PSXPORT_SBS_MODE=full PSXPORT_NOWINDOW=1 PSXPORT_VK_HEADLESS=1
  PSXPORT_NOAUDIO=1 PSXPORT_PAD_REPLAY=replays/scene-transitions/hut-entry-door-freeze.pad` → diverges
  @f389 (first diverge frame, not f491 — the prior windowed run's f375/f491 were from different
  session state). NOPAUSE run confirms the residual PERSISTS through 23180+ frames — never converges.
  Only 9 bytes in ALL of guest RAM diverge (entire 2MB stays byte-identical except this stack range).
- **RESOLVED (2026-07-11): SBS-full now 0-diff through f2000** after the four register-output mirror
  fixes below. The spawn-leaf r22/r23/r30 residual was downstream fallout of stale caller registers
  propagated by the libgpu/sequencer mirror chain; fixing those upstream mirrors cleared it.
- **CORRECTION (2026-07-11, same day): the RESOLVED claim above was FALSE** — it was based on the
  headless AUTO-NAV gate (0-diff through f2000), but AUTO-NAV does not exercise the path that
  triggers this diverge. A live windowed SBS-full run with real input re-hits the SAME diverge at
  the SAME address (0x801FE91A, f375) on the rebuilt binary WITH all four fixes. The four mirror
  fixes (keyRegMerge/clearOTagR/queueSync/drawSync) are real bugs and correctly fixed, but they were
  NOT the cause of this diverge. The diverge is a **render-walk recursion-depth divergence**: core A
  (native renderWalk) pushes one extra 0x20-byte frame vs core B (substrate renderWalk) between the
  fieldFrameXFaithful frame and the write site, so the two cores' render paths write the same stack
  address (0x801FE918 = cmdListDispatch's ra spill at sp+48) from different call depths. The render
  node lists are byte-identical (same nodes, same node+24 dispatch targets, same live flags); each
  render function (renderWalk/perObjRenderDispatch/cmdListDispatch/perModeDispatch) passes
  MIRROR_VERIFY individually. The divergence only appears in the LIVE chained walk — a register/
  state interaction MIRROR_VERIFY's per-function pre-state reset doesn't reproduce. Still OPEN.
- **NOT the #37 r16-r21/r31 register mirror** (that fix targets r16..r21/r31; this is r22/r23/r30).
  NOT a forcing artifact — the earlier HOLLOW-GATE "forcing caveat" note guessed the FORCES4C
  divergence was an entry-condition artifact; it is NOT — **the same divergence appears in natural
  free-roam play** (verified on a live windowed SBS-full session, no FORCES4C). Correcting that here.
- **Region = the spawn leaf (FUN_80092660) frame's incoming-register spills.** Leaf frame 64 B,
  sp≈0x801FE8E8 (leaf sp = updateTail_sp − 64; updateTail sp = 0x801FE928). Spill slots:
  sp+48=r22 → 0x801FE918, sp+52=r23 → 0x801FE91C, sp+56=r30 → 0x801FE920. These three words are
  what diverge. The leaf spills whatever r22/r23/r30 it was called with.
- **Root cause (wwatch-confirmed):** core A runs the action-arm spawn leaf `0x80092660`
  (pc=80092660, ra=80075B84 = updateTail's jal-site) and writes the region; core B does NOT run the
  spawn leaf on the same frame — its last writer attributed to the region is voiceMixTick
  (0x80075824) at a different sp (0x801FE908), i.e. B never touched these bytes this frame; the B
  values are STALE. So the two cores DISAGREE on whether any slot's action-arm fired: a slot is
  transiently kind==0xFF on A but not B (the slot table reads IDENTICAL on both cores when inspected
  after the fact — kind==0xFF is a 1-frame transient that updateTail itself decrements to 0xFE).
  r22/r23/r30 are CALLER-transit registers (gen updateTail's callee-saved footprint is r[16..21,31]
  ONLY — verified via `abi_extract 0x80075A80 --contract`); the #37 mirror can't touch them, so
  even a perfect updateTail mirror leaves this residual whenever the action-arm fires on one core
  only.
- **Why the cores disagree on the arm fire is the open question.** The arm fires for slots with
  kind==0xFF; kind is set to 0xFF by the A00-overlay per-area object-init handler (the A0X object-init
  family — 0x8010B37C etc., the same overlay-residency-gap cluster as #36/#37). Suspect: a native
  object-init/spawn path on core A primes a slot to 0xFF that the substrate path on core B primes
  differently or one frame late — a transient that updateTail's per-frame kind decrement then
  resolves, but not before the spawn leaf's frame spill diverges. Needs a wwatch on a slot's kind
  byte going to 0xFF to catch the priming site on both cores (the `PSXPORT_SBS_WW_ONVALUEDIVERGE`
  /slot-byte approach, armed before the divergence frame).
- **REFINEMENT (same session, slot-priming RULED OUT):** wwatch on slot 18's kind byte
  (0x800BE310) shows BOTH cores write it IDENTICALLY (same sp 0x801FE928, same backtrace, same
  ww-regs a2=18) — slot priming is NOT the cause; the slot table evolves the same on both cores.
  The real divergence is a **register-liveness / register-faithfulness gap** in r22/r23/r30 high up
  the field-frame call chain (neither updateTail r[16..21,31] nor fieldFrameX r[16,31] touch them —
  they transit from callers above, and the two cores' register files carry different stale values).
  Core B (oracle = correct reference) has r22=`0x800C0000` r23=`0x800BE358` r30=`0x80075C58` —
  updateTail's OWN constants (r20=0x800C0000, the arm-mask addr, the voiceMixTick jal-site) reused
  as scratch by the real game across the field-frame chain. Core A has r22=0 r23=0 r30=`0x8003CD08`
  (a render cmdListDispatch jal-site). So a native faithful mirror in the field-frame/render chain
  is NOT reproducing the exact r22/r23/r30 liveness the substrate does — a register-faithfulness
  gap (the f118-class family). **Definitive next step: `PSXPORT_MIRROR_VERIFY=all` rebuild + run**
  (per-invocation byte+reg compare of every wired override) to catch which native mirror writes the
  wrong r22/r23/r30. The live paused session can't take this further without a rebuild.
- **RESOLVING (2026-07-11, MIRROR_VERIFY=all run headless):** the per-mirror register-output bugs
  are the upstream cause. `PSXPORT_MIRROR_VERIFY=all PSXPORT_MIRROR_VERIFY_CONTINUE=1` surfaced a
  chain of native mirrors that compute memory writes correctly but publish WRONG register outputs
  (v0/v1/hi/lo), leaving stale caller registers across the whole field-frame/render chain — exactly
  the liveness gap that manifests as the r22/r23/r30 residual here. Fixed so far:
  - **0x80094B50 `Sequencer::channelKeyRegisterMerge`** — missing v1 = `~(KON_LO|bitLo)` (gen
    shard_3.c:22073 `r3=~(r0|r3)`). Fixed commit 0ed169e. MIRROR_VERIFY now passes it.
  - **0x80081458 `Render::clearOTagR`** — missing v0 = OT ptr (r16) AND v1 = dummy-tag word
    (`0x04000000|low24(0x800A5A4C)`). Fixed commit 1ff52cb. MIRROR_VERIFY now passes it (0 fails).
  - **Still open (MIRROR_VERIFY):** 0x80080F6C `Render::drawSync` (v0/v1 stale — dispatch target
    returns differ; subtler — the native relies on rec_dispatch(tableSlot60) to set r2/r3 but the
    substrate replay sees different returns) and 0x80082D04 `Render::gpuDmaQueueEnqueue` (hi/lo
    lo=0x200 vs 0x10 — a multiply the native doesn't reproduce). Each is a real faithful-mirror gap.
  - **drawSync RESOLVED (2026-07-11):** the drawSync v0/v1 mismatch was actually in its dispatch
    target `Render::gpuDmaQueueSync` (0x80083364, drawSync dispatches table+60 = this fn). Two bugs:
    (1) gpuDmaQueueSync never published v1 (r3) — gen's final r3 = 0x04000000 on normal exits
    (1024<<16, shard_0.c:34/58) / mem_r32(0x800A5AC8) on timeout exits; (2) gpuDmaQueueSync returned
    readyBit (0x04000000) as v0 on the mode==0 ready-success path but gen does `r2 = r0+r0` (=0) at
    shard_0.c:36 — v0 must be 0; and the mode!=0 tail returned 1 where gen returns 0. Fixed commits
    a4f0e3d + 81c9498. MIRROR_VERIFY now PASSES both 0x80083364 and 0x80080F6C.
  - **gpuDmaQueueEnqueue (0x80082D04) — REMAINING, deeper:** hi/lo (lo native=0x200 substrate=0x10/
    0x3800). Neither the function nor its HLE callees (FN_INT_MASK_SET/ISR_REGISTER/DRAIN/TIMEOUT)
    have any `mult`/`div` instruction. The lo divergence is a CALLEE REGISTER SIDE-EFFECT — gen's
    `Render::gpuDmaQueueDrain` (0x80082FB4, native override) and/or gpu_timeout_arm (0x800834A0,
    orphan) set lo incidentally where the native override doesn't match. Needs per-callee lo
    auditing. Four mirrors fixed this session (keyRegMerge/clearOTagR/queueSync/drawSync); this is
    the 5th and last before the gate advances past the libgpu band.
- **PRECISE WRITE-SITE (2026-07-11, live session re-pause @f408, wwatch on 0x801FE91A):** this is
  the **next layer of the f118 render-stack register-faithfulness family** (docs/findings/render.md
  "f118 residual — RESOLVED" fixed the lower slots 0x801FE8B8/8D0..8F6; this is the next slot UP at
  0x801FE91A..923 that those fixes didn't reach). wwatch caught: **core A** wrote 0x801FE91A from the
  RENDER chain — pc=0x8003CCA4 (perObjRenderDispatch), sp=0x801FE8E8, backtrace render-band
  (0x8003C0BC, 0x8003CD08=cmdListDispatch jal-site), ww-regs a0=0x15(slot idx) s2=0x15. **Core B**
  wrote it from updateTail's spawn-leaf callee — pc=0x80075824, sp=0x801FE908 (0x20 different), ww-regs
  s0=0x800C0000 s1=0x800BE1F8 (updateTail locals). The 0x20 sp gap = the render chain pushed a frame
  on A that B's render didn't (or vice versa) — a nested render frame (perObjRenderDispatch's
  callee, cmdListDispatch or deeper) isn't byte-faithful at this stack depth. Same shape as the f118
  layers the prior session peeled: each fix exposes the next slot up. NOT the spawn leaf's own bug.
- **Tooling used (works on the LIVE windowed debug-server session, no rebuild):** `sbs watch <addr>`
  (rewind-and-arm wwatch on both cores + re-step the divergent frame), then `sbs diff` (last-writer
  pc/ra/sp per core) and `sbs bt` (write-site guest-stack backtrace + `[ww-regs]` a0-a3/s0-s5 per
  core). `@a`/`@b` route reads to each core. Confirmed both cores run updateTail IDENTICALLY
  (wwatch on the arm-mask clear 0x800BE358: same sp 0x801FE928, same backtrace, same ww-regs) — the
  divergence is strictly the transient spawn-leaf spill, downstream of a slot-priming disagreement.
- **Lesson:** stale stack-frame residuals that "wander" (different bytes each re-inspection but same
  address range) are almost always a *does-this-leaf-run-this-frame* disagreement, not a
  register-value bug. Confirm by wwatch-ing the leaf's own spill slot and checking whether BOTH cores
  wrote it this frame; if only one did, the bug is upstream in whatever primes the leaf's trigger, not
  in the leaf or its caller's register mirror.
- **DEFINITIVE HEADLESS ANALYSIS (2026-07-11, BYTETRACE + MIRROR_VERIFY=all):** The diverge is NOT
  a render-path bug (contrary to the "PRECISE WRITE-SITE" note above — the last-writer map pointing
  at the render chain was a red herring; it captured the last write of f389, not the causal one).
  **BYTETRACE** (`PSXPORT_SBS_BYTETRACE=0x801FE918,0x801FE928`) on the 9-byte range reveals the bytes
  are written by **gameplay object-tick functions** (0x8007778C cull family, 0x800517F8, 0x801316CC)
  — NOT render functions. Both cores run the EXACT same ras (0x800777EC×5908, 0x8012F5A4×3146,
  0x80131728×2120, 0x80051834×1779 — identical between cores). The divergence is a VALUE/COUNT
  difference: for byte 0x801FE91C, totA=20753 vs totB=20754 (ONE-OFF); for 0x801FE920, totA=32550
  vs totB=33708 (1158-count gap). The settled-state classifier marks 0x801FE91C and 0x801FE920 as
  REAL (not PHASE/SOFT). This is a cadence divergence where one core runs a gameplay tick function
  one extra time or with different state, producing different stack-scratch residuals.
- **MIRROR_VERIFY=all SURFACED 19 FAILING MIRRORS** — the REAL scope of the faithful-mirror
  register-faithfulness gap. Fixed this session: 0x80082734 gpuLoadImageStream (missing HI/LO write
  from gen's `mult` — the streamer computed w*h into a C++ local but never wrote c->hi/c->lo;
  shard_5.c:13707 writes both halves). This resolved 0x80082D04 gpuDmaQueueEnqueue (which inherited
  the stale HI/LO via its fn-dispatch callee). **19 mirrors still fail:**
  - Render chain: 0x8003C048 (renderWalk), 0x8003CCA4 (perObjRenderDispatch), 0x8003C2D4
    (billboardCompose1), 0x8003D0BC — all "reg" mismatches.
  - Gameplay overlays (all ra=801087DC, the field-frame dispatch): 0x8013A730, 0x801360F4, 0x80139838,
    0x8013AC34, 0x801241BC, 0x80140544, 0x8014047C, 0x80144928, 0x8010B588 — "ram" + "reg" mismatches;
    the native spills wrong callee-saved register values (e.g. 0x801FFFF8 = initial SP where substrate
    spills 0x1F800000 = scratchpad base).
  - Main-band: 0x80079528, 0x8007496C, 0x80075D24, 0x80074A38, 0x80051C8C — "reg"/"ram" mismatches.
  - Demo: 0x80106AC4 (Demo::s3SubMachine) — "hi"/"ram"/"reg" (likely not f389-relevant — demo stage).
  Each is a real register-faithfulness gap; the gameplay-overlay ones (all from the same caller
  ra=801087DC) share a systematic pattern suggesting a common upstream cause in how the field-frame
  dispatch sets up callee-saved registers before calling overlay handlers.
- **NOT FIXED by the hi/lo fix alone:** SBS-full + hut-entry replay still diverges @f389 after the
  gpuLoadImageStream fix (MIRROR_VERIFY passes but the live chained walk still diverges). The 19
  remaining failing mirrors are the candidates; the gameplay-overlay ones (ra=801087DC) are the most
  likely f389 cause since the diverge bytes are written by gameplay tick functions.
- **PROGRESS (2026-07-11, same session, continued):** Fixed 7 more mirrors via MIRROR_VERIFY-driven
  TDD, bringing the failing count from 19 to 12:
  - **Spawn::spawnTypedChild family** (spawn.cpp): rewrote all 4 trampolines to reproduce the
    substrate's EXACT dispatch path (rec_dispatch(0x8007A980) with matching r4/r5/r6, r16/r17 swap,
    r31 jal-site, r3 typeByte). MIRROR_VERIFY PASSES 0x8013A730.
  - **AreaSlots::primeCountdown** (0x80074A38): v0=10, v1=entry addr. PASSES.
  - **AreaSlots::updateCell** (0x8007496C): added 24-byte frame + r31 spill. Still fails MV (v1).
  - **Str::length** (0x80079528): v1=v0 (length counter). PASSES.
  - **MusicCoord::setGain2** (0x80075D24): v0/v1 branch results. PASSES.
  - **ActorZonedAttacker** (actor_zoned_attacker.cpp): 3 GuestFrames added (0x80140544/4047C/44928).
    Still fail MV (need r31 jal-site fix like spawn).
  - **NodeXform::buildAxis** (0x80051C8C): GuestFrame REVERTED — the body already has its own
    BuildAxisFrame internally; adding a trampoline frame caused a DOUBLE frame that moved the diverge
    to f117 (regression). Lesson: check for existing internal frames before adding trampoline-level ones.
  - **Effect on f389:** NONE — the diverge is unchanged. The diverge bytes are written by functions
    that PASS MIRROR_VERIFY individually (0x8007778C/800517F8/801316CC). The issue is a one-count
    cadence difference (BYTETRACE: totA=20753 vs totB=20754 for byte 0x801FE91C) somewhere upstream
    that no individual mirror failure explains. The remaining 12 MV failures are: render chain
    (0x8003C048/CCA4/C2D4/D0BC — v0/v1 only), NodeXform (0x80051C8C), updateCell (0x8007496C),
    overlay (0x8010B588/1241BC/4047C/40544/44928), Demo (0x80106AC4).
  - **NEXT:** The f389 diverge is dead-stack-scratch from a cadence off-by-one. Finding it requires
    a different approach than MIRROR_VERIFY: either a per-frame write-count comparison for the
    diverge bytes (which function writes the Nth time on A but not B), or a deeper audit of which
    native field-frame override changes the iteration count of the gameplay tick loop.
- **FRAMEPROF TOOL REVEALED THE ROOT CAUSE (2026-07-11):** Built PSXPORT_SBS_FRAMEPROF=<frame>
  (sbs.cpp) — counts every store per (pc,ra) per core during the target frame, then reports the
  A-vs-B count deltas sorted by |delta|. First run at f389 surfaced the candidates. **CORRECTION after
  deeper analysis:** the gpuDmaSend (0x80082424) 2048-count delta was a FALSE ALARM — the TOTAL store
  count matches (A=2051, B=2048+3=2051); the ra split differs because native vs substrate set different
  r31 values, but the DMA send count is identical. The REAL one-sided gap is:
  **beh_id_routed_dispatch (0x80121978) — core A runs it 554×, core B runs it 0× during f389.** This is
  a native gameplay AI handler override (`game/ai/beh_id_routed_dispatch.cpp`) that core A's field-frame
  chain dispatches but core B's substrate chain doesn't reach. The object list is byte-identical at f388,
  so both cores should dispatch the same handlers — the divergence is in how the native
  `fieldFrameXFaithful` / `array8Dispatch` chain routes object ticks vs the substrate. Many other paired
  entries (same pc, same count, different ra) are expected native-vs-substrate call-site differences and
  are NOT bugs (the ra differs because native fieldFrameXFaithful at 0x80108C40 replaces substrate
  walkOnce at 0x8007B08C). The beh_id_routed_dispatch A-only gap IS the bug.

## HOLLOW GATE: free-roam SBS never runs native field-frame code (fieldFrameX / updateTail) (2026-07-11)

- **Trap:** "SBS-full 0-diff through fN" in FREE-ROAM does NOT gate native field-frame subsystems.
  In free-roam, pc_faithful dispatches the field-frame chain (e.g. AreaSlots::updateTail @0x80075A80)
  to the SUBSTRATE via fieldFrameFaithful — the NATIVE method never executes. So a native field-frame
  port can be committed "0-diff verified" while never having been byte-compared at all. (This burned the
  #37 updateTail fix: claimed 0-diff@f900, but an `asent` entry-probe showed 0 native calls.)
- **Only path that runs native field-frame code:** the pc_faithful fieldFrameX path, gated on stage
  sm[0x4c]==3. Reach it naturally (an area transition) OR force it: `PSXPORT_SBS_FORCES4C=<frame>:3`
  (sbs.cpp hook, added 2026-07-11) forces sm[0x4c]=3 on both cores mid-run so native updateTail runs
  under compare. ALWAYS confirm the native code actually executed before trusting a 0-diff — add/keep an
  entry-probe (`PSXPORT_DEBUG=asent` logs updateTail ENTER) and require count>0.
- **Forcing caveat:** FORCES4C bypasses the NATURAL transition, which may itself set guest state (e.g.
  callee-saved r22/r23/r30) the forced path doesn't — so a divergence under forcing can be a forcing
  ARTIFACT. The honest gate needs a REAL area/hut entry, which is currently blocked by the A0X MODE-
  overlay code-residency gap: cross-area `warp` recomp-MISSes at the destination overlay's object-init
  0x8010B37C before updateTail runs. Closing that gap (or a hut save-state) is the enabler for a true
  hut-entry #37 gate.
- **MEASURED (2026-07-11, headless SBS-full, AUTONAV=1, the gate this finding was built to run):**
  1. **Hollow gate CONFIRMED:** `PSXPORT_SBS_MODE=full AUTONAV=1 EXIT_FRAME=900 DEBUG=asent NOAUDIO=1`
     → `[asent]` count = **0**, `A/B identical` f0..f870, clean exit @f900. The #37 "0-diff@f900" claim
     (commit de72e40) proved NOTHING — native `updateTail` never executed once.
  2. **Force makes native code RUN:** `FORCES4C=400:3` → native `updateTail` ENTER logged (counter=14,
     loopEntered=1), action-arm spawn leaf 0x80092660 fired 4× (slots 18/19/22/23). The force mechanism
     works; the precondition (native body executes under compare) is met.
  3. **Diverges at f400 in r22/r23/r30** — Range 0x801FE91A..0x801FE923 = the spawn leaf's
     own-frame spills of r23 (@leaf+52) and r30 (@leaf+56). `gen_func_80075A80`'s callee-saved
     footprint is r[16..21,31] ONLY — it does NOT touch r22/r23/r30, which transit unchanged from
     `updateTail`'s CALLER (the field-frame chain). `asent` entry-probe on core A: r22=0 r23=0
     r30=**0x801FFFF8** (the INITIAL SP). Those entry values == the spilled values (faithful transit).
     **NOTE: the original version of this bullet concluded "it IS a forcing artifact" — that was
     WRONG.** The same 0x801FE918..0x801FE923 divergence appears in NATURAL free-roam play (no
     FORCES4C), verified on a live windowed SBS-full session @f491 — see the "OPEN: spawn-leaf frame
     residual" finding directly above. The divergence is a real does-this-leaf-run-this-frame
     disagreement (action-arm spawn fires on core A but not B for a transiently-0xFF slot), NOT a
     forcing entry-condition artifact. The #37 register mirror is not the fix for this (r22/r23/r30
     are outside updateTail's footprint); the upstream slot-priming disagreement is.
- **Lesson (extends "SBS gate honest"):** a green SBS gate only means what it EXERCISED. For any native
  code reached only under specific stage/mode state, force that state AND probe that the native body ran.
  **And:** a divergence under forced state is only actionable once you've shown the divergence is in the
  code-under-test's OWN footprint, not in entry conditions the force failed to reproduce (compare entry
  regs both cores; check the function's callee-saved footprint via `abi_extract <addr> --contract`).

## SBS skips gpu_present_ex → per-frame render bookkeeping never resets → wrong ordering (2026-07-10)

- **Symptom (USER):** SBS-full oracle pane drew the semi-transparent SEA over the fisherman
  sprite (standalone `PSXPORT_ORACLE=1` never does); some standalone-broken renders were
  wrongly "fixed" in SBS. Proof the SBS render was NOT isolated/identical to standalone.
- **Root cause:** SBS steps each core with `diff_mode=1`. `game/game_tomba2.cpp` per-frame tail
  RETURNS EARLY under diff_mode (before `gpu_present`). `GpuState::gpu_present_ex` is the ONLY
  place that resets the per-frame render bookkeeping: `s_prim_order` (OT-submission / VK-depth
  index), `s_seen3d`, `s_frame`, and the native per-vertex depth table (`projprim.reset()`).
  `grabPane` only reset the geometry BATCH (`gpu_gpu_frame_end`). So in SBS `s_prim_order`
  ACCUMULATED every frame and `s_frame` never advanced (freezing the `bg_range`/`obj_depth`
  per-frame span caches) → corrupted cross-frame draw ordering → semi over sprite; both panes
  diverged from their standalone counterpart.
- **Fix:** extracted the per-frame finalize (depth-table reset + `gpu_gpu_frame_end` +
  `s_frame++`/`s_prim_order=0`/`s_seen3d=0` — everything EXCEPT the window blit) into
  `GpuState::frame_finalize()`, called from BOTH `gpu_present_ex` (standalone) and the SBS
  per-core grab (`grabPane` → `gpu_present_finalize`). Each SBS core now runs byte-identical
  per-frame render bookkeeping to a standalone run.
- **Lesson:** SBS suppresses per-core PRESENT (`diff_mode`) but the RENDER FINALIZE bookkeeping
  (depth/order/frame counters) is render-correctness state that must still run per core per
  frame. When adding per-frame render state, reset it in `frame_finalize`, not `gpu_present_ex`.
- **Verified:** build clean; standalone abcompare byte + pixel IDENTICAL to pre-fix; SBS-full
  0-diff through f1500. Fisherman-cutscene picture = USER eyeball (autonav skips it headless).
- **Refs:** commit "sbs: run per-frame render finalize per core"; `runtime/recomp/gpu_native.cpp`
  (`GpuState::frame_finalize`), `runtime/recomp/sbs.cpp` (`grabPane`), `game/game_tomba2.cpp:94`.

## Panes ≠ standalone = SBS special-casing, NOT a cross-core leak (2026-07-10)

- **Symptom (USER):** in `PSXPORT_SBS_MODE=full`, pane A didn't look like standalone
  `PSXPORT_PC_SKIP=0` (pc_render) and pane B didn't look like `PSXPORT_ORACLE=1`. USER read
  it as state "leaking between cores."
- **Not a leak.** Cores are fully isolated: all HW backends (GTE/SPU/MDEC/GPU) are per-`Game`
  (bound per step), `Render`/`GpuGpuState` are per-Core, and SBS RAM is byte-identical A↔B
  (0-diff). The only shared object is the physical `GpuDevice` singleton (correct). The
  mismatch was **deliberate `game->sbs` branches** that made a pane render/execute differently
  than its standalone.
- **Root cause (pane A):** the widescreen margin re-include POKED guest RAM
  (`cull.cpp: c->mem_w8(o+1,1)`) to populate dynamic side-margin entities. That diverges core A
  from oracle B, so it had been bandaided with `!c->game->sbs` — which is exactly why pane A ≠
  standalone wide. A render enhancement writing guest RAM violates the read-only-overlay rule.
- **Fix (USER: "wide rendering must be guest-read-only"):** drop the poke entirely; re-include
  margin geometry READ-ONLY in every aspect via the existing `MarginRenderer` host overlay
  (`cull.cpp`). No poke, no `game->sbs`/`gpu_gpu_wide_engine` branch → pane A == standalone AND
  core A stays byte-identical to B (0-diff). Also dropped `gpu_gpu.cpp` AUTO `ww/=2`-under-SBS
  (each core derives full-window FOV like standalone; compositor letterboxes the pane).
  Trade-off: dynamic entities in the extreme wide margins drop until each type gets a read-only
  render (wide de-prioritized; `PSXPORT_MARGIN_POKE=1` restores the poke for A/B diffing).
- **Object-cull relaxation ≠ wide.** The "game over-culls even at 4:3" relaxation the PC side
  already does IS this read-only margin render (culled objects still draw, so they don't pop);
  it is aspect-independent and guest-write-free — do not confuse it with the removed guest poke.
- **Verified:** build clean; SBS-full headless 0-diff through f600. Live wide visual parity =
  USER eyeball. **Remaining special-case:** `native_fmv.cpp:676 if(game->sbs) return 0` skips
  FMV under SBS (blocking player the harness can't drive) — needs a non-blocking rework.
- **Refs:** commit "sbs: wide margin read-only in all aspects"; `game/render/cull.cpp`,
  `game/render/margin_render.cpp` (read-only host overlay, static type-0x03 only today).

## Faithful frontier: f117 -> 32k+ zero-diff; skip observable gate passes the area load (2026-07-08)

- **Mirror TDD fleet green** (wf_bf6b0541, ~20 rounds): every wired faithful mirror passes
  MIRROR_VERIFY=all (74k+ checks, zero mismatch). Fix classes found by the gate, in order:
  walkAll/walkAux/frameStartTick missing gen frames; rand ABI end-state (v0/v1/hi/lo of the LCG);
  sceneRenderListBuilder v0/v1; modePerFrameDispatch routing the REBUILT seaside handler on the
  faithful path; camera updateFaithful substrate leaves missing a0=CAM_OBJ. Standing rule
  confirmed twice: REBUILT natives (behaviors, per-area handlers, camera helpers) are pc_skip
  shortcuts — the faithful path dispatches substrate until each gets a faithful conversion.
- **SBS full+autonav: 32,500+ lockstep frames byte-identical** (NOPRESENT throughput run, still
  going at commit time) — from f117 at the start of the arc.
- **SV_CHECK (skip-vs-oracle observable gate) first pass**: the area-load fork (0x800452C0 oracle)
  passes on the full observable set including 512 KB SPU sample banks, WITH the preload_cel slot
  write-back fix (bug #29). Gate detail: skipCheck pre-delivers event 0xF0000009 so SPU-upload
  polls complete inline in the oracle leg.
- **refs**: commits 9156324..cc06984; game/core/verify_harness.{h,cpp}; observables.h.

## Strict mirror TDD gate landed (2026-07-08) — past "verified" claims are UNVERIFIED

- **USER directive**: "Things have gotten this bad due to not having TDD in the first place —
  [the earlier porting sessions] said [they] verified all the steps, meanwhile obviously [they]
  didn't verify anything." Consequence: NO existing "verified"/"byte-exact"/"gated" note in this
  repo may be trusted unless a gate run (SBS lockstep or the mirror gate below) is cited with its
  result. Treat unverified faithful mirrors as WRONG until a gate passes.
- **The gate**: `PSXPORT_MIRROR_VERIFY=all|0xADDR[,..]` + `MV_CHECK(c, addr, xFaithful())` at
  pc_faithful fork sites (game/core/verify_harness.h strictCheck): runs the native mirror AND
  replays the pure substrate body (EngineOverrides suppressed = core B) from the same pre-state,
  byte-compares RAM+scratchpad+ABI regs (v0/v1, s0-s7, gp/sp/fp/ra, hi/lo) with NO exemptions
  (no dead-stack window — the old VerifyHarness::run window exemption is exactly how false
  positives slipped), aborts with per-byte attribution. Yield-free mirrors only.
- **First catch**: armed on 0x80108B0C it named the walkAll (0x8007A904) missing sp-=24 frame
  (native writes at 0x801FE870+0x18 vs substrate) in ONE run — the same defect SBS took a whole
  lockstep run + DISPWATCH triangulation to localize.
- **Systematic audit (sonnet fleet, wf_6726383d)**: ALL 18 field-frame-path natives audited are
  missing their gen frame discipline (frame push/spills/jal-site ras) — the pre-TDD ports
  reproduced the STORES but not the STACK BYTES. Fix fleet + TDD loop: wf_bf6b0541.
- **refs**: game/core/verify_harness.{h,cpp}, docs/config.md MIRROR_VERIFY, skill sbs-diverge
  "per-function TDD gate" section, memory feedback_delegate_mirror_audits_to_cheap_agents.

## Rewind-and-arm is UNSOUND with live fibers — skipped; sbs diff shows detection-time bytes (2026-07-07)

- **symptom**: attached to a paused f33 divergence (main-thread stack top 0x801FFFC8..) over the
  debug server; `sbs diff` and live `r` dumps showed A and B IDENTICAL at the recorded range;
  after `sbs step 1` the wwatch hit mask=3 with BOTH cores writing the SAME value — the
  divergence appeared to have "healed", and a NEW diff (task0 state 0x801FE000 A=00 B=02)
  appeared instead.
- **cause (two tooling defects, both fixed)**: (1) `sbs diff` re-read LIVE memory — after
  rewind-and-arm both cores are restored to the pre-frame snapshot, so live reads match; the
  detection-time bytes were only on the terminal's stderr. (2) the rewind itself is UNSOUND when
  coro fibers are live: `sbs_restore_sched` deletes live Coros, and a task parked MID-BODY
  cannot be replayed by respawning from its entry — the re-step runs different code, producing
  artifact state (the fake 0x801FE000 diff). The old guard only checked core A's native_fiber.
- **fix**: recordDivergence snapshots the detection-time A/B bytes; `sbs diff` prints them with
  `^^` diff markers + live bytes labeled separately + the first differing byte's last-writer.
  New `sbs lw <hex>` queries the last-writer map over the debug server. The rewind is skipped
  whenever ANY fiber (coro or native) is live on EITHER core — last-writer map is the write-site
  source (log: "rewind skipped (fiber live — coro replay is unsound)").
- **FALSIFIED**: the sbs.cpp comment claiming the restore's fresh-coro re-entry "spawns a new
  fiber from a clean stack" and is therefore sound. It is only sound for tasks that never
  yielded mid-body — i.e. almost never in the faithful boot flow.
- **refs**: runtime/recomp/sbs.cpp (recordDivergence, sbs_restore_sched, dbgCmd), skill
  `sbs-diverge` (Do NOT section), this session's repro: scratch/logs/sbs_diverge_f33.log.

## SBS = strict pc_faithful, hard-wired (2026-07-07, ba4f50b) + FUTURE: pc_skip observable-output compare

Core A is hard-wired to pc_skip=false — no flag; the SBS harness IS the strict oracle compare,
no scratch mask ever. `PSXPORT_SBS_MODE=full ./run.sh` is the whole invocation.

**FUTURE HARNESS MODE (user 2026-07-07, not yet built):** a separate compare for pc_skip=true
that checks only state affecting the OBSERVABLE OUTPUT — pc_skip legitimately collapses cadence
(different transient scratch), but everything the game/player can observe (consumable interface
state, persistent world state, the picture, the audio) must still match the oracle. Design
sketch: reuse the two-core lockstep but with a curated observable-state compare set (task-slot
consumables, done flags, libcd dir cache, persistent RAM regions, fx/SPU tables) instead of the
full byte compare — the exact opposite of an allowlist: a positive list of what MUST match.

## Faithful SOP intro byte-exact (2026-07-07, c4c27a3) — frontier f114 packet-pool pointer

Sop::fieldModeFaithful (mirror of ov_sop_gen_80109450) + Sop::areaLoadFaithful (mirror of
ov_sop_gen_80109164, task-1 fiber body) landed; the ENTIRE SOP intro cutscene holds strict
byte-exact lockstep (autonav f29 -> f114). The pre-fiber slip machinery (sop_field_step defer,
RNG compensations) is pc_skip-only now.

**OPEN FRONTIER f114 — RESOLVED TO: the FIELD ENTRY (submode1 conversion).** PREWATCH persist
on 0x800BF544 (sbs_v26): f112/f113 both cores write only the frame-top pool reset (identical);
at f114 core B makes 9+ packet allocs from pc=0x80078CA8 ra=0x800793B4 at **sp=0x801FE910 =
the task-0 stack** — the allocs are the GAME task's own slice, not the frame loop. f114 is the
SOP intro completing (state 4 teardown -> sm[0x4c]++ -> submode0 s4c==1 -> sm[0x4a]++): the
FIELD AREA MACHINE begins. A runs native Engine::submode1 (the pre-fiber rebuild with
native_transition_area_load and the submode1Case0Faithful/Skip split); B runs guest 0x801088D8
whose state work builds sprite/BG packets (FUN_800793xx). **Next arc = the submode1 subtree
conversion** — the walkable field: mirror gen ov_game_gen_801088D8 (7 states on sm[0x4c], its
state-0 spawn-and-wait of the area-DATA load 0x800452C0, states 2..6 field running sub-machine
handlers), same recipe as fieldModeFaithful. This is the LAST major stage surface before field
gameplay is oracle-covered (where bug #29 gets tool-flagged).

## Faithful s48_2 chain discipline (2026-07-07, 640d23e) — frontier f29 = SOP field-mode

The RUNNING chain (frame -> s48_2_frame -> submode0) now mirrors the guest byte shape (frames,
spills, per-case jal-site ras from gen 80108784 / 8010882C). Autonav frontier f28 -> **f29**.

**OPEN FRONTIER f29 — the SOP field-mode subtree.** At f29 (SOP intro sm[0x50]==0 first work
frame) A runs native Sop::fieldMode (game/scene/sop.cpp:471) vs B's guest 0x80109450, and they
differ in SUBSTANCE, not just frames: different GP0 packet values in the packet pool
(0x800BFE68: A=A0A0/0E04 vs B=5A60/0A03), different file-table values (0x800BE0E0 A=075F vs
B=17D9), and A never makes B's 0x800834A0 (libgpu timeout-arm) call from jal site 0x80109218.
Sop::fieldMode is a pre-fiber-era REBUILD full of hand-tuned slip machinery (sop_field_step
defer, RNG compensations) — the same class demo_s0_step was, now obsolete under the fiber model.

**Next-session recipe:** convert case-by-case against gen ov_sop/ov_game 0x80109450 (find its
gen body first — 0x80109450 is the loaded MODE overlay's field-mode fn), with the established
pattern: guest frame discipline + call-site ras + organic yields (spawnAndWait primitive) +
lib leaves dispatched (lib-fallback directive). Delete the slip machinery as the fiber
structure supersedes it. Each sub-case lands with a strict autonav run naming the next byte.

## Faithful GAME-stage fiber + ra sweep (2026-07-07, 4f0afba) — autonav frontier f28 = native submode0 vs guest

GAME stage runs on the stage fiber (Engine::stageBodyFaithful); runGameStanza is pc_skip-only.
Two divergence classes closed en route, both caught by the last-writer map in one run each:
- **pc_render guest write** (f26): submit.cpp:537 dispatches guest transform-setup 0x800597AC
  from the render walk — guest-stack spills from the READ-ONLY overlay. Filed as issue #32.
  UPDATE 2026-07-07 (user, later session): UN-DEFERRED — "PSX render path should always be
  active underneath even when PC rendering; PC renderer shouldn't write to guest memory; then
  rendering should never affect diverges." FORCE_PSX_RENDER is no longer the accepted strict-
  compare workaround; fixing the render-walk guest writes is the active arc (issue #32).
- **Missing guest call-site ra** (f27 + menu wrappers): native handlers dispatching leaves with
  r31=DEAD0000 — the callee prologue spills r31, so every native dispatch must set the RE'd jal
  site first (s48_0/s48_1, demo s2/s3/s6 wrappers + inner cf2c/750d8/106824/106690 sites).

**OPEN FRONTIER f28 (autonav/New Game):** sm states lockstep-identical, but A's frame() takes
the NATIVE submode0 (SOP intro, s48==2/s4a==0) while B runs guest 0x80108784 (B writer
pc=0x8010882C ra=0x801087CC). Next arc: faithful conversion of the s48==2 sub-handlers
(Engine::submode0 SOP intro, submode1 field area machine) — same recipe: guest frame
discipline + call-site ras + organic yields on the fiber; per the standing directive these are
GAME/SOP engine code and must stay native (no substrate routing).

## Faithful IDLE LOOP zero-diff 137k+ frames (2026-07-07, 2acc712) — frontier now GAME-stage entry (autonav f26)

With the attract s7 dispatching the real 0x80106C24 phase machine (its FUN_80044BD4 area-load
and inner yields park the DEMO fiber naturally), the strict headless compare holds the ENTIRE
idle cycle — boot -> title -> menu -> 450-frame idle timer -> attract demo -> loop —
BYTE-IDENTICAL for 137,730+ lockstep frames ("[sbs] f137730: A/B identical").

**Frontier (needs input, PSXPORT_SBS_AUTONAV=1):** f26 on the New Game path — the GAME stage
entry (0x8010637C). runGameStanza is still the per-tick native dispatcher (the pre-fiber shape
DEMO had); it needs the same stage-fiber conversion (body on a Coro, substrate tails/yields via
the EngineOverrides primitives, lib glue dispatched per the lib-fallback directive). A-side f26
writer: pc=0x800597AC ra=DEAD0000 (native chain) vs B pc=0x80082D04 (libgs zero-fill) in the
main-thread stack top 0x801FFFC8... Also: the autonav run SEGFAULTS after the divergence pause
(rc=139) — diagnose alongside the conversion.

**Tooling:** PSXPORT_SBS_NOPRESENT=1 — skip pane readback+present for headless compares
(~1000x throughput once real 3D renders; the compare needs no pixels).

## Faithful frontier f2 -> f11 (2026-07-07 session 2) — task-1 + DEMO on native fibers; LAST-WRITER map lands

Strict compare progression (each step committed, pc_skip + GATE regression-checked):
f2 (task-1 fiber, eed1181) -> f8 (DEMO stage fiber) -> f9 (0x8005082C stale-args fix: a0-a2
must be zeroed before the prologue dispatch) -> f10 (menu machine state-0 runs the REAL
0x80106F80 — the Setmode native no-op was a pc_skip-era shortcut in the shared path) ->
**f11 OPEN** (all in dff6973).

**The reusable pattern:** the divergence names either (a) a cadence gap — fix by running the
arc as a native FIBER body (yields via the EngineOverrides-wired primitives; counts emerge from
real control flow), or (b) a pc_skip-era shortcut baked into a shared path — fix by giving the
faithful fork the REAL substrate dispatch (the platform acks the old "busy-loops forever" libcd
calls now: state-0 CdControlB Setmode, state-7 CdlPause teardown — core B proves it every run).

**f11 CLOSED (29fe1a0):** 0x80075A80 is the per-frame AUDIO-COMMAND QUEUE PROCESSOR (drains the
24-slot queue at 0x800BE1F8 into libsnd) — NOT an "attract render" as older comments claimed.
The faithful DEMO tail now dispatches the substrate body per the lib-fallback directive; the
native AreaSlots::updateTail port serves pc_skip. Frontier moved f11 -> **f461**: the title's
450-frame idle timer (sm[0x5a]) expiring into the ATTRACT DEMO launch — s7's phase machine
(0x80106C24) uses native+sync area-load shortcuts (native_transition_area_load etc.) in the
shared path; same faithful-fork treatment needed (fiber-yield for the cooperative loads,
substrate dispatch for lib glue). A-only writer at f461: pc=0x8009A480 (libsnd clear) ras
0x80106688/0x80079484. Note: without AUTONAV the strict boot compare idles into attract at
~f461 — 460 frames of boot + full DEMO menu flow hold zero-diff as of 29fe1a0.

**OPEN FRONTIER f11 (historical, closed above):** 0x800BE1F8 (A=0xC0 B=0x00), last-writer A pc=0x800998E4 (libsnd, stale
native ra) vs B pc=0x80099490 ra=0x80075C60 (substrate per-frame audio tick). The native music
engine (native_music/musicCoord, later-219) replaced the PSX sequencer wholesale — under strict
faithful the per-frame libsnd tick state must byte-match, so the audio engine needs its
faithful fork (dispatch the substrate tick under pc_faithful, or port it byte-exact). This is a
session-scale arc: map the audio tick call graph FIRST (FUN_80075C34/0x80075C60 family,
SsSeqCalledTbyT at 0x80099xxx) before chasing bytes.

**Tooling landed (dff6973):** per-core per-byte LAST-WRITER map ({pc, ra, frame} recorded on
every store while SBS runs; PSXPORT_SBS_LASTWRITER=0 disables) printed at divergence — the
rewind-free write-site mechanism. Rewind-and-arm is auto-SKIPPED when a native fiber is live
(fibers cannot be rewound; the replay used to hang in ~Coro::join — blocker gone). The store
hook chain now carries the write width.

## f0 strict-faithful divergence CLOSED by the faithful-execution model (2026-07-07); frontier now f2 FUN_800754F4 yield cadence

**Status: the f0 0x801FE808..0x801FEA99 divergence below is FIXED** (commits a80c9f8 + cb1f0d7,
implementing docs/faithful-execution.md). The "proposed resolution / dead-byte tracker" idea in the
anatomy entry below was superseded by the user's ruling: no exemptions — the bytes are produced
organically instead.

- **Ported scheduler primitives** as PcScheduler methods with full guest-stack discipline (frame
  descents, live s-reg/ra spills, task-slot writes): `yieldPrim` (FUN_80051F80), `spawnPrim`
  (FUN_80051F14), `spawnAndWait` (FUN_80044BD4), `forceClose` (FUN_80052010), `selfClose`
  (FUN_80051FB4). Registered in EngineOverrides at their guest addresses (boot.cpp); core B never
  consults the table. Byte shapes from the generated recomp bodies (gen_func_* in shard_2/3/5/6.c).
- **STAGE-0 native fiber body**: `Engine::startBinStageFaithful` is the complete ov_start_gen_8010649C
  port run on a Coro fiber (`PcScheduler::runStage0FiberStanza`) — per-iteration CdlFILE records at
  sp+16+i*24 (NOT one reused buffer; this was the 0x801FE848..A98 string-bytes gap), RECT at sp+400,
  XA CdlFILE at sp+408, guest s-regs live through the loops, SM loop suspending naturally inside the
  ported primitives. emit_fun_*_prologue byte blobs + stage0AdvanceFaithful step machinery DELETED.
- **Task-1 FUN_80044F58 body**: `Asset::loadTexgroup` frame discipline + `unpackGroupFaithful`
  (FUN_80044E84 frame + libgs LoadImage/DrawSync dispatched at the live guest sp) + lzDecompress
  guest-frame offset table (gen_func_80044D8C sp+0..28).

**Verified:** f0/f1 hold ZERO-DIFF strict. First divergence now **f2** (0x800AC638 gfx-ctx +
0x800BE36A.. area file-table + done_flag): core B's task-1 FUN_8004514C fiber YIELDS INSIDE
FUN_800754F4 (SEQ/VAB VRAM build; `[yieldpc] waitloop=0x8007542C sp=0x801FF188`) so its body spans
frames, while A's `preloadStage1AsTask` completes in one tick. Next chunk: port the FUN_8004514C arc
the same way — run it as a native fiber body (extend runTask1PreloadStanza or generalize the native-
fiber stanza) and RE FUN_800754F4's yield loop (Ghidra) so A parks at the same points with the same
frames. pc_skip (`./run.sh`, 120f stage=801062E4 sm48=2), GATE (200f), and standalone
PSXPORT_PC_SKIP=0 (200f reaches DEMO) all verified non-regressed.

**Known tooling issue:** the SBS rewind-and-wwatch replay can hang in `~Coro` (join) when tearing
down a desynced native fiber after RAM-snapshot restore — fiber C-stack position no longer matches
restored guest state. Watchdog SIGINT shows `runStage0FiberStanza -> ~Coro -> thread::join`. Harness
interaction only (the pre-rewind divergence report is already printed); fix when it next blocks a
diagnosis.

## f0 strict-faithful divergence ANATOMY (2026-07-07, historical — see CLOSED entry above) — dead MIPS stack scratch vs live yield context; reorg verified non-regressive

Strict compare (`PSXPORT_SBS_MODE=full PSXPORT_SBS_PCFAITHFUL=1`) diverges at lockstep f0,
0x801FE808..0x801FEA99 (task-0 stack). Verified IDENTICAL signature on pre-reorg c308fd6 (same
frame/range/write-pcs/CLASS) — the 2026-07-07 OOP reorg did not regress this; it is the frontier
c308fd6's own commit message left open ("Not yet closing the 0x801FE808 divergence").

**Anatomy (BYTETRACE 0x801FE808,0x801FEA99, both cores, f0):**
- B (substrate coro): runs the WHOLE ov_start 8010649C state-0 body in f0 — CdSearchFile per file,
  each call leaving its OWN stack locals (24-byte-stride entries with filename+";1" copy buffers,
  the "…AB1"/".DAT" ASCII at 0x801FE848..0x801FEA98) + FUN_80044BD4 wait-loop yield spills
  (ra=0x80044CA8 etc at sp+16, B-ras 0x80044C2C/0x80044C50/0x80044CA8) — suspended mid-wait at
  frame end with live sp = 0x801FE7F8.
- A (native faithful): startBinStageFaithful does the SEMANTIC work (sp descent 456 + s-reg saves,
  FUN_80081218 LoadImage dispatch, LibcdNative::searchFile filling the real dest tables, task-1
  native_task_spawn, RNG stamp, sm:=1, FUN_80044BD4-prologue emission) — but writes NONE of the
  substrate CdSearchFile stack locals (A×0 on every string byte) and its dispatched substrate
  leaves scribble their own frame zeros at 0x801FE818..834 (A×37, B×1).

**Structural conclusion (revalidates the "doomed slip-by-slip" verdict from the fiber-era note):**
the divergent bytes are transient MIPS callee stack locals. Native C++ cannot organically produce
them; emitting them synthetically (emit_fun_*_prologue) is hardcoded-offset transcription and
c308fd6 admits it does not close. Three standing directives collide on exactly this byte class:
(1) strict byte-exact pc_faithful, no residuals; (2) never route pc_skip=0 to the fiber; (3)
rebuild-don't-transcribe / no magic offsets. For PERSISTENT state all three hold together; for
DEAD transient stack scratch they cannot.

**Proposed resolution (pending user decision):** make the existing "memory the still-recomp side
never reads" exception MECHANICAL for stack bytes: extend SBS with a read-after-boundary tracker —
on divergence in a stack range, arm a READ-watch on core B; bytes B never reads before rewrite are
proven dead → exempt by the existing rule (not an allowlist; evidence per byte per run). Bytes B
DOES read back (live yield context: suspended-frame spills, task+0x00/02, done_flag, RNG stamps)
stay strict — and THAT bounded surface is closed properly by porting the scheduler primitives
(FUN_80051F80 yield / FUN_80044BD4 spawn-and-wait / FUN_80051F14) as native PcScheduler methods
wired via EngineOverrides (2026-07-07 mechanism) writing real context at the real guest sp, with
per-frame slice cadence matching the substrate scheduler loop.

**Refs:** $CLAUDE_JOB_DIR/tmp/sbs_faithful.log (auto-diagnosis), sbs_bytetrace.log (write
inventory), sbs_c308.log (baseline), scratch/decomp/task_spawn.c + f0_writers.c (RE),
game/core/engine.cpp startBinStageFaithful/stage0AdvanceFaithful + emit_fun_* (current state).

## PC_FAITHFUL byte-exact IN GAMEPLAY (2026-07-04) — M_GAMEPLAY 22,980+ frames zero-diff

After the fiber-only design (below), tested via `PSXPORT_SBS_MODE=gameplay
PSXPORT_SBS_AUTONAV=1` — autonav navigated both cores to field gameplay-start at
f216, then held zero-diff for **22,764+ frames of real gameplay** (~12 min of game
time). No divergence. Job #1 verified end-to-end.

**Choose SBS mode carefully:**
- `PSXPORT_SBS_MODE=gameplay` — psx_render on BOTH cores, gameplay differs (A native,
  B substrate). This is the correct mode to validate pc_faithful gameplay. Any
  divergence = real gameplay bug.
- `PSXPORT_SBS_MODE=full` — psx_render on B, pc_render on A. Suitable for testing the
  full end-to-end path, but the two render paths produce different GP0 packet counts,
  so the packet-pool write pointer at 0x800BF544 (and its snapshot at 0x800BF4F4)
  legitimately diverges once real rendering begins (see f217 finding below). This
  isn't a gameplay bug — it's the render-mode mismatch, by design.

## SBS tooling: wwatch fire now prints guest + host backtrace inline

Prior to 2026-07-04, wwatch captured `mWwBtA/B` (guest bt) and `mWwHostBtA/B` (host
bt via `backtrace()`) but only surfaced them via the `sbs status` debug-server
command — invisible in the auto-run log. Fixed: each `[sbs-ww]` fire now prints both.
The host bt is the key one when the guest stack is unwound (sp near stack top) — it
names the exact native C++ function that called `Core::mem_w*`. Resolve unknown
offsets via `addr2line -Cf -e scratch/bin/tomba2_port <hex>`.

### f217 (M_FULL only) — pool ptr snapshot divergence — RENDER-MODE, not gameplay

Under `PSXPORT_SBS_MODE=full` + autonav, divergence appears at f217 at 0x800BF4F4.
Root: `native_step_frame` at native_boot.cpp:105 copies the previous frame's pool
pointer from 0x800BF544 → 0x800BF4F4. On A (pc_render), `Render::frame` gates on
`mode.psxRender()` and skips the substrate render orchestrator (0x8003f9a8). On B
(psx_render), the substrate builds packets and advances the pool. So end-of-frame
0x800BF544 diverges → line 105 reads different values → 0x800BF4F4 diverges.

**Expected artifact of running two different renderers.** Not a bug. Under
M_GAMEPLAY both cores use psx_render, both advance the pool identically, no diff.

## Historical (2026-07-04) — Slips #6/#7/#8/#9 all obsoleted by the fiber-only design

`PSXPORT_SBS_MODE=full`: A/B byte-identical for at least 750 consecutive frames through the whole
boot chain (SCEA → OP.STR skip → title → DEMO). **Job #1 architecturally SOLVED.**

- **Fix (scheduler.cpp):** under pc_faithful (`Game::pc_skip == false`), NONE of the native task
  handlers run. DEMO/GAME/SOP/STAGE-0 stanzas all bow out early; every task routes to the Coro
  fiber and runs its substrate body end-to-end. A runs the same substrate B runs → byte-identical
  by construction. Under pc_skip (default `./run.sh`), the native handlers stay active and take
  the collapsed one-step shortcuts.
- **Why the prior slip-by-slip approach was doomed:** every native handler that reproduces a
  cooperative-yield substrate (startBinStageFaithful, s0PreYield's fake spawn, run_demo_body_
  faithful's two-tick split) had to guess FUN_80044BD4's per-frame wait-loop iteration count,
  FUN_80051F80's yield cadence, FUN_800506D0's sleep-decrementer read timing, RNG-stamp order,
  libgs DMA-state ordering, and so on. Byte-emit hacks matched f0/f1 but drifted at f6 (Slip #10
  candidate — task+0x02 cadence), f8 (task+0x56 RNG stamp), etc. Fiber-only under pc_faithful
  makes all of that trivial: the actual substrate does it.
- **Consequence:** the following slip notes below (#6, #7, #8, #9 and their step-spread cousins)
  no longer describe live divergences — they were papering the FUN_80044BD4 cadence gap with hand-
  ported stack writes. Under the fiber design, none of that matters — those code paths only run
  under pc_skip, which doesn't gate on byte-match. Kept as historical trail. `startBinStageFaithful`
  and `sync_preload_spawn` in engine_stage.cpp are now DEAD under pc_faithful (fiber takes STAGE-0
  first); they only reach the `startBinStageSkip` shortcut for normal PC play. Cleanup deferred.
- **What pc_faithful must NOT do (design invariant):** implement a "faithful" native reproduction
  of a cooperative-yield substrate body. If the body yields, it goes to fiber. Native OOP is for
  code that is either (a) leaf math / rendering / pure logic with no yields, or (b) a pc_skip
  shortcut that produces the same end-state without cadence-matching. There is no third category.
- **Refs:** scheduler.cpp `has_native_handler_for_entry` (returns false when `!pc_skip`);
  `run_demo_stanza` / `run_sop_area_load_stanza` / `run_game_stanza` guards; commit landing this
  is the "pc_faithful: all-fiber" commit.

---

## Historical Slips (pre-2026-07-04 — obsoleted by the fiber design above)

## Slip #7 (OPEN) — libgs DMA-state divergence at 0x800AC61C — call ordering between A's native startBinStage LoadImage dispatch and B's task-1 fiber preload chain (2026-07-04)
- **symptom**: SBS gameplay-mode (skip stack region via PSXPORT_SBS_LO=0x80010000/PSXPORT_SBS_HI=0x801FE000) f2 diverge at `0x800AC61C..0x800AC669` (78 B in libgs graphics context struct). Auto-diagnosis surfaces:
  - CLASS: ASYMMETRIC (only one core stored this frame — the ONE that wrote flips per run)
  - Struct: `libgs.gfx_ctx[0]+0x24`
  - Upstream divergence: **CUR_TASK** (A=0x801FE000/task-0, B=0x801FE070/task-1); all task slot states match
  - RNG, arm-mask, hword.0BED84, hword.0A4F7E, area.idx, cutMode, done_flag — all match
- **root cause (RE'd via Ghidra decomp of FUN_80097194 + FUN_80097540 + FUN_80096BF0)**: `FUN_80097194(param1=2, param2, ...)` computes `DAT_800AC61C = param2 >> (DAT_800AC62C & 0x1F)` (case 2). `param2` is the VRAM base (DMA target) computed by the libgs dispatcher chain called from `FUN_80081218` (LoadImage). Under gameplay-mode SBS:
  - **A** (native gameplay): `startBinStageFaithful` (game/scene/engine_stage.cpp) dispatches `FUN_80081218` inline at f0 during the fresh-entry native handler, matching substrate order. Verified with debug print — the dispatch DOES fire; final `0x800AC61C=0xFF98` on this specific run.
  - **B** (substrate gameplay): task-0 fiber runs substrate `0x8010649C` body. It also dispatches `FUN_80081218` inline, but the fiber timing puts the write at f2 (after some ticks of setup + yields). Final `0x800AC61C=0x5C22`.
  - Even though both cores dispatch the SAME `FUN_80081218` substrate body with the SAME rect + src args (verified: 0x800A5998 ctx ptr identical, no divergent writes on 0x800AC604/0x800AC614/0x800AC62C via PREWATCH), the **libgs internal DMA-state** at 0x800AC61C is `param2 >> shift`. If either A or B has done ADDITIONAL FUN_80097194 calls in between (via other libgs paths — preload chain, etc.) the "last value written" diverges.
  - The `libgs.gfx_ctx[0]` struct layout registered in the auto-diagnosis (base 0x800AC5F8, stride 0x100) confirms all 78 diverging bytes fall inside this one context struct.
- **why this is hard to close naively**: the divergence is not "some code A runs and B doesn't" — both cores run the SAME libgs dispatch chain. It's an ORDERING divergence: A dispatches LoadImage from native at tick X while task-1 fiber's preload also does LoadImages at tick Y; B's task-0 fiber dispatches LoadImage at tick X' with task-1 fiber's preload at tick Y'. X≠X' because A's native task-0 doesn't yield inside startBinStage but B's fiber does (via FUN_80044BD4 wait loop — see Slip #6). So the multi-caller LoadImage/FUN_80097194 sequence interleaves differently on A vs B, and the LAST value written to `DAT_800AC61C` (= a stateful DMA target) doesn't align.
- **fix path (bounded, depends on Slip #6)**: close Slip #6 first — that makes native task-0's tick cadence match substrate. Once task-0's yields align tick-by-tick with substrate, LoadImage dispatches from native startBinStage and task-1's fiber preload chain fire in the same order on A as on B, and the FUN_80097194 write sequence produces identical final `0x800AC61C`.
- **alternative fix path**: fully port `FUN_80081218` (and its libgs fn-ptr chain into FUN_80097194) natively — replace both dispatches with native code that produces identical guest writes deterministically without going through the guest fn-ptr indirection. Substantial (needs full RE of libgs DMA management + graphics context state model). Should only be undertaken if Slip #6 cascades to more of these ordering divergences.
- **refs**: `scratch/decomp/libgs_ctx.c` (Ghidra decomp of FUN_80097194 case 2 = the writer), `game/scene/engine_stage.cpp:startBinStageFaithful` (A's dispatch site), auto-diagnosis registry for `libgs.gfx_ctx` in `runtime/recomp/sbs.cpp` (48ceae1).

## Slip #6 CLOSED (2026-07-04, commit 656bd2b) — three-layer stack-scratch byte-match for FUN_80044BD4 + subframe + outer-yield emission
- **fix landed**: closed via BYTE-MATCH (not step-spread — the alternative fix from the original open note below turned out unnecessary). Three additions to `game/scene/engine_stage.cpp`:
  1. `sync_preload_spawn` now emits FUN_80044BD4's OWN prologue writes at sp+16..+32 (OLD s0/s1/s2/s3 + ra), with the substrate's caller-reg state at the state-0 jal site captured as `kBd4RegsStartBinState0 = {3, 2, 1, 5, 0x801067A8}` (the file-table build exit leaves s0=3/s1=2/s2=1/s3=5 via `addiu` constants at 0x8010672C-34, and MIPS `jal` sets ra=PC+8=0x801067A8). Previously only the FUN_80051F80/FUN_80051F14 sub-frame writes were emitted — the outer FUN_80044BD4 frame wasn't.
  2. XA CdSearchFile loop fixed to use `sp+0x198` (a SINGLE FIXED buffer for all three XA calls) rather than `cdlfile_buf_base + i*24`. The prior per-iter write was clobbering slots 0..2 of the file-table window (owned by loop 3's TOMBA2.IDX/IMG/DAT dir entries) with XA dir entries — 78-byte diff at 0x801FE848 onwards.
  3. Substrate's tail `jal 0x80051F80(1)` at 0x801067E4 (the outer state-machine yield after FUN_80044BD4 returns) emits ONE TICK LATER on B — task-0 has to resume from FUN_80044BD4's inner wait-loop yield, exit FUN_80044BD4, THEN run the tail-jal. So the write goes into `Engine::stage0Advance` step 0 (which runs on that next tick) rather than into `startBinStageFaithful`. Address = `startBinStage_sp - 8` (collides with the FUN_80044BD4 ra spill — last-writer wins with the outer 0x801067EC).
- **result** (SBS gameplay-mode, `PSXPORT_SBS_PRENAV=1`): f0 A/B IDENTICAL, f1 A/B IDENTICAL, first divergence at f2 = Slip #7 exactly (0x800AC61C..0x800AC669).
- **why byte-match worked (invalidates the original note's "fake-yield emission doesn't work" claim)**: the earlier fake-yield attempt only reproduced the FUN_80051F80 sub-frame spills — NOT FUN_80044BD4's own frame — and left an off-by-4 ra value AND a wrong-buffer XA layout. Once all three defects were addressed, byte-match works even without step-spread because the LAST value written to each byte is the same on both cores (the wait-loop's 48 iterations all write the SAME 0x80044CA8 to the SAME sp+16 address — one write suffices for content match). The step-spread alternative in the OPEN note below remains valid but is not needed for this specific cadence divergence.
- **surviving frontier**: Slip #7 (0x800AC61C libgs DMA state) — its finding said Slip #6 was a prerequisite; with Slip #6 closed we can now attack Slip #7 directly.

## Slip #6 (ORIGINAL OPEN NOTE, kept for the trail) — startBinStage f0 stack scratch: FUN_80044BD4 wait-loop yields ~48× on B while native task-0 doesn't yield at all (2026-07-04)
- **symptom**: headed SBS gameplay-mode f0 diff = 100 B stack scratch in `0x801FE808..0x801FE9E6`. Auto-diagnosis surfaces `CLASS: COUNT-MISMATCH — A wrote 1× vs B wrote 4×` for the target address, but PREWATCH on task-0+0x02 (`0x801FE002`) reveals A wrote 19× and B wrote 48× — B iterates the `FUN_80044BD4` wait loop ~48 times before task-1 fiber's substrate `FUN_80044F58` signals done_flag=1. Each iteration = one `FUN_80051F80(1)` call = one `sh a0, 2(v1)` + one `sw ra, 16(sp)`. That's 48 stack-scratch writes on B that A doesn't emit.
- **root cause**: native `startBinStageFaithful → sync_preload_spawn` under pc_faithful (mPcSkip=false) writes task-1 state=2 (via `native_task_spawn`) and returns immediately. Task-1's fiber (unlocked under pc_faithful (mPcSkip=false) by scheduler refactor `b849a4b`) then runs its substrate `FUN_80044F58` across scheduler ticks — but native task-0 has already RETURNED from startBinStage and progressed. On B, task-0's substrate runs the FUN_80044BD4 wait loop, yielding on each iteration, allowing task-1 to advance one substrate step per yield. The 48-iteration count is task-1's nested preload dependency chain depth (task-1's FUN_80044F58 body invokes its own FUN_80044BD4 calls, each needing task-0 to yield → task-1 slice → task-0 yield → ...).
- **why fake-yield emission doesn't work** (validated 2026-07-04): a first-pass port emitted the substrate's stack-prologue writes byte-faithful (sp descent 40 in FUN_80044BD4, 24 in nested FUN_80052010/FUN_80051F14/FUN_80051F80 with matching sw s0/ra spills at the same absolute addresses) but only ONE fake yield's worth of writes. Result: worsens the diff from 100 B to 116 B — the wait loop count mismatch shows up at `0x801FE002` (task+0x02) which is written by every yield.
- **fix path (bounded but substantial)**: `FUN_80044BD4`'s wait loop needs a genuine step-spread. Introduce `stage0_wait_step[3]` (or piggyback on existing `stage0_step`). On fresh startBinStage tick, do the setup + spawn + FIRST yield. On subsequent scheduler ticks in the STAGE-0 step-spread stanza, poll done_flag; if 0, emit ANOTHER fake-yield write + `scheduler_yield` to end tick; if 1, exit wait loop and proceed to sm[0x48]=1 + startBinCommonAdvance. This matches substrate's tick-per-iteration cadence exactly. Task-1 fiber runs in parallel across the same ticks. Requires:
  1. New `native_yield_prim(quota)` helper — sp descent 24 + `sw ra, 16(sp)` + task+0x02/+0x00 writes.
  2. New `native_task_close_prim(slot)` — sp descent 24 + prologue + guarded close body.
  3. Extend `native_task_spawn` (scheduler.cpp) with sp descent 24 + `sw s0/ra` prologue.
  4. Rewrite `sync_preload_spawn` to be the outer FUN_80044BD4 port: sp descent 40 + full prologue + FUN_80052010(2) via port + globals + FUN_80051F14 via port + FIRST wait iter. Set `stage0_wait_step=1` (defer next iterations to scheduler).
  5. Extend STAGE-0 step-spread stanza to consume `stage0_wait_step`: while step>=1 && done_flag==0, emit yield prim + `scheduler_yield`. When done_flag=1, clear step, fall through to normal step-0 (preload/rng/sm=1).
- **tooling landed this session** (`3cc4e0b`, `48ceae1`): SBS auto-diagnosis extended with struct-layout probe (identifies e.g. `libgs.gfx_ctx[0]+0x24` from raw addresses), upstream state cross-check (RNG.seed, arm-mask, done_flag, CUR_TASK, task-slot state), call-chain depth heuristic, guest stack window dump around write-sp. The COUNT-MISMATCH classification IS what surfaces this Slip in one auto-diagnosis dump — no more manual PREWATCH sweeps.
- **refs**: `game/scene/engine_stage.cpp:sync_preload_spawn` (current stub port — task-slot writes only), `runtime/recomp/scheduler.cpp:native_task_spawn` (base primitive port), scratch/decomp/task_spawn.c (Ghidra decomp of FUN_80044BD4 + FUN_80051F14 + FUN_80080860 syscall trampolines), `runtime/recomp/sbs.cpp` (auto-diagnosis registry).

## Slip #5 (OPEN) — massive RNG cadence skew: guest LCG at 0x80105EE8 runs 158× more advances on B than A (7411 vs 47) → downstream spawn coord divergences (2026-07-03)
- **symptom**: Post-Slip-#4, SBS gameplay mode surfaces a byte-range divergence at `0x800EE6D4..0x800EE9D7` at f217. Auto-diagnosis CLASS = ASYMMETRIC at f248, but PREWATCH from boot pins the ORIGIN write to f192 with **CLASS = VALUE-MISMATCH** — both cores wrote to 0x800EE6D4 (u16 coord) via the SAME call path (`array8Dispatch → ov_a00_gen_801158E0 → gen_func_8003116C`) with different values: A=0x2739 vs B=0x279A (delta ~100). Also 0x800EE6D6: A=0xF7BA vs B=0xF761. These are new-object position coords, copied from a stack-local `local_1e = (rnd & 0x7F) + 0x2731; local_1a = -0x831 - (rnd & 0x7F); local_16 = 0x17C3 - (rnd & 0x7F)` in FUN_801158E0 state 1 (RNG-driven decoration spawn).
- **root cause**: `FUN_8009A450` (linear-congruential RNG, state at `0x80105EE8`) is called **158× more on B than A**. Per-frame DISPWATCH count in the gameplay window: A ≈ 1 call every 14 frames, B = 7-8 calls per frame. `grep 'rec_dispatch.*8009A450\|0x8009A450' game/ runtime/recomp/` returns ZERO — no native code invokes the guest RNG. Every native handler that REPLACED a recomp'd handler that would have called `FUN_8009A450` (physics jitter, decoration spawn, animation phase, sfx pitch, target-picker) skips the RNG advance. RNG state drifts almost immediately after gameplay starts; every downstream consumer sees uncorrelated values on A vs B. Same family as the known "Target #4" `FUN_80083F50` sin-arg divergence in this same file — that's a cadence-timing symptom of the same class (native writes to the aliased `rec+0x0C` fire at different ticks than recomp).
- **f192 call chain, both cores** (via host-BT + Ghidra decomp):
  1. `Engine::fieldFrame` → `Array8Dispatch::tick` (native on A) / `gen_func_80026368` (recomp on B).
  2. Dispatches obj-slot handler = `FUN_801158E0` (A00 overlay, dispatched via rec_dispatch on both).
  3. `FUN_801158E0` state 1 (obj+4 == 1): reads timer at obj+10, decrements. When it hits 1: calls `func_0x8009a450()` (RNG) 3 times to compute `local_1e/1a/16`, then `func_0x8003116c(0x101, auStack_20, 0xffffffec)` which spawns a new obj and does `mem_w16(new_obj+44, mem_r16(a1+2))` — reading the stack locals we just wrote. Then a 4th RNG call sets the next-timer.
  4. New obj lands at `0x800EE6A8`; `+44 = 0x800EE6D4`. Value = `(rnd & 0x7F) + 0x2731`. A's `rnd & 0x7F = 8`, B's = 0x69 = 105. Same call path, different rnd → different coord.
- **why this is the same class as Target #4**: both are cadence-drift symptoms downstream of native handlers not making the same guest-state advances as their recomp equivalents. Target #4 = write-timing on `rec+0x0C` (aliased with `obj+0x42`). Slip #5 = RNG state advance count. Fixing individual native handlers to call the RNG (or write the aliased byte) at the same points as recomp would close both families.
- **fix classes (documented, not landed)**:
  - **Full native port of FUN_801337E4** (the `beh_background_actor_tick` candidate — extensive RE already captured in this file above under "FUN_801337E4 RE'd semantic model"). Native port has to preserve the aliased `rec+0x0C = obj+0x42` writes at the same substate transitions as recomp — that closes Target #4 directly. Recomp/native become tick-identical for that handler.
  - **RNG-cadence gating on !SBS for RNG-consuming native handlers**: audit `beh_*` native handlers for guest paths that would have called `FUN_8009A450` (grep the RE'd body for `func_0x8009a450` calls). Under SBS, invoke `rec_dispatch(c, 0x8009A450u)` the same number of times the recomp did. Faithful-mode-only — live PC gameplay stays skip-free. Same shape as Slip #3's `c->game->sbs` gate. This is the broad-coverage fix; the exact list of handlers to gate is large but bounded.
  - **NOT a workaround**: don't sync RNG state at frame boundaries (defeats the point — every consumer between resyncs still sees drift within a frame).
- **tooling landed this session that made this diagnosable in one go**:
  - Host C-stack backtrace at wwatch fire (runtime/recomp/sbs.cpp) — turned "stale c->pc = 0x80071768" (misleading) into `Cull::coneCull2b278` (correct) in one shot for Slip #4; same mechanic named `ov_a00_gen_801158E0 → gen_func_8003116C` for Slip #5.
  - Auto-diagnosis CLASS line (ASYMMETRIC / COUNT-MISMATCH / CALLSITE-DIVERGE / VALUE-MISMATCH / FN-DIVERGE) — first-read triage without eyeballing the raw wwatch log.
  - Object-record byte dump + cull-input dump + list-membership probe — pinned Slip #4 root (CULL_FAR_MULT enhancement) in three log lines.
- **refs**: docs/findings/sbs.md "FUN_801337E4 RE'd semantic model" (existing Target #4 finding), scratch/decomp/a00_801158e0.c (RE'd FUN_801158E0), scratch/decomp/rng_9a450.c (RE'd RNG), `$CLAUDE_JOB_DIR/tmp/sbs27.log` (DISPWATCH totalA=47 totalB=7411).

## Slip #3 CLOSED — submode1 case 0 collapsed FUN_80044BD4's yield; two-tick split matches coro cadence (2026-07-03)
- **symptom**: SBS gameplay mode, first surviving divergence at lockstep f217, 1-byte diff at `0x800EE0DD` (A=0x00 B=0x20). Write site FUN_8004B374 at `obj+0xd`, gated on `mem_r16(0x1F80017C) & 0x1F == 0`. Prior findings characterized it as a "scheduling-phase residual" tied to Slip #1/#2 but no root fix landed.
- **root cause**: cumulative `+1` skew of scratchpad frame-counter `0x1F80017C` between A and B, introduced between `cutscene up @f31` and `gameplay-start @f216`. Per-tick fc probe pinned the exact skew tick to **f114 in `Engine::submode1` case 0**: A's synchronous `transitionAreaLoad` + fall-through into case 1 (writes `sm[0x4c]=table[bf870]=2`) collapses in one lockstep tick; recomp's 0x801088D8 case 0 yields inside `FUN_80044BD4` (task spawn-and-wait), so B splits the same work across two ticks (yield mid-body → resume next tick → fall through to case 1). Net: A enters fieldRun state 2 one tick early, ticks fieldFrame one extra time, `0x1F80017C` runs +1 ahead permanently. Every `& 0x1F` / `& 7` / `& 3` phased flag in the object pool (of which there are many, e.g. FUN_8004B374 at `obj+0xd`, FUN_800312D4 in beh_camera_target_follow) samples out-of-phase.
- **fix**: `game/scene/engine_stage.cpp` `Engine::submode1` case 0 — split into **faithful** vs **PC** modes gated on `c->game->sbs`:
  - Faithful (SBS active): run `FUN_8005245C` + `transitionAreaLoad` on tick 1, set `mSubmode1LoadDeferred=true`, `return` without falling through (matches B's yield inside FUN_80044BD4). Tick 2: consume the flag, fall through into case 1 body (writes `DAT_1f800234=0` + `sm[0x4c]=table[bf870]=2`), matching B's coro resume.
  - PC (SBS off): run the load and fall through in one tick (skip the coro yield — no reason to inherit its cost in live gameplay).
  - The two modes deliberately do NOT converge — SBS demands cadence parity, live gameplay does not.
- **verification**: per-tick fc probe. Before: A leads B at `[2,1,2,0]` @f114 while B still at `[2,1,0,0]`; fc bump lands at A@f116 vs B@f117; skew=1 for the whole run; gameplay-start fc=95 vs 94. After: A and B track sm[0x4c] transition together at f115; fc bumps together at f117; skew=0 through gameplay-start (fc=94 vs 94). `0x800EE0DD` divergence at f217 vanished; the next surviving divergence is at `0x800EE489` (separate chase, previously masked by the fc-phase noise).
- **workflow lesson**: the "faithful vs PC mode" split is a general pattern the port needs when the native reimplementation collapses a recomp yield. Faithful mode = cadence-parity for SBS. PC mode = the runtime path. Gating on `c->game->sbs` is the mechanism.
- **refs**: `game/scene/engine.h:53-59` (Engine::mSubmode1LoadDeferred field), `game/scene/engine_stage.cpp` (Engine::submode1 case 0), `scratch/decomp/writers.c` (RE'd FUN_8004B374 + FUN_80077B5C), fc-probe transcript in `$CLAUDE_JOB_DIR/tmp/sbs5.log`.

## SBS crashes on rec_dispatch_miss(0x80109F7C) during DEMO attract cursor=1 (A01)
- **symptom (all SBS modes render/gameplay/full):** `[recomp-MISS 0] no recompiled fn for 0x80109F7C  (caller ra=0x800587F8, a0=0x800E7E80)` inside `gen_func_80058648` at frame ~1814 (or wherever attract sm[0x6e] reaches 1). Diagnostic: `[miss-state] stage=0x801062E4 sm=0x801FE000 sm[48]=7 [4a]=1 [4c]=0 [4e]=0 [50]=0 [52]=0 1f80019b=1 areaidx(800bf870)=1`. Blocks divergence surfacing — SBS aborts before the harness can compare RAM.
- **status:** RE'd — root cause identified; fix not landed (needs game-content RE to know the correct handler for A01).
- **cause:** attract cursor table `@0x8010770C = {0, 1, 3}` cycles through 3 attract areas. Cursor 0 → A00 (works). Cursor 1 → A01 (**crashes**). Cursor 2 → A03. `gen_func_80058648` is an object-init routine that dispatches on `DAT_800BF870` (area byte) via a jump table at `0x801445B8` (base 0x80144000 + addiu 17848). That address is BEYOND A01's overlay end (0x8013A678, size 0x316DC). A00/A04/A06/A08 all extend past 0x80144000 (A00 to 0x8014E944 etc.) and DO cover the JT — but A01/A02/A03/A05/A07 and every A0[A-L] overlay are shorter, ending before the JT. So the JT is a **conditional-existence** table valid only when a "big" area overlay is resident.
- **real-PSX behavior (inferred):** the JT works on PSX because a previous big overlay (like A00 loaded in attract cursor 0) leaves its JT bytes in RAM past the freshly-loaded smaller overlay's end. RAM at `0x801445BC` still contains A00's `jt[1]` value even after A01 overwrites `0x80108F9C..0x8013A678`. The PC-recomp fails-fast because when A01 is resident, the overlay-router doesn't know about a function at 0x80109F7C (A01 has no entry there; A0B / A0I do, and A00 has bytes there but not marked as fn).
- **fix path options (in priority order):**
  1. **DEEP RE (right):** disas gen_func_80058648 fully to determine whether the JT lookup is REALLY reachable with area==1, or if the code has an earlier guard that on real PSX diverts. If reachable-and-legit, the JT entry 0x80109F7C in A00's overlay is a REAL function entry — mark A00's function map to include `func_80109F7C` and let recomp emit it. If unreachable-should-be, find the missing guard (state check in a caller of 0x80058648).
  2. **HARNESS gate:** teach SBS to keep sm[0x6e]=0 (only attract cursor 0 → A00) so the crash never fires. Not a fix — a workaround that violates "no residuals" by masking the bug. Only acceptable as an interim WHILE the deep RE is in flight.
  3. **INTERPRETER FALLBACK:** revert the "no interpreter fallback" rule for miss addresses in slot-covered ranges (execute the RAM bytes as MIPS via interp). Ruled out by CLAUDE.md (later-254: interpreter GONE).
- **workflow gap surfaced:** this crash has been present since MODE=gameplay was reported broken 2026-07-02 (`memory/sbs_mode_gameplay_looks_broken.md`). Attempting to use SBS to root-cause any other divergence hits this first. Until fixed, the RAM-diff diagnostic is unusable — all 3 open audio/render bugs (#28-#30) that per user directive need SBS surfacing are blocked.
- **refs:** #27 investigation trail (led here via fadetrace showing native paths work in boot); `runtime/recomp/overlay_router.cpp` (route + slot logic); `game/scene/engine_demo.cpp:demo_frame_s7`; `generated/shard_2.c:gen_func_80058648`.


## FUN_801337E4 RE'd semantic model (2026-07-03) — full 5-way sub-state machine named; port-ready
- **RE source:** disas 0x801337E4..0x80133C10 on `scratch/bin/field_ram_230.bin`; jumptable at 0x80109E58 (5 entries: 0x80133868 / 0x80133874 / 0x8013392C / 0x80133A3C / 0x80133AB4). Named fields on `class Actor` (game/object/actor.h) — subState / counterA / subFlagX / retryDelay / oscPhase / posY / renderRec / targetDelta / type / stateEcho — with docstrings that cite this handler as the naming source.
- **role:** the state-1 tick sub-behavior called from the parent `beh_typed_table_seed_gate` handler (FUN_80133C14) every ACTIVE frame. Semantically, this is a **BACKGROUND-ACTOR oscillate + occasionally-turn state machine** — an ambient critter/prop that (a) breathes/sways via a sin-driven periodic display attribute (`renderRec[+0xC] = cos(oscPhase) >> 5`) and (b) can be triggered by a scene event (counterA / subFlagX) to compute a target-angle delta and drive a short turn/scan sub-state sequence that consults the PILOT-ACTOR region at 0x800E7E80.
- **section A (0x801337F8..0x80133838) — reset transients when stateEcho != 0:** if `Actor::stateEcho != 0` (parent handler wrote it in a "not-triggered" gate branch last frame, then something advanced state), clear stateEcho, reset `subState=0`, `renderRec[+0xC]=0`, `targetDelta=0`, and re-seed `posY` from the per-type table 0x8014A6E4. This is the ONE place `stateEcho`'s consumer lives — the parent handler is its writer.
- **section B (0x80133838..0x80133864) — 5-way sub-state dispatch:** `if subState >= 5: return; else jump *(0x80109E58 + subState*4)`. Jump-table entries:
  - **[0] INIT (0x80133868):** `oscPhase = 0; subState = 1;` then fall through into [1]'s body with v0=2 preset in delay slot for the `bne`-driven case-2 advance.
  - **[1] MAIN OSCILLATOR TICK (0x80133874):** the target-#4 hot path.
      - `if counterA != 0: subState = 2; return;` (falling through to case-2 body with v0=2)
      - `else if subFlagX != 0:` — turn-trigger path:
          - `a1 = obj[+0x56]` (Y-signed); `a0 = obj[+0x5F] << 4`;
          - `subState = 4;`
          - `rec_dispatch(0x80077768)` — sub-behavior returning nonzero/zero to say TURN LEFT / TURN RIGHT
          - `targetDelta = (v0 != 0) ? +256 : -256;`
          - `subFlagX = 0;`
          - exit epilogue (v0 sentinel = 1)
      - `else (counterA == 0 && subFlagX == 0):`
          - `if retryDelay != 0: retryDelay -= 1; return;` (throttle the oscillator tick)
          - `else:` **THE OSCILLATOR TICK**:
              - `cos = Trig::rcos(oscPhase)`  (`rec_dispatch 0x80083F50` — now `c->trig.rcos()`)
              - `renderRec[+0xC] = cos >> 5`
              - `r = c->rng.next()`  (`rec_dispatch 0x8009A450` — now `class Rng` with seed at 0x80105EE8)
              - `oscPhase = (uint16)(oscPhase + 68 + (r >> 8));`  ← **THE TARGET-#4 ACCUMULATOR**
  - **[2] POST-COUNTER-A (0x8013392C):** `if counterA == 0: subState = 3; return; else …` reads the pilot-actor region at 0x800E7E80: `*(0x800E7FC7)` (pilot mode byte), `*(0x800E7ED8)` (pilot yaw halfword), `*(0x800E7FE8)` (pilot state); computes `posY` from `(pilotState - 2) * 6 * 2` clamped against a small halfword table at 0x8014A6F4; writes `renderRec[+0xC] = ± *(0x800E7ED8)`; advances further only when pilotMode<3.
  - **[3] TURN-DIRECTION SEED (0x80133A3C):** reads `*(0x800E7FE8)` (pilot state again) and picks one of {32, 48, 64, 128} into `targetDelta` by lookup; then reads `*(0x800E7FC7)` (pilot mode) and negates targetDelta when it's set; advances `subState=4`.
  - **[4] TURN-EXECUTE (0x80133AB4):** counterA/subFlagX branch mirror of case [1] but WITH targetDelta consumption — reads `renderRec[+0xC]` as current angle, adds targetDelta, wraps into signed 12-bit window (±0x1000), decays targetDelta by 8 per tick (clamped to zero, whichever direction), and writes back both. When targetDelta reaches |delta| < 32 while angle |Δ| < 20, resets subState=0 (loops back to INIT).
- **why THIS matters for target #4:** the oscillator-tick accumulator update `oscPhase += 68 + (rng >> 8)` is DIRECTLY the divergence propagator that the SBS chase named. Its ONLY non-deterministic input is `c->rng.next()`, whose seed at `0x80105EE8` is a single-word global shared across every FUN_8009A450 caller in the whole game. **If A and B ever call `Rng::next()` a different number of times before this handler runs, or in a different order, their seeds are permanently forked and every subsequent `oscPhase += ...` differs by a jittered amount every frame.** The divergence at the write site (`renderRec[+0xC] = cos(oscPhase) >> 5`) is downstream of that seed-divergence. **Next-session probe (durable):** wwatch `0x80105EE8` from boot and log the FIRST frame the two cores' seed values differ + which caller wrote the diverging value. That names the target-#4 upstream at the SEED level (not at oscPhase level), and any earlier writer is the root cause.
- **port readiness:** the RE'd surface is captured. Full native port of FUN_801337E4 as a method (candidate: `Actor::runBackgroundActorTick()` or a free helper `background_actor_tick(Actor&)` in a `game/ai/background_actor_tick.cpp`) is the NEXT arc — it will replace the parent handler's `rec_dispatch(SUB_STATE1_TICK)` and, if the pilot-actor region 0x800E7E80 semantics get named at the same time (see below), turn the whole chain into named-field code.
- **still-un-RE'd sub-references** (all called via rec_dispatch by FUN_801337E4 for now):
  - `0x800E7E80` — a 512-byte region read as if it were a PILOT-ACTOR (mode / yaw / state at +0x147/+0x58/+0x168). Likely Tomba himself or the SOP camera-target actor; RE candidate for a `class PilotActor` view.
  - `0x8014A6F4` — halfword ANGLE-CLAMP table (parallel to `posY` table 0x8014A6E4), indexed by `type * 4`. Per-type clamp bands.
  - `0x80077768` — TURN-DIRECTION lookup (case 1's subFlagX path): returns nonzero to signal "turn +256", zero for "-256". Args: `(actorSlotType << 4, pilotYaw, 0)`.
- **refs:** game/object/actor.h field docstrings; game/ai/beh_typed_table_seed_gate.cpp STILL OPAQUE list; ov_a00_gen_801337E4 substrate body (generated/ov_a00_shard_0.c line 19260-19313).

## `PSXPORT_DISPWATCH=<addr>:ra=<caller_ra>` — caller-ra-scoped dispatch trace (2026-07-03, workflow-first tool extension)
- **finding:** Extended `PSXPORT_DISPWATCH` in runtime/recomp/overlay_router.cpp to accept a `:ra=<caller_ra>` suffix that filters by the caller's `c->r[31]` at dispatch entry. Motivation: FUN_80083F50 has ~30+ rec_dispatch call sites (grep `generated/ov_a01_shard_0.c` shows ~30 in that shard alone), so a plain `PSXPORT_DISPWATCH=0x80083F50` interleaves args from every caller and drowns the specific site we're chasing. The caller-ra filter isolates ONE call site so its per-frame per-core args are directly diffable.
- **also emits a richer log line:** frame number (via `Sbs::frame()`), core, addr, ra, `a0/a1/a2` with `int16` sign of a0 (for LUT-lookup callers whose small-negative args are meaningful), a0-as-node summary (state/n3), and stage. Same width as before but every field a decomp trace needs.
- **applied to target #4:** `PSXPORT_DISPWATCH=0x80083F50:ra=0x80133900` (the exact site in `ov_a00_gen_801337E4:19303-19304` where the state-1 helper dispatches cos of obj+0x42). At f118 both cores fire ~7 calls at that ra with `a0=0` (obj+0x42 reads 0 on both sides after the reset at line 19260) — SAME ARG on both cores. Since `func_80083F50` is a pure cos LUT (`gen_func_80083F50` in the main-text substrate; no override registered in the overlay slot 1831), same arg → same return. Yet the eventual `mem_w16(rec+12, cos>>5)` writes 0x56 on A vs 0x57 on B (per the earlier UPPROBE log).
- **still-unresolved contradiction:** a pure function with identical args returning different results is impossible unless (a) the LUT at 0x800A5AF0 (SIN_TAB, read by Trig::rcos) is diverged between the two cores, or (b) our caller-ra filter is catching MORE than the ov_a00 call site — ra=0x80133900 also appears in `ov_a04_shard_0.c:19099` (a different call chain), so some of the "7 hits" per frame may be from ov_a04 with args interleaved in a way that hides the true per-site divergence. Or (c) the reset+read window (line 19260 clears obj+0x42=0; line 19302 reads obj+0x42) is racing with another writer between them on one core but not the other.
- **next-session probes (durable extensions that would end the chase):**
  - **PC-narrow the filter beyond ra alone:** extend `:ra=X` to `:from=<caller_addr>` where the filter is on the CALL SITE'S PC (say the fn's `pc` at the moment of dispatch), not the return-address. Then two callers that happen to share a caller-ra (like the a00/a04 ambiguity here) get separated.
  - **Auto-diff the args across cores per-frame:** when SBS is on and both cores hit the filtered dispatch, print a single COMBINED line `[dispwatch-diff] f<N> ra=X a0: A=<va> B=<vb>` with a highlight when they differ. Eliminates the human-eye scan across a per-core stream.
  - **Diff the LUT the callee reads:** if the callee is a known-pure LUT function, log `mem_r16(SIN_TAB+2048)` on both cores before returning (the specific slot for cos(0)). If they differ, the LUT itself is diverged upstream — a hint the render/scratchpad drift touched .rodata.
  - **Guest+native backtrace at the dispatch:** already exists in BYTETRACE for writes (via `capBt`), but not for dispatches. Extend `capBt` to work on the dispatch entry too so the FULL chain (native → rec_dispatch → gen fn) is visible per call.
- **workflow-first payoff on future targets:** this same shape ("write differs but the immediate handler is byte-for-byte and the callees are pure") will recur — any target-#2-style architectural drift, any content handler whose leaf helpers call shared libgte utilities. The caller-ra filter closes the "which of 30 callers gave this arg" question in one line, and the proposed auto-diff/LUT-diff extensions above would close "which upstream state is diverged" without another manual session. Land them BEFORE the next investigation, not after.
- **refs:** runtime/recomp/overlay_router.cpp:157 (extended DISPWATCH block), $CLAUDE_JOB_DIR/tmp/dw_scoped.log (session capture: 7 hits/frame/core at ra=0x80133900 with a0=0 on both), the target-#4 finding block below (the concrete divergence the extension is meant to solve, still open).

## Target #4 upstream probe (2026-07-03) — `PSXPORT_SBS_UPPROBE=<addr>` landed; obj = 0x800EFFF4 named; obj+0x42 OVERLAPS rec+0x0C (KEY LAYOUT INSIGHT); divergence proven at FUN_80083F50-arg level but source not yet named (many callers pollute PSXPORT_DISPWATCH)
- **finding (2026-07-03, upstream probe built):** New `PSXPORT_SBS_UPPROBE=<addr>` env in runtime/recomp/sbs.cpp: when storeCb fires on `<addr>`, dumps `c->r[16]` (owning obj address in ov_a00_gen_801337E4), `obj[+0x42]`, `obj[+0x46]`, plus current `c->r[4]/r[2]/r[3]/r[31]`. Requires a BYTETRACE/ALLOCTRACE range that includes the addr to arm wwatch (storeCb only fires from wwatch_check). Used with `PSXPORT_SBS_BYTETRACE=<addr-2>,<addr+2>` to arm the minimum window.
- **obj address named:** for the first divergent record (rec+0x0C = 0x800F0036), **obj = 0x800EFFF4**. And `obj + 0x42 = 0x800EFFF4 + 0x42 = 0x800F0036 = rec + 0x0C` — **the object OVERLAPS its allocated render record such that `obj[+0x42]` and `rec[+0x0C]` are the SAME MEMORY LOCATION.** This is either an intentional embedded-record layout (common game-engine pattern) or a coincidence, but either way it means every write to rec+12 in `ov_a00_gen_801337E4` is ALSO a write to obj+0x42, and vice-versa. Line 19260 (rec+12=0) and line 19308 (rec+12=sin>>5) and line 19313 (obj+66=obj+66+68+sin>>8) all land on 0x800F0036.
- **divergence proven at FUN_80083F50-arg level:** at f118, both cores write to 0x800F0036 twice — one "reset to 0" write at ra=DEAD0000 (A native) / ra=0x80133CEC (B recomp) then one sin-value write at ra=0x80133910 (both). The reset write's ra difference is EXPECTED (native path doesn't update r[31]; recomp does — same semantic write from line 19260 in ov_a00_gen_801337E4). The sin-value write shows different results (A=0x56, B=0x57 at f118 pre-shift), so **FUN_80083F50 returned different values on A vs B for the same obj+0x42 read post-reset**. Since FUN_80083F50 is a pure sin LUT (disas 0x80083F50: masks arg to 0xFFF, LUT-lookup at 0x800A52F0/5AF0/42F0, no side effects), the ONLY explanation is different args at the call site.
- **why the wide PSXPORT_DISPWATCH couldn't confirm the arg cheaply:** FUN_80083F50 has ~30+ rec_dispatch call sites across the recomp (grep generated/ov_a01_shard_0.c for `0x80083F50u` — ~30 hits in a01 alone), so DISPWATCH's per-call `r[4]` log at that address shows a mix of args from many callers per frame (measured f-scoped args 0xF02, 0x290, 0xC9 on both cores identical — but those came from OTHER callers, not from ov_a00_gen_801337E4's call at line 19304). Naming the specific caller's arg needs a caller-scoped hook (e.g. gate DISPWATCH by `c->r[31] == 0x80133900u` — the ra pre-set at line 19303 immediately before the FUN_80083F50 dispatch).
- **key layout consequence for the next session's probe:** because obj+0x42 and rec+0x0C are the same address, watching obj+0x42 IS watching rec+0x0C. The UPPROBE's read of `obj[+0x42]` at write-time reads a stale value (the just-cleared 0 from line 19260), not the arg that was passed to FUN_80083F50 at line 19302. To capture the actual arg, hook FUN_80083F50 ENTRY (PC=0x80083F50) with a caller-ra filter — not the site of the write to rec+0x0C. Better yet, extend UPPROBE to snapshot `c->r[4]` at BOTH the moment of write AND the moment of a specific PC (0x80083F54 == first instruction of FUN_80083F50 body) via an interp/dispatch pre-hook.
- **NOT LANDED but the natural next steps:** (a) instrument `overlay_router.cpp:rec_dispatch` at addr==0x80083F50u with a caller-ra filter `c->r[31] == 0x80133900u` (the exact site) — logs A's arg vs B's arg for the state-1 helper's sin call only. (b) if the args differ, walk backward from what sets obj+0x42 between the reset (line 19260) and the sin-call (line 19302). But line 19302 comes AFTER line 19260 in execution order (L_801338F4 is fall-through from the state-1 switch case at L_80133874 which is line 19276 — L_80133838 → switch on obj+6 → L_80133874 → ... → L_801338F4). Nothing writes obj+0x42 between them EXCEPT the reset. So (b)'s "walk backward" targets are actually the writes to obj+0x42 in prior FRAMES or another handler's writes — which is likely the whole target-#2 architectural family (native render walks writing then restoring). May converge with target #2's fix, not a distinct target-#4 fix.
- **workflow lesson (the memory-overlap trap):** when a BYTETRACE candidate address is derived from a memory-record-pointer indirection (like `rec_ptr = obj[+0xC0]`), CHECK WHETHER THE OBJECT AND THE RECORD OVERLAP before assuming the writes are to independent fields. If `rec_ptr + N == obj + M` for any N, M, watching that address is watching BOTH fields, and the writes might be from BOTH the object-mutation site AND the record-mutation site with different code paths. In this case obj+0x42 and rec+0x0C coincide, and 3 distinct code sites in `ov_a00_gen_801337E4` write to that address per iteration (line 19260 reset, line 19308 sin-value, line 19313 accumulate) — the total write count and shape are the composite.
- **refs:** runtime/recomp/sbs.cpp `PSXPORT_SBS_UPPROBE` block in storeCb (~line 522), generated/ov_a00_shard_1.c:19242-19313 (the recomp state-1 helper — three writes per iter to the same overlapping address), $CLAUDE_JOB_DIR/tmp/upprobe*.log (session capture with per-frame per-core write sequence showing the value divergence at f118+).

## Target #4 (stride-0xC4 pool at 0x800F00xx) FALSIFIED as a beh_typed_table_seed_gate transcription defect — it's DOWNSTREAM propagation, not a case-swap (2026-07-03)
- **finding:** After the wide-BYTETRACE sweep named 5 stride-0xC4 ONE-SIDED bytes at 0x800F00xx as a target-#4 candidate, applied the target-#1 primitive: cross-check native `game/ai/beh_typed_table_seed_gate.cpp` (0x80133C14) against recomp `ov_a00_gen_80133C14` (generated/ov_a00_shard_0.c:20529-20600). **No transcription defect found** — native state 0 (lines 65-83) and state 1 (lines 87-108) match the recomp writes byte-for-byte:
  - state 0: same order, same constants (`obj+0x2a=0x22`, `obj+0x80=0x1e`, `obj+0x82=0x3c`, `obj+0x84=0x32`, `obj+4=1`, `obj+0=1`, `obj+0x29=0`, `obj+0x46=0`, `obj+0x86=0x64`, then `rec_dispatch(0x8004766Cu)`, then `obj+0x42=0`, `obj+0x60=-0xc8`, `obj+0x32=tbl[n3*2]`).
  - state 1: same branches (`g<0x1c` → stash `obj+0x62`; `g==0x22` → `obj+1=st`, `rec_dispatch(0x80077EBC)`, render tail; else → `rec_dispatch(0x800778E4)`, if v0==0 stash `obj+0x62`, else render tail).
  - LUI+ADDIU constant TBL_A6E4 (0x8014A6E4) = `0x80150000 + sign_extend(-0x591C)` = correct in the native file (line 46 comment agrees with the folded value).
- **where the divergence actually lives:** the writes at rec+0x0C (which decodes to the 0x800F0036 / 0xFA / 0x1BE / 0x282 / 0x346 absolute addresses — records are stride-0xC4, laid out from 0x800F002A) come from line 19308 in `ov_a00_gen_801337E4` (the state-1 entry helper, generated/ov_a00_shard_1.c):
  ```
  c->r[4] = (int16_t)mem_r16(obj+0x42);       // arg to FUN_80083F50
  rec_dispatch(c, 0x80083F50u);                // returns a value in r[2]
  c->r[3] = mem_r32(obj + 0xC0);               // rec_ptr (render-cmd record)
  c->r[2] = c->r[2] >> 5;
  c->mem_w16(c->r[3] + 12, (uint16_t)c->r[2]);  // ← THE DIVERGENT WRITE (rec + 0x0C)
  ```
  Both A and B execute this same recomp helper via the same dispatch path (native calls `rec_dispatch(0x801337E4)`, recomp calls `ov_a00_func_801337E4(c)` — both resolve to the same body). If both cores' obj[+0x42] and obj[+0x46] were equal at the moment of dispatch, they'd write the same value. **The 5 divergent live values prove obj[+0x42] (or the obj[+0x46] gate that decides whether FUN_80083F50 runs — line 19298-19300) differs upstream between the two cores.**
- **not a case-swap in this handler.** obj[+0x42] is initialized to 0 in state 0 by BOTH native and recomp (matches). Something ELSE writes to obj[+0x42] over the object's lifetime and puts native and recomp out of sync for these 5 objects. That "something else" could be another native handler with a transcription defect OR another target-#2-style architectural drift (native-render walk touching the field). Not narrowable without a wwatch on obj[+0x42] for one of the 5 divergent objects.
- **fix-path recommendation for the next session:** don't attack beh_typed_table_seed_gate. Do this instead: for record #0 (rec_ptr = 0x800F002A ⇒ its owning obj), wwatch `obj[+0x42]` (halfword) on both cores over the AUTONAV window with `PSXPORT_WWATCH=<obj_addr>+0x42,+2` and settled-state RA bucketing. First writer whose ra fingerprint differs between A and B names the real target. Or (b) run BYTETRACE on the WHOLE object records of the 5 divergent nodes (find their obj addresses from cmd_ptr backpointer) and see whether obj[+0x42]/obj[+0x46] are themselves flagged as REAL — if so, that's the actual defect address.
- **workflow lesson:** a candidate defect surfaced by BYTETRACE-shape (ONE-SIDED, live-values-differ) is a POINTER at where a divergence lands, not necessarily where it originated. When the direct handler's transcription matches, walk backward through the dispatch chain to find the actual writer. The classification stayed useful (bounded the surface, ruled out LUI+ADDIU + case-swap in this handler); it just didn't close the target in-place.
- **refs:** game/ai/beh_typed_table_seed_gate.cpp (native, verified byte-for-byte), generated/ov_a00_shard_0.c:20529-20600 (recomp gen_80133C14 body), generated/ov_a00_shard_1.c:19242-19313 (recomp gen_801337E4 helper — the actual divergent-write site at line 19308), game/world/graphics_bind.cpp:51-70 (obj_record_init native writing rec+0..0xc, +0x38/3a/3c, +0x40 — sets up records but doesn't touch rec+0x0C).

## Post-Slip-#1 wide BYTETRACE classification (2026-07-03) — most of remaining 1880 B ramdiff is low-signal; two actionable clusters named
- **finding:** Ran two `PSXPORT_SBS_BYTETRACE=<lo>,<hi>` sweeps to classify what fills the 1880 B ramdiff after Slip #1 closed.
  ```
  range=0x800EE000..0x800EEA00  →  2287 CLEAN,  30 PHASE, 34 SOFT-PHASE, 209 REAL  (3 ONE-SIDED, 0 SKEWED, 17 MIXED in top-summary)
  range=0x800EEA00..0x800EF400  →  2560 CLEAN,   0 PHASE,  0 SOFT-PHASE,   0 REAL  (fully clean; no writes settle here in the window)
  range=0x800F0000..0x800F1000  →  4049 CLEAN,   0 PHASE, 42 SOFT-PHASE,   5 REAL  (5 ONE-SIDED at stride 0xC4)
  ```
- **actionable cluster 1 — 5 stride-0x88 MIXED bytes at offset 0x7C of 4 spawn nodes in 0x800EE104..0x800EE324 = TARGET-#2 architectural (do-not-re-derive block above).** Live values MATCH between A and B; write COUNTS differ ~2× (native render walks trigger extra writes that `dualviewSnapshot.restorePre` rewinds). Not a scheduling drift; not a transcription defect. Already parked as native-render-vs-PSX-render.
- **actionable cluster 2 — 5 stride-0xC4 ONE-SIDED bytes at offset 0x36 of records in 0x800F00xx (0x36 / 0xFA / 0x1BE / 0x282 / 0x346) is a NEW candidate defect:**
  - **live values DIFFER** on all 5 (A=0xC2/0xA1/0x8A/0xA9/0x8E vs B=0x48/0x87/0x70/0xBF/0x80) → genuine settled-state divergence, not just a transient rate mismatch.
  - **RA signature is clean:**
    ```
    A-ras: DEAD0000×97   0x80133910×95   0x800799D0×3   (identical across all 5 addresses)
    B-ras: 0x80133910×94 0x80133CEC×94  0x800799D0×3
    ```
    ra=0x80133910 is inside `ov_a00_gen_80133774` state-1 entry helper (called from beh_typed_table_seed_gate 0x80133C14); ra=0x80133CEC is inside `ov_a00_gen_80133C14` right after `ov_a00_func_801337E4(c)` (line 20577 generated/ov_a00_shard_0.c). A's DEAD0000×97 replaces those writes with native-inline (no r[31] update).
  - **native handler already exists:** `game/ai/beh_typed_table_seed_gate.cpp` (0x80133C14, registered in behavior_dispatch.cpp:85). Native calls `rec_dispatch(0x801337E4)` for the state-1 entry helper. So both A and B execute the same recomp helper via the same dispatch — the divergence has to be either (a) in what a1..a3 registers hold at the dispatch, or (b) in whether native takes the same state-1 branch (g<0x1c / g==0x22 / else) as recomp.
  - **not yet root-caused this session** — deferred as a target-#4 candidate. Fits the taxonomy checklist: cross-check state-1 branch bytes in the native (game/ai/beh_typed_table_seed_gate.cpp:88-108) against decomp of FUN_80133c14 at scratch/decomp/a00/ (which needs regeneration — only functions.csv is populated for a00). Prevention rule for the LUI+ADDIU class already applies — the TBL_A6E4 constant (0x8014A6E4 = 0x80150000 + sign_extend(-0x591C) = 0x8014A6E4) IS correct (comment on line 46 notes this), so that specific defect class is ruled out.
- **not-actionable in the same range — 200 REAL bytes classified only by count difference, no top-summary shape.** These are 1–3-write count deltas on bytes whose top-3 divergent (value, count_A, count_B) rows didn't exceed the classifier's summary threshold — low-signal noise. Would need per-address wwatch to attribute individually; probably not worth the token spend when each is a 1-write transient.
- **workflow lesson (wide sweep is cheap):** the two BYTETRACE runs classified 9216 bytes in ~40 s of run time each — the same tool that found target #1 in seconds also names what's LEFT after each fix, so a fixed-target cadence stays productive. Sweep-first-then-decide beats "which byte should I hit next?" every time.
- **refs:** $CLAUDE_JOB_DIR/tmp/bt_wide_out.txt (post-Slip-#1 sweep of 0x800EE000..0x800EEA00), $CLAUDE_JOB_DIR/tmp/bt_f000.txt (0x800F0000..0x800F1000 sweep with stride-0xC4 signature), game/ai/beh_typed_table_seed_gate.cpp (native handler, RA source for A's DEAD0000 writes), generated/ov_a00_shard_0.c:20529-20599 (recomp of FUN_80133c14 state 1, RA source for B's 0x80133CEC writes).

## Slip #1 residual CLOSED — defer native DEMO substate 5 (LEAVE→GAME) by one tick to match coro yield cost (2026-07-03, 550178a)
- **finding:** After START.BIN cadence match (block below) aligned boot to f8, the DEMO→GAME transition still slipped one tick. STAGETRACE snapshot pre-fix: at f26 A entry=0x8010637C sm48=5 (native_start_stage rewrote task-0 entry synchronously inside the demo.frame() dispatch of substate 5); B still at entry=0x801062E4, catching up at f27. Both cores had reached sm[0x48]==5 together at f25 — the drift was entirely in "how long the LEAVE-GAME substate takes to actually flip the entry pointer."
- **root cause:** Native `demo.frame()` at sm[0x48]==5 dispatches native_start_stage inline in one scheduler tick. Coro of 0x801062E4 substate 5 in the recomp body yields at least once through FUN_80051f80 before completing, so B's entry rewrite lands one tick LATER than A's. Same class as the closed Slip #2 (SOP fieldMode case 0 yield vs native inline).
- **fix (mirror sop_field_step pattern):** New `SchedulerState.demo_leave_step[3]`. In `runtime/recomp/scheduler.cpp` DEMO native block, before calling `c->engine.demo.frame()`, read `sm[0x48]`. If it equals 5 and demo_leave_step==0, set step=1 and skip the dispatch (task saved as runnable). Next tick, actually call demo.frame() so native_start_stage runs and re-arms the step. Not applied on `demo_fresh` (prologue path) — fresh sm[0x48] is 0, never 5.
- **evidence — STAGETRACE post-fix:** f25..f27 both cores tick sm state in lockstep:
  ```
  f25 A entry=801062E4 sm48=5 | B entry=801062E4 sm48=5    (both reached LEAVE)
  f26 A entry=801062E4 sm48=5 | B entry=801062E4 sm48=5    (A DEFERRED — was 8010637C)
  f27 A entry=8010637C sm48=5 | B entry=8010637C sm48=5    (both rewrite same tick)
  ```
- **evidence — BYTETRACE on the target-#3 range 0x800EE0DC..0x800EE10D:**
  ```
  before: classified 41 CLEAN, 5 PHASE,           3 REAL
  after:  classified 46 CLEAN, 1 PHASE, 1 SOFT-PHASE, 1 REAL
  ```
  The ONE-SIDED PHASE bytes at 0x800EE0DE and 0x800EE108 (the ones the handoff named as target #3's residual) collapsed to CLEAN/SOFT-PHASE — those specific per-node animation-timer bytes were driven by the DEMO→GAME 1-tick drift.
- **ramdiff @f218 unchanged at 1880 B** (~629 spans dominated by other pool ranges). Slip #1 owned only the specific spawn-node animation-timer bytes surfaced in BYTETRACE, not the whole ramdiff. The remaining REAL byte in the range (0x800EE104, val=0x18 A×4 B×2, A-ras=DEAD0000/native × 70 vs B-ras=8007A964/recomp × 34) matches the target-#2 architectural signature (block below): same values, different write cadence between native and PSX render walks. Not a scheduling drift; not this target's shape.
- **workflow lesson:** the target-#3 PHASE bytes were labeled "scheduling drift, not transcription" (correct call from the handoff) — the primitive switch away from BYTETRACE-and-decomp toward STAGETRACE-and-scheduler-deferral was the right move. STAGETRACE at =2 showed the specific slip tick directly without needing a JSONL diff tool; the raw stderr trace of f22..f36 was enough to name it.
- **refs:** runtime/recomp/scheduler.cpp DEMO native block (demo_leave_step guard around `c->engine.demo.frame()`), runtime/recomp/game.h SchedulerState.demo_leave_step[3], $CLAUDE_JOB_DIR/tmp/st_fixed.log (post-fix STAGETRACE), $CLAUDE_JOB_DIR/tmp/bt_after.log (post-fix BYTETRACE). Target #3 = CLOSED.

## Port-review bug taxonomy — three named classes of native-transcription defect (2026-07-03)
Classes surfaced so far by the SBS BYTETRACE pipeline. When reviewing a native `beh_*` or ov subsystem file, actively hunt for these three shapes — they read as plausible C but each has a signature symptom:

1. **CASE-SWAP** — a state-machine `switch/if (v == X)` in the native transcription routes value X to the wrong branch, or omits a case the recomp handles. Signature: BYTETRACE ONE-SIDED byte writes on a state field (A writes some values but not others; B writes the full set). The file's own header comment tends to agree with the buggy code, not the decomp — trust the decomp. Fixed: `beh_anim_trigger_gates` (b1df8d7).

2. **EMPTY VIRTUAL STUB** — a native override registered as a `virtual` method whose body is empty or a `TODO`, silently satisfying the interface. Signature: entire subsystem doesn't observably run in the SBS but tests still "pass". Fixed: sunbright's TManhole (external session, referenced by the manager brief).

3. **LUI+ADDIU EFFECTIVE-ADDRESS DRIFT** — a `lui $hi, 0xHHHH` followed by `addiu $lo, $hi, IIII` was transcribed as the Ghidra-displayed hex literal (which shows the assembled pair but NOT the folded effective address). If `IIII` is negative (bit 15 set), the effective 32-bit value is `(0xHHHH0000) + sign_extend(IIII)` — the HIGH half becomes `0xHHHH − 1`, not `0xHHHH`. Off by exactly `0x10000`. Signature: a table/data pointer lands in unrelated memory; downstream side-effects look like arbitrary garbage. Fixed: `beh_prng_velocity_machine` (7f5e14c).
   - **Prevention rule:** never copy a hex literal from Ghidra's `lui/addiu` pair display. Compute `lui_hi_shifted + (int16_t)addiu_imm` from the generated recomp — the recompiler emits `(uint32_t)HHHHu << 16` then `+ (uint32_t)-N`, which folds correctly. Cross-check every 0x8015XXXX / 0x800FXXXX / etc. constant in a transcribed function against the same address in `generated/ov_*_shard_*.c` before landing.
   - **Static-scan candidate (future workflow-first tool):** grep native `beh_*.cpp` for `0x[0-9a-f]{4}[89ab][0-9a-f]{3}` constants and diff against the corresponding gen body — the high nybble should match the folded value, not the raw `lui` immediate.

## Target #2 (stride-0x88 rate mismatch) is NATIVE-RENDER-vs-PSX-RENDER, NOT a case-swap — DO NOT chase it with the target-#1 primitive
- **finding (2026-07-03):** After the beh_prng_velocity_machine fix, BYTETRACE the wider band 0x800EE180..0x800EE330 confirmed the periodic 4-node signal: byte at obj+0xC of nodes {0x800EE180, 0x800EE208, 0x800EE290, 0x800EE318} (stride 0x88) writes ~2× more often on A than B (totA≈190 vs totB≈100 per node), but LIVE VALUES MATCH on all 4 → same settled state, just different transient write cadence.
- **not a case-swap.** wwatch armed on 0x800EE18C, both cores hit the same word (write value=0x00000000). B's guest backtrace: `[sp+0x014]=0x8007A964 [sp+0x02C]=0x80108B68 [sp+0x044]=0x80106D08` — B is going through the RECOMP body of `FUN_80108B0C` (the field-frame update) which calls the RECOMP body of `FUN_80077A904` (the walker), whose per-node dispatch invokes handlers that write obj+0xC. A's stack is empty (native path, no guest stack maintained) — A goes through `Engine::fieldFrame() → objectList.walkAll()` NATIVELY, which also invokes the same handlers via `BehaviorDispatch::dispatchObj`.
- **root cause hypothesis:** In FULL mode `A native render, B PSX render` (sbs.cpp:342). On A, `Render::frame()` runs native render walks `rwalkAuxBcf4() / rwalkAuxBf00() / rwalkAuxEec0()` (engine_render_walk.cpp) which dispatch per-node RENDER handlers on top of the gameplay walk — each aux case invokes another `rec_dispatch` or `perObjRender` per node. B's PSX render uses the recomp body which drives handlers differently (or not at all for these render-time hooks). Result: same handlers, dispatched at 2× rate on A vs B — a settled state that MATCHES (final value equal), but the transient trail differs.
- **why this is NOT a port gap:** Live values are equal; only settling counts differ. `dualviewSnapshot.restorePre` rewinds guest state written by the native render, so A's extra dispatches don't persist into gameplay state (that's the whole invariant the restorePre was added for). BYTETRACE counts writes BEFORE any restore (wwatch fires in mem_wN), so the rate divergence is real in the count histogram but invisible in the settled RAM diff.
- **do-not-re-derive:** Do NOT bring the target-#1 primitive (decomp cross-check for lui/addiu drift + case-swap in a specific `beh_*.cpp`) to bear on this — the writer isn't one native handler; it's the ENTIRE native-render dispatch chain plus the gameplay walk. Fixing it means making `Render::frame()`'s per-node render dispatches guest-memory-free (the note at engine_stage.cpp:432 already flags this as the true end-state: "make ov_render_frame write ZERO guest memory so capturePre/restorePre can go too"). That's a distinct architectural workstream, not a decomp fix.
- **investigation cost:** ~150k tokens (one session) to pin the shape. Log so next session that sees a stride-0x88 rate signal in FULL mode reads THIS block first and doesn't re-open the wwatch/decomp loop.

## FIX case-swap ONE-SIDED byte 0x800EE0DE — off-by-0x10000 tbl constant in native `beh_prng_velocity_machine` STATE-0/STATE-2 → wrong geom-block table, wrong animation cmd list, +0xe timer never advances
- **symptom (2026-07-03):** After the anim_trigger_gates fix, BYTETRACE targeted 0x800EE0DE (ONE-SIDED: B writes val=0x01/0x02 30× each, A never). Live A=0xFFC5 (short-signed −59), B=0x0001 — A's animation-tick timer at obj+0x0e is stuck decrementing forever, never reloads from anim cmd list.
- **decisive tool step:** Extended BYTETRACE dump to print top RAs per side. A-ras=0xDEAD0000×60 (native path w/ sentinel r[31]), B-ras=0x80117AC0×88 (recomp gen_80117658 body, just after `rec_dispatch(0x80077B5C)` at L_80117AB8). The RA nail-pointed the divergence to native beh_prng_velocity_machine vs recomp FUN_80117658.
- **root cause:** Two `lui 0x8015; addiu $reg, -N` immediates were transcribed by copying the assembled register-load literals from Ghidra's view (0x8015C808, 0x80158574) instead of computing `0x80150000 + (int16_t)-N`. Off-by-0x10000. In native L76d8 (STATE 0, node[3]==0): `c->r[5] = 0x8015c808u` should be `0x8014C808` (recomp: `lui 0x8015` = 0x80150000, `addiu -14328` = 0x8014C808 — the high half is 0x8014, not 0x8015). Same class of bug in L7c7c (STATE 2 exit): `0x80158574` should be `0x80148574` (`lui 0x8015 + addiu -31372`). Both are pointers to geometry/animation tables the game uses — pointing 0x10000 too high lands them in unrelated data, so obj_set_geom loads a garbage cnt into obj+0xe, and the anim-tick decrement (FUN_80077B5C) counts through 0 → −59 without ever triggering the anim-cmd-advance branch.
- **fix:** Two chars in `game/ai/beh_prng_velocity_machine.cpp` — `0x8015c808u`→`0x8014c808u`, `0x80158574u`→`0x80148574u`. Verified via BYTETRACE re-run at 0x800EE0DC..0x800EE10D: `3 REAL → 1 REAL, 5 PHASE → 1 PHASE + 1 SOFT-PHASE`; the 0x800EE0DE row DISAPPEARED completely. Overall ramdiff @f217 dropped 2074 B → 1880 B (−9.3%). Remaining 0x800EE0DD divergence is the PHASE byte = slip #1 residual (target #3), not a case-swap.
- **lesson for the port at large — DO NOT copy hex-literal register values from Ghidra `lui/addiu` pair displays.** They read as one hex constant but only the low 16 bits of the addiu are meaningful; the effective address is `lui_hi + sign_extend(addiu_lo)`. When the addiu is negative, the effective value's HIGH half is `lui_hi − 1`. Cross-check every 0x8015xxxx / 0x800Fxxxx const in a transcribed function by computing `lui + (int16_t)addiu` from the generated recomp — the generator emits it correctly.

## `PSXPORT_SBS_BYTETRACE=<lo>,<hi>` landed — per-byte-value+ra bucketing with settled-state 4-class auto-classifier + suggested SBS-noise-filter ranges + top-RA on REAL rows
- **finding (2026-07-03, workflow-first invariant generalized):** Extended the tool the previous session named. `PSXPORT_SBS_BYTETRACE=<lo>,<hi>` arms wwatch over the range on both cores and buckets every 1/2/4-B store's constituent BYTES per address per core, keeping both `(value → count)` and `(ra → count)` histograms. At end of run (via `sbs bytetrace` REPL over the debug server; the atexit/SIGTERM dumper works too but the debug-server invocation is the reliable path — SIGTERM dumps can truncate under `timeout N`). Classifies every recorded byte into 4 buckets:
  ```
  CLEAN       live_A == live_B  AND  A.value_counts == B.value_counts     ← nothing to see
  PHASE       A.value_counts == B.value_counts  AND  live_A != live_B     ← snapshot-phase flicker
  SOFT-PHASE  value SETS match; per-value counts within ±max(2, hi/20)    ← 1-tick-off residual
  REAL        value sets differ OR counts differ by more than tolerance   ← real port gap
  ```
- **evidence — the same 800EE0DC..800EE10D span used for the previous "phase-vs-real" hand-diagnosis, now auto-classified in one command:**
  ```
  [sbs bytetrace] range=0x800EE0DC..0x800EE10D  recorded 49 unique byte addresses (settled state)
                classified: 41 CLEAN, 5 PHASE, 0 SOFT-PHASE, 3 REAL  (strict=0)

  (REAL bytes — the concrete port-gap targets:)
    0x800EE0DE  [ONE-SIDED]  totA=63 totB=92   top: val=0x01 A=0 B=30  val=0x02 A=0 B=30  val=0x00 A=4 B=32
    0x800EE104  [MIXED]      totA=73 totB=39   top: val=0x98 A=4 B=1
    0x800EE108  [MIXED]      totA=39 totB=70   top: val=0x8C A=0 B=4
  ```
  The **ONE-SIDED byte at 0x800EE0DE** is a genuine caller-side divergence: B advances through 3 sub-states (writes 0x00/0x01/0x02, ~30× each), A stays at 0x00 and NEVER writes 0x01 or 0x02. That's a state-machine one of the two paths is missing entirely — the same shape of bug as the `anim_trigger_gates` case-swap. Node = 0x800EE0DC, handler = `beh_prng_velocity_machine` (FUN_80117658).
- **evidence — wider sweep of a 2560-byte range around the node:**
  ```
  [sbs bytetrace] range=0x800EE000..0x800EEA00  recorded 2560 unique byte addresses
                classified: 2280 CLEAN, 34 PHASE, 33 SOFT-PHASE, 213 REAL  (strict=0)
  ```
  The 67 PHASE + SOFT-PHASE bytes (out of 280 divergent) can go into isDiffNoise as ranges (Section 2 of the dump emits paste-ready C code); the 213 REAL bytes are the concrete port-gap targets to decomp — the sweep also surfaces a periodic pattern at 0x800EE18C / 0x214 / 0x29C / 0x324 (stride 0x88 = pool record size) where A writes values ~2× as often as B. Same offset on 4 consecutive spawn nodes → same code writing them at a rate mismatch → likely one bug at the shared writer.
- **use pattern (documented for the next investigator):**
  1. When SBS reports a first-divergence range, run `PSXPORT_SBS_BYTETRACE=<lo>,<hi>` with `PSXPORT_DEBUG_SERVER=1`.
  2. Wait for the harness to reach settled state (usually SBS's own divergence-pause window is enough).
  3. Connect to :5959, send `sbs bytetrace`.
  4. Section 1 (CLEAN hidden by default; set PSXPORT_SBS_BYTETRACE_ALL=1 to show them) lists each byte with its class.
  5. Section 2 emits paste-ready isDiffNoise ranges for the PHASE|SOFT runs.
  6. Section 3 lists the top REAL bytes with shape (ONE-SIDED / SKEWED / MIXED) and the top 3 divergent (value, count_A, count_B) rows — the concrete decomp targets.
  7. Pick the ONE-SIDED byte with the largest value-set gap; decomp the handler that writes it; cross-check native vs decomp at the case-label level.
- **the workflow-first payoff** the manager called out is in this section 2 emit: name the phase-noise ranges ONCE, filter them from `isDiffNoise` FOREVER — the next PREWATCH hunt on the next divergence won't be misled by them, and every future bytetrace over the same address range will report them CLEAN (or classify a NEW divergence that shares the range without the phase-noise mask). Recurring-blocker fix at the tool level, not the finding level.
- **workflow lesson recorded:** when applying `sbs bytetrace` in practice, use the debug-server invocation (`PSXPORT_DEBUG_SERVER=1` + `sbs bytetrace` over TCP), not the atexit/SIGTERM auto-dump. The auto-dump has a race with `timeout N` — SIGTERM handlers doing large I/O can be truncated by the follow-up SIGKILL. Debug-server invocation runs on the main thread synchronously with the harness paused.
- **refs:** runtime/recomp/sbs.cpp (mByteTrace + storeCb decomposition + `dumpByteTrace` + REPL `sbs bytetrace` + env `PSXPORT_SBS_BYTETRACE` / `_ALL` / `_STRICT`); `$CLAUDE_JOB_DIR/tmp/bt5.log` (session capture with the 4-class classifier).

## Post-fix leading-edge f217 divergence (0x800EE0DD..0x800EE10C) is a SCHEDULING-PHASE residual, NOT another case-swap — value-set/count identical A vs B, only phase differs
- **finding (2026-07-03, workflow-first invariant applied a second time — value-distribution symmetry test):** After landing the case-swap fix, the SBS harness's first surviving divergence at f217 moved to a spawn node at 0x800EE0DC..0x800EE10C (48 B = one node record, handler = beh_prng_velocity_machine / FUN_80117658 in A00). Applied the same "settled-state compare" primitive one level deeper: for the single divergent BYTE (0x800EE0DD), tallied the exact value each core writes to it over a 30 s run. Result:
  ```
  Byte 0x800EE0DD — value distribution (settled state):
    val=0x00:  A=6078  B=6078       ← identical
    val=0x20:  A=196   B=196        ← identical
  ```
- **A and B write the EXACT same values with the EXACT same counts** — the byte oscillates between 0x00 (mostly) and 0x20 (occasionally). At f217 A happened to sample it at "0x00" phase, B at "0x20" phase. That's a 1–2 tick scheduling-phase offset, not caller-side divergence.
- **contrast with the case-swap fix:** the anim_trigger_gates bug had an A-only bucket in ALLOCRA (0x801298A4: A=8 B=0) — genuinely different counts. The residual bytes here have symmetric value distributions — the "A-only" tallies in the raw wwatch log are artifacts of the native path (ra=DEAD0000 on A) vs recomp path (ra=0x80117ACC on B) taking DIFFERENT ra's to write the SAME values the same number of times. The tool's per-ra bucketing correctly separates the two cases: same-value/same-count/different-ra = timing phase; different-value/different-count = real bug.
- **why this residual persists across long runs:** the residual 2074 B at f240 is composed of hundreds of small (1–7 B) fluctuating fields inside per-object animation state (node struct fields at +1/+2/+3, +0x2C/+0x30, +0x38, plus scattered animation timers). Every one of them oscillates on both cores over the same value set — but with a 1–2 tick phase offset that never resolves because the two paths (native scheduler on A, recomp coro on B) have permanently offset per-frame yield points. This is the tail of the earlier Slip #1 residual DEMO 1-tick lead (block below in this file — DEMO substate cadence between native and coro), reappearing after Slip #2 was closed. The +6 alloc-delta fix cleared the counter-count divergence (structural), but the temporal-phase divergence is a separate class.
- **fix classes (documented, not landed):**
  - **NOT another case-swap hunt.** Every byte at the leading edge shows symmetric value-distribution — no more "swapped-branch" bugs at this address family, on this evidence.
  - **Real fix = scheduling-cadence alignment.** Close Slip #1's residual 1-tick DEMO lead (see the "FULL-mode 2-frame A→B lead" block below) so the two cores tick object state at the same logical frames. The lead is documented but not yet fixed; it's the root of this and likely of the further per-frame fluctuating fields.
  - **Optional stopgap = mark these fields as SBS noise** (isObjectPoolNoise or a new isPhaseNoise) IFF the port itself doesn't care about the phase (bytes flicker between the same values, no gameplay reader gates on the specific tick they sample). Not yet done — the harness's job is to name divergences; suppressing an entire node struct's flicker fields could mask a real bug in one of them.
- **generalization of the tool (a note for the next session):** the ALLOCRA extension only covers 0x800ED098. To attribute per-byte residuals across an address range without a re-run per address, a `PSXPORT_SBS_BYTETRACE=<lo>,<hi>` bucketing extension (per-byte value distribution + ra bucketing at settled state) would generalize the invariant. Not landed this session because the residual pattern here (all bytes symmetric) doesn't need it — the wwatch + a small Python post-processor sufficed. Land it if a future +N-byte investigation needs it.
- **refs:** `$CLAUDE_JOB_DIR/tmp/ww800ee.log` (30 s wwatch capture over 0x800EE0DC..0x800EE10D), post-processed to the value-distribution table above; game/ai/beh_prng_velocity_machine.cpp:1-22 (the behavior handler owning this node); the "FULL-mode 2-frame A→B lead ROOT-CAUSED" block below (the Slip #1 residual that this residual is the tail of).

## +6 alloc delta at 0x800ED098 FIXED — case-swap bug in native `beh_anim_trigger_gates` STATE 0 (`v1 == 2` should be `v1 == 3`); post-fix net=+0, f240 ramdiff 4794 → 2074 B (−55%)
- **finding (2026-07-03, root-cause named + fixed via per-ra bucketed ALLOCTRACE):** The +3 net-alloc residual at 0x800ED098 was caused by a **case-label swap** in `game/ai/beh_anim_trigger_gates.cpp:54` — the native STATE 0 dispatcher had `if (v1 == 2) leaf(c, obj, 0x8012982C)` but the decomp/recomp ground truth dispatches FUN_8012982C on `v1 == 3`, NOT `v1 == 2`. FUN_8012982C is a 4-record-alloc init loop; with the swap, A's 2 objects with node[3]==2 wrongly went into the 4-alloc init (8 A-only allocs), while B's recomp path took the correct FUN_801296E0 branch for node[3]==2 (0 allocs). The scene has no node[3]==3 objects, so B never fired FUN_8012982C at all in this window. Net: A allocated 8 extra records; other symmetric-shape sites contributed a compensating −2, netting the +6 total delta.
- **evidence — per-ra ALLOCTRACE before vs after the fix:**
  ```
  BEFORE (30 s SBS FULL AUTONAV):
    totalA=944 totalB=938 net=+6
    0x801298A4  A=8  B=0   +8   ← FUN_8012982C 4-alloc loop, fired by beh_anim_trigger_gates on A only
    SBS *** DIVERGENCE at lockstep frame 217: 0x800ED098..0x800ED099 ***
    f240 ramdiff: 4794 B

  AFTER (same run, one-line native fix):
    totalA=938 totalB=938 net=+0                                 ← alloc count equal
    0x801298A4 no longer in ALLOCRA dump (both cores 0)          ← A-only bucket closed
    SBS *** DIVERGENCE at f217: 0x800EE0DD..0x800EE10C ***       ← 0x800ED098 no longer the first delta
    f240 ramdiff: 2074 B                                         ← −2720 B, −55%
  ```
- **how the bug came to be:** the native reimplementation of FUN_80129C00 was transcribed from disas of the overlay handler; the `slti` compare against 4 for the "0/1/2 → FUN_801296E0" branch was correctly written, but the equality check that peels off the FUN_8012982C branch was hand-set to the wrong constant. The file's own header comment even wrote "0/1/3 FUN_801296E0, 2 FUN_8012982C" — which is exactly the swapped mapping (native says n3=2 branches to 8012982C; decomp says n3=3 does). Updated the header comment to match the decomp.
- **decomp is the ground truth** (`scratch/decomp/collectables.c:1105 FUN_80129c00`, STATE 0 lines 1128-1141):
  ```c
  bVar1 = *(byte *)(param_1 + 3);
  if (bVar1 != 3) {
    if (bVar1 < 4) { FUN_801296e0(param_1); return; }   // 0/1/2 → FUN_801296E0
    if (bVar1 != 4) return;
    FUN_80129984(param_1); return;                       // 4 → FUN_80129984
  }
  FUN_8012982c(param_1);                                 // 3 → FUN_8012982C
  ```
- **how the ALLOCRA tool got us here** (the workflow-first invariant paying off): after the flagbit misdiagnosis burned a session on ordinal-point-in-time comparison, the settled-state per-ra table (landed this session) made this bug visible as ONE A-only bucket (0x801298A4 = 8 allocs) in a mostly-timing-symmetric table. That pointed at the recomp function containing 0x801298A4 (`ov_a00_gen_8012982C`), then its callers (`beh_anim_trigger_gates` STATE 0 node[3]==2 branch), then the diff between the native and recomp/decomp for that branch surfaced the case-label swap. The tool is meant to keep working: whenever a +N-alloc delta reopens, `PSXPORT_SBS_ALLOCTRACE=1` first, read ALLOCRA, and the A-only bucket names the caller.
- **workflow lesson (recorded):** when a native reimplementation's own HEADER COMMENT describes the state machine, cross-check it against the decomp — if they disagree, the header/transcription IS the bug. This particular file's comment WAS wrong (matched the buggy code, not the decomp).
- **refs:** game/ai/beh_anim_trigger_gates.cpp:5,55-60 (fix + updated header + inline citation), scratch/decomp/collectables.c:1105-1141 (FUN_80129c00 ground truth), generated/ov_a00_shard_1.c:15285 (recomp confirming decomp), $CLAUDE_JOB_DIR/tmp/fix1.log (post-fix session capture — net=+0, ramdiff 2074).

## PRIOR ATTRIBUTION (kept for the trail) — Per-ra bucketed ALLOCTRACE landed as a durable settled-state compare — `PSXPORT_SBS_ALLOCTRACE=1` now dumps at exit; +6 net traced to native-record_gate lump + one A-only recomp bucket ra=0x801298A4 (beh_anim_trigger_gates → FUN_8012982C init 4-alloc loop, 2 invocations)
- **finding (2026-07-03, workflow-first invariant landed):** Extended `PSXPORT_SBS_ALLOCTRACE=1` (runtime/recomp/sbs.cpp) to bucket every 0x800ED098 store by guest `r[31]` per core. At end of run (atexit + SIGTERM/SIGINT handler so `timeout N …` also emits), prints a settled-state per-ra table sorted by `|A-B|`. New REPL command `sbs allocra` for live inspection; env `PSXPORT_SBS_ALLOCRA_ALL=1` to show symmetric rows (hidden by default so the delta rows stand out). This ENCODES the timing-shift compensation the manager called out — ordinal-point-in-time comparison lied on the prior turn (flagbit STATE 0 read as A-only when both cores fire it 2×, 36 ordinals apart); the fix is comparing per-caller COUNTS OVER THE WHOLE RUN.
- **evidence — 30 s SBS FULL AUTONAV run, total A=944 B=938 net=+6 (matches prior ALLOCTRACE cum + 0x800ED098 byte delta at f217):**
  ```
              A_alloc B_alloc  A_rel B_rel   net(A-B):alloc,rel
  0xDEAD0000      196       0     27     0       +196,+27   ← native record_gate lump (A-only by construction; guest r[31] preserved from prior non-alloc caller)
  0x801298A4        8       0      0     0         +8, +0   ← A-ONLY RECOMP bucket ← real caller-side divergence
  0x80051BC4       14      96      0     0        -82, +0   ← symmetric-shape site: same code, different frame
  0x8013A3DC        0      26      0     0        -26, +0   ← B-only recomp — corresponding site runs native on A (part of DEAD0000)
  0x8003AEA4        0      24      0     0        -24, +0   ← same
  0x801350A0        0      20      0     0        -20, +0
  0x80135E30        0      16      0     0        -16, +0
  0x8013A9EC        0       8      0     0         -8, +0
  0x80136A14        0       8      0     0         -8, +0
  0x8007A7D0        0       0      0    24         +0,-24   ← RELEASE-only ra, B-only → complements 8013A3DC pair etc
  0x8007B220        0       0      0     3         +0, -3
  … (10 more rows, all small |A-B|)
  ```
- **attribution math:** DEAD0000 +196 (A-native) + 0x801298A4 +8 (A-only recomp) − 198 (sum of B-heavy non-native ra's) = **+6 net = the residual delta**. So the +6 is composed of two forces:
  1. A's native `record_gate` runs 196 allocs (untracked-by-ra because native code doesn't set r[31] for its C helper). Many of those correspond to B's non-DEAD0000 recomp buckets (0x8013A3DC/0x8003AEA4/0x801350A0/…). Their sum is ~198, so A's native + B's recomp are 2 apart — mostly timing-symmetric.
  2. **0x801298A4 = +8 A-only recomp = the concrete asymmetric caller.** Anchored at ra=0x801298A4 which sits inside `ov_a00_gen_8012982C` (overlay A00). FUN_8012982C is a 4-record-alloc init loop (generated/ov_a00_shard_1.c:15201). A invokes it 2 times → 8 allocs. B invokes it 0 times in the 30 s window.
- **the caller chain**: `beh_anim_trigger_gates` (native, FUN_80129C00, game/ai/beh_anim_trigger_gates.cpp:54) STATE 0 with `node[3]==2` does `leaf(c, obj, 0x8012982Cu)` = `rec_dispatch(c, 0x8012982C)`. On A, native BehaviorDispatch invokes `beh_anim_trigger_gates` which rec_dispatches to 0x8012982C, allocating 4 records via the internal loop. **On B (psx_fallback=1) the recomp of FUN_80129C00 exists (as verified by the dispwatch pattern on 0x8013B2E4 — B does invoke A00 handlers)** — but the specific object slot that reaches STATE 0 node[3]==2 doesn't fire on B in this window, likely because B's overall scheduling is 30 s worth of gameplay behind (5329 vs 6391 STATE-1 tick counts for flagbit → B is running slower).
- **confidence gap I'm being explicit about:** the manager asked "name the real caller." The largest A-only asymmetric recomp bucket is +8 (ra=0x801298A4 = beh_anim_trigger_gates STATE 0 → FUN_8012982C init). That accounts for 8 of the +6, but the actual +6 is a NET after B-heavy compensation ra's. Whether the +8 at 0x801298A4 is genuinely A-only OR a 30-s-window timing shift (like flagbit turned out to be — 2 fires each, 36 ordinals apart) is NOT PROVEN yet — the same category-error trap as before. To close it: run SBS for 90–120 s, watch whether B eventually fires FUN_80129C00 STATE 0 node[3]==2 twice too. If it does → same "timing shift, not caller divergence" pattern as flagbit → the +6 is not attributable to a single caller; it's a scheduling-cadence residual across ~10 boot-time behaviors. If B never does → real caller-side divergence, and the fix is same as manager's original (b): make B's overlay code path invoke this handler at the same time A's native path does.
- **workflow-first invariant delivered:** the per-ra bucket table + settled-state dump IS the tool the manager asked for — ordinal-point-in-time compare is now permanently misleading-proof (a timing-shifted alloc bucket shows delta=0 in ALLOCRA even if it "looked A-only" mid-run). Future +N-alloc investigations should ALWAYS start with `PSXPORT_SBS_ALLOCTRACE=1` then read the ALLOCRA table before drawing conclusions. And any A-only bucket must be verified as "A-only after long-enough run" not "A-only in a first-30-s snapshot" — that's the discipline the flagbit misdiagnosis is teaching.
- **refs:** runtime/recomp/sbs.cpp:143-153 (RaBucket + mAllocRa), storeCb bucket increments, dumpAllocRa impl, run() atexit + SIGTERM/SIGINT hooks; game/ai/beh_anim_trigger_gates.cpp:54 (native rec_dispatch to FUN_8012982C — the ra=0x801298A4 site); generated/ov_a00_shard_1.c:15201-15260 (FUN_8012982C 4-record init loop, ra=0x801298A4 = JAL-return after rec_dispatch to FUN_8007AAE8); `$CLAUDE_JOB_DIR/tmp/at2.log` (session capture).

## 3-slot 0x800ED098 delta caller **FALSIFIED** — flagbit_timer_machine STATE 0 fires 2× on BOTH cores (recomp path on B verified); +3 net source still unattributed
- **finding (2026-07-03, dispwatch probe on rec_dispatch to 0x8013B2E4):** Landed a per-address per-core dispatch counter in `overlay_router.cpp:rec_dispatch` (env `PSXPORT_DISPWATCH=0x…`, logs `[dispwatch] core=A|B addr=… node=… s0=… stage=…`). Runs in the router before slot-based routing, so it catches every rec_dispatch to the watched address regardless of overlay residency.
- **result — B DOES reach FUN_8013B2E4 via recomp, and hits STATE 0 the same 2 times as native A:**
  ```
  === A native beh_flagbit_timer_machine entries (PSXPORT_FLAGBIT_ENTRY=1) ===
    s0=0:    2 fires (INIT — the 3-record alloc site)
    s0=1: 6391 fires (RUNNING)
  === B recomp dispatches to 0x8013B2E4 (PSXPORT_DISPWATCH=…) ===
    s0=0:    2 fires  ← same INIT count as A
    s0=1: 5329 fires  ← slower RUNNING dispatch (B ~1000 behind A in 30s)
  ```
- **so `beh_flagbit_timer_machine` STATE 0 contributes 3+3 = 6 record allocs on BOTH cores — symmetric. Not the source of the +3 net delta.** My prior "A-only fires" claim was measured with a NATIVE-ONLY probe (BehaviorDispatch entry) and didn't check the recomp path; the corrected two-sided probe shows B also runs the same init exactly twice.
- **the ordinal-390 "3-alloc burst on A that B doesn't do" was a TIMING SHIFT, not a genuine allocation difference:**
  ```
  === per-ra decrement counts, full 30s run ===
  ra=80051A74 (FUN_800519E0 multi-record loop):  A=401  B=401  delta=+0    ← SYMMETRIC
  ra=80051BC4 (adjacent alloc site):             A= 14  B= 96  delta=-82
  ra=DEAD0000 (native record_gate, A-only):      A=196  B=  0  delta=+196
  first store landing at cnt=177 (peak of the "extra 3"): A@ordinal 392, B@ordinal 428
  ```
  Both cores follow the same total 401-count trajectory through the multi-record loop — they just get there at different wall-clock times. My prior "B stops at 180 while A goes to 177" was true at ORDINAL 390 but B still reaches 177, 36 stores later. Ordinal ≠ frame.
- **the +3 net residual IS real** — end-of-30s-run: A cnt=76, B cnt=79, delta=-3 (A holds 3 more allocated). Total decrement counts: A=944, B=938, delta=+6. Both figures match the ALLOCTRACE cum=+6 and 0x800ED098 byte delta of 3 reported at f217. But the -82 delta at ra=80051BC4 vs +196 at DEAD0000 (native-only, no B counterpart) shows the +6 net is scattered across many callers, some going through native record_gate on A and their recomp equivalents on B. **The +3 is not a single caller — it's a scheduling asymmetry between native+recomp on A and pure-recomp on B, distributed across allocs.**
- **manager's structural fix ("B's recomp dispatcher reaches overlay handler") is ALREADY SATISFIED:** overlay_router.cpp routes rec_dispatch(0x8013B2E4) via A00's resident switch, `ov_a00_func_8013B2E4` runs `ov_a00_gen_8013B2E4`, STATE 0 hits 2 times as expected. There is no missing dispatch path to close on this handler.
- **corrected next probe (NOT landed):** the +3 residual is not attributable to one caller. To close it we'd need per-frame per-ra delta over the whole run (extend ALLOCTRACE to bucket by-ra) — that surfaces WHICH ra's contribute the residual +6 alloc after the timing shifts settle. Only then can we point at a specific native vs recomp asymmetry. My earlier close-out on flagbit_timer_machine was premature.
- **workflow lesson (record dead ends too):** the initial `[flagbit] A=2 B=0` result was measured with a native-only probe (behavior_dispatch.cpp:106 entry), which structurally could never fire on B (psx_fallback=1). Concluding "B misses this behavior" from that measurement was a category error. The correct instrumentation for "does B run this behavior?" is at the RECOMP-router entry (`overlay_router.cpp:rec_dispatch`), not the NATIVE-dispatcher entry. When probing whether behavior X fires on the substrate side, hook the substrate router, not the native handoff.
- **refs:** runtime/recomp/overlay_router.cpp:157-179 (rec_dispatch + PSXPORT_DISPWATCH), game/ai/beh_flagbit_timer_machine.cpp:51-55 (PSXPORT_FLAGBIT_ENTRY), `$CLAUDE_JOB_DIR/tmp/dw2.log` + `fe.log` (session captures — both-sides symmetric STATE 0 hit count).

## PRIOR FINDING (kept for trail; falsified by the block above) — 3-slot 0x800ED098 delta CALLER NAMED — native `beh_flagbit_timer_machine` (FUN_8013B2E4) fires on A but NOT on B; each STATE 0 entry runs FUN_800519E0(node, count=3, …) = 3 record allocs
- **finding (2026-07-03, probe 1 landed — per-store ra logging + core-attribution):** Added the two probe hooks the handoff called for and the previous "indirect via Sop::fieldMode" hypothesis is FALSIFIED in favor of a specific native-vs-PSX behavior-dispatch asymmetry:
  1. `mem.cpp:wwatch_check` now prints `[wwatch] core=%p store […]=… by pc=… ra=… stage=…` — pc alone can't attribute the caller (recomp always emits pc at basic-block entry of the target fn), but ra IS the caller's return address inside the recomp substrate.
  2. `sbs.cpp` prints `[sbs] core-map A=%p B=%p` once at boot so the raw `[wwatch]` stream can be split A vs B.
  3. `graphics_bind.cpp:GraphicsBind::recordAlloc` logs `__builtin_return_address(0)` when `PSXPORT_RECALLOC_TRACE=1` — attributes the native beh_*.cpp caller (addr2line resolves the ra to file:line).
- **evidence — decrement-sequence divergence, values-at-ordinal split**  (`$CLAUDE_JOB_DIR/tmp/ww2A.log` + `ww2B.log`):
  - First divergence at ordinal 390 (both cores identical for the prior 389 stores). Both hit cnt=180; A continues decrementing 180 → 179 → 178 → 177 (three EXTRA allocations) then starts releasing. B stops at 180 and starts releasing immediately (180 → 181 → 182 …).
  - All three extra decrements on A carry `pc=8007AAE8 ra=80051A74`. ra=0x80051A74 sits INSIDE `FUN_800519E0` — specifically the instruction after the `jal FUN_8007AAE8` in the multi-record allocation loop (decomp: `piVar3 = FUN_8007aae8(); *(iVar6 + 0xc0) = piVar3; …; while (iVar5 < node[+9]);`).
- **evidence — attribution to the native flagbit behavior via a targeted probe:** Instrumented `beh_flagbit_timer_machine.cpp:S0` to log at every state-0 entry (env `PSXPORT_FLAGBIT_TRACE=1`). In a 30 s SBS FULL-mode AUTONAV run: **A fires STATE 0 twice (node=0x800FE198, cnt_before=491, stage=GAME), B fires it ZERO times.** Each STATE 0 calls `leaf4r(nd, 3, mem[0x800ECFD4], 0x8015AABC, 0x800519E0)` = FUN_800519E0 with param_2=3 = a 3-record init loop = exactly 3 decrements of 0x800ED098. **2 native fires × 3 allocs each = A's +6 alloc lead** (matches the ALLOCTRACE cum=+6 figure from the block below; +3 is what remains after RELEASE counts also diverge).
- **why B misses it:** `FUN_8013B2E4` is OVERLAY code (per beh_flagbit_timer_machine.cpp:1 header — "overlay handler, disassembled from scratch/ram/field_seaside.bin"). The native BehaviorDispatch table (game/object/behavior_dispatch.cpp:106) has it registered so A's native object-list walk (`c->engine.behaviors.dispatchObj`) invokes native `beh_flagbit_timer_machine`. B runs with `psx_fallback=1` — B does NOT enter the native scheduler's object-walk path, so it never consults `BehaviorDispatch`. B's PSX-side scheduler is the recomp resident dispatcher; if that dispatcher's handler-jump for 0x8013B2E4 lands on address whose recompilation isn't in B's set (overlay), B silently does nothing at that handler. Result: A dispatches the behavior + allocates 6 records; B never dispatches + never allocates → +6 alloc lead, +3 net after releases also diverge.
- **falsifies the prior "indirect via Sop::fieldMode case 0" hypothesis** (block below): the 3 spawn.dispatch calls in Sop::fieldMode DO stamp handlers, but the +3 slot delta at gameplay-start is not from downstream allocations by those stamps' handlers — it's from a completely different behavior handler (`FUN_8013B2E4` = flagbit_timer_machine, called from the object-walk during boot/DEMO), which A owns natively but B never reaches. Update the prior finding when you have bandwidth; leave it below as the "step that got here" trail.
- **corrected fix direction:** two paths, pick per the port's architecture:
  - **(a) match the FULL-PSX B path.** Make the SBS harness's B path invoke `BehaviorDispatch::dispatchNative` first (like A), and drop the `psx_fallback` gate for BehaviorDispatch. This aligns the two cores on native behavior ownership but partially defeats the "full PSX comparison" purpose. Cheap.
  - **(b) match the NATIVE A path from B via recomp** — the intended long-term shape. Ensure FUN_8013B2E4 is EITHER in B's recomp set OR make B's resident overlay-loader populate the same behavior handler. Structural, matches the port's "PSX substrate is behavioral ground truth" invariant.
  - Either way, the "correct" behavior is that BOTH cores run the flagbit behavior at boot; only the mechanism differs.
- **refs:** runtime/recomp/mem.cpp:75-79 (wwatch print with ra), runtime/recomp/sbs.cpp:636 (core-map print), game/world/graphics_bind.cpp:71-80 (GraphicsBind::recordAlloc `__builtin_return_address(0)` under PSXPORT_RECALLOC_TRACE), game/ai/beh_flagbit_timer_machine.cpp:63-66 (PSXPORT_FLAGBIT_TRACE), game/object/behavior_dispatch.cpp:106 (native registry entry for 0x8013B2E4), scratch/decomp/ram_f1000_all.c L31632-31683 (FUN_800519E0 multi-record loop decomp = the 3-alloc site), `$CLAUDE_JOB_DIR/tmp/ww2A.log`+`ww2B.log`+`flag.log` (session captures).

## 3-slot 0x800ED098 delta ISOLATED to Sop::fieldMode case 0 spawn loop — new `PSXPORT_SBS_ALLOCTRACE=1` per-frame allocator counter landed
- **finding (2026-07-03, attack (a) probe on FUN_8007AAE8):** Landed a per-frame per-core write-counter for 0x800ED098 in the SBS harness (`PSXPORT_SBS_ALLOCTRACE=1`). Logs every frame where A's allocation count differs from B's, with per-frame and cumulative deltas. Zero cost when off; exact-address check (a == 0x800ED098) so neighboring bytes don't false-count.
- **result from a clean SBS FULL-mode run** (`$CLAUDE_JOB_DIR/tmp/alloctrace4.log`):
  ```
  f61  A: this=47  cum=47  | B: this=0   cum=0   | delta this=+47 cum=+47   (initial spawn on A)
  f62  A: this=0   cum=47  | B: this=47  cum=47  | delta this=-47 cum=+0    (B mirrors — converged)
  f117 A: this=343 cum=390 | B: this=0   cum=47  | delta this=+343 cum=+343 (SOP intro spawn burst — A)
  f118 A: this=67  cum=457 | B: this=340 cum=387 | delta this=-273 cum=+70  (B mirrors 340 of 343)
  f119 A: this=1   cum=458 | B: this=67  cum=454 | delta this=-66 cum=+4    (B still catching up)
  f120 A: this=0   cum=458 | B: this=1   cum=455 | delta this=-1  cum=+3   <-- FIRST +3 lands here
  f158 A: this=322 cum=780 | B: this=319 cum=774 | delta this=+3  cum=+6   <-- SECOND +3 (area transition)
  f159..f190 A=B every frame                     | cum stays at +6
  ```
- **the +3 lead lands in TWO discrete events, each +3 exactly:**
  1. **f117-f120 SOP intro window** — A spawn burst is 3 objects LARGER than B's. Cumulative goes from 0 → +3.
  2. **f158 area transition** — A spawns 322 vs B 319, diff of exactly 3. Cumulative goes from +3 → +6.
- **exact number 3 = Sop::fieldMode case 0's spawn loop count** (sop.cpp:252-261): `for (int i = 0; i < 3; i++) c->engine.spawn.dispatch(3, 3, 1)`. The loop spawns 3 scene objects per SOP entry. Both events are SOP case 0 entries (initial SOP intro at f117-f120 + area transition at f158).
- **decomp of both sides shows spawn.dispatch itself is NOT the direct writer of 0x800ED098** (spawn.cpp:180-223 + generated/shard_5.c:11262-11307 gen_func_8007A980):
  * Native `Spawn::dispatch(cls=3, ...)` → `spawn_variant_native(cls=3)` → `pool_spawn(POOL_VAR3)` where `POOL_VAR3 = { free_head=0x800ED8D4, cnt=0x800ED8C5 }` (spawn.cpp:199). **NOT 0x800ED098.**
  * Recomp `gen_func_8007A980` for cls=3 dispatches to `func_8007A12C` (shard_5.c:11296). Per the spawn.cpp:198-200 comment, native `pool_spawn(POOL_VAR3)` is byte-perfect reimpl of 0x8007A12C (verified in commit af27fd8).
- **so the +3 delta at 0x800ED098 is INDIRECT.** The 3 spawn.dispatch calls do NOT touch 0x800ED098 directly. But they stamp `node[0x1c] = per-scene handler` (sop.cpp:260) — pointers from the SOP overlay table at 0x8010c98c. When object-list dispatch runs these 3 nodes on subsequent frames, their handlers may allocate downstream objects from the MAIN pool at 0x800ED098 (the 520-slot free-list initialized by Pool::init and drained by FUN_8007AAE8 = the allocator we identified earlier). One extra allocation per stamped handler × 3 handlers = exactly +3 on A.
- **why the handlers may differ:** the 3 stamped handlers come from `mem_r32(t + 8)` where `t` walks the table at 0x8010c98c (stride 12). Same table, same reads on both cores. **BUT** the per-scene handlers run at different logical frames on A vs B (due to residual DEMO slip + native scheduler cadence). The list-position or timing gate may cause the handlers on B's coro to skip their downstream allocation while A's runs it. Or a handler may bail on `mem_r16(0x800ED098) < 3` (the pool-low guard in `entity_spawn`, spawn.cpp:132) at a different frame count.
- **fix path — 2 concrete next-session probes:**
  1. **Wide-window WWATCH on 0x800ED098 across f117-f120 + f158.** `PSXPORT_WWATCH=0x800ED098,0x800ED09A` will log EVERY decrement site with pc. Diff A's write log against B's for that window — the 3 pc's that appear on A but not on B name the exact downstream callers.
  2. **Trace `node[0x1c]` handler pc's for the 3 stamped objects.** After Sop::fieldMode case 0 completes, log `mem_r32(node + 0x1c)` for each of the 3 spawned nodes on both cores. If handlers match, the divergence is inside the handler execution (frame-timing gated). If handlers differ, the divergence is in the stamp itself (table read anomaly).
- **cum delta discrepancy (+6 in trace vs +3 in actual ed098 count):** allocator RELEASES (increments of 0x800ED098) also differ — A has 3 more releases than B, netting the visible +3 in current free-slot count. The trace counts decrements only.
- **fix direction (NOT landed — needs next session):** decomp `FUN_8007A980` (the spawn function in the SOP overlay) vs native `Spawn::dispatch` and compare — find which path each takes for allocation. Likely one uses the 0x800ED098 pool via FUN_8007AAE8 (the free-slot allocator we RE'd earlier) and the other uses a different pool or skips a leaf. Once identified: align the codepaths so both allocate through the same pool for the SOP objects, and the +3 delta closes.
- **refs:** runtime/recomp/sbs.cpp `mAllocTraceOn`/`storeCb` + arm block, game/scene/sop.cpp:252-261 (the 3-iteration spawn loop — the exact-count source), generated/ov_sop_shard_0.c L_801094F8 (recomp equivalent), scratch/decomp/functions.csv (find FUN_8007A980 + FUN_8007AAE8 bodies for the next decomp), $CLAUDE_JOB_DIR/tmp/alloctrace4.log (session capture).

## FULL-mode 2-frame A→B lead ROOT-CAUSED: native DEMO/GAME dispatcher runs substates ~1 frame ahead of the coro/recomp path — introduced twice (+1 at DEMO leave, +1 at GAME cutscene entry); durable `PSXPORT_SBS_STAGETRACE=1` diagnostic landed [PARTIALLY CLOSED — Slip #2 fixed 31fa879; Slip #1 residual remains, tracked below]
- **finding (2026-07-03, attack (a) — stagetrace of sm[0x48..0x4e] + task-entry per core per lockstep frame):** In `PSXPORT_SBS_MODE=full` with the new `PSXPORT_SBS_STAGETRACE=1` diagnostic (runtime/recomp/sbs.cpp), the 2-frame A→B lead that produces the `0x800ED098` off-by-3 divergence has TWO distinct +1-frame slips, both at NATIVE-vs-RECOMP stage-body cadence boundaries. Not a specific opcode bug — a structural pacing gap.
- **evidence — the exact slip frames** (from `$CLAUDE_JOB_DIR/tmp/stagetrace.log`, condensed):
  ```
  f26 A entry=8010637C(GAME) sm48=5/4a=1 | B entry=801062E4(DEMO) sm48=5/4a=1   <-- SLIP #1: A leaves DEMO 1 frame early
  f27 A entry=8010637C     sm48=2/4a=0 | B entry=8010637C     sm48=5/4a=1       (B just entered GAME; still holding DEMO's stale sm48=5 in RAM)
  f28 A entry=8010637C sm48=2/4a=0/4e=1 | B entry=8010637C sm48=2/4a=0/4e=0     <-- A already 1 sm4e ahead
  f29 A cut=1                            | B sm48=2/4a=0/4e=1                    <-- SLIP #2: A raises the cutscene flag 1 frame before B reaches sm4e=1
  f30 A cut=1                            | B sm48=0/4a=0/4e=0                    (B doing intermediate sm reset)
  f31 A cut=1                            | B cut=1                               <-- B finally reaches cutscene up
  ```
  Convergence resumes: from f31 onward A and B tick sm[0x4c/0x4e] in lockstep, and both hit `gameplay-start` at exactly f216 (SBS nav's own 60-frame-cutscene-clear timer synchronizes them). But the 2-frame gap that was open during f26..f31 = 6 lockstep frames worth of *scene-time drift* — during those 6 frames, native A ran pool.init + object allocations that PSX B ran later, cumulatively producing the +3 slot count in 0x800ED098 at f217.
- **native path advantage — where it comes from:** two scheduler branches in `runtime/recomp/scheduler.cpp` that run stage BODIES synchronously on A (native_content=true) while B runs them as pure recompiled coros that yield mid-body:
  1. **DEMO native (scheduler.cpp:127-169):** A's DEMO fresh entry runs `c->engine.demo.stageMain()` (full prologue + s0 in one C call) THEN `c->engine.demo.frame()` (one substate dispatch) in the SAME scheduler tick. B's coro path runs 0x801062E4 recomp body — its own prologue equivalent yields ONCE mid-way (via `FUN_80051f80` per-frame yield), so the prologue costs one extra scheduler tick.
  2. **GAME native (scheduler.cpp:210-273):** A's GAME fresh entry runs `c->engine.stagePrologue()` then `c->engine.frame()` (which dispatches sm[0x48], reaches Engine::fieldRun, and its sm[0x4e]=0 case in one C call runs Pool::init + resetControlBlock + seedAreaObjects + placeAreaObjects + reset75240 + setupViewScroll + finalViewInit ALL in one C call, then sets sm[0x4e]=1). B's coro of 0x8010637C yields mid-way through the SAME sequence — its recomp body of 0x80106b98 sm4e=0 case does 8 rec_dispatch calls whose deep leaves yield to the scheduler, spreading the work across multiple ticks.
- **why "cadence match" is the real fix, not the SBS-side hold band-aid:** the SBS harness EXISTS to name real gameplay divergences. A 2-frame lead in the boot pool-alloc phase is REAL scene-time desync that gameplay reads see (docs/findings above: 0x800ED098 gates spawn thresholds). Suppressing it inside SBS (option b) would tell every future divergence session "no problem here" while the port itself would still ship a native path that reaches gameplay state 2 frames ahead of the substrate baseline — divergences from that lead would masquerade as genuine bugs. Correct fix is to make native DEMO/GAME dispatcher's per-tick work match what the recomp substrate does per tick.
- **fix direction (NOT landed — needs the next session):** either (a) split the native DEMO/GAME fresh-entry work so the C call yields after each substate the recomp body would have yielded after (mirroring `FUN_80051f80`'s frame boundaries — likely requires each Engine::frame() step to return to scheduler if it has done its "one substate's worth"), OR (b) make the coro path skip its `FUN_80051f80` yield when the substate is one that native would have run inline (extends `rec_dispatch` with a per-fn hint). Option (a) is cleaner but touches every native stage dispatcher; option (b) is one-place but pollutes the substrate. Preferred (a) — the recomp yield IS the ground truth for stage-body pacing, so native should match it.
- **first-attempt fix TRIED & REVERTED (this session):** removed the `c->engine.demo.frame()` call after `demo.stageMain()` on DEMO fresh entry in scheduler.cpp (line 148), on the hypothesis that native fresh entry was running TWO DEMO substates (s0 via stageMain + s1 via frame) vs coro's one. Change worked in isolation — trace shows f2 A sm4a=0 instead of 1 — but did NOT reduce the overall lead: A still reached GAME_ENTRY at f26 and B at f27 as before. Post-fix ramdiff at f240 stayed at 4794 bytes (same as pre-fix). This proves the DEMO substate cadence isn't the actual bottleneck. What IS the bottleneck (verified from the reverted-fix trace):
  * f1: A enters DEMO (task-0 entry rewritten from 8010649C→801062E4). B stays at 8010649C until f8. That's a **7-frame START.BIN lead** (native `startBinStage` in scheduler.cpp:400 runs the file-table build synchronously in one tick; B's coro of 0x8010649C takes 7 ticks to progress the recomp body).
  * By f13 both cores converge at sm48=3/4a=0 (B rapidly progressed through DEMO substates s3→s0→s1→s2→s3 in f6..f13 = 8 frames to A's 8 frames — B caught up 6 of the 7 lost frames).
  * DEMO progression is then EQUAL rate f13..f25 (both stay at sm48=5/4a=1 by f25 = LEAVE_DEMO_TO_GAME).
  * f26: A's DEMO substate s5 calls `native_start_stage` synchronously, rewrites task-0 entry to GAME_ENTRY. B's coro-of-0x801062E4 substate s5 does the SAME work through the recomp body, but takes 1 more tick (f27). That's Slip #1 = **1 frame in the DEMO→GAME transition, specifically at substate s5's stage-swap machinery, not in the DEMO substate DISPATCH cadence.**
  * f27..f29 A GAME transitions to cutscene up (3 frames). f27..f31 B does the same in 4 frames. That's Slip #2 = **1 frame in GAME's early ticks**. Not narrowed to a specific substate yet — needs sm[0x4c]/sm[0x4e] progression comparison of every A vs B tick during f27..f31.
- **corrected fix direction (for the next session):** the lead is at the STAGE-SWAP boundary (DEMO→GAME) and the GAME early ticks, not the DEMO substate cadence. Instrument native_start_stage (native side) vs the recomp body's equivalent stage-swap path — find what work native does in the swap tick that recomp defers to next tick. Same for GAME's first 4 ticks.
- **per-tick verbose stagetrace landed (`PSXPORT_SBS_STAGETRACE=2`):** logs EVERY tick in f22..f36 with the full sm state (sm[0x48/0x4a/0x4c/0x4e/0x50] + task-0 base state + init48 selector + cut flag). Confirmed the slip windows tick-by-tick:
  * **Slip #1 = DEMO sub-machine 0x80106AC4 (main-menu, dispatched from `demo_frame_s3`).** f24 both cores sm48=3/4a=1. f25 A sm48=5, B still 3. Iter 25 A: `demo_frame_s3` → `rec_dispatch(0x80106ac4)` returns v0=2 with sm[0x68]==2 → advances sm48=5 in one tick. Iter 25 B: SAME rec_dispatch — but returns a value that doesn't advance sm48. B needs an extra tick (f26 catches up). The sub-machine is called with `rec_dispatch` (SYNC advertised) on both sides, so the divergence is INSIDE the sub-machine body — likely a per-frame input/timer read that native saw as "advance now" and coro saw as "one more tick". Concrete symptom: v0 differs on the SAME frame between the two cores. This is the specific writer of the +1 in Slip #1.
    - **decomp of FUN_80106AC4** (scratch/decomp/80106ac4.c): a 2-state menu handler on sm[0x4a]. State 0 arms sm[0x5a] to 0x1c2 (450-frame timer) and jumps to state 1. State 1 runs 2 sub-calls (0x80106690/0x80106824), decrements sm[0x5a], returns 1 if timer expired (→ s7 attract). Otherwise reads `DAT_800E7E68` (this-frame pad-edge bits): bit 0x20/0x80 = Up/Down (return 0 sets sm[0x68]); bit 0x4008 = Cross+? → **returns 2 → advances DEMO to sm48=5 (LEAVE-DEMO to GAME)**; bit 0x2000 = Circle → returns 3. **So the specific value that races is `DAT_800E7E68 & 0x4008`.** On A that bit set at f25; on B not until f26.
    - **DAT_800E7E68 is the pad-edge bitmap** (this-frame pressed-button transitions; menu.cpp:27 confirms). Both cores drive the same pad tap via SBS navStep (Cross every 12 frames × 6 frames). It's set from within a guest fn (probably called from `demo_frame_s0`'s `rec_dispatch(0x8005082cu)` input-reset leaf or its equivalent per-frame update). Both cores call the SAME fn — but at different LOGICAL frames because A ran the fresh-entry substates faster than B (see the 7-frame START.BIN native lead earlier, compensated to a 1-frame residual by f25). The 1-frame residual is enough that A's pad-edge poll at iter 25 catches the Cross edge, B's at iter 26. **True root cause: pad-edge polling is on a per-Core timing that inherits the earlier 1-frame START.BIN native lead** — the ENTIRE Slip #1 is just this earlier residual reasserting itself at the first input-gated substate. Not a bug in FUN_80106AC4 or its callers — a bug in the START.BIN native ownership pacing.
    - **implication for fix (Slip #1):** don't touch DEMO's sub-machine or 0x80106AC4. The correct fix is to make `Engine::startBinStage` (scheduler.cpp:400 native path) take the SAME number of ticks as the coro of 0x8010649C on B does. Concrete measurement (from stagetrace f0..f8): A rewrites entry from START.BIN to DEMO at f1 (native runs the whole file-table build + stage-swap in one tick); B stays at 8010649C for 7 ticks. **Land the fix at startBinStage — spread the file-table build over the same tick count as the recomp of 0x8010649C uses.** Everything downstream is just this residual.
    - **naive fix TRIED & REVERTED (023142f):** removed the `native_content && fresh && resume_pc == 0x8010649Cu → c->engine.startBinStage()` special-case at scheduler.cpp:397, letting A fall through to `rec_coro_run(c, 0x8010649C)` with the recomp body (same as B's path). Rationale: if B's coro of 0x8010649C works with the CD sync layer, A's recomp path should too. **Result: recomp-MISS on 0x80051FA4 immediately at boot** (`no recompiled fn for 0x80051FA4` at first-tick abort). A's recomp set is a strict subset of B's — native ownership is required.
    - **plumbing step-spread LANDED (this session):** SchedulerState now holds `uint8_t stage0_step[3]`; a new scheduler branch (before the generic path) re-enters `Engine::stage0Advance(step)` on each subsequent tick with entry=0x8010649C and stage0_step<7. `startBinStage` was split — file-table build stays on fresh entry, the preload SM was moved to `stage0Advance` (steps 0-6): step 0 = preloadTexgroup, step 2 = preloadStage1, steps 1/3/4/5 = padding to match measured coro cost, step 6 = `native_start_stage(1)` + `scheduler_yield`. Boot is now aligned: **both cores enter DEMO at f8** (was A@f1 vs B@f8). Verified via stagetrace.
    - **but ramdiff unchanged at 4794 bytes.** A GAME @f26 B GAME @f27 unchanged. A cutscene @f29 B cutscene @f31 unchanged. The residual 1-tick lead A→B in DEMO progression persists — but critically, ramdiff at f240 is IDENTICAL to pre-fix. That means the START.BIN native-lead was NOT actually driving the ramdiff — the divergence source is elsewhere (Slip #2 in SOP fieldMode, or the pad-edge race in FUN_80106AC4, or both). The step-spread is structurally correct + a durable prerequisite for future full alignment (any future alignment on top must start from a matching START.BIN cadence), but it does not by itself resolve the SBS divergence.
    - **residual DEMO 1-tick lead diagnosis:** even with boot aligned, the DEMO substate progression rate differs. A native runs demo.frame() per tick; B coro runs the recomp body's per-iteration loop. The Cross-tap that triggers LEAVE-DEMO fires on different iterations because native's per-frame pad read cadence and coro's recomp-body yield points don't line up. Tried a demo_frame_s0 shift (stageMain not calling s0 inline; let demo.frame() dispatch it on the same tick) — no effect. The residual is at a finer grain than substate dispatch, likely inside 0x80106AC4's timer/pad-edge polling that runs at different logical frames on the two paths. Beyond the scope of START.BIN alignment. Next session should attack Slip #2 (SOP fieldMode mid-body yield) — since the ramdiff didn't move when START.BIN was aligned, SOP is likely the dominant contributor.
  * **Slip #2 = SOP-fieldMode LOAD BLOCK on B's coro-side takes 2 ticks; native does it in 1.** f29 A: sm50=1 cut=1 in one tick (Sop::fieldMode case 0 does `native_sop_area_load` + `pool.init` cluster + sets sm50=1 + cut=1 all inline). f30 B: sm48=0/4a=0/4c=0/4e=0 (all reset — likely coro re-entered from the top of the GAME loop after a mid-body yield in SOP fieldMode recomp). f31 B: catches up (sm50=1 cut=1). The `mem_w8(0x1f800137u, 1)` (cut=1) that Sop::fieldMode does at case 0's end (sop.cpp:271) is one tick behind on coro because SOP's recomp body yields somewhere in the load path.
    - **decomp of recomp ov_sop_gen_80109450 case 0 (L_8010949C):** non-looping (called once per GAME frame). Does `rec_dispatch(0x8007E9C8)` (screen fade), then `rec_dispatch(0x80044BD4)` — this is the yield site. FUN_80044BD4 decomp (scratch/decomp/bd4.c) shows `while (DAT_1f80019b == 0) FUN_80051f80(1)` — spawns a slot task and yields at least once waiting for the callback to set `1f80019b=1`. Native `native_sop_area_load` runs the callback INLINE = zero yields. **Cost: coro pays ~2 ticks (dispatch + first yield-return), native pays 1.** That's the +1 tick.
    - **fix landed this session:** added `SchedulerState.sop_field_step[3]` counter and deferred native `Sop::fieldMode` case 0 completion by one tick — first entry sets `sop_field_step[0]=1` and RETURNS without touching sm[0x50]; second entry runs the actual work + resets `sop_field_step[0]=0` for the next area load. Screen stays black one tick longer on A, matching B's coro-side wait.
    - **verified:** A cutscene up @f30 (was @f29), B cutscene up @f31 (unchanged) → 2-tick gap closed to 1-tick. Ramdiff at f240 UNCHANGED at 4794 bytes (dominated by arena-offset chains at 0x800F0000, not gameplay timing). The 0x800ED098 pool count delta of 3 slots didn't change either — suggests either Slip #1 (still 1-tick DEMO residual) is the writer of the 3-slot lead, OR the lead accumulates elsewhere during the cutscene/gameplay window.
- **fix path narrowed:** for Slip #1, either (a) instrument the 0x80106AC4 recomp body to see WHICH iteration returns v0=2 on B vs A (grep decomp for it — likely a menu input debouncer), OR (b) hold native's demo_frame_s3 for 1 extra tick when the sub-machine indicates "just advanced to LEAVE-DEMO" (matches coro's tick count without touching recomp). For Slip #2, either (a) find the mid-body yield in SOP's recomp fieldMode and either drive it sync in the substrate or defer native by 1 tick, OR (b) same hold-one-tick approach in native Sop::fieldMode. Attempts NOT landed this session — needs the decomp analysis of 0x80106AC4 first (the actual "does one iteration of a menu-input debouncer take a different count on native vs coro") before a real fix; a blind 1-tick hold is a stopgap, not a root cause.
- **durable tool landed:** `PSXPORT_SBS_STAGETRACE=1` (runtime/recomp/sbs.cpp) — on every SBS lockstep frame during f0..f250, prints A vs B TASK0_ENTRY + sm[0x48/0x4a/0x4c/0x4e] + CUT_FLAG whenever any of them change on either core. Re-usable for any future "when did the two cores diverge in stage progression?" hunt. Zero cost when off.
- **refs:** runtime/recomp/sbs.cpp `stagetrace` block (~line 675 in `Sbs::Impl::run`), runtime/recomp/scheduler.cpp:127-169 (DEMO native), :210-273 (GAME native), :283-331 (coro fallback for B — the recomp-substrate path), game/scene/engine_stage.cpp:571-584 (Engine::fieldRun case 0 — the 7-native-call cluster that runs in one C call on A), game/scene/engine_demo.cpp:660-661 (Demo::frame's pool.init call — never reached on B), scratch/decomp of 0x80108784/0x80106b98 (recomp bodies whose per-frame yields set the substrate cadence), `$CLAUDE_JOB_DIR/tmp/stagetrace.log` (session capture).

## FULL-mode next gap: `0x800ED098` = pool free-slot count, native leads PSX by 3 slots — REAL port gap (gameplay reads it against literals `< 2`, `< 4`, `< 10`, `< 13`) [PROXIMATE CAUSE ATTRIBUTED — see block above]
- **finding (2026-07-03, PREWATCH after arena/counter filters landed):** First surviving divergent byte in FULL mode is `0x800ED098` (halfword actually — the diff is on the low byte, A=0x34 vs B=0x37 = decimal 52 vs 55, off by 3). RE'd the writer via disas of pc=0x8007AAE8: it's the FREE-SLOT-STACK ALLOCATOR (`FUN_8007AAE8`): reads count at 0x800ED098, checks `> 0`, decrements, pops next slot from stack at `*0x800E7E74`, advances cursor. So `0x800ED098` = **object-pool free slot count**, initialized to 520 at `game/world/pool.cpp:68` (`mem_w16(0x800ED098u, 520)`), decremented once per object allocation.
- **NOT noise-filter — gameplay reads it against literals:** `grep -rE 'DAT_800ed098|0x800ed098' scratch/decomp/ game/ runtime/` names many concrete threshold checks that gate object spawning: `entity.cpp:47` (`< 2 → obj[4]=3 return 0`), `graphics_bind.cpp:53` (`<= 0 → obj[+4]=3 return 1`), `beh_multi_record_phase_machine.cpp:80` (`< 10 → obj[4]=3`), `collectables.c:132` (`< 0xd`), `collectables.c:992` (`< 4`), `collectables.c:1047` (`< 4`), `ram_f1000_all.c:21186` (`< 2`), `ram_f1000_all.c:17946` (`< param+8`). A 3-slot lead by native gp WILL cross these thresholds at different frames — real gameplay divergence, potentially observable as "one core spawns a collectable, the other doesn't."
- **root cause (traced via PREWATCH under clean filter):** Both cores write initial `0x00000208` (520) to `0x800ED098` from the same native code path (`pc=0x8009A420` stale). A does it at f28, B at f30. **2-frame boot lead.** Then during boot, both cores decrement as objects allocate. At f59 wwatch caught A ripping through a LOOP of allocations (writing 0x1E8 → 0x1E7 → 0x1E6 → … all in one frame — probably a bulk-scene-init). B goes through the same loop but at a different frame. Cumulative +3 lead by gameplay-start f216.
- **why the 2-frame lead:** native gp reaches the field earlier than the PSX substrate coroutine yields into it. Same phenomenon as the 0x800BF878 frame counter off-by-1, but the counter measures fieldFrame calls (native leads by ~1) while the pool count measures allocator calls (native leads by ~3 = several allocs per lead-frame).
- **fix path (NOT landed this session — substantial):** align the two paths' first-field-frame entry. Options: (a) instrument boot to log every fieldFrame call on both cores until they agree on the first-call frame, then find the divergent decision point; (b) have SBS hold the native path's Engine::fieldFrame for N frames post-`gameplay-start` until B catches up (surgical, keeps the observation clean without touching the game code); (c) fix native scheduler to yield-once-then-run instead of run-immediately, matching PSX coroutine cadence. (b) is the SBS-side band-aid that keeps the harness surfaces clean; (a)/(c) are the real fix.
- **refs:** game/world/pool.cpp:68 (init to 520), scratch/decomp/collectables.c:132/948/992/1047 (literal-threshold readers), game/world/entity.cpp:47 (< 2 gate), game/world/graphics_bind.cpp:53 (< 0 gate), `$CLAUDE_JOB_DIR/tmp/ed098.log` (session capture: f28 A / f30 B init + f59 A allocator loop).

## FULL-mode SBS filters — arena offset + counter noise-filtered; next real byte is `0x800ED098` (off by 3, likely object count) [SUPERSEDED — this block was written before the readers were grep'd; the NOT-noise-filter finding is above]
- **change (2026-07-03):** Post-arena-analysis, added two entries to `Sbs::Impl::isObjectPoolNoise` (runtime/recomp/sbs.cpp): (a) `0x800ED550..0x800EDF00` — the 3-list × N-slot object-list pointer array whose entries all inherit the native-vs-PSX-gp allocator arena offset (verified 2026-07-03 in `$CLAUDE_JOB_DIR/tmp/full5.log`: both cores initialize the head to `0x800F7734` at different boot frames, then diverge as objects allocate — no gameplay reader takes a literal 0x800F77xx / 0x800F4Dxx address, `grep` = 0 hits, so the offset is invisible to game logic); (b) `0x800BF878..0x800BF87C` — the per-frame counter at `engine_stage.cpp:393` (native reaches field one frame earlier than the PSX coroutine yields into it, so off-by-1 for the whole session, bounded and no gameplay reader consumes the exact value).
- **also this session — PREWATCH filter improvement:** the `divergent = mask!=3 || va!=vb` check was flagging every asymmetric identical-value write as "divergent" (mask=1 or mask=2 with va=vb=0 = both cores did the same boot-init write to zero, just at different frames). Refined to: on asymmetric-mask hits, compare the writer's value to the OTHER core's *current* byte; only pause if they truly differ. Verified against 0x800BF878 (which prev-flagged as divergent at f1 boot noise) → now correctly pauses at f116 (real ahead-by-one moment). ~5x now vs 100x forever cleanup — every future PREWATCH hunt starts clean.
- **also this session — ramdiff / recordDivergence consistency:** `sbs ramdiff` and the range-expansion in `recordDivergence` used `isRenderRegion` alone; changed to `isDiffNoise` so all filter categories apply consistently. Ramdiff now hides arena pointer chain + counter along with libcd/audio noise.
- **verified impact:** `PSXPORT_SBS_MODE=full`, ramdiff span/byte counts:
  * pre-filter: 1836 spans / 5005 B (region 0x80010000..0x80200000).
  * arena filter only: 1699 spans / 4608 B.
  * arena + counter: 1698 spans / 4607 B.
- **next real gap:** first surviving divergent byte is now `0x800ED098` (A=0x34, B=0x37, off by 3 — likely an object-count field associated with the list-head pointer array we just filtered). Not investigated this session; PREWATCH it in the next.
- **refs:** runtime/recomp/sbs.cpp — `isObjectPoolNoise`, filter improvement in `storeCb` divergent-check, `ramdiff` cmd using `isDiffNoise`.

## FULL-mode SBS — first byte 0x800BF878 is a per-frame counter off-by-1 (timing); volume driver is the 0x800F0000 object-pool base-address divergence (~3.2K bytes) [FILTERED 2026-07-03]
- **symptom (2026-07-03):** After the RENDER-mode fix landed (previous block), FULL-mode SBS still shows ~5.5K bytes of steady divergence between native gp (A) and PSX-substrate gp (B). First byte to differ = `0x800BF878` (A=0x61 B=0x60 at f217 — off by ONE). Top three pages in the histogram: `0x800F0000:3232` (object pool), `0x801F0000:1048` (scheduler control — the "differs by design" region), `0x800E0000:814` (tail of packet pool / gameplay state right after it).
- **status of the first-byte finding (0x800BF878):** RE'd — it's a per-frame counter incremented once per `Engine::fieldFrame` call. `engine_stage.cpp:392-393` does `mem_w32(0x800bf878, mem_r32(0x800bf878) + 1)` unconditionally. Both cores hit this via different PCs — `PSXPORT_WWATCH=0x800BF878,0x800BF87C` shows:
  * Core A (native gp) writes with `pc=0x8008E0C0` (stale from last rec_dispatch — FUN_8008E0C0, a small helper unrelated to the counter — native code doesn't update pc, so this is just the last-known guest PC).
  * Core B (PSX gp) writes with `pc=0x80108B0C` — the field-frame overlay body containing the `sw` to 0x800BF878.
  Same write, different PCs — trivially expected. But A ends the frame at 0x61 and B at 0x60 → one core ran `fieldFrame` one MORE time. Sequence of stored values interleaved from the wwatch log: A ascends 0x5B→0x5C→…→0x61, B ascends 0x59→0x5A→…→0x60. A leads B by 2 at start of the trace window, ending at +1.
- **is this a real port gap or expected native-vs-substrate timing?** LIKELY EXPECTED. Native `Engine::fieldFrame` is called top-down from `native_scheduler_step`'s s48_2 dispatch; PSX substrate reaches the field-frame overlay via the scheduler coroutine at the same slot. One extra tick per boot is a common signature of a boot-transition frame differing between the two paths (native enters field one frame earlier or PSX yields for one extra frame during the mode transition). Not a gameplay bug on its own; just contributes 1 byte to the FULL diff.
- **volume driver (the actual FULL-mode work):** the `0x800F0000:3232`-byte page divergence is dominated by object-pool base-address differences visible in the pointer-chain span at `0x800ED554+` (the head slot of some object list). Every 4-byte pointer in the chain differs by a constant offset:
  * A base = `0x800F7734`, entries at +0x44 stride.
  * B base = `0x800F4D80`, entries at +0x44 stride.
  * Delta = 10676 bytes = 157 × 68-byte objects.
  A and B allocate the same object list at different arena positions — that's not a per-write bug; it's an ALLOCATOR divergence. Every pointer chain in the object system inherits that offset, which is how 1 root divergence blooms into ~3.2K bytes on this page. Naming the specific writer of `0x800ED554` (the pool head that seeds the chain) is the next PREWATCH target.
- **methodology confirmed:** the same PREWATCH primitive works for FULL mode. The `divergent-only pause` filter (mask≠3 OR different values) catches genuine asymmetric stores; the earliest hits at f1 and f9 are boot-init writes (mask=1 or mask=2, values 0=0) — these are timing-only noise from the two cores staggering through boot, not real divergences. Filter them by ignoring hits where `mWwVa == mWwVb == 0` and mask ≠ 3 (both cores wrote same value, just at different times — no real divergence). That's a small further improvement to the filter that would automatically clean up boot noise.
- **next step (concrete):** PREWATCH `0x800ED554`. When it fires, capture the native + substrate write sites for the head-of-list pointer; that names the object-list allocator on both sides. Compare — the semantic divergence is the difference in what each allocator returns for the same object type. Then port either (a) the native allocator to match the substrate's arena position (if we want binary-exact parity) or (b) accept that the offset is fine and add the 0x800ED554→bounded-chain range as a noise filter (native-vs-PSX allocator difference is EXPECTED just like scheduler-control blocks). Judgment call — depends on whether any code READS the specific object address as a literal (grep for `0x800F77` and `0x800F4D` fixed refs — if none, the offset is invisible to game logic).
- **refs:** game/scene/engine_stage.cpp:391-431 (Engine::fieldFrame), scratch/decomp/ram_f1000_all.c (writers), `$CLAUDE_JOB_DIR/tmp/full3.log` (session PSXPORT_WWATCH capture).

## `0x800BF81E` RENDER-mode divergence — writer is `DualviewSnapshot::restorePre` bulk memcpy rewinding `submitPage810c`'s gameplay-relevant writes (native-render decoupling by design) [FIXED 2026-07-03 — b9ccd2d]
- **symptom (2026-07-03):** Under `PSXPORT_SBS_MODE=render`, the first SBS-detected byte to differ at frame 217 is `0x800BF81E` (A=1, B=0), and it cascades into a ~5K bounded diff under FULL mode via `param_1[0x147] = DAT_800bf81f >> 4` (ram_f1000_all.c:44026). Both cores execute IDENTICAL `mem_w8` sequences (=1 from FUN_80056C00, =0 from overlay pc=0x8010810C) — yet A ends at 1 and B at 0. Writer bypasses `mem_w*` entirely.
- **status:** ROOT CAUSE CONFIRMED. Not a `mem_w8` writer at all — a `memcpy(c->ram, snapshot_ram, 0x200000)` in `DualviewSnapshot::restorePre` (runtime/recomp/dualview_snapshot.cpp:18) called from `Engine::fieldFrame` (game/scene/engine_stage.cpp:428). The flow inside fieldFrame:
  1. Gameplay update block (L395–402) — writes `0x800BF81E = 1` (FUN_80056C00 via `modePerFrameDispatch`).
  2. `dualviewSnapshot.capturePre(c)` (L406) — snapshots full guest RAM with byte=1.
  3. `mRender->frame()` (L407) — the native render orchestrator (or PSX substrate under `psxRender()`).
  4. `submitPage810c()` (L408) — the overlay-side render submit that ALSO clears `0x800BF81E = 0` (pc=0x8010810C in the wwatch trace).
  5. **Only under `!psxRender()`** (L427): `dualviewSnapshot.restorePre(c)` — bulk-memcpy restore of the full 2MB ram → byte flips 0→1.
  6. `postRenderTick` + `areaUpdateTail` — don't touch this byte.
  So on A (native render → step 5 runs), byte ends at 1. On B (PSX render → step 5 skipped), byte ends at 0.
- **evidence** (session logs @ `$CLAUDE_JOB_DIR/tmp/sbs14.log`+): every `mem_w*` trace on 0x800BF81E fires with `after=<value>` matching intended; wwatch shows both cores do `=1` then `=0` per frame; SBS trace shows A[0x800BF81E]=1 STABLE from f188 onward while B stays 0; `PSXPORT_SBS_FORCE_PSX_RENDER=1` (new knob) forces both cores through the L427 `!psxRender()` false branch → NO divergence anywhere (both stay at 0, no cascade). That knob was the definitive bisector: it isolated the writer to something guarded by `psxRender()`, and the fieldFrame path is the only such guard on that byte.
- **is this a REAL port gap?** Yes but subtle. `restorePre` is the "render must leave NO guest side effect" mechanism (docs comment in engine_stage.cpp: "the native renderer must leave NO guest-memory side effect — only native GAMEPLAY may write guest memory"). But `submitPage810c` is classified as render-side (L408, called between capturePre and restorePre), so its writes get rewound. The bug: those writes include `0x800BF81E = 0` which is READ by later gameplay logic via `param_1[0x147] = DAT_800bf81f >> 4` (ram_f1000_all.c:44026) — a real gameplay signal. Under native render (A), that read sees stale =1; under PSX render (B), it sees the intended =0. So native render's decoupling drops a gameplay-relevant clear. That's the port gap.
- **fix options (pick one):**
  1. **Move the clear OUT of submitPage810c into a native gameplay leaf** that runs BEFORE `capturePre` (line 406). Semantically the cleanest — separates the render-buffer-write parts from the gameplay-state-write parts of the overlay submit.
  2. **Move `capturePre` AFTER `submitPage810c`** (L408 → L406) so the snapshot includes the =0 clear. Simple but risks pulling in other post-submit render side effects into the snapshot.
  3. **Selectively include only render-buffer regions in the snapshot restore** — instead of a full 2MB memcpy, restore only the address ranges owned by the render pipeline. Requires enumerating those.
- **new SBS tooling in this session (runtime/recomp/sbs.cpp):** `PSXPORT_SBS_PREWATCH=<hex>` arms the write-watch at boot (before f217 pause); pause only on DIVERGENT stores (not same-value shared writes); per-store `[sbs-ww]` log with A/B attribution + pc + stage; `PSXPORT_SBS_FORCE_PSX_RENDER=1` forces both cores through PSX render (the culprit-bisecting knob). These land as the durable workflow-first output of the hunt.
- **refs:** game/scene/engine_stage.cpp:391–431 (Engine::fieldFrame full flow — capturePre / render / submit / restorePre), runtime/recomp/dualview_snapshot.cpp:13–18 (capturePre/restorePre memcpy), game/game_tomba2.cpp:108 (Engine::frameUpdate diff_mode early return — WHY frameUpdate isn't the location), scratch/decomp/ram_f1000_all.c:44026 (downstream reader that makes the byte gameplay-relevant), scratch/decomp/ram_f1000_all.c:37494/37567 (the `=1` writer FUN_80056C00). Previous incorrect hypothesis kept for history below.

## Native gameplay is nearly deterministic — first real port gap is `0x800BF81E` latch at f217 (SUPERSEDED)
- **symptom (2026-07-03):** Post Phase-25/26 SBS parity tooling landed, RENDER mode (both cores run native gameplay, one native render / one PSX render) shows the FIRST divergence at frame 217 at exactly `0x800BF81E..F` — identical to FULL mode's first divergence. Bounded RAM diff in RENDER: 356 bytes total = 355 on 0x801F0000 page (scheduler control blocks — matches earlier "differ by design" note) + **1 byte at `0x800BF81E`** on 0x800B0000. FULL mode oscillates ~5K bytes because that one seed byte cascades: `param_1[0x147] = DAT_800bf81f >> 4` (ram_f1000_all.c:44026 and neighbors) feeds many objects downstream.
- **status:** IDENTIFIED, ROOT CAUSE PARTIALLY NAMED. The seed of divergence is `game/scene/engine_stage.cpp:1233` — `if (c->mem_r8(0x800BF9C3u) & 0x80u) c->mem_w8(0x800BF81Eu, 1);`. Predicate `0x800BF9C3 & 0x80` evaluates differently between core A and core B at f217. Writer of `0x800BF9C3` is unknown from decomp grep alone (`scratch/decomp/ram_f1000_all.c` shows only the READ at line 36997 — the write goes through some struct-relative store Ghidra didn't associate as a DAT_ symbol). Since both RENDER cores run identical native gameplay, the differing predicate must trace back either to the 0x801F0000 scheduler-control page (timing-sensitive writer) or to a still-shared shared static missed by deglobalize Phases 17–24.
- **quick observation on RENDER-mode result:** native gameplay determinism is *essentially* achieved — 1 gameplay-state byte off vs "zero" hoped-for. Not the mass corruption feared. The "shared static contaminates SBS" concern that motivated the OOP arc is mostly resolved.
- **investigation 2026-07-03 (session continued):** Ran the interactive `sbs watch` + a new `PSXPORT_SBS_PREWATCH=<hex>` env-var (arm the SBS write-watch at boot, before f217) and per-store `[sbs-ww]` attribution logging (which core wrote which value, pc, stage). Findings:
  1. `0x800BF9C3` is NOT the divergent predicate the trace led us to blame — direct writes at `PSXPORT_WWATCH=0x800BF9C3,0x800BF9C4` never fire. The byte is only READ from decomp; its write path is elsewhere (indirect or overlay).
  2. Every frame f210..f217 both cores execute the IDENTICAL two-store sequence on `0x800BF81E`: `=1` from `pc=0x80056C00` (FUN_80056C00 — the per-object handler that runs `DAT_800bf81e=1` when `param_1[5]<2 && param_1[0x29]!=0`, decomp lines 37494/37567), then `=0` from `pc=0x8010810C` (an overlay function outside MAIN.EXE text). 102 stores each core. Same values, same pc, same order, same frame. YET at the divergence pause: A[0x800BF81E]=1, B[0x800BF81E]=0.
  3. Direction that survives (open): a write BYPASSES `mem_w*` on core A only. Candidates: (a) a per-frame native-render code path writes/leaks into `0x800BF81E` on A but not on B (native-render only runs on A in RENDER mode; B renders via PSX substrate), (b) a `memcpy` / direct `c->ram[]` store somewhere per-frame, (c) a DMA or GPU path modeled without going through `mem_w*`. Grep of `game/render/`, `runtime/recomp/gpu_native*.cpp`, `gpu_gpu.cpp` for `0x800BF81` = no hits. Bulk `memcpy(&c->ram[…], …)` sites are all boot/verify-gate, not per-frame.
  4. Native gameplay determinism IS confirmed via the identical mem_w8 sequences on both cores through f210..f217 — the port gap is on the RENDER side leaking into game state, not native gp.
- **next step (interactive, still concrete):** launch with `PSXPORT_SBS_PREWATCH=0x800BF81E` and set a much WIDER `PSXPORT_WWATCH=0x800BF800,0x800BF830` (byte-level per-store log across the whole neighborhood — pc-level attribution). If the mystery writer emerges near but not at 0x800BF81E, its address+pc will show up. If no store nearby fires either, the write is definitely via a memcpy or DMA path — grep `game/render/` and `runtime/recomp/gpu_*` for any `memcpy`/`memset`/`c->ram` per-frame, then instrument those. Alternative: adaptively temporarily instrument `mem_w8` to fprintf on the exact address (single-purpose logging, safer than picking apart the gp code).
- **refs:** game/scene/engine_stage.cpp:1190 (Engine::frameStartTick), engine.h:210 (step (f) contract), scratch/decomp/ram_f1000_all.c:36997 (only DAT_ read), scratch/decomp/ram_f1000_all.c:44026 (downstream `>> 4` reader — the cascade fanout), scratch/decomp/ram_f1000_all.c:37494/37567 (the `=1` writer FUN_80056C00). PREWATCH+per-store logging tooling: runtime/recomp/sbs.cpp `PSXPORT_SBS_PREWATCH`.

## SBS gameplay-mode panes look "impossibly different" — not a harness bug, that IS the gameplay divergence
- **symptom (USER 2026-07-02):** "SBS shows something no standalone flag combo reproduces; both panes have similar-but-not-same issues, impossible to describe." Concrete repro (post-deglobalize e395095, autonav to free-roam @f216, `PSXPORT_SBS_DUMP`): A pane (native gp, PSX render) shows a mostly BLACK frame with only the floating "Go to the Burning House" objective banner; B pane (PSX gp, PSX render) shows the full village scene.
- **status:** RESOLVED — SBS is doing exactly what it's designed to do. The reason no standalone combo reproduces the A pane visually: standalone with `PSXPORT_RENDER_PSX=1` renders solid black at f400 (its own separate plumbing gap), so there is no standalone route to "native gp state + PSX render pipeline + composite present" that SBS's per-pane readback provides. The A pane is a first-hand look at the wrong-SFX bug's guest-state footprint rendered through PSX.
- **evidence at f1672 gameplay via `sbs ramdiff`:** 2085 RAM spans / 5595 B diverge between A (native gp) and B (PSX gp) in the widened 0x80010000..0x80200000 region + 12 B in scratchpad. Real, large gameplay divergence — same class of native-vs-recomp divergence that produces wrong SFX under native. Not a harness artifact.
- **what still HAS to be true for SBS to be trustworthy:** each core's per-instance HW state must not leak into the other's frame. As of e395095 the per-Core bind list is: `gte_bind` (GTE regfile), `projprim_bind` (native depth cache — retired 2026-07-03; was the last known cross-core-shared render state called out in e5d554b), `spu_bind`, `mdec_bind`, `cdc_bind`, `xa_bind`. No known remaining render-side shared statics.
- **known separate issue (not this):** standalone `PSXPORT_RENDER_PSX=1` renders black at f400 even with `PSXPORT_AUTO_SKIP=1` reaching free-roam. Plumbing likely stale; low priority because SBS's B pane shows the full scene via psx_fallback, which is the actually-used PSX render route.
- **refs:** runtime/recomp/sbs.cpp:227 (apply_mode gameplay = PSX render on both), runtime/recomp/scheduler.cpp:114 (native_content gate), runtime/recomp/projprim_state.h + gte_beetle.cpp (per-Core depth cache, e395095), scratch/screenshots/sbs_gameplay_pp.png (reproduction).

## SBS DEMO attract-path crash — hits an unrecompiled overlay function 0x80109F7C
- **symptom:** with SBS running headless and no autonav (`PSXPORT_SBS=1 PSXPORT_SBS_MODE=<any>`, no `PSXPORT_SBS_AUTONAV=1`), letting DEMO idle to its attract timeout aborts core A with `rec-MISS 0x80109F7C` at `demo_frame_s7` → `ov_demo_gen_80106E28` → `gen_func_80059D28` → `gen_func_8005950C` → `gen_func_80058648` → `rec_dispatch_miss`. Standalone at 1500 frames of DEMO idle never enters the attract path (`sm[0x48]` stays at 2), so this is SBS-scoped.
- **status:** OPEN, LOW PRIORITY (dodged by the normal SBS use path — `PSXPORT_SBS_AUTONAV=1` drives out of DEMO into GAME before the attract timer fires). Not the "both panes look weird" bug.
- **cause hypothesis:** SBS's `step_core` sets `0x1f80019d=1` (the Start-skip flag) every frame the DEMO SM is in intro-FMV sub-state 1, cycling DEMO substates faster than standalone → reaches `demo_frame_s7` phase-1 → `rec_dispatch(0x80106e28u)` → chains into overlay code (a0b/a0i/etc) that is NOT recompiled into the resident set. Interpreter is gone → fail-fast miss BY DESIGN. Fix would be seeding those attract-path functions into the recomp set OR having Demo::frame's s7 stay native through phase-1 without crossing into unrecompiled overlay.
- **refs:** game/scene/engine_demo.cpp:635 (demo_frame_s7), runtime/recomp/sbs.cpp:295 (intro-FMV skip flag).

## SBS gameplay-mode "both sides look identical / no divergence" — SUPERSEDED by the block above
The old "both panes identical" symptom is not reproducible on the current tip (e395095) — the diff region was widened to full main RAM (2 MB) in this session, and the per-Core deglobalize (d78086c..a147219..e395095) removed the shared render/timing/repl statics that could have masked divergence. Panes now show clear visual divergence AND `sbs ramdiff` reports thousands of RAM spans. Kept below for history.

## SBS gameplay-mode "both sides look identical / no divergence" — investigation, PARTIAL step, root cause NOT yet pinned
- **symptom:** `PSXPORT_SBS_MODE=gameplay` shows the two panes as visually identical, and no divergence surfaces on the debug server, even in cases where native gameplay demonstrably perturbs behavior (e.g. wrong SFX under native, correct under `PSXPORT_GATE=1`). USER read (2026-07-02): "both cores run the same code, and same renderer."
- **status:** OPEN. One necessary widening applied (below). Sufficiency NOT verified.
- **code reading (what SHOULD be different between A and B in gameplay mode):**
  - `sbs.cpp:504-505` sets `g_a->psx_fallback=0, g_b->psx_fallback=1`.
  - `apply_mode(M_GAMEPLAY)` forces `g_render_psx=1` on both (SAME renderer BY DESIGN — that's the point of isolating gameplay).
  - `scheduler.cpp:114` reads `native_content = !psx_fallback` → on B skips the native DEMO/GAME/SOP dispatchers and routes stage bodies through recomp coroutines.
  - `Engine::frameUpdate` (`game_tomba2.cpp:113`) runs on BOTH cores; in `diff_mode` it just calls `rec_dispatch(0x800788AC)` (per-frame state-update leaf, same on both) + `spu_audio_frame_logic()`, then returns before host output. So the frame-body is recomp on both; only the SCHEDULED STAGE BODIES differ (A native, B recomp).
  - Per memory [[byte-perfect-ab-is-baseline-not-truth]] and countless "0-diff verified" per-native notes, every promoted native was gated on a bit-exact A/B vs `rec_super_call`. If ALL covered natives are bit-exact, RAM at frame boundaries IS identical between A and B — meaning "same panes / no divergence" is the CORRECT current output, not a harness bug. This would mean SBS's failure to surface the wrong-SFX perturbation isn't a bug in psx_fallback gating — it's that the perturbation lives outside what SBS diffs.
- **what SBS does NOT diff:** SPU register state, VRAM, per-frame transient scratchpad within a frame (only frame-boundary snapshot is compared). If a native subsystem calls a libsnd/SPU leaf via `rec_dispatch` with wrong args, the SPU registers diverge but guest RAM does not — SBS can't see it.
- **necessary but insufficient fix (2026-07-02):** default diff region was `0x800B0000..0x80110000` (384 KB, sbs.cpp:121) — covered core engine data but MISSED CD-loaded content > 0x80110000 including area bundles at 0x80182000 (`game/audio/music_list.h:29`) carrying per-area VAB/tone bindings. Widened to `0x80010000..0x80200000` (~2 MB, full main RAM). This helps if the perturbation writes into content memory; does NOT help if it's SPU/VRAM/transient-only.
- **next step to actually PIN the "both sides same" issue:** run widened SBS gameplay-mode on a known-broken scene (fisherman splash SFX, Tomba land SFX). If a divergence NOW surfaces, the earlier default was the whole story and we're done. If STILL no divergence, add SPU-state diffing (per-instance SPU state exists via `spu_bind`, but not compared at frame boundary) — write the SPU register+voice snapshot into the SBS check_divergence path.
- **refs:** wrong-SFX investigation 2026-07-02, `runtime/recomp/sbs.cpp:504-505` (psx_fallback per core), `runtime/recomp/scheduler.cpp:114` (native_content gate), `game/game_tomba2.cpp:113` (Engine::frameUpdate diff_mode branch), `runtime/recomp/spu_beetle.c` (per-instance SPU — candidate for the extra diff).

## SBS both cores freeze after Start — diff_mode skips the SPU/XA advance → intro-cutscene XA clip wait never clears
- **symptom:** `PSXPORT_SBS_MODE=both` — after reaching the field, BOTH panes freeze: core A (native) renders the field but static, core B (full-PSX) black/stuck ("everything frozen, music keeps playing"). Headless: core B `0x801fe0e0`(task-2 state)=2 forever, GAME loop counter `0x1f800198` stuck ~34. The leading "SBS loop hangs on core B's `step_core`/`co->resume()`" hypothesis is FALSE — the lockstep loop iterates fine (core A advances to counter 3532 in autonav); core B is genuinely stuck at a wait, still yielding each frame.
- **status:** PARTIAL — XA clip wait FIXED (later-271); a DEEPER recomp-coro register-corruption freeze remains (next block).
- **cause:** in SBS both cores set `diff_mode=1`, and `ov_frame_update` (game_tomba2.cpp) early-returned on `diff_mode` BEFORE calling `spu_audio_frame()`. So the per-core XA stream never advanced → `xa_stream_voice_busy()` stayed true → task-2 (`0x801fe0e0`) never cleared → the intro-area cutscene `while(*(0x801fe0e0)!=0)` wait never cleared → freeze. The later-270 fix (advance XA when active even headless) was entirely bypassed in SBS because diff_mode returns first. This is the SAME audio-gated wait as later-270, surfaced via the SBS/diff_mode path.
- **fix (later-271):** added `spu_audio_frame_logic()` = `spu_audio_frame_ex(0)` (spu_audio.c): advances spu_update + drains spu_render (driving `CDC_GetCDAudioSample` → XA decode) but suppresses ALL output (no SDL device feed, no WAV, no native-music mix). `ov_frame_update`'s diff_mode branch now calls it `quota`× before returning. Per-core SPU/XA bind makes it safe; the two cores share the one output device so neither feeds it (no double audio). Verified: core B task-2 `0x801fe0e0` now clears 2→0 (clip completes).
- **refs:** runtime/recomp/spu_audio.c (spu_audio_frame_ex/spu_audio_frame_logic), engine/game_tomba2.cpp (ov_frame_update diff_mode branch), [[Headless audio-gated game LOGIC hangs]], later-271

## SBS core-B (full-PSX) field FREEZES deeper — recompiler mis-emitted FUN_8003c048's in-function jump-table `jr` (skips epilogue → corrupts the GAME-loop's callee-saved s0/SP)
- **symptom:** after the later-271 XA fix clears the clip wait, core B's field STILL freezes (genuinely stuck: ~4 bytes of main RAM change over 30s, all housekeeping counters; scratchpad 0-diff; `0x1f800198` stuck at 34; field SM `0x801fe048` byte-identical over time). The SHIPPING NATIVE core (A) runs the field fine.
- **status:** FIXED (later-272) — recompiler jump-table recovery + cross-boundary switch-target seeding. Core B's GAME loop counter now advances (3791→3953 over 10s; was stuck 34); selftest asserts sustained field progress.
- **root cause (the corruption):** the per-frame entity-update dispatcher FUN_8003c048 (`ov_render_walk`) does an IN-FUNCTION jump-table `jr v0` (0x8003C0AC, table 0x80014db8) to its 33 entity-type handlers (in-function labels that loop back, sharing the function's epilogue). The recompiler FAILED to recover that table — its base reg is built `lui v0,0x8001 ; addiu s3,v0,0x4db8` (the addiu's src != dst, a separate-temp form `find_jump_tables` didn't handle) AND v0 is reused by a nearer scratch `lui v0,0x1f80` that mis-matched as the table HI. So the `jr` fell back to `rec_dispatch(c, target); return;` — which SKIPS the function epilogue (the `r29 += 112` + s0–s3 restore). Dispatching the first entity (the area's terrain actor, ~loop iter 36) therefore leaked 112 (0x70) bytes of guest SP and lost callee-saved s0–s3 → the GAME-loop coro's base reg s0 (0x1f800000) corrupted to 1 → its counter write `*(s0+0x198)` diverted to main RAM `0x80000198` (the "alive" RAM-diff bytes — a red herring) and the sm[0x48] dispatch read garbage → field spun dead. Pinned via the `btyield` C-level fiber backtrace + the guest SP shifting exactly one 0x70 frame at the corrupting yield.
- **fix (later-272, recompiler, TDD):** (1) `find_jump_tables` (emit.py) now tries the STRICT idiom first and only falls back to an ENHANCED scan (separate-temp base reg `lui tmp,HI; addiu base,tmp,LO` + dropping the scaled-index reg so a reused scratch `lui` isn't mismatched) for jrs the strict logic MISSED — strict-first is essential so already-correct recoveries are byte-identical (an unconditional enhanced pass corrupted A00 FUN_80124328's switch). Unit tests `test_jumptable_base_built_in_separate_reg` (RED without the enhanced fallback). (2) emit_module now SEEDS cross-boundary switch targets: a jump-table case that lands OUTSIDE the jr's own function (a sibling pointer-table fn splits the switch, so later cases fall in its body) is reached via `default: rec_dispatch` and must be a function entry — else recomp-MISS the moment the runtime index selects it (A00 jr 0x80124354 → 0x80124448/0x80124488 inside fn 0x801243E8). This was always latent; the FUN_8003c048 fix merely let the field progress far enough to hit it (111 seeded in MAIN + 20+ across area overlays). (3) TDD gate: selftest `startgame` now asserts the GAME loop counter keeps advancing >50 over 600 frames after the intro clip (RED before the fix, stuck at 34; GREEN after, +600).
- **refs:** tools/recomp/emit.py (_scan_jt_idiom strict/enhanced, find_jump_tables strict-first, emit_module cross-boundary seeding), tools/recomp/test_emit.py (test_jumptable_base_built_in_separate_reg), runtime/recomp/selftest.cpp (sustained-progress assertion), generated/shard_6.c (gen_func_8003C048 switch), later-272

## (historical) the symptom investigation that led to later-272
- **status:** SUPERSEDED by the FIXED block above — kept for the diagnostic trail.
- **cause (precise):** the GAME-stage coro (slot 0, body `ov_game_gen_801063F4` reached via the 0x8010637C prologue) is supposed to loop {dispatch sm[0x48] handler; bump `*(s0+0x198)`; yield} with s0=s1=`0x1f800000`, s2=1 (set ONCE by the prologue — the loop never re-seeds them, unlike the NATIVE GAME-coop path which force-resets r16/r17/r18 every frame at re-entry, native_boot.cpp ~400). A precise C-level backtrace of the fiber thread at the yield (PSXPORT_DEBUG=btyield; `backtrace()` in ov_switch's coro branch) shows: for the first ~57 loop iterations r16=`0x1F800000` (correct), then r16 becomes `1` and r17 becomes `0xAC6` (and r18=`0xB8A`, r19=`0x80014DB8` = the entity-update jump table from FUN_8003c048) and STAYS corrupt forever (3669/3729 yields). With r16=1 the sm[0x48] dispatch reads `*(r17+312)`=garbage → matches no case → NO handler runs → field frozen; and the counter bump `*(r16+0x198)`=`*(0x199)` writes to main RAM `0x80000198` instead of scratchpad `0x1f800198` (EXACTLY the 0x80000198/0x80000199 bytes the RAM-diff shows changing — that's the "alive" counter, a red herring). So a deep field-handler callee (FUN_8003c048 entity update / the cutscene path) corrupts the loop's callee-saved s0/s1 across a coro yield and they are never restored on the return to the loop. This is a recomp-coro callee-saved-register / guest-stack fidelity bug: the continuous fiber loop accumulates corruption the native per-frame-re-seed model is immune to.
- **NOT the cause / ruled out:** (a) SBS lockstep loop hanging on step_core — false, loop iterates. (b) The 0x800bf839 flag the field sub-state-2 handler 0x80109628 checks — poking it to 1 does NOT advance the field, because the corrupt dispatch never reaches that handler. (c) Two stage coros alive — the 8 `ov_start_gen_8010649C` backtraces are from boot (slot 0 = START then GAME), temporal, not concurrent.
- **the corruption mechanism (SHARP, btyield SP evidence):** the corruption is a one-time event at GAME-loop yield #36 (the first frame the field reaches its full sub-state-2 path). Comparing the last-good yield #35 to #36:
  - #35 (good): r16=`0x1F800000` r17=`0x1F800000`, guest **r29=`0x801FE9C8`**, SHALLOW guest stack (only `0x801065E4`/`0x80106470` = the GAME loop frame).
  - #36 (corrupt): r16=`1` r17=`0xAC6` r18=`0xB8A` r19=`0x80014DB8`, guest **r29=`0x801FE958`** (exactly **0x70 = one TASK STRIDE** lower), DEEP guest stack still present (`0x801093B0`→`0x80109630`→`0x801094E0`→`0x801088B0`→`0x8007A8F0`→`0x80052098`→`0x801087CC`→`0x801065E4`→`0x8010645C`).
  - The C backtrace at #36 is SHALLOW (thread_main→ov_game_gen_801063F4→yield). So the field chain RETURNED in C (C stack unwound to the loop), but left the GUEST SP (r29) and callee-saved s0–s3 at the deep callee's values — i.e. a recomp function in the field chain returned WITHOUT restoring guest r29/s0–s3.
  - The chain runs THROUGH `FUN_80052078` (`0x80052098` on the guest stack = the COOPERATIVE TASK DISPATCHER): the GAME stage's sm[0x48] handler (0x80108784 @ 0x801087CC) calls FUN_80052078, which dispatches the field-mode task NESTED, which runs the entity update → yields. So this is a NESTED cooperative-task dispatch under the coro: the guest SP shifts one task stride for the nested task, and on the way back the outer GAME loop's r29/s0–s3 are not restored. This is the crux of "full-PSX field doesn't run under coroutines."
- **leads for the fix (next session):** (1) find the recomp function in the chain (FUN_80052078 / 0x801087CC / 0x80108784) whose nested-task-dispatch path returns without restoring the caller's guest r29/s0–s3 — likely the recomp/coro handling of a NESTED FUN_80052078 dispatch needs to save+restore the full register context (r29 + s0–s7) around the nested task, the way the top-level scheduler does via task_ctx. (2) The native GAME-coop path is immune because it re-seeds r16/r17/r18 AND re-enters at the loop top each frame (native_boot.cpp ~400); a STOPGAP (not a real fix) would be to do the same for the coro path, but the real bug is the nested-dispatch context save/restore. Catch the exact failing epilogue by single-frame-stepping yield #35→#36 with btyield.
- **refs:** runtime/recomp/native_boot.cpp (ov_switch btyield C-backtrace, GAME-coop re-seed ~400, coro path), generated/ov_game_shard_1.c (ov_game_gen_801063F4 loop: bump *(r16+0x198) + rec_dispatch 0x80051F80), engine/engine_stage.cpp (ov_game_func_80108784 field dispatcher), tools/disas.py + capstone-on-dump for the 0x80109xxx field overlay

## SBS panes render BLACK for the field (both panes all-black / region-nonzero=0)
- **symptom:** `PSXPORT_SBS_MODE=both` dump at free-roam is all black; `PSXPORT_GPU_TRACE=1` readback shows `region-nonzero=0/76800 ... batch tri=0 tex=0 worldquads=1109` — the native core EMITS 1109 world quads but the geometry batch is empty at `grab_pane`.
- **status:** fixed (render_queue.cpp flush diff_mode gate)
- **cause:** `psxport_settings.ini` has `fps60=1`, so `g_fps60_on` is set. `RenderQueue::flush` (render_queue.cpp) diverted the sorted queue to `fps60.rq_capture()` and RETURNED without the inline `gpu_emit_rq_item` loop — on the assumption that `fps60_present_vk` would emit+present both interpolated halves later. But under SBS both cores set `diff_mode=1`, and `ov_frame_update` (game_tomba2.cpp:129) early-returns on `diff_mode` BEFORE `fps60_frame_commit`/present — so the captured queue was NEVER drawn. Net: world quads queued, fps60 captured them, nothing emitted them to the batch, `gpu_gpu_render_readback` rendered an empty batch = black pane. (Matches the trace exactly: worldquads>0, tex=0.)
- **fix:** gate the fps60 capture on `!core->game->diff_mode` (render_queue.cpp flush). In diff_mode the per-core present is suppressed and the SBS composite reads the geometry batch directly via `gpu_gpu_render_readback`, so the flush must do the inline emit. Verified: both panes now full (native region-nonzero=76800/76800 tex=11382; PSX 76796/76800 tex=4497); scratch/screenshots/sbs.png.
- **refs:** engine/render_queue.cpp flush (g_fps60_on && !diff_mode); engine/game_tomba2.cpp:129 (diff_mode early-return); runtime/recomp/sbs.cpp grab_pane; scratch/screenshots/sbs.png

## Full-PSX SBS modes (gameplay/both) fail-fast at a scheduler yield-return (post-interpreter)
- **symptom:** `PSXPORT_SBS_MODE=both` (or `=gameplay`) aborts at frame 0 with `[recomp-MISS 0] no recompiled fn for 0x80051FA4 (caller ra=0x80051FA4, a0=0xFF000000)`. `mode=render` is fine (runs indefinitely).
- **status:** fixed (later-264) — full-PSX runs recompiler-only via thread-fiber coroutines + load-time overlay-identity routing; both cores boot START→DEMO→GAME with 0 recomp-MISS, no crash/hang.
- **cause:** `mode=both`/`mode=gameplay` run core B as FULL PSX (`psx_fallback`): the cooperative tasks run under the recompiled substrate via the generic path `rec_coro_run(c, resume_pc)`. Since the interpreter removal (later-254) `rec_coro_run` is just `rec_dispatch` (dispatch.cpp:29) — it can only ENTER a recompiled function at its TOP. A yielded task resumes at its saved `r31` which is a MID-FUNCTION PC (0x80051FA4 = the `lw ra,16(sp)` right after the yield in the task-switch primitive FUN_80051F80, which stores the scheduler state at 0x1F800138). No recompiled entry exists there → fail-fast.
- **fix (later-264, USER directive "recompiler-only; condvars with pause-resume"):** each full-PSX cooperative task runs on its OWN fiber thread (`Coro`, runtime/recomp/coro.{h,cpp}) that BLOCKS on a condvar at a yield — the whole nested C call stack is preserved, so resume CONTINUES exactly there, recompiler-only, no interpreter. Strict ping-pong so the shared `Core::r[]` is save/restored around the handoff like the old longjmp scheduler. `ov_switch` coro-yields (or `Coro::exit_now` on task-end) when `cur_is_coro`, else longjmps. `Coro::cancel` unwinds a fiber blocked mid-yield before destruction (else `~Coro` destroys a condvar with a live waiter → hang/abort). Native path UNTOUCHED (coros fire only in psx_fallback). Also fixed two secondary blockers surfaced by the full-PSX miss-loop: (a) stage/task entries owned natively (DEMO 0x801062E4, GAME 0x8010637C, START 0x8010649C, sub-task 0x8004514C) were unseeded → seeded in emit.py; (b) the overlay router identified the resident overlay by a content signature = the raw .BIN's first bytes, but the game MUTATES its header pointer table at runtime (GAME +0x08 swapped 0x80106510→0x80106470) so the live bytes no longer matched → 0x801063F4 mis-routed → MISS. Fixed by recording the resident overlay at LOAD time (`overlay_note_load`, when the fresh image still matches its signature) into a per-core `SchedulerState::resident_ov[slot]`; the router routes by that IDENTITY, robust to later mutation. See [[overlay-router-resident-by-load-identity]].
- **refs:** runtime/recomp/coro.{h,cpp} + test_coro.cpp; runtime/recomp/native_boot.cpp (ov_switch, the coro path in native_scheduler_step); runtime/recomp/overlay_router.cpp (overlay_note_load + resident_overlay); runtime/recomp/cd_override.cpp (loaders call overlay_note_load); tools/recomp/emit.py EXTRA_SEEDS/OVERLAY_EXTRA_SEEDS; journal later-264

## Headless audio-gated game LOGIC hangs — SPU/XA stream only advanced when an output consumer exists
- **symptom:** headless (no audio device, no WAV) — a field/cutscene that starts an XA voice/BGM clip and waits `while(*(0x801fe0e0)!=0)` for it to finish NEVER advances; the clip's read head never moves so the wait can't clear. Looks like a freeze ("frozen, music would be playing").
- **status:** fixed (later-270)
- **cause:** `xa_decode_next_sector` (xa_stream.c) is driven ONLY by `CDC_GetCDAudioSample`, called per output sample from `spu_update` inside `spu_audio_frame`. But `spu_audio_frame` early-returned when neither the SDL device nor a WAV capture was active (headless) — so `spu_update`/`CDC_GetCDAudioSample` never ran, the XA read head never advanced past the clip's end LBA, `s_active` never cleared, and any game LOGIC blocked on clip completion hung. Output gating was conflated with stream-clock gating.
- **fix:** `spu_audio_frame` now also advances the SPU+XA when `xa_stream_is_active()` (a clip is streaming that logic may wait on), discarding the rendered samples; OUTPUT stays gated (SDL queue / WAV write only when their consumer is active). One spu_update == one video frame == correct realtime-equivalent pacing, so the clip can't over-advance. Windowed/shipping runs always have a consumer, so they're unchanged. Verified: the intro voice clip now PLAYS then COMPLETES headless (selftest `saw_clip`+`clip_ended`); native AUTO_SKIP + SBS both still clean.
- **refs:** runtime/recomp/spu_audio.c (spu_audio_frame consumer gate + xa_stream_is_active decl), runtime/recomp/xa_stream.c (CDC_GetCDAudioSample drives decode), later-270

## Full-PSX field-mode does NOT fully run under coroutines past the intro cutscene (deeper limit)
- **symptom:** after the prologue-split + SPU-advance fixes, the full-PSX (psx_fallback / SBS core B) field reaches free-roam, runs its loop, plays + completes the intro XA clip — but then the GAME outer loop counter *(0x1F800198) stops advancing (stuck ~34); the coro keeps yielding but the field-mode (sm[0x48]=2, sm[0x4a]=0 submode0, sm[0x4e]=1 → overlay 0x80109450) doesn't progress.
- **status:** known-issue (full-PSX coroutine-resume limit; the SHIPPING game is the NATIVE path, which runs the field fine — 4000 frames clean)
- **cause:** UNRESOLVED — the recompiled field-mode runs deep cooperative-wait/yield chains under the coro; a yield observed mid-callee shows s0(r16)=1 (a callee local, not the 0x1F800000 base), i.e. the field-mode is parked deep in a sub-wait, not at its outer loop. Whether this is a missing native HLE the recomp field expects, a real recomp-coro register/stack fidelity bug, or simply more audio/resource-gated waits is NOT yet determined. This is the diagnostic full-PSX path; do NOT block the shipping native game on it.
- **fix:** none yet. Next session: from the selftest freeze, capture the GUEST BACKTRACE of the GAME coro (not the unreliable `mem_r32(sp+16)` waitloop heuristic — it reads stale stack on inner frames) to find the exact wait loop + condition; compare to what the native field-mode (ov_sop_field_mode) does at that point. Diagnostics: `PSXPORT_DEBUG=sched,yieldpc` (ov_switch logs ra/waitloop/r16); `PSXPORT_SELFTEST=startgame PSXPORT_SELFTEST_VERBOSE=1`.
- **refs:** runtime/recomp/selftest.cpp, engine/engine_stage.cpp (ov_game_submode0 / 0x80109450 field-mode), runtime/recomp/native_boot.cpp (coro path), later-270

## Full-PSX (SBS core-B / psx_fallback) FREEZES after Start — recompiler SPLITS the GAME stage fn (prologue doesn't flow into its loop)
- **symptom:** full-PSX path (PSXPORT_SBS_MODE=both core B, or PSXPORT_SELFTEST=startgame) — mashing Start at the title DOES start the game (reaches GAME stage 0x8010637C) but the field then FREEZES: the GAME field state machine never advances (sm[0x48] stuck at 0, never reaches free-roam 2). sched log shows `slot 0 coro start st=3 entry=0x8010637C` exactly ONCE, then nothing — the coro is `done=1` after one resume with no `ov_switch` yield.
- **status:** FIXED (later-269) — recompiler emits a fall-through tail-call at deliberate re-entry seeds.
- **cause:** the GAME stage fn 0x8010637C is a cooperative task: a PROLOGUE (0x8010637C..0x801063F0, sets sm from *(0x1F800134), clears fields) that FALLS THROUGH into its infinite LOOP at 0x801063F4 (dispatch sm[0x48] handler → bump *(0x1F800198) → `jal 0x80051f80` yield → `j 0x801063f4`). Both 0x8010637C AND 0x801063F4 are seeded as separate functions (OVERLAY_EXTRA_SEEDS["GAME"]) because the native game_coop path re-enters the loop top per frame. But emit.py emitted a bare `return;` at every function's end (emit_func) — so func_8010637C (cut at the 0x801063F4 boundary) RETURNED after the prologue instead of flowing into the loop. On the full-PSX coro, rec_coro_run(0x8010637C) ran the prologue, returned (done=1), and the scheduler reaped the task as finished (native_boot.cpp coro path line ~366) → the GAME stage never ran its loop → field frozen at sm[0x48]=0. (Native path didn't hit this: it runs ov_game_frame / re-enters 0x801063F4 directly, never rec_coro_run'ing 0x8010637C as a blocking coro.)
- **fix:** emit_func now emits a TAIL-CALL to `hi` (the next fn) instead of `return` when the body FALLS THROUGH (last insn not an unconditional `j`/`jr`) AND `hi` is a deliberate re-entry seed (`reentry` = OVERLAY_EXTRA_SEEDS for that overlay). So func_8010637C ends `ov_game_func_801063F4(c); return;` → the prologue flows into the loop; the loop blocks at ov_switch (true mid-fn coro resume) and never returns. SCOPED to re-entry seeds on purpose: a global fall-through tail-call regressed the native field (executed previously-dead fall-through regions in area overlays that `jal` undiscovered fns → recomp-MISS 0x80124448); the discovery scan doesn't seed those, so only the documented stage-split seeds get the tail-call.
- **TDD gate:** `PSXPORT_SELFTEST=startgame` (runtime/recomp/selftest.cpp) boots one psx_fallback core, mashes Start, asserts it reaches GAME free-roam (sm[0x48]==2) AND the GAME loop runs (*(0x1F800198) advances). RED before the fix (stuck sm=0), GREEN after. NOTE: it does NOT assert the loop keeps advancing forever — entering the first area starts a long one-shot voice clip and the field's `while(*(0x801fe0e0)!=0)` wait legitimately pauses the loop until the clip ENDS, and clip progress is driven by REAL-TIME audio consumption (CDC_GetCDAudioSample), which headless logic-frames outrun. That audio-gated cutscene is NOT a freeze (with audio it resolves: clip→end_lba→s_active=0→task2 state 0→wait clears). A headless fast-forward XA pump would let the test run the whole cutscene deterministically — future workflow improvement.
- **refs:** tools/recomp/emit.py (emit_func body_falls_through + reentry param; OVERLAY_EXTRA_SEEDS), generated/ov_game_shard_0.c (ov_game_gen_8010637C → ov_game_func_801063F4), runtime/recomp/native_boot.cpp (coro path ~328-373, ov_switch ~116), runtime/recomp/selftest.cpp, runtime/recomp/xa_stream.c (CDC_GetCDAudioSample drives the clip), later-269

## SBS core-B (full-PSX recomp) FREEZES at the title — dead libetc VSync counter (timebase)
- **symptom:** `PSXPORT_SBS_MODE=both` — core B (full PSX) sits frozen at the Tomba!2 title; core A (native) does not. sched log loops `slot 0 coro resume 0x801062E4` + `slot 1 coro start 0x80044F58` forever, never transitioning.
- **status:** PARTIAL — vblank counter fixed (later-268, commit 106dcda); menu-state divergence still UNCONFIRMED/open
- **cause:** DEAD TIMEBASE (confirmed): the libetc VSync counter `DAT_800abde0` was never advanced — `ov_vsync`/`frame_tick` in timing.cpp are dead code (VSync 0x80085900 is TRAPPED by sync_overrides, so they never run), and nothing else bumped `0x800abde0`. It sat at 0 forever (verified via debug server `r 0x800abde0` on both cores = `00 00 00 00` indefinitely). Native paths reimplement their own pacing and ignore it, but RECOMP code (full-PSX core B, and any still-recomp leaf) reads `0x800abde0` to pace animations/timers → frozen in place.
- **fix:** added `timing_frame_tick(Core*)` (timing.cpp) — bumps `c->game->timing.vblank` and writes `0x800abde0` once per native frame; called at the top of `native_step_frame` (native_boot.cpp) for ALL cores so the recomp timebase advances. Verified: `0x800abde0` now increments (was stuck 0); native plain + AUTO_SKIP field gates still 0-miss; emit 12/12 + test_coro pass; SBS both no crash.
- **2nd candidate (REAL, needs windowed confirm it's THE visible freeze): s7 attract phase=7.** Core B's demo correctly idles to the attract substate `sm[0x48]=7` (s7) but its phase index `sm[0x4a]=7` is INVALID — the s7 phase machine 0x80106C24 expects phase ∈ {0,1,2}; 7 falls through to the do-nothing epilogue → the attract demo NEVER launches → the title sits as a static (frozen-looking) image forever. This is a coro-resume fidelity divergence in the recompiled menu sub-machine, separate from the timebase. **Verified Linux-side via state** (no render needed): the DEMO task is pinned at `0x801FE000` (entry 0x801062E4), so `r 0x801FE048`=sm[0x48]=07 and `0x801FE04A`=sm[0x4a]=07 on core B (`sbs show b` then `r`/`rw`). NOTE on reading the SM: use the FIXED task base `0x801FE000` for the demo — do NOT follow `*(0x1f800138)`, which flips to the per-frame loader task `0x801FE070` (entry 0x80044F58) when that slot is the current task. The debug server CAN read scratchpad (host_ptr maps 0x1F800000..0x1F800400 → scratch[]); the live `r`/`rw` are scratchpad-aware (only `dumpram`'s main-RAM dump needs the `.spad` sidecar).
- **open for next session:** (a) root-cause WHY sm[0x4a]=7 on the coro path (native s2→s7 resets sm[0x4a]=0; the recomp transition apparently doesn't, or a stale 7 survives). A write-watch on `0x801FE048` must be armed at/near boot (the s7 transition happens within ~4s) — arm it from frame 0, not after a sleep. (b) confirm windowed whether the vblank fix + an sm[0x4a] fix actually unfreezes the title (headless SBS renders BLACK at the menu).
- **refs:** runtime/recomp/timing.cpp (timing_frame_tick, dead ov_vsync/frame_tick), runtime/recomp/native_boot.cpp native_step_frame, runtime/recomp/sync_overrides.cpp ov_vsync_trap (0x80085900), engine/engine_demo.cpp (s7 phase machine 0x80106C24), scratch/handoff_two_freezes.md, later-268

## recomp-MISS 0x80146478 in narration cutscene — PSX core never loads the A00 AREA CODE overlay
- **symptom:** `PSXPORT_SBS_MODE=both` (windowed user repro: Start ×2 in the main menu → narration cutscene → "freezes" a few seconds in). Process aborts with `[recomp-MISS 0] no recompiled fn for 0x80146478 (caller ra=0x8003F6F0, a0=0x8017C858)`. The new miss diagnostic prints `resident overlay for this slot = SOP` and `[miss-state] stage=0x8010637C sm[0x50]=2 1f80019b=1` (load marked DONE).
- **status:** FIXED (later-274) — the 0x80146478 miss no longer occurs; the narration now advances to a SEPARATE, later miss `0x8010BF54` (a SOP-overlay routing issue, tracked below). This is a SHIPPING-game bug, NOT SBS-specific: the native single-core game (`PSXPORT_REPL=1`, `newgame` then `run 2500` with NO Start) reproduced the same miss. AUTO_SKIP hid it by SKIPPING the intro (pressing Start ends the narration before the offending render runs).
- **the actual trigger (live backtrace, corrects the 0x8010A0E0 prediction below):** the abort backtrace was `ov_draw_otag → ov_scene_native → gen_func_8003C048 → gen_func_8003CCA4 → gen_func_8003CDD8 → gen_func_8003F698 → MISS`. So it was reached from the NATIVE `ov_scene_native` calling the NATIVE `ov_render_walk` — but `submit_render_walk` (engine_render_walk.cpp) FELL BACK to `rec_super_call(0x8003C048)` because the narration scene has a render-list node of TYPE 16 (jump-table tgt 0x8003C148), which the native walk did not own. The whole-body super-call then ran the recompiled per-object chain (8003CCA4→8003CDD8→per-area dispatch 8003F698→A00's 0x80146478, not resident) → miss. The native per-object path (`submit_perobj_flush → native_gt3gt4`) bypasses 8003F698 entirely and would never miss; the only thing forcing PSX was the one unowned node type.
- **cause (corrected — supersedes the earlier "quick-path skipped the A00 load" theory):** A00 is legitimately NOT loaded during the intro narration, and that is CORRECT — the SOP intro's area-load body (real LAB_80109164, faithfully = native_sop_area_load) loads area DATA only (to 0x8018A000), never the A00 CODE overlay; the code overlay loads LATER, in the field sub-mode (sm[0x4a]==1, ov_game_submode1 → native_transition_area_load → ov_80045080). During the intro (sm[0x4a]==0, SOP signature 0x3C021F80 present, so the NATIVE ov_sop_field_update DOES run), ov_sop_field_update rec_dispatches the still-recompiled SOP ENTITY UPDATE 0x8010A0E0, whose render chain (MAIN 0x800ACB94 → 0x8003D07C → 0x8003CD08) calls the RECOMPILED render walk gen_func_8003C048, which dispatches a node to the per-AREA render dispatcher func_8003F698 (0x8003F698; switches on the area index 0x800BF870, =0 = A00) → FIXED `jal 0x80146478` = A00's GT3/GT4 submitter (`ov_gt3gt4_caller`). A00 code isn't resident (SOP is) → recomp-MISS. The NATIVE render walk `ov_render_walk` handles these GT3/GT4 submitters IN C (no overlay dispatch) and would NOT miss — the bug is that this render goes through the RECOMPILED walk (reached from the un-ported entity update 0x8010A0E0), not the native one. Verified state at miss: sm[0x48]=2 sm[0x4a]=0 sm[0x50]=2, sopsig=0x3C021F80, areaidx(0x800BF870)=0, 1f800234=0; A00 (LBA 374) loaded 0 times.
- **fix (later-274):** own the remaining simple master-walk node cases in the NATIVE walk so it never falls back to the recomp body (`submit_render_walk`, engine_render_walk.cpp). The walk now handles, besides the existing PEROBJ/DEFAULT: the render-nothing skip cases (types 9-14,21-31 → 0x8003C2AC) and the special-effect leaf cases (types 16-19 → leaves 0x8003C2D4/464/5F8/788). The four leaves only call resident-MAIN library fns (matrix/GTE/libgpu) — never 0x8003F698, never an overlay address — so rec_dispatching them (content stays PSX) can't miss; their prim span is depth-tagged like the other owned cases. type-4 (per later-239 occlusion note) and type-20 (its leaf conditionally jal's the field overlay 0x8011be5c) deliberately KEEP falling back. This makes the field/cutscene render stay PC-native (the user directive: reduce recompiler dependency) and removes the only path that reached the unloaded area submitter. Verified: 0x80146478 miss gone; AUTO_SKIP free-roam 0-miss; emit 14/14; SELFTEST=startgame PASS. The earlier prediction (port 0x8010A0E0 / the 800ACB94→8003CD08 chain) was for a DIFFERENT entry that AUTO_SKIP exercises; the live shipping-path trigger was the walk fallback, fixed here without porting that chain. Did NOT bandaid (no force-load of A00, no miss-swallow).
- **repro (headless, ~90s, dies at the miss) — NATIVE single core (shipping path):** `printf 'newgame\nrun 2500\nquit\n' | PSXPORT_REPL=1 PSXPORT_VK_HEADLESS=1 PSXPORT_NOAUDIO=1 ./scratch/bin/tomba2_port scratch/bin/tomba2/MAIN.EXE` — letting the narration PLAY (no Start) hits `[recomp-MISS] 0x80146478` + `[miss-state] sm[4a]=0 ...`. (SBS repro also works: `PSXPORT_SBS_MODE=both` + cross-then-start `PSXPORT_SBS_KEYS`.)
- **refs:** engine/sop.cpp ov_sop_field_update (rec_dispatch 0x8010A0E0 entity update + native ov_render_walk), engine/engine_submit.cpp ov_render_walk (native walk, handles GT3/GT4 natively) + ov_gt3gt4_caller (0x80146478 orphan), engine/engine_stage.cpp ov_game_frame/ov_game_submode0, MAIN func_8003F698 (0x8003F698 per-area render dispatch, table 0x80015268) + render chain 0x8003CD08/0x8003D07C, runtime/recomp/hle.cpp (resident-overlay + miss-state diagnostics), runtime/recomp/overlay_router.cpp (`ovload` channel), later-273

## oraclediff: narration GAME STATE is convergent native-vs-PSX — cutscene bugs are RENDER-only
- **symptom:** intro-narration render bugs (void scene draws sea / missing effect, cliff water corrupt); oraclediff (PSXPORT_SELFTEST=oraclediff) showed a huge ~615-byte all-zero-vs-populated divergence at 0x80102xxx that looked like missing scene-setup state
- **status:** known-issue (RAM diff exhausted; bugs are render-only — needs the Phase-2 software-GPU/VRAM diff, docs/oracle.md)
- **cause:** the 0x80102xxx block is the guest **CdSearchFile directory/file cache** (FUN_8008b8f0 builds it via FUN_8008bbe8; 128-entry stride-0x2c dir table @0x80102D6C, stride-0x20 file table @*0x80102728, read buffer @0x80104368). The native port resolves CD files via disc_find_file natively and NEVER builds this guest cache → all-zero on native. By-design native replacement, not a bug. With it excluded, the ONLY remaining narration divergences are benign: 0x80105EE8 = the PRNG LCG state (DAT*0x41c64e6d+0x3039, drifts because the cores pull rand() different counts), 0x80105BAC = a 0..15 callback-ring counter (timing), 0x80105EF8 = the stdio/printf line buffer (FUN_8009ae60 putchar accumulator — differs only because the oracle prints "CD newmedia" debug strings). I.e. the native side runs the cutscene LOGIC correctly; the picture is wrong, not the state.
- **fix:** od_is_cd_cache() exclusion + per-page histogram in selftest.cpp run_oraclediff. Next: build the software-GPU oracle (docs/oracle.md Phase 2) to diff VRAM/render — that is the only thing that can surface void-effect + cliff-water.
- **refs:** later-278, runtime/recomp/selftest.cpp od_is_cd_cache, scratch/decomp/ram_f1000_all.c FUN_8008b8f0/FUN_8009ae60, docs/oracle.md

## oraclediff: FREE-ROAM GAME STATE is also convergent — the native opening gameplay matches the PSX oracle
- **symptom:** after fixing the narration render bugs, need to confirm the ACTUAL playable game (free-roam field) is correct native-vs-PSX, not just the scripted cutscene.
- **status:** VERIFIED CONVERGENT (later-282) — native free-roam onset state == PSX oracle, only benign drift.
- **cause/result:** extended run_oraclediff with a FREE-ROAM checkpoint — both cores are parked at the first walkable-field frame (MODE overlay 0x80109450 flips 0x3C021F80 SOP-narration → 0x801138A4 walkable-field; SOP scene byte 0x800bf9b4 back to 0; native f1131, oracle f1133) and the 0x800B0000..0x80110000 engine band is diffed. Result: the ONLY diverging bytes are the exact same three benign ones as the narration beats — 0x80105BAC (0..15 callback-ring counter), 0x80105EE8 (PRNG LCG), 0x80105EF8 (stdio line buffer). Every other byte is identical. So the native side loads + initialises the free-roam field with the SAME game state as the real PSX. Render eyeball also matches: the oracle soft-GPU field frames (Tomba falling over the seaside cliff → top-down water-island → stuck-in-tree "Aaaahh") all appear in the native VK shots too (they were just at different frame offsets — the opening is a scripted-camera sequence, so frame-N snapshots misalign; the STATE-SYNC barrier is what makes the comparison meaningful).
- **render verified too (later-282b):** run_oraclediff sets B->gpu.soft_gpu=1 and, at the free-roam checkpoint, dumps BOTH cores' framebuffers state-aligned — native VK (scratch/screenshots/oraclediff_freeroam_native.ppm) vs PSX soft-GPU oracle (oraclediff_freeroam_oracle.ppm). At onset+40 (past the field fade-in) they are a near-perfect match: same island geometry, same Tomba pose (stuck on the cliff pole), same terrain/bridge/vines, same camera + colors. GOTCHA: the RAM diff must run at the EXACT onset (overlay-flip frame) — free-running even 30 frames past it drifts (3→663 diverging ranges) because the two cores animate the scripted opening at slightly different sub-frame cadence (interp does real CD loads). So diff at onset, step only for the (content, not pixel) render dump.
- **fix:** run_oraclediff now runs the narration scene checkpoints AND a trailing free-roam overlay-sig checkpoint (od_advance_to_ovsig, shared diff_band lambda) + a state-aligned native-vs-oracle render dump. run_oracle dumps the oracle's soft-GPU free-roam field framebuffer (scratch/screenshots/oracle_field_h*.ppm).
- **refs:** later-282, runtime/recomp/selftest.cpp (run_oracle free-roam capture, run_oraclediff free-roam checkpoint + od_advance_to_ovsig + diff_band), docs/oracle.md

## oraclediff: interactive-play SCAN added (harness); Tomba NOT yet controllable at onset; frontier was STALE
- **symptom:** need to know if the native port matches the PSX oracle during INTERACTIVE play (Tomba under player control), not just the scripted opening; and the frontier said "wire the orphaned PC-native ov_terrain into the field terrain path".
- **status:** HARNESS added (later-283); interactive-MOVEMENT convergence NOT yet verified (Tomba not controllable at the tested checkpoint — first commit OVERCLAIMED it, corrected same session). Also CORRECTED the stale ov_terrain-orphaned claim.
- **cause/result (interactive scan):** extended run_oraclediff with an interactive-play scan — from the aligned free-roam onset both cores are driven with IDENTICAL pad input (hold D-pad Right = walk, PAD_RIGHT active-low ~0x0020) for 90 frames, then both framebuffers are dumped (oraclediff_freeroam_{native,oracle}.ppm). The frames MATCH — BUT this is the SAME still-convergence already proven at the onset (later-282), NOT proof of interactive movement: **verified that at this checkpoint Tomba is in the scripted "caught on the fishing line" pose and does NOT move under input** — holding Right for 1400+ frames (to f2500) OR mashing Cross/Circle/Square/Triangle leaves him in the EXACT same on-screen position in the native core (scratch/screenshots premove/postmove/m1500/m2500/mash2). So the "free-roam" overlay-flip onset (0x801138A4) is the field LOADED + rendering, but the player does not yet have walk control. Reaching real control (freeing Tomba) is the genuinely-hard part the handoff flagged; button-mash + long walk-holds do NOT free him — needs RE of the opening/fisherman-pull sequence.
- **RAM-diff caveat (why the framebuffer is the verdict, not the RAM count):** during the walk the engine-band RAM diff explodes to ~1000+ ranges, but it is RENDER-PATH NOISE, not gameplay. Gameplay and render state share the same node structs; the native-VK vs PSX-soft-GPU render paths populate each node's render-cache fields (matrix cache node+0x98, render-command array node+0xC0), the ordering table (~0x800ED000..0x800F1000) and the render-queue lists (0x800F24xx) DIFFERENTLY. These pages are IDENTICAL at the onset (before either core renders the field) — that is why the onset RAM diff is clean (3 benign ranges) but any post-render-step is not. A clean RAM gameplay gate would need per-field gameplay/render separation of the node structs (a larger RE); not done — deliberately NOT masked with a broad address exclusion (would risk hiding a real object-struct divergence). The DONE summary excludes the interactive walk's noise (summary_div frozen after the free-roam onset diff).
- **CORRECTION — ov_terrain is NOT orphaned:** the frontier/handoff claimed the PC-native terrain (engine_submit.cpp ov_terrain → native_terrain.cpp terrain_render_pc) had no caller and that field terrain rendered via the PSX substrate. FALSE (verified live, `debug terrgte`/`terrpc`): ov_render_walk (called from ov_sop_field_update, sop.cpp:218) routes the default-case terrain fn 0x8002AB5C → ov_terrain → terrain_render_pc every field frame (node 0x800ED8D8/0x800ED960, draws its geomblk quads PC-native float+depth). The field GROUND is also native (ov_field_entity_render, 0x80109fe0, table 0x800F2418). So "wire the orphaned ov_terrain" was ALREADY DONE. At the opening the only still-PSX render dispatches are GTE/scratchpad SETUP leaves (0x8003C2D4 matrix-compose, 0x8013DD34 cull/bound), intentionally left PSX per the RENDER directive; the per-object effect cases idx1/2/3/8 do not fire at the opening (`ccase` silent). ⇒ the render-leaf frontier is DONE-or-BLOCKED at the verifiable opening; advancing the effect leaves needs a LATER AREA that exercises them (extend the oracle envelope first).
- **fix:** runtime/recomp/selftest.cpp run_oraclediff — PAD_RIGHT define, diff_band returns range count + count-only mode (label==nullptr), interactive-play walk block + summary_div. docs/port-progress.md CURRENT FRONTIER later-283 block.
- **refs:** later-283, runtime/recomp/selftest.cpp (run_oraclediff interactive-play scan), engine/engine_render_walk.cpp (ov_render_walk terrain routing), engine/sop.cpp:218, engine/native_terrain.cpp, docs/port-progress.md

## AUTONAV: reaching real player control (s4e==9 -> 1); interactive play is A/B byte-identical (2026-07-08)
- **symptom:** SBS AUTONAV (and AUTO_SKIP) stopped at the intro-cutscene end (cutscene flag
  `0x1F800137`==0 for 60 frames) and called that "gameplay-start". But at that onset Tomba is in the
  scripted "caught on the fishing line" pose and does NOT respond to input (later-283, docs/oracle.md).
  So genuine INTERACTIVE play (walk/jump/attack) was never exercised — pc_faithful during real play was
  UNTESTED.
- **status:** RESOLVED. AUTONAV now reaches real player control; a full interactive walk/jump run is
  BYTE-IDENTICAL A/B (pc_faithful vs recomp oracle) for 20,000+ lockstep frames, ZERO divergences.
- **the "controllable" predicate (RE'd):** the field-running sub-machine `fieldRun`/`fieldRunFaithful`
  (game/core/engine.cpp, mirror of gen 0x80106B98) is a 12-way switch on `sm[0x4e]` = task0
  (0x801fe000) + 0x4e. **s4e==9** ("L_80106FC4", engine.cpp case 9) is the scripted caught HOLD — it
  only advances on a Cross-EDGE (`0x800E7E68 & 8`) while the scripted-camera gate `0x800BF89C`==2 is
  armed. **s4e==1** ("L_80106D00 — the RUNNING field frame", engine.cpp case 1) is genuine free-roam:
  it reads the interactive scene-trigger byte 0x800BF839 for movement/menu/area-exit dispatch. The
  cutscene flag `0x1F800137` belongs to the SEPARATE SOP intro-narration machine (game/scene/sop.cpp
  Sop::fieldMode, sm[0x50]) and clears when THAT machine hits its state-4 RESET — at which point
  fieldRun's own sub-machine has already been steered into s4e==9 (engine.cpp case 0: `if 0x800BF89C==2
  sm[0x4e]=9`). So flag==0 reaches the caught pose, NOT control. Predicate for "player can walk" =
  **sm[0x4e]==1 sustained** (30 consecutive frames to reject transient pass-through: s4e visits
  6/7/8/10/11 leaving 9, and case 5 sets s4e=1 transiently for an area-7 re-arm).
- **VERIFIED (live, debug server, single native core):** after AUTONAV's new AWAIT_CONTROL phase taps
  Cross to release the fishing-line hold and s4e settles at 1, holding Right MOVES the player node
  (0x800FE8E8, handler 0x8006F2D0 "pad_child_linker") pos.x 3940->3951 before a wall — whereas the SAME
  held Right at s4e==9 leaves pos flat. Camera scratch (0x1F8000D0 block) also pans only after control.
  Confirms s4e==1 is the real control gate, not a render/animation artifact.
- **nav extension:** runtime/recomp/sbs.cpp navStep — new Phase AWAIT_CONTROL between SKIP_CUT and DONE:
  tap Cross every 20 frames, wait for sm[0x4e]==1 for 30 consecutive frames, then DONE. The old
  "gameplay-start" log became "field-rendering (still scripted-caught)"; the new terminal log is
  "player-controllable @fN (s4e settled at 1)". PSXPORT_SBS_POSTDRIVE=1 then drives the walk-Right /
  jump-Cross script on BOTH cores past control.
- **DEAD-CODE FIX (workflow-first):** the POSTDRIVE walk/jump script in navStep's DONE case was
  UNREACHABLE — Sbs::Impl::run() stopped calling navStep() once nav_done latched (`else feedInput()`),
  so DONE never re-ran. Fixed the dispatch to keep calling navStep when postdrive is armed
  (`if (!nav_done || sbsPostdriveOn())`), else fall back to feedInput() (host keyboard/debug-server).
  Factored the postdrive env check into file-scope `sbsPostdriveOn()`.
- **KEY RESULT:** `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_POSTDRIVE=1` headless —
  reaches control at f246, then walk/jump-drives both cores identically to f20070: every frame
  "A/B identical", NO [sbs] DIVERGENCE / WRITE-SITE / abort. So pc_faithful is byte-exact to the recomp
  oracle through genuine interactive play (walk + jump), not merely the scripted opening. This CLEARS
  pc_skip=OFF for real seaside free-roam play — no first-interactive-play bug found.
- **refs:** runtime/recomp/sbs.cpp (navStep AWAIT_CONTROL phase, sbsPostdriveOn, run() dispatch fix),
  game/core/engine.cpp fieldRun/fieldRunFaithful (predicate RE source), docs/oracle.md later-283
  (the "not controllable at onset" it corrects), scratch/logs/sbs_long.log.

- **OPEN (2026-07-10, found incidentally while validating the MIRROR_VERIFY fast path, NOT
  investigated):** `PSXPORT_SBS_MODE=full PSXPORT_VK_HEADLESS=1 PSXPORT_NOAUDIO=1
  PSXPORT_NATIVE_FRAMES=100000` with **no** `SBS_AUTONAV`/`SBS_POSTDRIVE` (plain idle boot, no input
  driving) diverges deterministically at lockstep frame 1019, byte range 0x1F80009C..0x1F8000A0
  (scratchpad), reproduced identically on unmodified `main` (confirmed by reverting the journal
  patch below and re-running — same frame, same byte range, same native/oracle hit-count table).
  This does NOT contradict the KEY RESULT above (autonav+postdrive walk/jump 0-diff through f20070)
  — different scenario (no input at all vs a scripted walk/jump script), so plausibly a different
  code path (e.g. an idle-only timing/counter divergence never exercised once autonav starts
  feeding input). Not root-caused; logged here so it isn't re-discovered from scratch. Repro: unset
  any `SBS_AUTONAV`/`SBS_POSTDRIVE`, run the command above, grep for `DIVERGENCE`.

## SKIP-mode frame alignment (USER 2026-07-10) — mechanism landed, first fork wired, honest frontier

- **goal:** `PSXPORT_SBS_MODE=skip` compares core A (`pc_skip=true`, the real `./run.sh` shortcut
  config) against core B (the pure recomp oracle, `psx_fallback=1`). A's collapsed-multi-step forks
  (every `if (game->pc_skip) shortcut(); else faithful();` site — CLAUDE.md "The 5 paths") finish a
  multi-frame substrate load in ONE native call, so A's game state used to race ahead of B's at the
  SAME lockstep `mFrame` — checkObservables() covered for this with a 60-consecutive-frame
  "settled-divergence" tolerance instead of comparing every frame. USER directive: replace the
  tolerance with an actual barrier ("drag behind on skips… so it should always be visually and
  audio-wise identical on the same frames"), compare-mode only.
- **mechanism (landed):** `Sbs::skipCompareMode()` / `Sbs::skipRendezvousReached(c, addr, minVal,
  label)` (runtime/recomp/sbs.h, impl in sbs.cpp). A fork's shortcut leg calls
  `skipRendezvousReached` right after doing its own (harmless, host-only) work but BEFORE flipping
  any guest-visible "load complete" state; while the sibling core (read via `Sbs::Impl::coreId` +
  `mem_r16`) hasn't independently driven the SAME shared-layout field to the same value yet, the
  caller idles (no state advance, re-check next frame) instead of proceeding. Pass-through (always
  `true`) outside `MODE=skip`, so every fork site is a genuine no-op in `./run.sh` and every other
  SBS mode — no new `PSXPORT_*` gameplay toggle, gated entirely through the existing `Game::sbs`
  back-pointer. A wait that never resolves within `kRvTimeoutFrames` (3600 = 60s @ 60fps) **aborts**
  with both sides' state dumped (fail-fast diagnostic, not a silent hang) — this is a hard
  requirement, not a nice-to-have: a naive "just wait forever" barrier would turn a bad predicate
  into an un-diagnosable stuck gate. `dumpRendezvousSites()` (atexit/SIGTERM, shares the ALLOCTRACE
  dump registration) prints per-label checks/stalls/maxWait so a fork whose predicate silently
  never fires (checks==0 across a run known to exercise it) or that's stuck waiting at exit is
  visible in the log, not just inferred.
- **kObsPersist dropped 60 → 1** (checkObservables, sbs.cpp): now that alignment removes the
  legitimate reason for transient drift at a WIRED fork, the observable compare is strict per-frame
  — first differing frame reports (and by default `abort()`s; `PSXPORT_SBS_SKIP_CONTINUE=1` demotes
  to log-and-continue for triage runs, same shape as `MIRROR_VERIFY_CONTINUE`). Covers: the 5 fixed
  observable regions (AUDIO fx_table/fx_area_ptrs/seq_slots/global_scale, libcd file-table), the
  area-fx dereference, and SPU RAM (VAB sample banks).
- **visual compare:** `MODE=skip` now auto-arms the existing `checkPaneDiff()` pixel-diff (was
  opt-in via `PSXPORT_SBS_RENDERDIFF`) — this pixel-diffs A's pc_render pane against B's
  psx_render/oracle pane (±40/channel tolerance for PSX-fixed-vs-float dither noise), tracks the
  worst frame, dumps over-threshold frames as side-by-side PPMs to
  `scratch/screenshots/renderdiff/`. **What it covers, honestly:** STRUCTURAL differences in the
  rendered PICTURE only (missing/misplaced geometry, wrong fills/colors) — it does NOT cover audio
  (the SPU-write-log compare + checkObservables' SPU-RAM section do), and it does NOT cover any
  non-visual guest state (the RAM/scratch divergence + rendezvous checks do). No new mechanism was
  built for this — it was already the right tool, just gated off by default; this pass just turns
  it on for the mode it's most relevant to.
- **fork inventory — what's actually a "collapsed multi-step init" (rendezvous candidate) vs a
  per-frame parity fork (NOT a rendezvous candidate):** grepping every `pc_skip`/`mPcSkip` site
  (`game/`, `runtime/recomp/`) turns up ~25 hits, but most are STEADY-STATE forks — two
  IMPLEMENTATIONS of the SAME per-frame tick (native shortcut vs a literal byte-exact mirror of the
  substrate), both already running once per lockstep frame on both cores by construction
  (`fieldFrame`/`fieldFrameFaithful`, `fieldRun`/`fieldRunFaithful`, `sceneEventFifo`/…Faithful,
  `submode1`/…Faithful, `areaModeDispatch`/…Faithful, `sceneStateStep`/…Faithful,
  `modePerFrameDispatch`/…Faithful, `postRenderTick`/…Faithful, `frameStartTick`/…Faithful,
  `walkAll`/`walkAllFaithful`, `walkAux`/`walkAuxFaithful`, `ObjectTable::dispatch`/`dispatchFaithful`,
  `CutsceneCamera::update`/`updateFaithful`, `Cull::farMult` skip/faithful, `Array8Dispatch::tick`/
  `tickFaithful`). These do NOT need a rendezvous barrier — they don't collapse frames, so there's
  nothing to align.
  - **genuine load-collapse forks (rendezvous candidates), in rough boot order:**
    1. **`Engine::startBinStage` (startBinStageSkip vs startBinStageFaithful)** — the START.BIN
       ISO9660/CD directory + XA-singleton file-table build. **WIRED THIS PASS** at the
       `stage0AdvanceSkip` step-0 gate (game/core/engine.cpp), predicate = sibling's
       `CUR_TASK`-relative `+0x48` preload-SM halfword reaching `>=1`, label `start_bin_load`.
       **MEASURED RESULT (2026-07-10, `MODE=skip`, `PSXPORT_SBS_EXIT_FRAME=150`):
       checks=1 stalls=0 maxWait=0** — the two cores were ALREADY aligned on this specific field
       every time it was checked. Reading the RE more carefully post-measurement: the substrate's
       own `CdSearchFile` loops (in `startBinStageFaithful`'s mirror of the gen body) also run with
       no yield-capable primitive inside them, so BOTH the native shortcut and the real substrate
       resolve the whole file table synchronously within one scheduler tick — task+0x48 reaching 1
       is NOT where the "~10+ substrate ticks vs ~5 native ticks" cadence gap (docs/config.md
       "Boot-preload TRANSIENT regions") actually lives. This is an honest negative result, not a
       wasted wire: it rules out task+0x48 as the drift source and narrows where the REAL gap is
       (next point).
    2. **`asset.preloadTexgroup`/`asset.preloadStage1` — the texgroup + stage-1 VRAM/relocation
       build invoked inline by `startBinStageSkip`/`stage0AdvanceSkip` (native) vs spawned as a
       task-1 fiber body (`PcScheduler::runTask1PreloadStanza`, substrate/faithful) — NOT YET
       WIRED.** This is where the isPcSkipScratch mask's "preload cel_h/task-state/metadata" boot-
       transient regions (0x800BE0E0, 0x800BED80, 0x800ECF54, 0x800ED000, 0x800EF478, 0x80105C10,
       0x80105D00..0x80105F00, 0x80157000..0x8017D000, sbs.cpp) point — the actual multi-frame
       collapse this task set out to fix. Candidate predicate: task-1's slot state
       (`native_fiber[1]`/`task_started[1]` don't exist on the pure-substrate oracle core, so the
       predicate must be a GUEST-visible field the substrate's own task-1 body sets on completion —
       needs its own RE pass, same shape as the `start_bin_load` wire above, before landing).
    3. **The two native CD readers** (`cdlibcd_new_media`/`cdlibcd_cache_file`, pc_skip shortcut,
       vs `LibcdNative`'s faithful chain, game/cd/libcd_native.h) — used inside
       `startBinStageSkip` itself; likely converges with fork 1's measurement (same synchronous-CD
       assumption) but not independently measured.
    4. **`Demo::s0Skip`/`s0Faithful`** (game/scene/demo.cpp) — attract→title reload-menu-resources
       collapse. Not measured this pass.
    5. **`pc_scheduler.cpp`'s DEMO/GAME task stanzas** (`runTask1PreloadStanza`'s native-vs-fiber
       split, `runStage0StepStanza`) — the SCHEDULING side of forks 1-2; already touched by fork
       1's wire (shares `stage0_step`) but the DEMO/GAME per-task collapse itself isn't separately
       gated.
  - **first post-alignment divergences surfaced (2026-07-10, `MODE=skip PSXPORT_SBS_SKIP_CONTINUE=1
    PSXPORT_DEBUG=skiprv`, headless, no autonav needed — these are all pre-control boot-window
    hits):** NOT investigated past this triage; recorded honestly as the new frontier, not
    allowlisted.
    - `f2` **SPU RAM / VAB banks** `@0x01020 A=00 B=33` — fires before any pc_skip fork logic has
      had a chance to run at all (2 lockstep frames in); most likely an SPU init-order artifact
      unrelated to the load-collapse forks above (nothing to rendezvous against yet at f2). Needs
      its own RE pass to confirm — flagged, not fixed.
    - `f459` **AUDIO fx_area_ptrs** `@0x800A4F7E A=02 B=FF` and the resulting **AUDIO
      area_fx_deref** `@0x8014C124 A=04 B=00` — downstream of area/asset content that differs
      between the two cores at this point; the natural suspect given the fork-2 gap above
      (texgroup/stage1 preload not yet rendezvous-gated) but not confirmed by tracing the actual
      write site — next session should `PSXPORT_SBS_WW_ONVALUEDIVERGE`/rewind-watch
      `0x800A4F7E` to pin the writer before assuming this is fork 2's fault.
  - A subsequent, unrelated crash (`rec_dispatch_miss 0x80111A20`, an un-recompiled A02-overlay
    function reached via the DEMO stanza around f3169 in a longer smoke run) is a pre-existing
    MODE=skip coverage gap, NOT caused by this change — confirmed by the same miss being reachable
    via the DEMO/menu path regardless of the rendezvous mechanism; out of scope for this pass.
- **gates:** standard `SBS_MODE=full AUTONAV=combat` 95s gate — **0-diff**, unaffected (this
  change is entirely `MODE=skip`-scoped: `skipRendezvousReached` is a pass-through outside
  `M_SKIP`, and `kObsPersist`/`checkObservables` are only reached from `checkDivergence()`'s
  `mMode == M_SKIP` branch). `MODE=skip` smoke run to f3169 — the rendezvous barrier itself never
  stalled/timed-out/deadlocked (confirmed via `dumpRendezvousSites` at a clean `SBS_EXIT_FRAME`
  cutoff); the run's eventual failure is the pre-existing, unrelated recomp-miss above.
- **refs:** runtime/recomp/sbs.h (skipCompareMode/skipRendezvousReached API doc), runtime/recomp/
  sbs.cpp (Impl::skipRendezvousReached, dumpRendezvousSites, checkObservables strictness,
  checkPaneDiff auto-arm), game/core/engine.cpp `Engine::stage0AdvanceSkip` (the one wired fork),
  docs/config.md `SBS_MODE=skip` section (rewritten), docs/bug-hunt-workflow.md PC-SKIP-ON cell.

## SBS panes ≠ standalone configs — process-global enhancement/render state (FIXED 2026-07-10)
- **symptom (USER):** `PSXPORT_SBS_MODE=full` panes look like NEITHER `PSXPORT_PC_SKIP=0 ./run.sh`
  nor `PSXPORT_ORACLE=1 ./run.sh` — "they leak into each other".
- **status:** FIXED (structural deglobalization, 3 layers).
- **cause:** three families of process-global state meant per-core configs were impossible:
  1. `g_mods` (aspect/ires/ssao/light/shadows/fps60) — ONE global for both cores;
  2. `oracle_mode()` — a process-wide cfg predicate, so SBS core B never got the ORACLE pin
     (standalone `PSXPORT_ORACLE=1` forces enhancements off in `native_boot_run`, which SBS cores
     never execute — they boot via `dc_boot_init`);
  3. the VK render TARGETS (`s_vram_tex` guest-VRAM image, atlas snapshot, depth, float semi
     intermediate, vertex buffers) lived on the `GpuDevice` singleton — both cores rendered
     through ONE set of GPU surfaces.
- **fix:** all three per-Game. `class Mods` = `Game::mods` (mods.cpp; overlay edits its game's
  instance); `Game::oracle` + `Game::setOracle()` (seeds from PSXPORT_ORACLE in native_boot_run;
  SBS calls it on core B whenever fb_b — every enhancement gate, incl. `gpu_gpu_wide_engine(Core*)`,
  painter-order force, observer, billboards, reads `game->oracle`); render targets moved onto
  `GpuGpuState` (`ensure_targets()`, lazy) — only window/device/samplers/PIPELINES stay on
  GpuDevice. SBS pins BOTH cores' mods factory-neutral (a guest-poking enhancement — the widescreen
  cull re-include — on one core would break the byte gate BY DESIGN) and oracle-pins core B.
- **NOT equivalent to the pane picture being the standalone present:** panes are still produced by
  `gpu_gpu_render_readback` (raw display-region readback + 4:3 letterbox composite), not the real
  present pass, and SBS still skips FMVs + force-skips the DEMO intro-FMV sub-state (timeline
  differs from a standalone run). For a picture-faithful ./run.sh-vs-oracle compare use
  `tools/abcompare.py` (two isolated processes; docs/abcompare-design.md).
- **gates:** build clean; abcompare golden drive byte-identical pre/post (5 probes RAM 0-diff,
  same pixel diffs); `SBS_MODE=full` headless 120 s — A/B identical through f25560, no divergence.
- **refs:** runtime/recomp/mods.{h,cpp}, game.h (Mods member + oracle/setOracle),
  gpu_gpu_internal.h + gpu_gpu.cpp (per-Game targets, ensure_targets, threaded
  upload_vram/readback_vram/dump_to), gpu_gpu_device.h (pipelines-only), sbs.cpp (per-core pin),
  native_boot.cpp (setOracle), rmlui_overlay.cpp (per-game mods editing).

## SBS pane parity vs standalone configs — VERIFIED pixel-faithful (2026-07-10, follow-up)
- **symptom (USER, after the deglobalization):** panes still "completely different" from
  `PSXPORT_PC_SKIP=0 ./run.sh` and `PSXPORT_ORACLE=1 ./run.sh`.
- **status:** FIXED + mechanically verified.
- **cause:** the SBS-only DEMO intro-FMV guest poke (`mem_w8(0x1f80019d,1)` while DEMO SM[0x48]==1)
  tore the demo machine's FMV sub-state down in ~1 frame while a standalone run spends the guest's
  own strNext timeout there — the SBS timeline ran tens of frames AHEAD of both standalones, so at
  any frame index every pane showed a different game moment (95% pixel diff).
- **fix:** poke removed (sbs.cpp stepCore); both cores now take the guest's own timeout path — the
  same one standalone ORACLE takes. Slower wall-clock, frame-parity with standalone.
- **verify tool:** `PSXPORT_SBS_SHOT=<frame>:<prefix>` (sbs.cpp) dumps each pane separately at one
  lockstep frame → diff against standalone REPL `run N; shot`.
- **measured:** SBS pane B @ lockstep f500 vs standalone `PSXPORT_ORACLE=1` @ REPL f502 =
  **0.00% structural** (tol 40 + ±1px). Pane A vs `PC_SKIP=0` @ f502 = 1.35% floor (best across
  501..503; settings-ini ruled out) — a pc_render-specific residue, same open bug family as below.
- **frame-index bookkeeping:** SBS lockstep fN ≡ standalone REPL frame N+2 (constant origin offset,
  REPL runs 2 boot frames before its first command; timeline identical).
- **exposed open bug (pc_render, demo/attract):** with byte-identical guest RAM, pane A (pc_render)
  draws a DIFFERENT CAMERA than pane B and drops Tomba + the DEMO blink text; standalone PC_SKIP=0
  shows the same wrong picture, so this is the renderer-boundary-leak family (same as abcompare
  f160 Tomba+Zippo missing in the prologue cutscene), now honestly visible in the SBS panes.
- **refs:** runtime/recomp/sbs.cpp (stepCore poke removal + PSXPORT_SBS_SHOT), scratch/sbscmp/
  (evidence quads), tools/abcompare.py (the standalone-vs-oracle compare).

## SBS pane A = the USER's real config (2026-07-10, follow-up 2)
- **symptom (USER):** panes still didn't visually match the standalone runs — because the user's
  standalone runs are WINDOWED with their settings ini (aspect=AUTO → widescreen, fps60=1), while
  the harness pinned both cores factory-neutral (pane A was a 4:3/30fps picture vs a wide standalone).
- **fix:** core A keeps the user's mods (settings ini loads in the Game ctor); ONLY core B is
  oracle-pinned. The byte-gate conflict is solved at the root instead of by neutering the pane:
  the one guest-poking enhancement (widescreen cull re-include, cull.cpp) is suppressed under SBS
  (`game->sbs`) so core A's guest evolution stays byte-identical to core B — cost: dynamic-entity
  pop-in at wide pane margins (standalone wide re-includes them; the pane uses the read-only static
  margin). fps60 presentation is inert under diff_mode (panes sample real frames), so pane A shows
  30fps real frames while a standalone fps60 run interpolates — static content identical.
- **wide pane plumbing:** grabPane samples the wide FB width (was cropped to 4:3);
  gpu_gpu_present_sbs2 letterboxes each pane by its own aspect (was hard-coded 4:3);
  wide AUTO under SBS derives from HALF the window (two panes side by side).
- **verified (headless):** byte gate 0-diff through f13890 with the user ini live on core A;
  pane B @f500 vs standalone ORACLE @f502 = 0.00%; pane A at the known 1.35% pc_render floor.
  Windowed wide panes = USER eyeball (headless AUTO resolves 4:3, so the wide leg can't be
  machine-checked headless).

## SBS self-surfacing sweep 2026-07-15 — pc_faithful CLEAN; pc_skip audio-init fork diverges
- **MODE=full (pc_faithful byte-exact): 0-diff on every route tried** — all 6 committed replays,
  PSXPORT_SBS_AUTONAV=combat (f23340), and extended AUTO_SKIP to f57390 (2x the prior ~f30k baseline).
  Job #1's faithful path has no low-hanging divergence on drivable routes.
- **MODE=skip (pc_skip vs oracle — the DEFAULT config the user plays) diverges early, all AUDIO:**
  (1) f2 SPU RAM @0x1020 (VAB banks): oracle has bank data by f2, pc_skip still zero — the pc_skip
  one-shot audio/VAB load lands SPU RAM on a different frame than the faithful multi-step load
  (also the f0 527-byte RAM diff 0x801FE7C0.. is the same boot-order gap). (2) f462 area-FX audio
  pointer @0x800A4F7E / @0x8014C124 — per-area SFX state disagrees. Both = the pc_skip AUDIO-init /
  area-SFX fork, not pc_faithful, not render. (The 80-88% MODE=skip renderdiff is pc_render vs
  psx_render by design, not a state bug.)
- **STRATEGIC:** the f2 gap is what forces PSXPORT_SBS_SKIP_CONTINUE=1 to run MODE=skip at all —
  fixing the pc_skip audio-init fork UNBLOCKS MODE=skip as a clean self-surfacing oracle for the
  user-facing (pc_skip) bugs. Next high-value thread. Root-cause f2 first (upstream of f462).
- Logs: scratch/logs/sbs{1..9}*.log.

## MODE=skip f465 area-FX divergence — RESOLVED (2026-07-15): rendezvous on 0x1F80019A
- Repro: MODE=skip AUTO_SKIP SKIP_CONTINUE — f465 [AUDIO fx_area_ptrs] @0x800A4F7E A=02 B=FF +
  @0x8014C124 A=04 B=00. Same FORK PATTERN as the f2 VAB fix (7eced30): a collapsed SYNCHRONOUS
  native load racing ahead of the oracle's cooperative multi-frame task — here the ATTRACT-DEMO area
  load (demo.cpp:878 demo_frame_s7 phase0 -> Sop::transitionAreaLoad, the native inline transcription
  of the 0x800452C0 task body; oracle spawns task slot 1 via FUN_80044BD4 + yield-waits many frames).
- Classification: TIMING (phase-skew), NOT value — both cores converge to 0x02 by f3000 (BYTETRACE
  settled = CLEAN). Writer core A: Sop::transitionAreaLoad -> gen_func_8007566C -> 800753D4 -> mem_w16
  at f464.
- Fix SHAPE: a skipRendezvousReached barrier before the SV_CHECK(...transitionAreaLoad...) at
  demo.cpp:878 (+ engine.cpp:1666/2279 siblings), SBS-harness-only, like 7eced30.
- BLOCKED: the safe monotonic key is NOT identified. The oracle's completion signal is in task SLOT 1
  (0x801FE070, stride 0x70) which FUN_80044BD4/FUN_80051FB4 REUSE for every cooperative spawn (texgroup
  preload @f2, VAB build, this attract load) — so slot-1 entry/done-mark or 0x1F80019B FALSE-PASSES on
  stale state (the exact trap 7eced30 flagged). NEXT: RE FUN_80044BD4 (Ghidra, not dynamic inference)
  for a per-THIS-invocation monotonic marker (a spawn generation/sequence counter, or a demo/transition-
  specific sm field like sm[0x6d]/[0x6e] if written monotonically not reused). Do NOT gate until confirmed.
- Logs: scratch/logs/skip_area_fx_repro.log, pw_800A4F7E.log, bytetrace_fxarea_all.log.

## Job #1 replay sweep — pc_faithful byte-exact across the full replay library (2026-07-15)
- STATUS: VERIFIED CLEAN. SBS-full (PSXPORT_SBS_MODE=full SBS_AUTONAV NOWINDOW NOAUDIO) over EVERY
  replay in replays/ is 0-diff — no divergence, "A/B identical" at every 30-frame checkpoint:
    - scene-transitions/hut-entry-door-freeze.pad → 0-diff to f22020 (timeout, not a diff)
    - scene-transitions/hut-entry-alt.pad → 0-diff to f16080
    - bugs/dark-screen-repro.pad (122KB, ~30k input frames of real gameplay) → 0-diff to f52500
    - + AUTO_SKIP boot/attract/free-roam idle → 0-diff to f30720 (this session's bd4 gate)
- IMPLICATION: the faithful path (pc_skip=false) is byte-exact for boot, attract-DEMO, hut entry (both
  variants), and a long gameplay session. No SBS divergence remains to root-cause with the CURRENT
  replay set. Surfacing new Job#1 bugs now requires NEW replays reaching NEW territory (deeper areas /
  more mechanics) — the USER flagged this as later work ("SBS-specific recording ... for a later time").
- Logs: scratch/logs/sbs_replay_{hut-entry-door-freeze,hut-entry-alt,darkscreen}.log, sbs_postfix.log.

## Phase 5 — autonomous headless SCENE EXPLORATION under SBS: VALIDATED recipe (2026-07-15)
- GOAL (USER): drive into new scenes/dialogue headlessly to (a) unblock scene-gated fallthroughs
  (native fns whose dispatch callers only fire in specific areas — codemap --substrate-fallthrough)
  and (b) surface new SBS divergences beyond the 3 recorded replays.
- MECHANISM (already existed, now validated): `PSXPORT_SBS_AUTONAV=1` drives title→NewGame→GAME→skip
  intro cutscene→**real player control @~f246** (nav phase machine REACH_GAME→AWAIT_CUT→SKIP_CUT→
  AWAIT_CONTROL→DONE; "player-controllable @f246 (s4e settled at 1)"). Then `PSXPORT_SBS_KEYS=
  "FROM-TO:BTN,..."` (btn = up/down/left/right/cross/circle/square/triangle/start/select) injects
  timed input to BOTH cores in lockstep (sbs.cpp feedInput). Movement CONFIRMED: holding right moved
  Tomba's X (G_ADDR+0x2E = 0x800E7EAE) 0x0F64→0x1770. Standalone equivalent: AUTO_SKIP=1 + REPL
  press/release (or FORCE_HOLD), position at 0x800E7EAE.
- RECIPE:
    PSXPORT_NOWINDOW=1 PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_NOAUDIO=1 \
      PSXPORT_SBS_KEYS="300-1200:right,1300-2200:up" ./scratch/bin/tomba2_port <MAIN.EXE>
  (keys must start AFTER f246 = control handoff.) Add PSXPORT_MIRROR_VERIFY=<addr> to gate a
  scene-gated fallthrough once the route reaches its area.
- STATUS: mechanism validated + 0-diff on a blind 4-direction walk of the START field (byte-exact,
  no new divergence there). NEXT: DIRECTED routes to reach area EXITS / dialogue (blind walk hits
  walls, stays in start field). Options: (a) chain off hut-entry replay then explore the hut interior;
  (b) RE the start-field exit locations for a scripted route; (c) trigger the fisherman dialogue.

## Phase 5 exploration — composition works, all reachable scenes 0-diff (2026-07-15)
- PAD_REPLAY + SBS_KEYS COMPOSE: a replay reaches an area, then SBS_KEYS explores inside it (both cores,
  lockstep). Verified: hut-entry-door-freeze.pad → hut, then scripted walk = SBS-full 0-diff f15450.
- Coverage exercised this session under SBS-full, ALL 0-diff (no divergence, no Job#1 bug):
  start-field free-roam (blind + long directed walks), hut interior, combat (dark-screen), the two
  hut-entry routes, the 122KB dark-screen gameplay session. The pc_faithful path is byte-exact
  everywhere currently reachable.
- LIMITATION: blind directional walking does NOT cross area boundaries (start field is bounded; the
  only exits reached are via the recorded replays' routes). To reach GENUINELY new areas (village,
  deeper zones) + trigger the scene-gated fallthroughs (build/angleCmp/CutsceneCamera) + surface new
  divergences, need RE'd routes: find the start-field exit-zone trigger (position → SceneTransition/
  fieldMode area load) and script SBS_KEYS to it, OR record new replays (USER, windowed). This is the
  Phase-3/4/5 rate-limiter now — the easy oracle-gate-able work is done; the rest needs new scene reach.
