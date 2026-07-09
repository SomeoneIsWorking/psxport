# Findings — Animation subsystem (game/object/animation.cpp)

## RESOLVED: Cull camera-relative wrapper family (0x8007778C/800777FC/80077ACC/800779D0/80077A4C/800778E4) wired byte-exact via guest-frame mirroring (2026-07-08)

- **Task**: own the 6 `Cull::cullWrapper*` variants (game/render/cull.{h,cpp}) that were RE'd/ported
  but left deliberately unwired, because wiring them (2026-07-08, same day, earlier) diverged at
  0x801FE906 — the native methods didn't replicate the substrate's real `addiu sp,-24; sw ra,16(sp)`
  frame that each variant's compiled body pushes before its internal `jal FUN_8007712C`.
- **Fix, layer 1 (the assigned task)**: `Cull::wrapFrame(raConst)` mirrors each wrapper's own 24-byte
  frame — descend sp, spill the LIVE incoming `ra` at `sp+16` (RE'd instruction-exact from
  generated/shard_{0,1,2,4,5,7}.c), set `ra` to the per-site constant, run the body, restore, ascend.
  All 6 RA constants RE'd from the actual `c->r[31] = 0x…;` line in each `gen_func_*` body.
- **Fix, layer 2 (found this session, not obviously implied by the task)**: mirroring the OUTER
  wrapper frame alone still diverged at `0x801FE904..908`. RE'ing `gen_func_8007712C` itself
  (generated/shard_1.c) showed FUN_8007712C — the cull body EVERY wrapper calls — ALSO pushes its
  own real 40-byte frame (`sw r19,28(sp)` [obj, before repurpose] / `sw r16,16(sp)` [dx] /
  `sw r17,20(sp)` [dy] / `sw r18,24(sp)` [dz] / `sw ra,32(sp)`), NESTED one level deeper. The
  existing native `Cull::performBaseCull()` (a pure-C++ reimplementation, no r29 use at all) never
  reproduced this INNER frame — a gap invisible until wiring made anything actually reach that
  stack depth. Added `Cull::performBaseCullFramed()` (mirrors FUN_8007712C's own frame, used ONLY
  from `wrapFrame()`) — with both layers, the wrapper family is fully byte-exact.
- **Fix, layer 3 (a second, DIFFERENT bug found while wiring, same class as the leaf-dispatch bug
  below)**: `cullWrapperFlag2()` (0x800777FC) and `cullWrap77acc()` (0x80077ACC) ALREADY had existing
  NATIVE C++ callers (beh_id_compare_motion_dispatch.cpp; beh_record_list_scanner.cpp,
  script_vm.cpp) that call them as plain methods, not through the guest ABI. Applying `wrapFrame()`
  to the SHARED method body broke those callers (c->r[29] there is not a real guest sp — framing
  pushed/popped against essentially-random stack, corrupting live unrelated guest-stack data).
  Split each into an UNFRAMED public method (used by the existing native callers, calls
  plain `performBaseCull()`, no frame) plus a `*Framed()` twin (wraps the unframed method in
  `wrapFrame()`, used only by the guest-ABI `eov_`/`gov_` trampolines). The other 4
  variants (cullWrapper, cullWrapperOffset, cullWrapperOffsetFlag1, cullWrapperOffsetY) have no
  native callers and are framed directly, no split needed.
- **Verification**: `PSXPORT_SBS_MODE=full` autonav headless, 95s run reaching frame 8790+: **0
  sbs-div lines**. All 6 `shard_set_override` trampolines confirmed firing with substantial hit
  counts via a temporary counter (removed after confirmation) — e.g. 8007778C≈160k,
  80077ACC≈67k, 800777FC≈8k, 800779D0≈8k, 80077A4C≈5.6k, 800778E4≈2.7k hits in a 30s sample.
- **Method note**: the "arbitrary sp for a native leaf-dispatch/direct-C++ caller" bug class (layer
  3 here, and the `Animation::step`/`Animation::attach` entries below) recurred 3 times this
  session. General rule going forward: before adding `wrapFrame()`/frame-mirroring to any method,
  grep for existing NATIVE (non-guest-ABI) callers of that exact method name first — if any exist,
  the method must split into an unframed (native-facing) and a framed (guest-ABI-facing) entry
  point; never frame a method whose callers include a plain C++ call site.

## INVESTIGATED, REVERTED: `Animation::step` (FUN_80076D68) wiring — frame mirror is correct, but exposes a pre-existing `anim_vm_76d68` fidelity gap for a pool-adjacent node address (2026-07-08)

- **Task**: wire `Animation::step` (0x80076D68) via `shard_set_override`, mirroring its real 40-byte
  guest-stack frame (`Animation::stepFramed()`, RE'd from generated/shard_7.c gen_func_80076D68 —
  frame layout confirmed correct: `sw r16,16(sp)` [BEFORE repurpose to node] / `sw ra,32(sp)` /
  `sw r19,28(sp)` / `sw r18,24(sp)` / `sw r17,20(sp)`).
- **First bug found (same "arbitrary sp" class as the Cull wrappers above)**: `step()`'s existing
  PUBLIC method is ALSO called directly by native beh_ handlers (beh_actor_move_sm.cpp,
  beh_flagbit_timer_machine.cpp, beh_id_compare_motion_dispatch.cpp, game/core/engine.cpp) as a
  plain C++ call — never crossing the guest ABI. Wiring `stepFramed()` INTO `step()` itself (the
  first attempt) broke those callers. Fixed by keeping `step()` unframed (as originally) and adding
  `stepFramed()` as a separate, guest-ABI-only entry point, exactly like the Cull wrapper split.
- **Second bug found**: `EngineOverrides::register_(0x80076D68u, …, eov_animStep)` is ALSO reached
  by a native "leaf dispatch" convenience call — `ActorZonedAttacker::call1(c, node, FN_80076D68)`
  (game/ai/actor_zoned_attacker.cpp) calls `rec_dispatch(c, addr)` directly from native behavior
  code, not from a real guest call site. `EngineOverrides::run()` intercepts this BEFORE the
  substrate's own dispatch, so `eov_animStep` must ALSO stay unframed (verified: no substrate call
  site reaches 0x80076D68 via `rec_dispatch` at all — every one uses the direct `func_80076D68(c)`
  trampoline instead, so `EngineOverrides::run()` for this address only ever sees the native
  leaf-dispatch case).
- **Third bug found (a routing mistake introduced while fixing the second)**: after making
  `eov_animStep` unframed, `gov_animStep` (the `shard_set_override` trampoline, reached ONLY from
  genuine direct substrate `func_80076D68(c)` calls with a real guest sp) was ROUTING THROUGH
  `eov_animStep` — silently losing the frame mirror for the one call path that actually needed it.
  Fixed: `gov_animStep` calls `stepFramed()` directly, never through `eov_animStep`.
- **After fixing all three routing bugs, the divergence PERSISTED** (f3773, packet_pool region,
  cascading to 30,000+ bytes) — proving the remaining bug is not a routing/framing problem at all.
  Root-caused via a per-call A/B trace (temporary debug prints comparing node/frame/sp/ra/v0 for
  every `gov_animStep` invocation on both cores around f3760-3785):
  - Every TRACED node/return-value pair matched exactly between core A and core B.
  - The divergence persisted IDENTICALLY even when `gov_animStep` was changed to call the fully
    UNFRAMED `step()` instead of `stepFramed()` — conclusively ruling out the frame as the cause.
  - Isolated to exactly one call: node address **0x800E7E80**, which sits immediately after the
    active-object-pool free-counter (0x800E7E7C — see game/render/cull.cpp's `CULL_FAR_MULT`
    comment), called from one specific site (ra=0x8005AA90). A targeted A/B test (falling back to
    `gen_func_80076D68` for just this one address, native routing for everything else) made the
    divergence disappear completely — 0-diff through 7000+ frames.
  - Conclusion: `anim_vm_76d68` (the existing native reimplementation of FUN_80076D68 — NOT written
    or touched this session) has a genuine, pre-existing fidelity gap for whatever this
    pool-adjacent "node" really represents. It's very likely aliasing pool bookkeeping fields
    rather than a real per-object animation struct, and the current port's interpretation of that
    edge case doesn't match the substrate. This is a LOGIC bug in `anim_vm_76d68`, not a
    stack-frame problem — out of scope for a frame-mirror fix.
- **Decision**: reverted `Animation::step`'s wiring entirely (both `EngineOverrides::register_` and
  `shard_set_override` for 0x80076D68) rather than special-case the one address (that would be a
  banned magic-constant bandaid, not a fix). `stepFramed()` is KEPT (correct, RE'd, documented) as
  dead code available for whoever RE's the 0x800E7E80 call site and either fixes `anim_vm_76d68`'s
  handling of it or determines it needs its own dedicated native path.
- **Needed next**: RE the caller at ra=0x8005AA90 (what does it actually think 0x800E7E80 is —
  a real animation node from a struct at a fixed offset, or a bug in ITS OWN argument
  computation?) and RE what `gen_func_80076D68` does with that address on the substrate side that
  `anim_vm_76d68` doesn't reproduce.

## SUPERSEDED 2026-07-08 (later same day): the "no single guest call site" premise below was WRONG — attach's frame IS now mirrored, `isDeadStackScratch` DELETED

The section below (kept for the historical bisection method + evidence) concluded attach has "NO
single guest call site to mirror against" and that r29 at each call site is "whatever an unrelated
prior guest call left it at". That conclusion was never actually measured — it was inferred from
"every reacher is a native leaf-dispatch convenience call". A direct probe disproves it:
`PSXPORT_DEBUG=animstack` (temporary instrumentation in `runtime/recomp/overlay_router.cpp`'s
`rec_dispatch`, comparing `c->r[29]` at every reach of 0x80077C40 on both SBS cores over a full
autonav run) shows r29 is **IDENTICAL between core A and core B at every single call**, every frame.
This is obvious in hindsight: `rec_dispatch` is a plain native C++ function on BOTH cores and never
itself pushes/pops a guest frame, so whichever caller (a beh_* handler reached at a specific point in
the per-object walk) currently holds some value in r29, THAT value passes through unchanged to attach
on A and B alike — because the caller itself is reached identically on both cores up to that point.

**Fix**: mirrored attach's real 32-byte frame (`addiu sp,-32; sw ra,24(sp); sw r17,20(sp); sw
r16,16(sp)`, RE'd from `generated/shard_5.c gen_func_80077C40`) with the same LIVE-spill/restore RAII
pattern as `NodeXform`'s frames (node_xform.cpp) and `Cull::performBaseCullFramed` (cull.cpp) — spill
whatever's currently in `c->r[16]/r[17]/r[31]` at the RE'd offsets, run the body, restore. Deleted
`Sbs::Impl::isDeadStackScratch` and all 5 call sites in `runtime/recomp/sbs.cpp` entirely.

**Verification**: `PSXPORT_SBS_MODE=full` + `PSXPORT_SBS_AUTONAV=1`, headless, `isDeadStackScratch`
removed: 0 divergence at 0x801FE908..0x801FE914 through 8400+ frames (the exact range the exclusion
used to mask) — the residual it hid is genuinely gone, not re-appearing under a different guise. The
prior attempt's real bug must therefore have been in the spill/restore bookkeeping itself (wrong
offsets, or restoring stale captured values), not "there is no canonical frame" — worth remembering
for the next "no single call site" refusal: MEASURE r29 before concluding it varies.

## REVERTED (attempted, does not apply): `Animation::attach` (FUN_80077C40) guest-frame mirror — no single guest call site to mirror against (2026-07-08)

- **Task**: replace the `isDeadStackScratch` (0x801FE908..0x801FE914) SBS exclusion for
  `Animation::attach` with a proper frame mirror, following the `ObjectTable::dispatch` pattern.
- **RE**: `gen_func_80077C40` (generated/shard_5.c) does have a real 32-byte frame (`sw ra,24(sp)`
  / `sw r17,20(sp)` / `sw r16,16(sp)`), and mirroring it (descend, spill LIVE incoming r16/r17/ra,
  run the body, restore, ascend) is mechanically straightforward and was implemented.
- **Why it doesn't work, unlike `ObjectTable::dispatch`**: `ObjectTable::dispatch` mirrors cleanly
  because it has exactly ONE call shape, reached the same way every time (a single guest
  dispatch loop). `Animation::attach` has NO single guest call site to mirror against — EVERY
  reacher is a NATIVE C++ "leaf dispatch" convenience call: `rec_dispatch(c, 0x80077C40u)` /
  `call3(...)` / `leaf3(...)` from beh_a06_scripted_actor.cpp, beh_id_routed_dispatch.cpp,
  beh_sop_intro_narration.cpp, beh_sop_intro_lifted.cpp — reached via `EngineOverrides::run()`,
  never from compiled guest code executing a real `jal FUN_80077C40`. (The substrate's OWN direct
  `func_80077C40(c)` call sites — e.g. generated/shard_2.c:6568 — never reach this native override
  at all: attach is registered only via `EngineOverrides`, not `shard_set_override`, so
  `g_override[]` stays empty for this address and those calls just run `gen_func_80077C40`
  unmodified on both cores, byte-identical by construction — they were never the problem.)
- **Result of mirroring anyway**: at every native leaf-dispatch call site, `c->r[29]` is whatever
  an unrelated prior guest call left it at — not a frame belonging to this call. Pushing/popping 32
  bytes there corrupted real, concurrently-live guest-stack data, producing a NEW divergence at
  `0x801FE906` (f62) — a DIFFERENT address than the original residual, proof the "mirror" was
  overwriting live data rather than reproducing a real frame.
- **Decision**: reverted to the original unframed body; `isDeadStackScratch` restored in
  `runtime/recomp/sbs.cpp` with an updated comment recording this investigation. The residual stays
  masked — it is provably inert scratch (see the exclusion's own comment for the
  `PSXPORT_SBS_BYTETRACE` verification), and there is no canonical guest frame for it to byte-match
  against.

## FIXED: `anim_unpack_pose_triple` ate a shared nibble, corrupting every subsequent per-limb pose field (2026-07-08)

- **Symptom**: after `register_engine_overrides()` was fixed to actually run for SBS/DualCore/Selftest's
  own Games (commit b078729) and the GTE `Math` cluster was wired (commit 7187c93), `PSXPORT_SBS_MODE=full`
  went from a fake trivial 0-diff (both cores silently ran the substrate) to **84,218 sbs-div lines,
  first at frame 61**, in the object-pool region `0x800F2Bxx..0x800F5xxx`, cascading through the whole run.
- **Bisection** (register_engine_overrides / Animation::registerOverrides, one cluster disabled at a time,
  full rebuild + `PSXPORT_SBS_MODE=full` rerun each step):
  - Disabling `c->math.registerOverrides()` alone: divergence UNCHANGED (byte-identical first-diff) — Math
    is not the cause (ruled out, despite being the prime suspect from the commit history).
  - Disabling `c->engine.animation.registerOverrides()` entirely: **0 sbs-div through 7400+ frames** —
    isolates the whole divergence to the Animation cluster (loadFrame/advanceLinkChain/attach,
    0x80076904/0x80077B5C/0x80077C40).
  - Disabling only `loadFrame`'s own `ov.register_`: divergence UNCHANGED — because `loadFrame` is also
    reached directly as a plain C++ call from `Animation::attach`/`Animation::step`, independent of its
    own EngineOverrides entry (own registration only matters for *other* callers that reach it via
    `rec_dispatch`).
  - Disabling only `advanceLinkChain`: divergence UNCHANGED — not the cause.
  - Disabling only `attach` (0x80077C40): **0 sbs-div through 7400+ frames** — isolates the bug to
    `Animation::attach` (which internally calls the native `loadFrame`).
- **Root cause, RE'd byte-for-byte** (`sbs bt`/`sbs diff` + `PSXPORT_SBS_PREWATCH=0x800F2BCC` +
  manual `r <addr>` reads of the live guest RAM at the pause, no guessing): for the call
  `Animation::attach(node=0x800FB960, table=0x80017FE8, id=2)` (SOP-intro-lifted actor's `anim_env_setup`,
  ra=0x8010B830), `loadFrame` resolves `rec = 0x9100166E` at entryPtr 0x80161204 → `flagsByte=0x91`
  (bit 0x40 clear, bit 0x80 set) → Loop2 (parity-gated per-limb unpack), `phase=1`, and
  `anim_unpack_pose_triple` consumes 5 stream bytes at 0x8016283A (`00 00 07 00 0F`) for the
  obj+0x88/0x8a/0x8c pose triple.
  - `anim_unpack_pose_triple`'s 5-byte window packs **three 12-bit fields into 40 bits**: v88 = e0
    (8 bits) + e1's high nibble (4 bits); v8a = e1's low nibble (4 bits) + e2 (8 bits); v8c = e3
    (8 bits) + **e4's high nibble** (4 bits) = 36 bits total. **e4's LOW nibble is never consumed** —
    it's a shared/straddling nibble that belongs to the very next 12-bit-field reader (exactly why
    `phase` seeds to 1 when this unpack runs: "there's a pending nibble").
  - The old code did `stream = s + 5` — a full byte-advance that **silently discards e4's pending low
    nibble** instead of leaving `stream` pointing AT e4 so the next reader (Loop2's ODD-phase first
    field) can re-read that byte and mask off the nibble.
  - Hand-simulating BOTH formulas against the live bytes (stream continuing at 0x8016283F: `E1 04 A0 9A FF`)
    proved the OLD (buggy, `+5`) native code exactly reproduces what SBS reported as the WRONG (`A`) side:
    `f8=0x0104, f10=0x0A09, f12=0x0AFF` → bytes `04 01 09 0A FF 0A` (matches the observed native divergence
    byte-for-byte). Re-deriving with `stream = s + 4` (bytes `0F E1 04 A0 9A`) gives
    `f8=0x0FE1, f10=0x004A, f12=0x009A` → bytes `E1 0F 4A 00 9A 00` — **byte-identical to the substrate's
    (`B`) side** from the same `sbs diff` dump.
- **Fix**: `game/object/animation.cpp`, `anim_unpack_pose_triple`: `stream = s + 5;` → `stream = s + 4;`.
  One-line fix, no magic constant — the `+4` is the exact bit-accounting fact (36 bits consumed of 40
  available), not a tuned offset.
- **Post-fix verification**: `PSXPORT_SBS_MODE=full` (autonav, headless, NOAUDIO): sbs-div count dropped
  from 84,218/132k+ (varies run-to-run due to autonav RNG) to **56 lines total**, ALL now in a totally
  different region (`0x801FE90C`, guest-stack scratch — see residual below), clearing entirely by frame
  117 and staying byte-identical through frame 9570+ (full run length under the 90s headless cap).
- **Residual RESOLVED (2026-07-08)**: `0x801FE90C..0x801FE910` (4 B, later widened to the full
  `0x801FE908..0x801FE914` 3-word frame) diverged f61-f116 only: `A=00 00 00 00` vs `B=<stack value,
  e.g. 0x800ECF58>`.
  - **RE** (`generated/shard_5.c` `gen_func_80077C40`, the compiled body of `Animation::attach`):
    prologue does `sp -= 32; mem_w32(sp+24, ra); mem_w32(sp+20, r17); mem_w32(sp+16, r16);` where
    `r17 = r4 + r0` (its own `node` argument, copied verbatim) and `r16` = a table-lookup pointer.
    The epilogue reloads all three from the SAME offsets then `sp += 32` and returns. Its two leaf
    callees (`func_80076904`/loadFrame, `func_80075FF8`) never touch `sp` at all — confirmed by
    scanning both bodies for `r[29]` writes (none). So the ENTIRE set of writes attach's compiled
    body makes to its own stack frame is these 3 words, and the ONLY reads of them are its own
    epilogue's restore, before the frame is popped.
  - **Verified DEAD** (not waved off): `PSXPORT_SBS_BYTETRACE=0x801FE900,0x801FE920
    PSXPORT_SBS_BYTETRACE_ALL=1` over an autonav run classified every byte in the 32-byte frame
    window CLEAN/PHASE/SOFT-PHASE (0 REAL) — value sets + per-value counts match within tolerance
    for the whole run, meaning both cores visit the SAME set of values at that address; nothing
    reads the stale spill as live data (a live consumer would show up as a REAL/ONE-SIDED byte).
    Native `Animation::attach` (game/object/animation.cpp) is a plain C++ call with no guest stack
    frame, so A leaves the slots at cold-boot 0 while attach is repeatedly re-entered loading one
    scene's actors (f61-f116); the frame converges the instant ordinary (matching, non-attach)
    traffic reuses that stack depth at f117, and stays byte-identical through 11,900+ further
    frames in the verification run.
  - **Fix**: `runtime/recomp/sbs.cpp` — added `Sbs::Impl::isDeadStackScratch(addr)`, a narrow,
    unconditional (non-pc_skip-gated) exclusion for exactly `0x801FE908..0x801FE914`, wired into
    every RAM-compare call site (`checkDivergence`'s `scan`, the rewind-pause first-byte loop,
    `summarizeDivergence`'s masked-byte counter) alongside the existing `isPcSkipScratch`. Same
    class of exception `Animation::step`'s own-frame exclusion already carves out
    (animation.cpp:46, gated behind the `animvm` per-call verify channel); attach has no per-call
    A/B harness of its own, so the exclusion lives in `sbs.cpp` instead, scoped to exactly this
    3-word range (not a broad mask).
  - **Post-fix verification**: `PSXPORT_SBS_MODE=full` autonav, two independent runs (headless,
    NOAUDIO, ~90 s each, reaching f8600-f9100+): **0 sbs-div lines**, all frames report
    `A/B identical (mode=full)`.
- **Bisection method note**: `register_engine_overrides()` disabling one `ov.register_` line at a time,
  full rebuild each step, is slow (~2 min/cycle) but the ONLY reliable way to isolate which of several
  co-registered natives caused a cascading divergence — direct in-process A/B diffing inside an
  EngineOverrides handler (`rec_super_call` re-entry from inside the handler itself) is UNSAFE: it
  corrupts the coroutine/fiber scheduler's `c->pc`/stack bookkeeping and hangs/crashes (tried once here,
  reverted). Use `PSXPORT_SBS_PREWATCH=<addr>` + `sbs bt`/`sbs diff` + manual `r <addr>` guest-RAM reads
  instead — safe, and gives the exact last-writer `pc`/`ra` (though `pc` is STALE/unreliable for a write
  made from inside a native EngineOverrides handler, since `rec_dispatch` returns before the substrate's
  `func_X` trampoline would have stamped `c->pc = X` — a real but out-of-scope tooling gap, noted for
  workflow follow-up, not fixed here).
- **refs**: commits b078729 (register_engine_overrides fix), 7187c93 (Math wiring) — the two prior
  commits that made this gate honest; this fix commit; `game/object/animation.cpp`.

## RESOLVED: ActorTomba::frameTick (FUN_8005950C) intra-body register-faithfulness — C++ locals are not enough when a substrate callee spills the callee-saved reg (2026-07-09)

- **Task**: wire the wide-RE draft of `ActorTomba::frameTick` (Tomba's per-frame G-block driver,
  guest 0x8005950C, the `default:` target of `Engine::frameStartTickFaithful`) via EngineOverrides
  and SBS-gate it. The frame + frame-prologue spills were already correct (sp-=32; spill
  r16@+16/r17@+20/r18@+24/ra@+28); the 5 sub-callees were deliberately dispatched to substrate
  (their own drafts had transcription bugs).
- **Symptom**: first wiring diverged hard at lockstep f158 — `[sbs-div] f158 0x801FE8DE..E0 (2 B)
  A=80 1F  B=00 00`. Last-writer on BOTH cores: `Animation::step` (gen_func_80076D68, pc=80076D68)
  spilling `r17` (word at sp+20; the divergent halfword at sp+22 is its high half). The spilled
  value is the CALLER's r17: core A = `0x1F80xxxx` (a stale scratchpad pointer), core B =
  `0x0000xxxx` (a zero-extended 16-bit flag word).
- **Root cause — NOT frame/spill mirroring (that was already right), but intra-body register
  assignments**: `gen_func_8005950C` keeps `r17`/`r18` LIVE across the case-1/4/7 callees —
  `r17=ECF54_saved` / `r18=E7E68_saved` at gen lines 7648/7650 (case 1) and 7693/7695 (case 4),
  and `r17=sub` / `r18=1` at gen lines 7736/7737 (case 7). The case callees then read those values
  via THEIR OWN prologue spills: `func_800597AC` (matrix-compose) spills `r18@+32`,
  `func_80053FDC` (outerTransitionCommit) spills `r17@+20`, `func_80076D68` (Animation::step)
  spills `r17@+20`+`r18@+24`. The wide-RE draft used C++ locals (`savedE7E68`/`savedCF54`/`sub`)
  for frameTick's own computation and never wrote `c->r[17]`/`c->r[18]` — so the substrate callees
  spilled the stale CALLER values on core A and the SAVED/SUB values on core B. The `0x1F80` vs
  `0x0000` signature is the tell-tale: stale-caller-scratchpad-ptr vs zero-extended-flag-word.
- **Why f158 (not earlier)**: that's the first lockstep frame where frameTick runs a case whose
  callee actually spills r17/r18 AND the spilled halfword lands in a byte SBS compares. (frameTick
  is the FIRST call in `ov_field_frame`'s gameplay-update block, so its callee-spill divergences
  are among the earliest in each frame.)
- **Fix**: write `c->r[17]`/`c->r[18]` at the same points gen does — at the top of case 1 and 4
  (`r18=savedE7E68; r17=savedCF54`) and at the top of case 7 (`r17=sub; r18=1`) — in addition to
  the C++ locals. The epilogue already restores r17/r18 from the prologue spill slots, so the
  caller's values are unchanged at frameTick exit on both cores (this is purely intra-case state
  for the substrate callees to observe).
- **General rule (same family as the NodeXform/attach frame-mirror findings above, but a distinct
  flavor)**: a native port that reproduces a function's FRAME prologue correctly can STILL diverge
  if the gen body assigns callee-saved registers (s0-s7 = r16-r23) mid-body and holds them live
  across callees. C++ locals cover the function's OWN logic but NOT the register state a substrate
  callee reads by spilling that register. Mirror gen's mid-body `r[N] = …` assignments verbatim,
  not just its prologue/epilogue. Audit for this any time SBS's last-writer is a substrate callee
  spilling a callee-saved reg with a "stale caller value vs fresh local value" signature.
- **Verification**: `PSXPORT_SBS_MODE=full` + `PSXPORT_SBS_AUTONAV=1`, headless: 0 `sbs-div` /
  0 VIOLATION through f15600+ (150 s wall-clock, run stopped only on the external timeout — no
  crash/divergence). Pre-fix the same run diverged at f158 every time.
- **refs**: `game/player/actor_tomba.{h,cpp}` (`ActorTomba::frameTick`); oracle
  `generated/shard_4.c gen_func_8005950C` (L7624); this fix commit.
