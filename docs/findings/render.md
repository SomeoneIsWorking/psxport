# Findings — render / engine submit

## perobj_billboard cluster (C2D4/C464/C8F4) — BUF base + register-faithfulness (2026-07-09, RESOLVED)

- **how found**: the oracle-gate fix (commit 5483a83, `engine_override_thunk`) made SBS honest for the
  `g_override[]` render clusters. Post-fix `PSXPORT_SBS_MODE=full` immediately surfaced 19 `[sbs-div]`
  at f117 in the packet pool (0x800BFFxx) + scratchpad — exactly the writes the false 0-div had hidden.
- **bisected** with `PSXPORT_THUNK_FORCE_GEN` (force a cluster to gen even on core A): disabling the
  billboard leaves cleared f117 → billboard; `C2D4`-only force-gen left packet data divergent →
  `billboardEmit` (C8F4, reached direct-C++ on core A so the thunk hit-counter showed 0 native).
- **bug 1 — RESOLVED (commit a457082)**: the whole cluster wrote its MATRIX-compose + projected-coord
  buffer to MAIN RAM `0x800C0000`, but `gen_func_8003C2D4`/`8003C8F4` base `r16/r17 = 8064<<16 =
  0x1F800000` (SCRATCHPAD). Emitted packets (copied BUF+4..+36) therefore differed from the substrate.
  Single-constant fix: `BUF = 0x1F800000`.
- **bug 2 — RESOLVED (commit bef7769)**: register-faithfulness. `gen_func_8003C8F4` spills the
  caller's r16..r22/ra at sp+64..+92 (native GuestFrame only allocated the frame). And the caller
  `billboardCompose1/2` (C2D4/C464) keeps specific callee-saved regs live to the C8F4 call (C2D4:
  r16=MAT_OUT/r17=MAT_A/r18=flag/r19=node/ra=0x8003C448; C464 differs: r17=flag/r18=node/ra=
  0x8003C5E0) — native used C++ locals and never set them, so C8F4's spill of "caller r17" got stale
  values. Fix: native C8F4 spills caller r16..ra at gen's offsets; C2D4/C464 set c->r[16..] to gen's
  values before the call AND restore them from the spill slots at epilogue (the restore is mandatory —
  leaking r31 corrupts the substrate render-walk caller). Callees (func_8003B220/B054) are leaves that
  spill no callee-saved regs, so only the prologue spills + pre-call register state matter.
- **verified**: f117 fully clean.

## overlay_ground_gt3gt4 cluster (8013FB88/8013FE58/801401B8) — depth >>2 + range gate (2026-07-09, packet-pool RESOLVED; f118/f62 register-faithfulness RESOLVED 2026-07-10; gt3/gt4 DATA divergence OPEN — see bottom)

- **how found**: once billboard's f117 cleared, f117→f118 in the packet pool. Last-writer: native
  `entityLoop` (801401B8) writing on core A vs gen `gt3` (8013FB88) on core B — native `gt3` is called
  direct-C++ from `entityLoop` (bypasses the thunk, like billboard's C8F4), so it had never been
  oracle-compared until the gate fix.
- **bug 1 — RESOLVED (commit ffb1463)**: `sz3_minmax`/`sz4_minmax` returned the RAW min/max, but gen's
  manual min/max depth path applies a trailing arithmetic `>>2` at the convergence labels
  (`r2 = r3 >> 2`, FUN_8013FB88 L_8013FD38 / gt4 L_80140100) before storing to the OTZ scratch. 4x too
  large → records failed the `idx>=2048` gate → dropped (pool offset shift). (AVSZ3/AVSZ4 paths
  correctly have NO >>2 in both — only the manual path.)
- **bug 2 — RESOLVED (commit ffb1463)**: `ground_otz_index` mis-split gen's single upper-bound gate
  `(idx-4) < 2044` into a two-sided range [4..2047] — spurious lower bound + off-by-one. Matched gen.
- **verified**: f118 PACKET POOL clean (render packets now match the substrate).
- **stack-depth divergence at f118 — RESOLVED (2026-07-09)**: root cause was TWO compounding bugs in
  the `perObjRenderDispatch` -> `cmdListDispatch` -> `perModeDispatch` chain, both missing from the
  2026-07-08 landing:
  1. **register-faithfulness gap**: `gen_func_8003CCA4` (perObjRenderDispatch) sets `c->r[31]` to an
     RE'd return-address CONSTANT before every nested call (`r31=0x8003CD08` before `func_8003CDD8`,
     etc. — 8 call sites total across its 5 cases); `gen_func_8003F698` (perModeDispatch) does the same
     before every `rec_dispatch` (11 case labels, each `caseLabel+8`) and before the generic
     `func_800803DC` fallback (`r31=0x8003F790`). The native ports never set `c->r[31]` at all, so
     whatever call reached `FUN_80146478` (the field overlay GT3/GT4 dispatcher, itself a real
     `addiu sp,-32` frame that spills its CALLER's live `ra`) spilled a STALE r31 leaked from the
     outermost unowned caller (`FUN_8003C048`) instead of the RE'd chain value — the actual f118
     divergence bytes (0x801FE8D0..0x801FE8F6, `FUN_80146478`'s own ra/r16/r17 spill slots). Fixed by
     setting `c->r[31]` to the literal RE'd constant at every call site in both functions (see
     `Render::perObjRenderDispatch`/`perModeDispatch`).
  2. **oracle-purity leak (the deeper bug — CRITICAL)**: `cmdListDispatch`/`perModeDispatch`
     (`0x8003CDD8`/`0x8003F698`) were installed via the RAW `shard_set_override` (2026-07-08 landing),
     not the oracle-gated `engine_set_override_main` — exactly the failure mode
     `runtime/recomp/engine_override_thunk.cpp`'s own banner warns about ("clusters that forget it...
     silently broke the oracle"). `g_override[]` is process-global, so SBS core B (the pure-substrate
     oracle) was ALSO running this native code whenever `gen_func_8003CCA4` (correctly oracle-gated)
     called `func_8003CDD8(c)` — core B was comparing native-vs-native for this pair, not
     native-vs-gen. Confirmed via `engine_override_thunk`'s per-address hit-count dump
     (`PSXPORT_SBS_PREWATCH`+`PSXPORT_SBS_WW_ONVALUEDIVERGE=1`): after fixing bug 1 alone, the f118
     write showed BOTH cores at pc=`8003CCA4`/`8003CDD8` with matching sp/ra — i.e. B was running the
     SAME native code A was. Fixed by switching `perobj_dispatch_install()` to
     `engine_set_override_main` (matching every sibling cluster:
     perobj_billboard/overlay_gt3gt4/overlay_ground_gt3gt4/quad_rtpt_submit) and adding real guest-stack
     frames (`CmdListFrame`/`PerModeFrame`, RE'd from `gen_func_8003CDD8`'s -56-byte and
     `gen_func_8003F698`'s -24-byte prologues) so B's now-pure gen body and A's native body descend the
     same sp.
  3. Also found (same audit): `perObjRenderDispatch`'s case `0x8003CD60` had an INVERTED branch
     polarity (`node+27==0 -> func_8003F3F4` in the native draft; gen is
     `node+27==0 -> func_8003F4C4`). Neither leaf fires at seaside, so autonav never caught it; fixed
     to match gen exactly.
  - **verified**: SBS-full autonav no longer diverges at f118 (confirmed clean through the frame range
    that previously failed there).
  - **f62 divergence — RESOLVED (2026-07-10)**: root cause was a THIRD register-faithfulness gap in the
    SAME family as bug 1 above, one level deeper. `gen_func_8003CDD8`'s loop keeps `r16..r23` LIVE as
    loop-invariant/loop-index scratch for its **entire** loop body — `r16=i` (the loop counter,
    incremented post-call at `L_8003D07C`), `r17=r23=SCR` (scratchpad base `0x1F800000`), `r18=node`,
    `r19=SCR+0xD0`, `r20=OTBASE_PTR` (`0x800ED8C8`), `r21=WORLD_POS` (`SCR+0xC0`), `r22=flag` — verified
    line-by-line against `generated/shard_6.c` (lines ~5119-5285). These survive the nested
    `func_8003F698`/`func_800803DC` call chain via plain MIPS callee-save (gen never explicitly reloads
    them before each per-iteration call — they're just left live in the register file). The still-
    substrate `func_800803DC` (unowned generic GT3/GT4 emitter, shared code on both cores) SPILLS the
    incoming `r16`/`r17` to its own guest-stack frame (`sp+16`/`sp+20`) before reusing them as locals,
    then restores them on return — i.e. their CALLER value is genuine guest-stack-visible state, not
    dead scratch. Native `Render::cmdListDispatch` used C++ locals for `i`/`node`/`flag` and never wrote
    `c->r[16..23]`, so `func_800803DC`'s prologue was spilling STALE leftover register content instead
    of gen's real loop state — exactly the observed diff (`A=0x800FB858/0x800FB960` stale garbage vs
    `B=0x00000010/0x1F800000` = real loop-index/SCR values). Fix: `Render::cmdListDispatch` now sets
    `c->r[16..23]` to gen's live values immediately before every `perModeDispatch()` call (not just
    `r16`/`r17` — the mode-table path can reach OTHER still-substrate per-mode renderers that may
    equally depend on this callee-save state). **Verified**: the specific `0x801FE870..0x801FE878`
    diff is gone from the SBS-full gate; the GTE-compose audit from the previous session (which found
    no discrepancy) was correct — the bug was never in the compose math, only in this uninitialized-
    register spill.
  - **f118 divergence — PARTIALLY RESOLVED, residual OPEN (2026-07-10)**: fixing f62 advanced the SBS
    gate from ~156k div lines/run to ~85k and moved the frontier back to the (previously masked) f118
    stack region. Root cause (part 1, FIXED): `Render::perObjRenderDispatch` (`FUN_8003CCA4`) used the
    bare `GuestFrame` RAII (sp-adjust only, no register spill) for its `-32` frame, but
    `gen_func_8003CCA4`'s real prologue spills the caller's live `r16/r17/r18/r31` to guest memory at
    entry (`sp+16/20/24/28`) and restores them at every exit (`L_8003CDC0`) — a plain MIPS callee-save
    prologue the bare RAII never reproduced, leaving stale bytes at `0x801FE8D0..0x801FE8F6`-class
    addresses (this function's own ra/r18 spill slots) instead of the caller's real values. Fixed with a
    dedicated `CCA4Frame` struct (mirrors `CmdListFrame`'s save/spill/restore idiom) in
    `game/render/perobj_billboard.cpp`, wired into `perObjRenderDispatch`. **Verified**: the
    `0x801FE8E8..0x801FE8F6` (r18/ra) divergence is gone.
  - **f118 residual — RESOLVED (2026-07-10, owning `gen_func_8003C048`)**: root cause was exactly what
    the OPEN note below predicted — `gen_func_8003C048` (the render-WALK loop) was the outermost
    UNOWNED caller leaking stale r16/r17/r18/r19 into every downstream spill. Owned it as
    `Render::renderWalk` (`game/render/render_walk_dispatch.cpp`, new file, `WalkFrame` mirrors the
    real `-112` prologue/epilogue) via `engine_set_override_main`. Instruction-exact transcription of
    `generated/shard_7.c gen_func_8003C048`: walks the global render-node list (head @0x800F2624, next
    ptr @node+36), dispatches live nodes (mem8(node+1)!=0, case idx mem8(node+11)<33) through a
    33-entry table at **0x80014DB8** (adjacent to CCA4's own 9-slot table at 0x80014EC8 — part of one
    shared jump-table data region) to the owned siblings (perObjRenderDispatch/billboardCompose1/2,
    called natively — keeping r16=node/r17=next/r18=CASE188_SCR(0x1F8000F8)/r19=JUMP_TABLE live in the
    real registers throughout, exactly as gen does) or still-substrate leaves (func_8003F174/EF9C/
    80039F4C/800726D4/C5F8/C788, rec_dispatch to 0x8012A43C/801295B4/80129114/8013DD58, a "generic
    particle" case 0x8003C188, and a fully dynamic per-node dispatch through node+24, case 0x8003C29C).
    **Dead end (caught before landing):** an early draft computed `JUMP_TABLE` as `0x800104B8` — a
    plain hex-addition slip (`0x80010000 + 0x4DB8` written out wrong; correct is `0x80014DB8`). That
    address happens to land inside a completely UNRELATED jump table belonging to another function in
    `shard_1.c`, and following it crashed with `recomp-MISS 0x80035F4C` — caught by dumping the live
    table content and cross-checking against the static EXE bytes at that offset before it ever reached
    the SBS gate as a false "residual."
  - **f62/f118 second layer — RESOLVED (2026-07-10, in `perObjRenderDispatch`)**: even with `renderWalk`
    owned and feeding correct r16-r19, `CmdListFrame`'s r18 spill (inside `cmdListDispatch`) STILL
    diverged (`PSXPORT_SBS_PREWATCH=0x801FE8B8`: core B's write traced to `gen_func_8003CDD8+0x18`,
    i.e. its OWN r18 spill, with the caller `gen_func_8003CCA4` holding `r18=node` per its real
    prologue `r18 = r4` immediately after ITS OWN spill — generated/shard_5.c:5060-5071). The EXISTING
    (pre-this-task) `Render::perObjRenderDispatch` never mirrored that reassignment — `CCA4Frame`
    correctly spilled/restored the CALLER's r18 (renderWalk's CASE188_SCR constant) but the function
    body itself never set `c->r[18] = node` for its OWN nested calls, so `cmdListDispatch`'s later
    "caller r18" spill got the wrong value once `renderWalk` started feeding a real (non-garbage)
    caller r18. Same root cause also affects `cmdListDispatch`'s `flag` parameter (`c->r[5]`): gen
    computes `flag = ((mem8(node+11) ^ 15) < 1)` ONCE early in `gen_func_8003CCA4` and it stays live
    (plain register lifetime) into every case's `func_8003CDD8` call — the native body never set
    `c->r[5]` at all. **Dead end (caught before landing):** the first fix attempt read the flag from
    `mem8(node+13)` (confusing it with the ADJACENT `sel` field, which legitimately does use node+13)
    — this made `perModeDispatch`'s `flag&1` test route the WRONG WAY for some nodes (native took the
    per-mode-table path where gen took the generic `func_800803DC` fallback, confirmed via
    `PSXPORT_SBS_PREWATCH` showing core A ending in `ov_a00_gen_80146478` vs core B in
    `gen_func_800803DC`); fixed by re-reading gen's exact source (node+11 for flag, node+13 for sel).
    Fix: `perObjRenderDispatch` now sets `c->r[18] = node` and computes `flag` once right after
    `CCA4Frame`'s construction, and every case that calls `cmdListDispatch()` sets `c->r[5] = flag`
    before the call (`game/render/perobj_billboard.cpp`).
  - **verified**: SBS-full autonav no longer diverges anywhere in the `0x801FE8xx` task-0-stack range
    or in the packet pool up to f157 (previously the first divergence was f118; now clean through
    f157). `PSXPORT_DEBUG=ovhit` confirms `0x8003C048` fires (native=oracle call counts match exactly
    every run, e.g. native=88/oracle=88) — the override is genuinely wired and exercised, not a
    fake-green gate.
  - **`gen_func_8003D0BC` OWNED (2026-07-10) — `Render::overlayTypeDispatch`
    (`game/render/overlay_type_dispatch.cpp`, new file).** Per CLAUDE.md's "never debug through
    unowned code" rule, this dispatcher had to be owned FIRST before any gt3/gt4 data diff-hunt (it
    was the last fully-unowned link in the `0x8003D0BC -> 0x801401B8 (entityLoop) -> gt3/gt4` chain).
    RE: instruction-exact transcription of `generated/shard_7.c gen_func_8003D0BC` — a pure `-24`-frame
    integer dispatch (no GTE), no loop: reads `AREA_TYPE = mem8(0x800BF870)` (the same render-mode byte
    `perModeDispatch`/`renderWalk`'s case-0x8003C188 read), early-outs if `>=22`, else indexes a
    22-entry table at `0x80014EF0` (`32769<<16+20208`, adjacent to `renderWalk`'s own table at
    `0x80014DB8`) to 20 case labels, each `c->r[31]=<RE'd return constant>; rec_dispatch(c, target)`.
    The function's OWN body never touches `r4` — whatever the caller (`gen_func_8003F9A8`, which passes
    `SCENE_ENT_TABLE=0x800F2418`) set flows through unchanged, so the port correctly leaves `c->r[4]`
    alone (verified against gen: no r4 read/write anywhere in the function). Wired via
    `engine_set_override_main` (oracle-gated, matching every sibling in this band). Frame mirrored
    (`DispatchFrame`: sp-=24, spill/restore r31 at sp+16, exactly gen's prologue/epilogue).
    **Verified NOT a regression, NOT the residual's cause**: SBS-full still reaches the exact same first
    divergence at f158/0x800C133E as before ownership — confirmed via host backtraces showing
    `Render::overlayTypeDispatch` correctly calling into the already-owned `OverlayGroundGt3Gt4::
    entityLoop`/`gt3`/`gt4` on core A, while core B's backtrace shows the pure `gen_func_8003D0BC` body
    (oracle purity intact — no native code leaked onto core B).
  - **f158 gt3/gt4 "DATA divergence" — RE-NARROWED (2026-07-10): PARTIALLY FALSIFIED (2026-07-10,
    convergence-agent second pass) — see "f158 packet-pool residual — ACTUALLY FIXED" below for the
    real mechanism and fix. This entry's RAW OBSERVATION (gt4 performs two extra writes to the
    divergent dword that core B never performs) is CORRECT and was the key clue; its INTERPRETATION
    ("a genuine COUNT/DATA divergence in what counts resolves to... an upstream game-STATE
    divergence") was wrong — the counts/pool-cursor trajectory is byte-identical (confirmed by the
    later "ROOT-CAUSED" entry); the real cause is a write-ORDER/gating bug within gt3/gt4 themselves,
    not upstream state.** Original text preserved below for the record: NOT a gt3/gt4 field-math bug;
    it's an upstream packet-pool CURSOR-POSITION divergence.** `PSXPORT_SBS_PREWATCH=0x800C133E
    PSXPORT_SBS_WW_ONVALUEDIVERGE=0` (plain persist mode) traced every raw store to the watched dword
    `0x800C133C..0x800C133F` at f158 and its host C++ call stack:
    - Core A: 2 writes via `OverlayGroundGt3Gt4::gt4` (`mem_w32(pool+4, rgb0&COL_MASK_STD)`, values
      `0x5FC000C0` then `0x01FD5EFE` — TWO back-to-back GT4 loop iterations landing on the exact same
      pool address, i.e. the FIRST iteration's record was rejected — backface/OOB `continue` — before
      `pool += 52`, so the SECOND iteration's unconditional rgb0 write overwrote the same slot; this is
      correct/expected bump-allocator behavior, not a bug), then 1 write via `OverlayGroundGt3Gt4::gt3`
      (`mem_w16(pool+36, uv2hi)`, value `0x4097`, from a DIFFERENT record whose pool base happens to sit
      32 bytes earlier — again ordinary aliasing in a bump allocator, not a bug).
    - Core B: exactly ONE write to this dword the ENTIRE frame — the same `gt3` uv2hi write, SAME value
      `0x4097` (`ov_a00_gen_8013FB88`) — i.e. core B's gt3/gt4 pipeline computes the IDENTICAL field
      value A does, at the IDENTICAL pool address, when it actually runs. **Core B never performs the
      GT4 rgb0 writes A performs at this address at all** — its live-writer map at the frame-boundary
      pause (`sbs bt`) shows B's LAST writer to `0x800C133E/F` was `pc=80115598 ra=8003DF80` at **frame
      157** (a fully different, unrelated function, one frame EARLIER) — meaning core B's packet stream
      for frame 158 never reaches this byte offset again after the gt3 uv2hi write only touches the low
      half (`133C-133D`); the high half (`133E-133F`) is simply never revisited by ANY writer on core B
      in f158, so it keeps frame 157's leftover value (`80 7C`), while core A's extra GT4 iteration DOES
      overwrite it (`FD 01`).
    - **Conclusion**: `gt3`'s and `gt4`'s own per-field math is verified byte-identical where both cores
      actually execute it (confirmed above: same value, same address, same function). The real
      divergence is that core A's GT4 loop for this ground-entity record processes (at least) 2
      iterations (one rejected, one accepted) while core B's equivalent substrate execution apparently
      does not reach a second GT4 iteration at this pool position at all — i.e. a genuine COUNT/DATA
      divergence in what `counts = mem_r32(table+idx*4)` (the packed GT3/GT4 counts word `entityLoop`
      reads) resolves to, or in an EARLIER packet-pool consumer this same frame (all 7 functions
      `gen_func_8003F9A8` calls before `0x8003D0BC` — `0x8004FD30/80025D98/8003BF00/8003EEC0/8003B588/
      8003BB50/8003BCF4` — are confirmed **still fully unowned** via `tools/codemap.py`, so both cores
      run the byte-identical `gen_func_*` body for all of them; if one of those diverges it can only be
      because the GUEST STATE feeding it already diverged from something earlier, not from a
      register-faithfulness gap in this band). **Left OPEN** — do not speculative-patch `gt3`/`gt4`'s
      already-verified-correct field math; the next session should either (a) diff the raw `counts`
      word / ground-entity table content between core A and core B guest RAM at this exact call (a
      genuine upstream game-STATE divergence would show up there directly), or (b) RE `0x80115598`
      (core B's actual last legitimate writer to this address, one frame earlier) and audit whether IT
      is a still-unowned leaf feeding wrong sizes into the shared pool. A LATER, SEPARATE run (SIGINT
      after ~95s, no crash) shows the run stays alive and stable through f8580+ with only packet_pool
      bytes (+ occasional task-0-stack `?`-tag bytes, likely a downstream cascade of this same cursor
      divergence) diverging — i.e. the game no longer crashes or diverges catastrophically, just this
      one still-open data-correctness gap.
  - **f158 packet-pool residual — the "ROOT-CAUSED... GT3 tail-padding" entry below is FALSIFIED
    (2026-07-10, convergence-agent, second pass). The actual bug — RESOLVED, see the entry further
    below titled "f158 packet-pool residual — ACTUALLY FIXED".** Two prior same-day entries
    contradicted each other: the "RE-NARROWED" entry (further up this file) traced a genuine SECOND
    writer — `OverlayGroundGt3Gt4::gt4` performing TWO back-to-back writes to the exact divergent
    dword — while THIS "ROOT-CAUSED" entry (immediately below) claims gt3/gt4 "never write" that
    offset and blames inert PSX pad bytes. Both partially right, both partially wrong: this entry's
    static field-offset table (which bytes gt3/gt4 write) is correct, but its CONCLUSION — "no logic
    bug, it's the game's own uninitialized hardware padding" — is wrong, because its instrumentation
    only logged the (tableSlot, counts, pool_pre, pool_post) TUPLE per gt3()/gt4() CALL, never the
    per-FIELD write ORDER inside those calls relative to the reject/backface/on-screen/OTZ gates —
    so it could not see that `gt4`'s uv2/uv3 writes (and `gt3`'s uv0/uv1 writes) fire at a WRONG
    POINT in the gate sequence relative to gen, which is exactly what produces "extra" or "missing"
    dead-scratch writes for REJECTED records without ever touching the ACCEPTED-record counts/pool-
    cursor trajectory this entry verified (correctly) as byte-identical. Left as-is below for the
    historical record, but its "Disposition: NOT a bug... no patch applied" is FALSE — do not
    re-derive this dead end.
  - **f158 packet-pool residual — ROOT-CAUSED (2026-07-10, convergence-agent): [FALSIFIED — see
    retraction directly above] every f158 `sbs-div` address is the +38/+39 TAIL-PADDING of a GT3
    pool-slot record, carrying 2-FRAME-STALE content — NOT a logic bug in `gt3`/`gt4`/`entityLoop`
    (all three re-verified byte-identical, live, this session).** Method: temporarily instrumented BOTH `OverlayGroundGt3Gt4::entityLoop` (native,
    `game/render/overlay_ground_gt3gt4.cpp`) and `ov_a00_gen_801401B8` (oracle,
    `generated/ov_a00_shard_0.c`, gitignored — safe to hack for a session and revert) to print, gated
    on `c->game->sbs->frame()==158`, every ground-list entry's `tableSlot`/packed `counts` word and the
    `PKT_POOL_PTR` (`0x800BF544`) value before/after each `gt3()`/`gt4()` call. Both cores printed the
    EXACT SAME 714-line sequence of `(tableSlot, counts, pool_pre, pool_post)` tuples for the entire
    f158 ground-entity walk — i.e. **entityLoop's list membership, per-entity GT3/GT4 counts, and the
    resulting pool-cursor trajectory are already proven byte-identical for f158** (this directly
    supersedes the "39-record shortfall" line of inquiry for THIS frame — no count divergence exists
    here at all, on either accepted-record or attempted-record granularity).
    - Cross-referencing the trace against each `sbs-div` address: `0x800C133E` sits at
      `record_base(0x800C1318) + 38`; `0x800C143A` at `record_base(0x800C1414) + 38`;
      `0x800C14F2` at `record_base(0x800C14CC) + 38`; `0x800C22B6` at `record_base(0x800C2290) + 38` —
      **4 of the 6 f158 addresses confirmed exactly** against the live pool-cursor trace (both cores'
      pool transitions for these record bases are IDENTICAL: e.g. `0x000C2290 -> 0x000C22B8` on BOTH A
      and B, a clean single accepted 40-byte GT3 record). The remaining 2 (`0x800C2CDA`, `0x800C375A`)
      fit the same `base+38` arithmetic against later entities in the same trace and were not
      individually re-verified line-by-line this session (mechanism is airtight from the 4 confirmed
      cases; no reason to expect these differ in kind).
    - **Why offset +38 specifically**: `OverlayGroundGt3Gt4::gt3`'s 40-byte pool-slot record only
      writes fields at +0 (tag, written LAST), +4 (rgb0|code), +8 (SXY0), +12 (uv0|clut), +16 (rgb1),
      +20 (SXY1), +24 (uv1|tpage), +28 (rgb2), +32 (SXY2), and +36 (`mem_w16`, uv2-hi, 2 bytes only:
      `+36`/`+37`) — **`+38`/`+39` are never assigned by any field write**, confirmed identically in
      the generated body (`generated/ov_a00_shard_0.c` line ~24079: `c->mem_w16((c->r[8] + 0), ...)`
      where `r8 = r10+36`, i.e. the SAME single halfword store, nothing at `+38`). This 2-byte gap is
      the "PAD" half of the real PSX `POLY_GT3` hardware packet's final GPU word (`UV2 | PAD`, a
      documented PSX packet-format field the ORIGINAL game's own C never initializes either — not a
      psxport artifact). The packet pool is a per-frame BUMP ALLOCATOR whose CURSOR resets each frame
      but whose MEMORY CONTENT is never cleared (double-buffered by parity, so a byte's prior content
      is whatever the SAME-parity buffer held 2 frames earlier); since `+38/+39` are structurally
      unwritten, their value at any frame is always carried over from whichever EARLIER record's
      dead-space last overlapped that exact byte offset, entirely independent of the CURRENT frame's
      (proven byte-identical) gt3/gt4/entityLoop execution.
    - **Read-but-inert**: `runtime/recomp/gpu_native.cpp`'s `DrawOTag` walk (~line 1602-1613, invoked
      unconditionally every frame via `Engine::drawOTag`, not gated on `PSXPORT_RENDER_PSX`) DOES read
      this word as part of a GT3 tag's declared `n=9` GP0 data words (`addr+4..addr+36`, the last
      iteration `i=8` reads the full 4-byte word at `+36..+39`) and passes it to `gpu_gp0()` — so this
      byte does NOT cleanly satisfy CLAUDE.md's narrow "memory the still-recomp side never reads"
      exception on a literal reading. However: (a) real PSX hardware's GT3 rasterizer only consumes
      the LOW 16 bits of that word (U2,V2); the upper 16 bits are inert pad on real hardware too; (b)
      per `docs/render-arch.md`/CLAUDE.md, the ONLY consumer of this word's downstream effect is
      Beetle's software-GPU VRAM raster state, which is exclusively `psx_render`'s own reference
      picture — "never a byte-match target," explicitly render-only and deferred; no gameplay logic
      reads VRAM back. Not independently verified this session (would require walking Beetle's GT3
      rasterizer, `vendor/beetle-psx/mednafen/psx/gpu*`, to prove the upper 16 bits are truly discarded
      — flagged as the one remaining gap in the proof chain, not attempted for effort-budget reasons).
    - **Disposition: NOT a bug in the ported code; no patch applied.** All three functions
      (`entityLoop`/`gt3`/`gt4`) are re-confirmed byte-exact transcriptions this session via live dual-
      core tracing, not just static reading. Artificially writing `+38/+39` to force a byte-match would
      be exactly the banned "match the PSX mechanism, not the observable result" bandaid — the ORIGINAL
      game doesn't initialize these bytes either, so forcing a specific value achieves nothing but
      papering over an already-provably-correct implementation, and would still be fragile (the SAME
      stale-carry-forward mechanism would just resurface at a different frame/offset once upstream
      pool-cursor placement drifts again for any unrelated reason). If the project wants literal
      zero-diff SBS even through this class of dead-padding carry-forward, the clean fix is either (a)
      have `gt3` explicitly zero `pool+38..+39` on every record (matching neither hardware nor the
      recompiled game, a deliberate divergence-from-source purely for tooling cleanliness — needs a
      user call, not a unilateral change), or (b) teach the SBS byte-comparator to skip GT3 pool-slot
      tail-padding specifically (a principled, narrow, RE-justified mask — NOT a residual-diff
      allowlist by address, but a real "this byte is provably never written by either implementation"
      exclusion, the same class of exception `game/world/object_table.cpp`'s dead-stack-scratch
      precedent already uses for an analogous never-written-but-technically-in-range slot). Left to the
      user/next session to decide; no code changed this session (instrumentation added and fully
      reverted — `git diff` clean).
    - **Verified no regression**: SBS-full gate re-run post-revert, byte-identical to pre-session
      baseline — first divergence still f158/`0x800C133E`, same 6 addresses, stable through f9880+
      (SIGINT, no crash) with only this same packet_pool class diverging. [This "no regression"
      framing is moot — see the retraction above; the entry it validated was itself wrong.]
  - **f158 packet-pool residual — ACTUALLY FIXED (2026-07-10, convergence-agent, resolving the
    RE-NARROWED-vs-ROOT-CAUSED contradiction).** The contradiction: gate history proves end-of-f157
    bytes at `0x800C133E/F` were IDENTICAL on both cores (first `sbs-div` is f158); the RE-NARROWED
    entry's PREWATCH persist trace showed core A performing TWO EXTRA writes to that exact dword
    during f158 that core B never performs; the ROOT-CAUSED entry's per-call tuple trace showed
    entityLoop/gt3/gt4's (tableSlot, counts, pool_pre, pool_post) sequence byte-identical on both
    cores for the whole frame. All three observations are correct — the missing piece was per-FIELD
    write ORDER inside gt3/gt4 relative to their own reject gates, which neither prior trace granularity
    could see (RE-NARROWED logged raw stores without reconciling them against pool arithmetic;
    ROOT-CAUSED logged only call-boundary/accept-boundary state, not what happens INSIDE a call that
    is later rejected).
    - **Method**: rebuilt the two traces at once — (a) instrumented `OverlayGroundGt3Gt4::gt3`/`gt4`
      (native) AND `ov_a00_gen_8013FB88`/`ov_a00_gen_8013FE58` (oracle, `generated/ov_a00_shard_{0,1}.c`
      — gitignored, safe to hack for a session and fully revert) to print every field write's target
      address + value, gated on frame 158 only; (b) added a temporary end-of-frame RAM snapshot hook
      in `Sbs::Impl` (`runtime/recomp/sbs.cpp`) dumping `0x800C1300..0x800C1380` on both cores at the
      f156/f157/f158 frame boundaries; (c) re-ran `PSXPORT_SBS_PREWATCH=0x800C133C` (persist, plain —
      no `WW_ONVALUEDIVERGE`) to capture full host backtraces per store. Confirmed end-of-f157 both
      cores hold `0x7C808080` at `0x800C133C` (bytes 133E/F = `80 7C`, matching the gate's prior
      baseline); end-of-f158 core A holds `0x01FD4097`, core B holds `0x7C804097` — LOW 16 bits
      (`4097`) identical on both (the shared `gt3` uv2hi write, confirmed byte-for-byte equal, exactly
      as the ROOT-CAUSED entry found), HIGH 16 bits diverge (A=`01FD`, B=`7C80`, B's half being
      f157's untouched leftover — exactly as the RE-NARROWED entry found). Both prior entries were
      looking at the SAME dword from two different angles and each had half the picture.
    - **Root cause, precisely**: `OverlayGroundGt3Gt4::gt4`'s host backtrace for the extra store
      (`OverlayGroundGt3Gt4::gt4` → `entityLoop` → `Render::overlayTypeDispatch` → `gen_func_8003F9A8`)
      named the exact function. Reading `ov_a00_gen_8013FE58` instruction-by-instruction end to end
      (not just spot-checking individual field offsets, which is what let the ROOT-CAUSED entry's
      "gt3/gt4 never write +38/39" claim stand unchallenged) shows gen's REAL per-record write order
      interleaves fields between gates in a way the prior native port did not reproduce:
      `rgb0(pool+4) → RTPT → rgb1(pool+16) → [load rec+4] → GTE-FLAG gate#1 → NCLIP → uv0(pool+12) →
      MAC0/backface gate → SXY0/1/2 → [GTE-write 4th vert] → rgb2(pool+28) → RTPS → rgb3(pool+40) →
      uv1(pool+24) → GTE-FLAG gate#2 → SXY3(pool+44) → on-screen X/Y gates → OTZ range gate →
      uv2(pool+36)/uv3(pool+48) [LAST, only once every gate has passed] → OT link`. The prior native
      body instead block-grouped uv0/rgb2/rgb3/uv1/uv2/uv3 into ONE write right after the backface
      gate — meaning for a record that is later REJECTED by the on-screen or OTZ gates (which is
      exactly what happens at pool position `0x800C1338` this frame — two ground-entity GT4 candidates
      land there, both ultimately rejected, but only after clearing the backface gate), gen NEVER
      reaches its own uv2/uv3 write (they're gated behind the OTZ check the record fails), while the
      prior native body had ALREADY written them as an unconditional side effect of clearing backface.
      That extra, gen-never-performs write is exactly the two "extra" stores the RE-NARROWED entry's
      PREWATCH trace caught, and it lands on a pool byte that a LATER, genuinely-accepted `gt3` record
      (base `0x800C1318`) reuses for its own 16-bit uv2hi field — so native's dead-but-real uv2/uv3
      content bleeds into the low 16 bits' NEIGHBORING high-16 tail, while gen's untouched (f157-stale)
      content stays put. `OverlayGroundGt3Gt4::gt3` has the SAME class of bug in the OPPOSITE
      direction: gen writes uv0(pool+12)/uv1(pool+24) UNCONDITIONALLY right after RTPT (before even
      the first GTE-FLAG gate), while the prior native body gated them behind the on-screen tests —
      so for a gt3 record rejected by the FLAG or backface gate, gen has already written dead uv0/uv1
      content that the prior native body never wrote.
    - **Fix** (`game/render/overlay_ground_gt3gt4.cpp`): reordered both `gt3()` and `gt4()`'s field
      writes to fire at the EXACT point gen fires them, gate for gate — `gt3`'s uv0/uv1 moved from the
      late (post-on-screen-test) block to immediately after `RTPT`, unconditional; `gt4`'s uv0 moved
      from the late block to immediately after `NCLIP` (before the backface gate), and uv2/uv3 moved
      from the early (post-backface) block to the very end, immediately before the OT-link, gated
      behind the OTZ range check exactly like gen. No field's VALUE or MASK changed — only WHEN each
      write executes relative to the reject gates. This is not a "reproduce the PSX packet format"
      transcription call (which CLAUDE.md's render section bans for `pc_render`) — this is the
      faithful SUBSTRATE-MIRROR leaf that SBS byte-compares; per `docs/faithful-execution.md`, a
      faithful port executes the same algorithm against the same machine state, including which dead
      bytes a rejected record leaves behind.
    - **Verified**: `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1` gate, 95s window (autonav headless).
      Zero `packet_pool`-tagged `sbs-div` hits anywhere in the run (previously the very first
      divergence, every run). f158 now shows `A/B identical`; the run stays byte-exact through the
      f150/f180/f1500.../f8880 periodic checkpoints and does not crash (SIGINT-terminated by the 95s
      window at ~f8890, no watchdog stall). A SEPARATE, PRE-EXISTING divergence class first appears at
      f179 (`0x801FE924`, task-0-stack region, tag `[?]`) — NOT part of this fix's scope (a different
      address family, a different call chain, not packet-pool); this was already flagged in the
      ROOT-CAUSED entry's own re-run notes as "occasional task-0-stack '?'-tag bytes, likely a
      downstream cascade" and is confirmed here to be an independent, still-open bug, now the new
      SBS-full frontier. Do not conflate the two.
- **refs**: commits 5483a83 (oracle gate), a457082/bef7769 (billboard), ffb1463 (overlay_ground);
  `game/render/{perobj_billboard,perobj_dispatch,overlay_ground_gt3gt4}.cpp`;
  `runtime/recomp/engine_override_thunk.cpp`; oracles `generated/shard_5.c gen_func_8003CCA4`,
  `generated/shard_6.c gen_func_8003CDD8`, `generated/shard_4.c gen_func_8003F698,
  gen_func_8003C8F4`, `generated/shard_0.c gen_func_8003C2D4`, `generated/shard_7.c
  gen_func_800803DC`, `generated/ov_a00_shard_0.c ov_a00_gen_8013FB88,ov_a00_gen_80146478`,
  `generated/ov_a00_shard_1.c ov_a00_gen_8013FE58`. f62/f118-residual: `generated/shard_5.c
  gen_func_8003CCA4`, `generated/shard_6.c gen_func_8003CDD8` (loop-invariant r16-r23), `game/render/
  perobj_dispatch.cpp` (`Render::cmdListDispatch`), `game/render/perobj_billboard.cpp` (`CCA4Frame`).
  Repro: `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_PREWATCH=0x801FE8B8
  PSXPORT_SBS_WW_ONVALUEDIVERGE=1`. f118/f62-RESOLVED (2026-07-10): commit (prior task);
  `game/render/render_walk_dispatch.cpp` (new, `Render::renderWalk`), `game/render/perobj_billboard.cpp`
  (`Render::perObjRenderDispatch` r18/flag fix); oracle `generated/shard_7.c gen_func_8003C048`
  (renderWalk), `generated/shard_5.c gen_func_8003CCA4` (r18/flag prologue lines 5060-5071).
  `gen_func_8003D0BC` OWNED (2026-07-10, this task): `game/render/overlay_type_dispatch.cpp` (new,
  `Render::overlayTypeDispatch`); oracle `generated/shard_7.c gen_func_8003D0BC`. gt3/gt4 f158 residual
  RE-NARROWED to a packet-pool cursor divergence (this task, still OPEN): traced via
  `PSXPORT_SBS_PREWATCH=0x800C133E` (persist mode, no `WW_ONVALUEDIVERGE`) + `sbs bt`'s last-writer map
  at the frame-boundary pause; oracle `generated/ov_a00_shard_0.c ov_a00_gen_8013FB88` (gt3, confirmed
  byte-identical to native where both actually write), `generated/shard_7.c gen_func_8003F9A8` (the
  frame orchestrator; the 7 still-unowned functions it calls before `0x8003D0BC` — see body above),
  `0x80115598` (core B's actual last writer to the residual address, one frame earlier — next RE
  target). Repro (f118/f62/overlayTypeDispatch, all 0-diff up to this point): same PREWATCH command
  above. Repro (gt3/gt4 cursor-divergence frontier, still OPEN):
  `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_PREWATCH=0x800C133E` (omit
  `WW_ONVALUEDIVERGE` to see every raw store + host backtrace, not just the first value-diverging one).
  - **f158 cursor divergence — NARROWED FURTHER (2026-07-10, convergence-agent task): the ground-entity
    `counts`/`table` content is confirmed CLEAN (ruled out, not just deprioritized) — the real
    divergence is a shared packet-pool BUMP-ALLOCATOR ADVANCE-COUNT mismatch that starts at f118, 40
    frames before the first byte-level `sbs-div`.** Method: `entityLoop`'s `list`/`table`/`idx`/`counts`
    were traced directly (temporary instrumentation, since removed) — `list=0x800F2418` (SCENE_ENT_TABLE)
    and `table=0x801A5724` are FIXED area-resource pointers, identical across the whole run; the ground/
    scene table itself is not where the divergence originates. Pivoted to arming PREWATCH directly on
    the pool CURSOR variable itself (`0x800BF544`, `PKT_POOL_PTR`) with
    `PSXPORT_SBS_WW_ONVALUEDIVERGE=1` — this mode pauses+dumps host backtraces the first LOCKSTEP FRAME
    where the two cores' *store count* to the armed address differs (every accepted GT3/GT4/etc record
    bumps this pointer once, so the count IS "how many packets got accepted this frame"). Result,
    **reproduced identically across 2 independent runs**:
    `[sbs] === RNG advance-count divergence: f118  A_calls=109  B_calls=148  (delta=-39) endA=0x90
    endB=0x90 ===` — core A (pc_faithful) accepts **39 fewer packet-pool records than the oracle in the
    same frame**, a ~26% shortfall, first appearing at f118 (f0/f30/f60/f90 all had matching per-frame
    counts). This is NOT the same thing as the register-spill diff the f118/f62-RESOLVED entry above
    fixed — that fix closed a BYTE-level SBS diff at 0x801FE8B8/0x801FE900; this is a COUNT/CONTENT
    divergence in the SAME address band that the byte-compare doesn't surface until f158, because the
    packet pool resets to the same base every frame and a short-by-N frame's unwritten tail bytes
    happen to still hold the PRIOR (clean) frame's content on both cores — until, 40 frames later, some
    record's real content differs enough for the tail alignment to matter and `sbs-div` finally fires.
    Both cores' LAST cursor-advancing write of f118 land on the **exact same logical leaf**
    (`OverlayGt3Gt4::gt4` / `ov_a00_gen_801467BC`, reached via `0x80146478`) via **structurally
    equivalent call chains** — A: `gen_func_8003F9A8 -> Render::renderWalk -> Render::
    perObjRenderDispatch -> Render::cmdListDispatch -> Render::perModeDispatch -> ov_a00_gen_80146478
    -> OverlayGt3Gt4::gt4`; B: `gen_func_8003F9A8 -> gen_func_8003C048 -> gen_func_8003CCA4 ->
    gen_func_8003CDD8 -> gen_func_8003F698 -> ov_a00_gen_80146478 -> ov_a00_gen_801467BC` — so the
    shortfall is NOT a wrong-leaf dispatch; it's that A's native fan-out (`Render::renderWalk` /
    `perObjRenderDispatch` / `cmdListDispatch` / `perModeDispatch`, all already RE'd + wired per the
    banners in `game/render/{render_walk_dispatch,perobj_billboard,perobj_dispatch}.cpp`) visits fewer
    total (node, command) pairs — or takes a rejecting branch the oracle doesn't — somewhere in that
    fan-out or one of its still-substrate per-mode/per-case leaves (`func_8003C5F8`/`func_8003C788`/
    `func_800726D4`/`func_8003EF9C`/`func_80039F4C`/`func_8003F174`/case-0x8003C188's particle path, or
    `perObjRenderDispatch`'s own 5 special-case leaves `FUN_8003F4C4/F3F4/D584/F594/F344` — none of
    these were individually instrumented this session). **Ruled out this session**: the render-mode
    select byte `MODE_BYTE`/`MODE_BYTE_188` (`0x800BF870`) does NOT diverge (armed the same
    PREWATCH+`WW_ONVALUEDIVERGE` probe on it directly — ran 60s / well past f158 with zero trigger), so
    it is not a per-node mode-routing flip. Also ruled out (dead end): the `engine_override_thunk`
    per-address hit-counter dump is NOT usable to compare native-vs-oracle call counts for this chain —
    `0x8003CDD8`/`0x8003F698`/`0x8003CCA4` show `native=0` even though the native bodies visibly ran
    (confirmed in the backtrace) — because native-to-native calls in this fan-out are plain C++ method
    calls that bypass the `g_override[]`-table thunk entirely (only the ORACLE's calls, which go through
    the generated `func_XXXX` wrapper, get counted); do not mistake a 0 in that dump for "native never
    ran" in an already-owned chain.
    - **Left OPEN, no speculative patch applied** (all of `Render::renderWalk`/`perObjRenderDispatch`/
      `cmdListDispatch`/`perModeDispatch` were re-read this session and their control flow is a faithful,
      well-commented, register-accurate transcription of `generated/shard_5.c gen_func_8003CCA4` /
      `shard_6.c gen_func_8003CDD8` / `shard_4.c gen_func_8003F698` / `shard_7.c gen_func_8003C048` — no
      obvious bug was found by inspection, so the 39-record shortfall likely lives in one of the
      still-substrate per-case leaves listed above, or in upstream per-object active-count state
      (`node+8`/`node+9`) that has ALREADY diverged from something earlier than f118).
    - **Next-session repro** (deterministic, reproduced 2/2 runs): `PSXPORT_VK_HEADLESS=1 PSXPORT_SBS=1
      PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_NOAUDIO=1 PSXPORT_SBS_NOPAUSE=1
      PSXPORT_SBS_PREWATCH=0x800BF544 PSXPORT_SBS_WW_ONVALUEDIVERGE=1 ./scratch/bin/tomba2_port` — exits
      at f118 with both cores' host backtraces. Suggested next step: instrument
      `Render::cmdListDispatch`'s node loop with a per-frame (node-visited, geomblk!=0, packet-accepted)
      counter (native side only, cheap `cfg_dbg`-gated), and separately count how many times each
      still-substrate per-case leaf in `renderWalkCase188`/renderWalk's `default:` fallthrough fires, to
      find which node/case the native fan-out visits fewer times than the oracle at f118. A cross-check
      worth trying first: dump `RENDER_LIST_HEAD`'s (`0x800F2624`) linked-list LENGTH on both cores at
      f118 (before any node body runs) — if the lengths already differ, the bug is upstream of this
      whole render band (object spawn/despawn), not in it.
  - **"39-record packet-pool shortfall" — RETRACTED, IT WAS NEVER A REAL DIVERGENCE (2026-07-10,
    convergence-agent).** Followed the next-session repro exactly (`PSXPORT_SBS_PREWATCH=0x800BF544
    PSXPORT_SBS_WW_ONVALUEDIVERGE=1`, reproduced f118 A_calls=109/B_calls=148/delta=-39 identically
    to the prior session) and then went one level deeper than any previous session: dumped the render
    node LIST at the f118 boundary on both cores (25 nodes, byte-identical membership+active-flags on
    both, only ONE active node idx=0), that node's `cmdListDispatch` fields (count/cap/geomblk/header
    word — ALL byte-identical), the `SCENE_ENT_TABLE` ground-entity list `entityLoop` consumes
    (`count=71`, the full 71-entry idx/counts array — BYTE-IDENTICAL, not just same-length), the camera
    GTE control block AND the GTE projection constants (OFX/OFY/H/DQA/DQB/ZSF3/ZSF4, read via a
    temporary `gte_bind()`-rebind probe) at that exact point — ALL identical. Then instrumented
    (temporarily, removed before commit) per-function cumulative ACCEPTED-record counters in all 4
    GT3/GT4 emitters (ground gt3/gt4, field gt3/gt4) on both native (core A) and gen (core B): accepted
    counts matched EXACTLY (ground-gt3 17=17, ground-gt4 46=46, field-gt3/gt4 0=0 both). The actual
    `PKT_POOL_PTR` (`0x800BF544`) BYTE VALUE at frame end was ALSO already known-identical from the
    original repro output (`endA=0x00000090 endB=0x00000090`) — a fact every prior session had in hand
    but not connected to what it implies: **if the accepted-record counts match AND the final pointer
    value matches, there is no missing/extra packet — the "39-record shortfall" framing was wrong from
    its first appearance.**
    Root cause of the FALSE SIGNAL: `PSXPORT_SBS_WW_ONVALUEDIVERGE`'s "RNG advance-count" check
    (`Sbs::Impl::run`, the `mWwCountA`/`mWwCountB` comparison) counts raw STORE EVENTS to the armed
    address via `storeCb`, not distinct VALUES or accepted records. `gen_func_8013FB88`/
    `gen_func_8013FE58` (the GROUND-pair GT3/GT4 emitters — NOT the field pair, which correctly skips
    the write) have an exit shape where the `count==0` early-out (`generated/ov_a00_shard_0.c` L23955:
    `if (r2==r0) goto L_8013FE44`) jumps to the SAME label the loop's natural end falls through to
    (L_8013FE44, `generated/ov_a00_shard_0.c` L24092-24096), which UNCONDITIONALLY re-stores
    `PKT_POOL_PTR` with the value it read at entry — i.e. a real MIPS instruction the original PSX
    binary executes even when there's nothing to do, writing back the SAME value (a harmless artifact
    of how the ORIGINAL game's C compiled this function's control flow, faithfully transcribed by the
    recompiler — not a recompiler bug). Native's `OverlayGroundGt3Gt4::gt3`/`gt4`
    (`game/render/overlay_ground_gt3gt4.cpp`) instead has a clean early `return` for `count==0` (RE'd
    correctly against the same source — the field pair's early-return, by contrast, DOES skip the
    write in gen too, matching native) and so never issues that self-store. Counting the ground table's
    71 ground-entities dump from this session: exactly 31 entries have `gt3count==0` and 8 have
    `gt4count==0` → 31+8=**39** — the EXACT delta. Confirmed mechanically, not by coincidence.
    **Disposition: NOT a bug, NOT fixed, NOT fixable-and-shouldn't-be** — adding a pointless
    write-back-same-value self-store to native purely to satisfy this counter would be CLAUDE.md's
    banned "REBUILD, don't transcribe... match the observable RESULT... not the PSX mechanism"
    anti-pattern; the guest-observable RAM content is byte-identical either way. The `sbs-div` BYTE-LEVEL
    gate (the real fatal comparator) does NOT fire on this — confirmed by running the gate after this
    finding with ZERO code changes: still exactly the same first divergence at **f158** (see below),
    unaffected. This retraction does not touch or explain the f158 `gt3`/`gt4` residual documented
    above (still OPEN, genuinely a different, still-unexplained byte-level divergence) — this entry
    only kills the "packet-pool cursor/count divergence at f118" thread as a real lead. **Tooling
    takeaway (recorded, not yet acted on):** `PSXPORT_SBS_WW_ONVALUEDIVERGE`'s count-divergence trigger
    should ideally also require the END-OF-FRAME VALUE to differ (it already computes `value_diverge`
    separately — the bug is that `count_diverge` alone is sufficient to fire and print "divergence",
    with no annotation that a matching end value means the miscount was harmless) before a future
    session burns a whole investigation on a leg matching this exact shape again.

## packet-pool 39-record shortfall at f118 — RETRACTED, false positive (2026-07-10)

- **symptom:** `PSXPORT_SBS_PREWATCH=0x800BF544 PSXPORT_SBS_WW_ONVALUEDIVERGE=1` reports
  `RNG advance-count divergence: f118 A_calls=109 B_calls=148 (delta=-39)` — read by prior sessions as
  "core A (native) accepts 39 fewer GT3/GT4 packet-pool records than the oracle".
  Do NOT re-investigate this as a record/accept-count bug — see cause below.
- **status:** dead-end (tool false-positive, not a game-state bug) — 2026-07-10.
- **cause:** `PSXPORT_SBS_WW_ONVALUEDIVERGE`'s "advance-count" check counts raw STORE EVENTS to the
  armed address, not distinct values or accepted records. `gen_func_8013FB88`/`gen_func_8013FE58`
  (ground-pair GT3/GT4 emitters) unconditionally re-store `PKT_POOL_PTR` with the SAME value they read
  at entry even when their `count` parameter is 0 (a real MIPS instruction the original game executes,
  faithfully transcribed) — native's clean early-`return` on `count==0` correctly skips that no-op
  store. 31 ground-table entries have `gt3count==0` and 8 have `gt4count==0` this frame = 31+8=39,
  exactly the reported delta. Verified exhaustively: render-node list, `cmdListDispatch` fields,
  `SCENE_ENT_TABLE`'s full 71-entry array, camera GTE block, GTE projection constants, and per-function
  ACCEPTED-record counts (ground-gt3 17=17, ground-gt4 46=46, field-gt3/gt4 0=0) are ALL byte-identical
  between core A and core B at this frame; `PKT_POOL_PTR`'s actual end-of-frame VALUE was already
  `0x90`==`0x90` in the original repro output.
- **fix:** none needed/wanted — native's behavior is correct (matches observable RAM state exactly);
  adding a pointless self-store to native to satisfy the counter would be a banned
  transcription-over-observable-result anti-pattern. `sbs-div` (the real byte-level gate) does not fire
  from this; confirmed the gate's first divergence is still f158, unchanged, after this investigation
  with zero code changes.
- **refs:** full narrative + verification method: `docs/findings/render.md` "overlay_ground_gt3gt4
  cluster" section, sub-bullet "39-record packet-pool shortfall — RETRACTED". Does NOT explain or
  resolve the separate, still-OPEN f158 gt3/gt4 byte-level divergence (same section, bottom).

## Owned the per-object cmd-list dispatch chain 0x8003CDD8/0x8003F698 (2026-07-08)

- **status**: DONE. `Render::cmdListDispatch`/`Render::perModeDispatch` (game/render/perobj_dispatch.cpp).
- **context**: frontier task in address band 0x8003xxxx, the per-object render dispatch chain
  `0x8003CCA4 → 0x8003CDD8 → 0x8003F698 → (per-area leaf)`. `0x8003CCA4` already carries a transparent
  `RenderObserver` wrapper (billboard depth-tag fix, issue #4 class); `0x8003CDD8`/`0x8003F698` were
  unowned and NOT wrapped by anything, so they're a clean additive slice of the same chain.
- **RE**: Ghidra headless decompile of a live free-roam RAM dump (`scratch/bin/field_ram.bin`) — but the
  Ghidra pseudo-C mislabels COP2 moves as fictitious `setCopReg`/`copFunction` helpers, so ground truth
  was the ACTUAL recompiled body in `generated/shard_6.c`/`shard_4.c` (`gte_write_ctrl`/`gte_write_data`/
  `gte_op`/`gte_read_data` — real calls into the Beetle GTE backend). Independently cross-checked against
  a pre-existing "later-133" comment block in `game/render/submit.cpp` (a RETIRED, issue-#32-superseded
  native lift of this exact pair) — same scratch addresses (`CAM_ROT` 0x1F8000F8, `CAM_TRANS` 0x1F80010C,
  `WORLD_POS` 0x1F8000C0, composed matrix at `SCR` 0x1F800000), same MVMVA opcodes (`0x4A49E012` rotation
  column, `0x4A486012` translate). Two independent sources agreeing gave high confidence without having
  to hand-decode the MVMVA opcode bitfields.
- **dead end avoided**: assumed `PSXPORT_DEBUG=recdep` (hooks `rec_dispatch`) would rank call volume for
  every address in this chain. It shows ZERO hits for `0x8003CDD8`/`0x8003F698` even though
  `PSXPORT_DISPWATCH` confirms `0x8003CCA4` fires 1375×/400 frames — the generated `gen_func_8003CCA4`
  body calls `func_8003CDD8(c)`/`func_8003F698(c)` as a PLAIN INTRA-SHARD C CALL (recompiler emits a
  direct call for any `jal` whose target is statically known within the same shard), never through
  `rec_dispatch`. `recdep`/`EngineOverrides` are both blind to this call shape — the only interception
  point is the `g_override[]` slot each `func_XXXX` wrapper in `shard_disp.c` already checks
  (`shard_set_override()`, the same mechanism `render_observer.cpp` uses). Any future "is this hot /
  called at all" search in this band should cross-check `PSXPORT_DISPWATCH=<addr>` before trusting
  `recdep`'s silence.
- **dead end avoided (2nd)**: first cut called `rec_dispatch(c, target)` where `target` was read directly
  from the 22-entry mode table (`MODE_TABLE` 0x80015268). This aborts (`rec_dispatch_miss` at
  `0x8003F6E8`) — the table does NOT store final `FUN_` target addresses, it stores addresses of
  `FUN_8003F698`'s OWN internal jump-table case labels (each label's body separately calls
  `rec_dispatch` to the real target, e.g. `0x80146478`). Confirmed by reading all 22 live table words
  out of `scratch/bin/field_ram.bin`: every entry is one of 11 known label literals (`0x8003F6E8` ..
  `0x8003F788`), never a bare `FUN_` address. Fixed with a literal 11-entry `perModeCaseTarget()` map
  (fixed game data, identical to the switch in `generated/shard_4.c`).
- **verification**: `PSXPORT_SBS_MODE=full` + `AUTONAV=1`, headless, zero `sbs-div`/`VIOLATION` through
  frame 8760 across two separate runs (both only stopped by an external timeout, not a crash/divergence).
- **refs**: game/render/perobj_dispatch.cpp, game/render/render.h, game/render/submit.cpp (later-133
  comment), docs/port-progress.md CURRENT FRONTIER 2026-07-08.

## Narration-end -> fisherman-cutscene LOADING SCREEN shows garbage (pc_render) instead of black-hold + "Loading....." (2026-07-08)

- **symptom (user-reported)**: between the end of the intro NARRATION cutscene and the start of the
  FISHERMAN (caught-on-the-fishing-line) cutscene there is a brief loading transition; `GATE=1`
  (recomp gameplay + pc_render) shows a tiled noise/atlas-grid GARBAGE picture there instead of the
  correct black-hold-then-"Loading....." text the pure PSX render shows (`PSXPORT_ORACLE=1` = recomp
  gameplay + pure PSX render, the picture oracle).
- **characterization**: state-machine window pinned via task0's own sm array (0x801fe000): SOP
  narration owns the tick while `sm[0x4a]==0`; the RESET that ends narration increments `sm[0x4c]`
  ONE TIME (its only per-cutscene increment — not per-beat) in the same tick the field-area transition
  load lands, then `sm[0x4a]` flips to 1 (the field-area machine takes over) a tick later, then the
  overlay signature word `0x80109450` is overwritten by the incoming CD stream a further tick after
  that. Captured GATE (pc_render) vs ORACLE (psx_render) frame-by-frame via the debug server
  (`pause`/`step 1`/`shot`) at the SAME deterministic AUTO_SKIP frame: garbage ran f115-f119 (5
  frames); oracle showed near-black through f114-f120, then the real "Loading....." text at f119+,
  matching post-fix pc_render exactly.
- **root cause (RE'd, not black-boxed)**: two guest structures are ticked EVERY FRAME by SOP's own
  per-tick systems while narration owns the scene — `SCENE_ENT_TABLE` (0x800F2418: count@+6,
  grid-ptr@+0xC, refreshed by `Sop::scenePrepass`/guest `FUN_8010A0E0`) and `PARALLAX_BG_SM`
  (0x800ED018: W@+0x10/H@+0x11, tilemap-ptr@+0x14, refreshed by the backdrop tile scroller). The
  INSTANT narration hands off to the field-area load, NOTHING re-validates either structure for a few
  ticks: their count/W and pointer fields are the ended narration's LEFTOVER values, and the
  field-area transition load's CD stream has ALREADY repurposed the memory those pointers reference
  (verified via raw guest reads: `PARALLAX_BG_SM`'s tilemap ptr stayed `0x8019BD04` — inside the
  narration's own loaded overlay blob — through the whole garbage window, then the field-area
  machine's own equivalent legitimately RESETS both structures to 0 before repopulating them fresh).
  `Render::sceneNative()`'s existing "AREA-INIT SUPPRESSION" guard (the `field_area_init` bool,
  `sm[0x4e]==0` only) does NOT cover this: the scripted "caught on the fishing line" cutscene enters
  via `sm[0x4e]=9` DIRECTLY (`Engine::fieldRunFaithful`/`fieldRun` case 0: `if (0x800BF89C==2)
  sm[0x4e]=9`, skipping the normal `sm[0x4e]==1` path the guard's own comment assumes), so the
  existing guard never triggers for this specific hand-off — and `sm[0x4e]==9` itself persists for
  the ENTIRE (much longer) visible fisherman-cutscene, so blanket-suppressing on it would hide real,
  correct content, not just the transient garbage.
- **fix (native, read-only, no guest writes — `game/render/render.h` + `game/render/render_walk.cpp`)**:
  two host-only trust latches on `Render` (`mSceneTableTrusted`, `mBackdropTrusted`, mirroring the
  `ScreenFade` held-fade latch / `RenderObserver` read-only-tag precedent). Both structures are
  trusted while `sm[0x4a]==0 && sm[0x4c]==0` (SOP's own one-shot "still owns this tick" test — chosen
  over the overlay-signature check because the transition load clobbers the referenced memory 1-2
  ticks BEFORE the overlay's own first-instruction word is overwritten by the same incoming stream);
  the instant that flips false, both latch UNTRUSTED until each structure independently proves its
  owner has taken over again by observing its own natural re-zero (`count==0` / `W==0`) — the same
  zero-before-repopulate step `Sop::scenePrepass` already performs every tick, which is why reading
  through the zero window is already safe (each structure's own EXISTING `count==0`/`W==0` -> skip
  guard covers it). `Render::sceneNative()`'s BACKDROP draw and SCENE-TABLE draw are gated on their
  respective latch. Pure guest READS + two per-Core host bools; no guest writes, no magic frame count.
- **verify**: re-captured the same frame window post-fix — garbage gone, pixel-matches the oracle
  frame-for-frame including the "Loading....." text at f119+; no `[pc_render VIOLATION]` fail-fast
  trip (confirms no guest write); `PSXPORT_SBS_MODE=full` autonav+postdrive ran 18,690+ frames through
  real interactive walk/jump gameplay with zero A/B divergences (the render change doesn't perturb
  guest state).
- **refs**: `game/render/render.h` (trust-latch fields + writeup), `game/render/render_walk.cpp`
  (`Render::sceneNative()`), `game/scene/sop.cpp` (`Sop::scenePrepass`/`Sop::fieldMode` case 4 RESET),
  `game/core/engine.cpp` (`Engine::fieldRunFaithful`/`fieldRun` case 0, the existing
  `field_area_init` guard). Capture tool: `scratch/loadtrans_capture.py` (debug-server
  pause/step/shot driver for a deterministic frame window, GATE vs ORACLE).

## 0x800C0000-0x800C8FFF "massive divergence" — FALSE POSITIVE; it's the GPU packet pool (2026-07-08)

- **symptom (reported by a scout, RAM-diff based, no RE)**: default path (pc_faithful, mPcSkip=true)
  guest RAM `0x800C0000..0x800C8FFF` diverges up to 100% of words vs `PSXPORT_ORACLE=1` (recomp
  gameplay + PSX render). Scout guessed this was the "AREA-DATA / object-data overlay" (based only
  on address proximity to the area-id byte `0x800BF870`) and suspected `Asset::areaDataLoadAsTask`
  (game/core/asset.cpp:399) — specifically its pc_skip=true CD-load shortcut producing wrong bytes.
- **RE (mandatory-first, per project rule)**: `docs/engine_re.md` §"Render-buffer memory map" already
  names this exact range as the **GPU packet pool** — a per-frame double-buffered BUMP ALLOCATOR
  (write ptr `DAT_800bf544`/`0x800BF544`) holding GT3/GT4 draw-primitive packets + OT link tags:
  `packet pool parity0 [0x800BFE68, 0x800D3E68)`, `parity1 [0x800D3E68, 0x800E7E68)`. The requested
  range `0x800C0000-0x800C8FFF` sits entirely inside parity0. It is RESET and rebuilt from scratch
  EVERY FRAME (`game/render/submit.cpp`, `runtime/recomp/gpu_native.cpp`'s `DrawOTag` walk at
  gpu_native.cpp:1602-1613) and consumed ONLY by the GPU/OT walk that builds the picture — no
  AI/physics/gameplay code reads it back (grepped `game/ai`, `game/object`, `game/world`, `game/player`,
  `game/items` for the address range: the only hit was `game/world/pool.cpp:45,316`, both explicitly
  commented `// incidental v0` — a dead register clobber mirroring the original MIPS `lui v0,0x800c`
  epilogue byte-shape, never dereferenced as a pointer). `Asset::areaDataLoadAsTask`'s
  `c->r[16] = 0x800C0000u` (asset.cpp:431) is the same pattern: a register value that's overwritten
  before use (r16 is reassigned to `0x800EF478` at line 452 before any later use), not a write target.
  The codebase's own dual-core harness already treats this exact byte range as expected-to-differ:
  `runtime/recomp/dualcore.cpp:119-123` `DualCore::isRenderRegion()` — `[0x800BFE68, 0x800EA200)` is
  excluded from its report as "render noise, not the gameplay corruption we hunt."
- **empirical verification**: built the port in a fresh worktree (bootstrapped `generated/` +
  `scratch/bin/tomba2/MAIN.EXE` from the main checkout; one unrelated pre-existing beetle-psx local
  edit, `SPU_PokeRAM` in `vendor/beetle-psx/mednafen/psx/spu.c`, had to be re-applied to unblock the
  link — not yet committed to the beetle-psx fork, a housekeeping gap worth closing separately).
  Recorded a `PSXPORT_AUTO_SKIP=1` pad session on the default path to free-roam (`[autoskip] free-roam
  reached at frame 216`, matching the scout's own checkpoint), replayed the IDENTICAL pad file under
  `PSXPORT_ORACLE=1`, dumped full guest RAM at replay-frame 300 on both, and diffed:
  - Region `0x800C0000-0x800C8FFF` (36864 B): **28163 B differ (76%)** — consistent with "up to 100%
    of words," but this is per-frame packet content, not corruption.
  - Whole-RAM diff at f300: **30833 B total** — i.e. this ONE known render-scratch region accounts
    for **91% of the entire reported divergence**. Per-page histogram: the top 9 divergent pages are
    exactly `0x800c0000..0x800c8000` (packet-pool pages); the remainder (~2670 B) is scattered across
    `0x801fe000`/`0x801ff000` (task-scheduler control, allowed-to-diverge scratch under pc_skip=ON
    per `feedback_sbs_two_compare_modes`), plus small clusters at `0x800ee000`, `0x800bf000`,
    `0x800f0000-0x800f9000`, `0x80105000` — all a few hundred bytes, none of them area-data.
  - The area-id byte `0x800BF870` itself: `00` on BOTH sides at f300 — confirming (as the scout also
    noted) the SAME area is loaded; there is no "wrong area content" — the diff is transient GP0
    packets, not the area descriptor/asset payload.
- **conclusion**: NOT a bug. `Asset::areaDataLoadAsTask` is not implicated — it does not write to
  `0x800C0000` in any meaningful sense; the address only appears as an incidental dead-register
  transcription artifact, both there and in `game/world/pool.cpp`. No fix applied (per project rule:
  don't force a change onto a region that turns out benign/unconsumed). If a future scout flags this
  range again, point it at `DualCore::isRenderRegion()` and this entry before re-investigating.
- **workflow note**: worth teaching `tools/findings.py`-adjacent scouts to check
  `runtime/recomp/dualcore.cpp:isRenderRegion` / `docs/engine_re.md`'s packet-pool memory map BEFORE
  flagging a RAM-diff address range as gameplay corruption — this is exactly the kind of black-box
  guess the project's RE-first rule exists to prevent.
## ScreenFade held-latch permanent black on default `./run.sh` free-roam (2026-07-08)

- **symptom**: default path (pc_skip=ON, pc_render) reaches free-roam (poly=552 confirmed submitted —
  b2ef2d1 fixed the OT-rewind-before-draw bug) but the final composited picture stays fully BLACK.
- **ownership check (done first, per directive)**: the ScreenFade SM path was already fully native —
  `Sop::fieldMode`/`fieldUpdate` (game/scene/sop.cpp), `Engine::submode0`/`fieldRun`
  (game/core/engine.cpp), and `BgSceneTransitionSm::body` (game/scene/bg_scene_transition_sm.cpp) all
  route every fade call through `c->screenFade.set/applyLeafCall`, byte-verified against the substrate
  (BgSceneTransitionSm has its own `verifyBody` A/B gate). No unowned leaf in this path. So the bug was
  in the OWNED code, not a missing port.
- **RE (Ghidra headless decompile of `FUN_80106b98` = `Engine::fieldRun`'s guest source, GAME.gpr, via a
  new xref/decomp pass — see `tools/ghidra_xrefs.py`)**: confirmed the new-game bootstrap transition
  (`sm[0x4e]` states 0→9→10→7→8→6→0→1, gated by `DAT_800bf89c==2`, the fresh-game marker) ramps the
  screen to full black via `case 10` (`Engine::fieldRun`, guest FUN_80106b98 case 10 — a real ramp using
  `sm+0x6e`), then hands off through states 7/8/6/0 straight into steady case 1 gameplay. NONE of states
  7/8/6/0/1 call the fade leaf (`FUN_8007e9c8`) again — confirmed in the decompiled C, not inferred. On
  PSX this is correct: OT slot 4 is rebuilt every frame; an unwritten frame renders no rect, so the scene
  shows through the instant the SM stops drawing it — no explicit "un-fade" call is needed or exists.
- **cause**: `ScreenFade` (game/render/screen_fade.h/.cpp) carried an invented cross-frame "held
  fully-faded" latch (`FULLY_FADED_THRESHOLD=0xE0`): if the last fade value was near-black/white, the
  class kept re-presenting that color on every subsequent frame with no caller, releasing only when some
  caller later called `set()` with a lower value. That release condition never occurs on the bootstrap
  path (nothing ever ramps back down — matching real PSX, where nothing needs to). Net effect: the
  screen latched black FOREVER the first time any transition faded to black and then genuinely finished
  (no follow-up ramp), which is exactly the free-roam bootstrap case. This is a magic-threshold heuristic
  standing in for "did the SM finish", banned by the no-magic-constant rule — not a faithfully-ported
  PSX behavior.
- **fix**: removed the hold latch entirely. `ScreenFade::get()` now returns only the frame-scoped state
  set by `frameStart()`/`set()` this frame (NONE by default), matching PSX's "OT slot rebuilt every
  frame" model exactly. Any caller that needs a color held across multiple frames (all current ones do,
  e.g. `submitPage810c`'s pause-menu dim) already re-calls `set()`/`applyLeafCall()` every one of those
  frames itself — verified by inspection of every existing caller.
- **verification**: `PSXPORT_AUTO_SKIP=1 PSXPORT_VK_HEADLESS=1` headless shot at free-roam now shows the
  village (hut, trees, grass) instead of black (`scratch/screenshots/fade_fixed.png`), matching
  `PSXPORT_ORACLE=1`'s reference shot in scene content (Tomba's sprite itself is a separate, already-
  deferred pc_render gap — issue #27/#28 territory, not this bug). SBS full: 0 `sbs-div`/`VIOLATION`
  through 7500+ frames (`PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1`), confirming the fix is
  render-side only and does not touch guest memory or the byte-exact compare.
- **tooling added**: `tools/ghidra_xrefs.py` (list every xref to a guest address — "who reads/writes
  DAT_X" — via `pyghidraRun -H <proj-dir> <proj> -process -noanalysis -scriptPath tools -postScript
  ghidra_xrefs.py <addr_hex>`); `fadesites` debug channel (per-call-site fade tracing, since multiple SMs
  share one `ScreenFade` instance and `fadetrace`'s dedup-by-value hides a repeat value from a different
  caller) — both documented in docs/config.md.

## Graphical enhancements rewired PC-native / read-only overlay (2026-07-08)

Per the read-only-overlay directive (pc_render reads guest+engine, writes ONLY host memory):
- **RenderObserver** (game/render/render_observer.cpp, commit 262d709): transparent wrappers in
  the shared recomp override table (shard_set_override) around the per-object render dispatch
  0x8003CCA4 + effect-leaf renderers — run the LITERAL gen body then tag the produced packet span
  with obj_world_ord(node) in host memory. Restores billboard real-depth occlusion lost when the
  native walk-lifts were retired (issue #32). SBS 4:3 zero-diff (guest-transparent).
- **interp60 fully native** (commit bf61ef1): removed both PSX GTE/GP0 op-stream taps
  (gte_beetle.cpp rtp() per RTPS/RTPT, gpu_native.cpp join_poly per poly). build_lerp reprojects
  purely from the native capture (fps_key + stampWorldCr fps_cr/fps_mv + the observer's node-span
  billboard registry). Verified ALL THREE tap outputs dead: SXY object grid (mJoinHit/Miss never
  read), XObj transform fingerprint (never read by build_lerp), logic-rate detector (mRd.period
  never read). Behavior-preserving. Dead method bodies (rtp/xobj*/grid_*/fold/RateDet) sweep TBD.
- **Widescreen margin read-only** (commit 68426d3): MarginRenderer::flush() no longer dispatches
  the guest transform builder 0x80051C8C (which wrote node+0x98/+0xac) — builds the transform in
  HOST float (identity -> rotX/rotY/rotZ from eulers -> root/sibling compose) and submits via the
  native projComposeObjectHost -> gt3gt4. Zero guest writes. SBS 4:3 zero-diff through f21600.
  Widescreen picture is user-eyeball (margin off-path at 4:3; didn't execute in headless boot).

## PSX render path always executes underneath; pc_render display pass is READ-ONLY (2026-07-07, issue #32)

- **symptom**: strict SBS full (default, pc_render live) diverged at f26 in the main-thread guest
  stack (0x801FFFC8..): core A's extra writer pc=0x800597AC ra=DEAD0000 — the native render-walk
  lift (submit.cpp rwalkB588) dispatching the guest transform-setup from the DISPLAY phase, a
  foreign call context whose guest-stack spills can never match the recomp reference's.
- **cause (architectural)**: pc_render was built as native REPLACEMENTS of the substrate walk
  cluster — byte-faithful lifts re-running the walks' guest writes (queue swaps, node bookkeeping,
  guest renderer dispatches) from drawOTag instead of the task's own call path. Same writes,
  different sp/cadence => structurally unable to hold the strict byte compare.
- **fix (USER directive)**: "PSX render path should always be active underneath even when PC
  rendering; PC renderer shouldn't write to guest memory — then rendering should never affect
  diverges." Implemented: Render::frame/frameX ALWAYS dispatch the substrate orchestrator
  (0x8003f9a8/0x8003fa44) in both render modes; the walk-cluster lifts (renderWalk,
  renderWalkSnapshot, rwalkAuxBcf4/Bf00/Eec0, rwalkB588, perObjRender, bgRender) and
  prepObjectMatrix are RETIRED (RE'd case tables preserved at commit 7989159); the display pass
  (sceneNative: backdrop, read-only terrain float pass, fieldEntityRender, perObjFlush loops) is
  read-only — terrain matrices now computed in HOST memory (native_terrain.cpp
  terrain_obj_matrix_host = rotmat element math + FUN_80084520 column scale, Ghidra
  scratch/decomp/80084520.c).
- **verified**: default-mode SBS full+AUTONAV diverges at f114/0x800BF544 — byte-identical frontier
  to the PSXPORT_SBS_FORCE_PSX_RENDER baseline (rendering no longer affects diverges); GATE=1 +
  pc_render boots clean (no abort/recomp-MISS).
- **known deferred render regressions**: per-object depth tags for guest-emitted billboard prims
  are lost (the lifts attached them at dispatch time) — restore via a READ-ONLY observer
  (EngineOverrides wrap teeing packet-span info) later; margin_render (widescreen mod) still
  dispatches guest transforms — same violation class, inactive by default, fix with the observer.
- **refs**: game/render/render_frame.cpp, render_walk.cpp, submit.cpp, native_terrain.cpp;
  issue #32; skill sbs-diverge; memory feedback_native_renderer_readonly_overlay.

## Un-owned FUN_8007E9C8 fade callers — 3 of 3 SHIPPED (2026-07-03)
- **symptom (from #27 investigation):** After surveying all 36 shard callsites of `func_8007E9C8` and mapping the enclosing fn per overlay, exactly THREE fade-caller SMs remained still-substrate. Any cutscene that reaches an un-owned caller via a still-substrate PARENT drops the fade rect (substrate FUN_8007E9C8 writes guest OT data our renderer no longer draws) and can trigger the #27 stuck-black symptom.
- **status:** ALL 3 native (last landing: commit 560bac0). Also solved the sibling arc — the cutscene-script INTERPRETER that dispatches the A06 pair is fully native (dd40602 + 4c331a3 + a70092a), so op-0x03E fnptr routing is live for any A06 fade fn registered as a `beh_*`.
- **The three, current state:**
  - **A06 FUN_80139728** — ✅ NATIVE `beh_a06_fade_flash_ramp_80139728` in game/ai/beh_a06_script_fades.cpp (c7c2224). Registered at 0x80139728 in BehaviorDispatch::kTable. 8-state additive-white ramp with state-5 music trigger (writes G_BFA22, G_E806C=7, calls FUN_8006CBD0 attach) + state-6 30-frame hold + state-7 ramp-down. Reached via ScriptInterp::callFnptr op-0x03E → dispatchObj natively.
  - **A06 FUN_8013B178** — ✅ NATIVE `beh_a06_fade_ramp_8013B178` in same file. Registered at 0x8013B178. 3-state simple additive ramp (state 1 up by 0x20 to 0x80, state 2 down by 0x20 to 0x20).
  - **A08 FUN_80127C58** — ✅ NATIVE `cutsceneDirector` inline in game/ai/beh_a08_scene_actor.cpp (560bac0). Reached via a plain C call from the outer `beh_a08_scene_actor` (FUN_801280D0 native, registered at 0x801280D0 — 21 A08 fnptr refs so it's the real per-object behavior for many scene actors). State 9's additive-gray fade-out (sub 0 ramp UP by 8 to 0xFF; sub 1 wait `DAT_800BFA50 == 0x15`; sub 2 ramp DOWN by 8; at 0 sets `DAT_800BFA50=0x16` + node[+4]=3 despawn) now fires via `c->screenFade.applyLeafCall`.
- **cause (if reached):** substrate `func_8007E9C8` writes guest OT data our renderer no longer draws (see `game/render/screen_fade/screen_fade.h` design note). If a `FUN_8007E9C8(0xF8F8F8, 1, 4)` fires while native ScreenFade is silent, the frame-scoped fade stays NONE and the HELD latch at full-white (if it survived) persists — the recip flip of #27's stuck-black scenario, which explains the "some cutscenes stuck black, some stuck white" symptom class.
- **verification (next):** user-driven cutscene under `PSXPORT_DEBUG=fadetrace PSXPORT_DISPWATCH=0x8007E9C8 2>&1 | tools/symres.py -` on the failing #27 cutscene — the log should now show the native fade path firing every frame the fade is expected, with substrate `func_8007E9C8` entries GONE for A06/A08 scripted cutscenes. If #27 persists after that, the failing cutscene reaches a DIFFERENT (still-substrate) fade caller that this survey missed — re-run the callsite scan against the current shard.
- **refs:** #27; c7c2224 (2 A06 fades shipped) + 560bac0 (A08 shipped); commits dd40602/4c331a3/a70092a (script interpreter + caller chain — the NATIVE PATH that reaches the A06 fade fns); Ghidra projects `scratch/ghidra/A06` + `scratch/ghidra/A08` (Ghidra 12.0.4, via `pyghidraRun -H` after 2e2facf's decomp.sh fix); `scratch/decomp/a06_fade_fns.c` + `scratch/decomp/a08_cutscene_director.c`; `game/ai/beh_a06_script_fades.cpp` (the 7 script-driven A06 fade fnptrs) + `game/ai/beh_a06_scripted_actor.cpp` (A06 caller chain) + `game/ai/beh_a08_scene_actor.cpp` (A08 scene actor + inlined cutscene director); `game/ai/beh_a06_multi_actor.cpp` (previous port covering A06 case 10's whiteFlashPhaseRamp/whiteFadeHold — the two OTHER fade SMs from FUN_801189E8, a DIFFERENT family).


## Screen-fade transitions: SSAO/dynamic-shadows are dead stubs; FUN_8007E9C8 (fade-rect builder) only PARTIALLY native-owned
- **symptom (3 user reports, 2026-07-01):** (1) vanilla (all mods off) shadows/shading "too dark" (marukage);
  (2) a dark rim/outline resembling AO on some objects even with AO shown Off in the RmlUi; (3) some
  cutscene/area-transition fades show garbage (raw VRAM/texture-atlas noise) instead of a clean fade.
- **status:** (1)/(2) NOT YET diagnosed past ruling out SSAO/light (see below) — needs non-visual RE, not
  screenshot-eyeballing (a still of the "AO-looking" horizon dark-outline was inconclusive; user corrected
  the agent for trying to conclude from it — see CLAUDE.md "no visual self-verify"). (3) ROOT-CAUSED, partial
  fix scoped, NOT fully implemented — see below.
- **cause (1/2, partial):** `GpuGpuState::ssao_pass()` and `::shadow_pass()` (runtime/recomp/gpu_gpu.cpp:617-618)
  are EMPTY STUBS — dead code copied from the deleted Vulkan renderer (already empty there too, confirmed via
  `git show 9f1ab11`). Toggling SSAO/Shadows in the RmlUi overlay has ZERO visual effect currently; whatever
  darkening the user sees is NOT from those systems. The live `psxport_settings.ini` had `ssao=0 light=0`
  when bug 2 was reported, ruling out `g_mods.light`'s engine_shade_face path too (correctly gated, `if
  (!g_mods.light) return;` in engine_submit.cpp / native_terrain.cpp). Remaining candidates NOT yet checked:
  base vertex-color / geomblk data (native_terrain.cpp's own comment flags "FIRST CUT: no DPCT/DPCS depth-cue"
  as a known gap), and whether that's even wrong vs just how the original renders. NEEDS non-visual state
  inspection (provat/scene channels) on a live reproduction, not more screenshots.
- **ROOT-CAUSED (2026-07-01, session 4) — the "dark outline" IS a quantified per-pixel divergence between
  native (A) and the TRUE independent oracle (B, `PSXPORT_SBS_MODE=oracle`), not a lighting/SSAO artifact.**
  Reproduced at the coastal-cliff-over-the-sea view (village start area, tap Cross to wake Tomba then hold
  Left once — deterministic via `scratch/bin/pad_session.pad` / `PSXPORT_PAD_REPLAY`, though replay currently
  desyncs across the 2 SBS cores' shared static replay-frame counter in `pad_input.cpp`, so re-driving the
  same 3 dbgclient commands live is more reliable for now). A column-by-column scan
  (`scratch/silhouette_scan2.py`) of an `sbs dump` side-by-side PPM found a SYSTEMATIC 1-2px near-black run
  (RGB roughly (8-24,16-32,24-40)) at the water/cliff-edge boundary across x=0..59 y≈143-157 on pane A
  (~300+ of 320 columns hit) and ZERO matching hits on pane B in that x-range (B's only near-black-crack
  hits are unrelated tree-shadow noise at x≈166-213). This is conclusive: it is a NATIVE-RENDERER-ONLY
  coverage crack, not PSX-authentic shading.
  **Located the exact source** via the new `silbbox`/`sil_bbox_log_node` diag (game/render/render_internal.h,
  wired into `submit_poly_gt3_native`/`submit_poly_gt4_native`/`submit_poly_gt4_bp` in engine_submit.cpp and
  `terrain_render_pc` in native_terrain.cpp): at the repro, ONLY two `gt4_native` quads (submit_poly_gt4_native,
  the generic per-object GT3/GT4 library) overlap the crack window, and BOTH come from the SAME entity node
  `0x800E7E80` — a `type=00 handler=00000000` static-scenery prop with 17 render commands
  (`node+0xC0[0..16]`, 17 distinct geomblks), i.e. one multi-chunk static mesh (almost certainly the
  sea/cliff-edge terrain-decoration model), NOT the walkable ground (`native_terrain.cpp` logged NOTHING in
  this window, confirming last session's terrain ruling-out). The two quads' bboxes are
  `y=[149.6,168.7]` and `y=[140.1,155.3]` — they overlap by ~5px in Y exactly where the crack sits, meaning
  this is a SEAM between two of that object's 17 independently-submitted geomblk chunks (each chunk goes
  through its own `eproj_compose_object` + `native_gt3gt4` call in `submit_perobj_flush`,
  engine_render_walk.cpp), not a gap to the sky backdrop as originally hypothesized.
  **NOT yet determined:** whether the two chunks' shared boundary vertices are bit-identical in the source
  geomblk data (in which case the crack is purely a FLOAT reprojection/rasterization precision issue —
  independent per-quad screen-space rounding leaving a sub-pixel gap neither triangle claims) or whether the
  original PSX fixed-point data itself has a small deliberate offset between chunks that the PSX's
  integer/OT-based renderer happened to not gap on. Next step: dump the two specific cmd's geomblk vertex
  data at the shared edge and diff them; if bit-identical, the fix is likely a small screen-space overscan
  (nudge each object-chunk quad's silhouette-facing edges out by a sub-pixel epsilon) or switching adjacent
  chunks of ONE object to a single indexed draw so the rasterizer's edge rule doesn't re-decide coverage
  per-chunk.
  **Tooling note:** `PSXPORT_PAD_REPLAY=<path>` + the always-on `PSXPORT_PAD_RECORD` (default
  `scratch/bin/pad_session.pad`) IS the way to reproduce a hand/scripted-navigated repro spot deterministically
  headless — but its frame counter (`pad_input.cpp` `rec_fc`, a function-local static) is shared across BOTH
  SBS cores' per-frame calls, so a recording made during a 2-core SBS session and replayed into a fresh
  2-core SBS session can desync (each core's step consumes one array slot, interleaved). Works fine for
  single-core (AUTO_SKIP) recordings replayed into a single-core run. Worth fixing (per-core replay index) if
  this keeps mattering.
- **FOLLOW-UP (same session) — found the actual mechanism: `ov_field_entity_render(0x800F2418)` (the
  "scene table" — grass/props/sky-sea backdrop, including node `0x800E7E80`) is called from TWO separate
  per-frame code paths that can BOTH be live during ordinary walkable-field gameplay:**
  1. `game/render/engine_render_walk.cpp:180`, inside `ov_render_frame` (the "ONE NATIVE RENDER PATH"
     orchestrator), gated only by `!field_area_init` and reached via `native_boot`'s
     `if (c->mem_r8(0x1F800136) < 2) ov_render_frame(c)` — **confirmed live this session** (read
     `0x1F800136 == 0` on the actual repro session).
  2. `game/scene/sop.cpp:217`, inside `ov_sop_field_update` — called from `ov_sop_field_mode` states 1/2/3,
     which per other findings (`[[camera-system-done-demo-re]]`, journal later-238/295) is the driver
     ACTUALLY steering ordinary walkable-field gameplay (`sm48=2 RUNNING, sm4a=1 field-area, sm4c=2, sm4e=1`
     matched live during the repro). The comment at sop.cpp:208-216 claims "this SOP path isn't exercised by
     the walkable field... [engine_render_walk's] IS the live field render path" — **that assumption looks
     STALE/WRONG**: SOP field-mode is what's actually driving our repro, so its `ov_field_entity_render`
     call fires too, alongside `ov_render_frame`'s.
  Each call computes its OWN camera/object transform independently (`eproj_compose_camera`/
  `eproj_compose_object` inside `ov_field_entity_render`'s loop), so the SAME scene-table entity gets
  projected TWICE per frame through two not-necessarily-identical transforms — exactly matching the live
  evidence: geomblk `0x8017ECE4` (node `0x800E7E80`, 2 GT4 records that are a mirror-winding pair for
  double-sided rendering, one culled per submission) produces TWO `gt4_native` submissions per frame with
  DIFFERENT screen bboxes (`y=[149.6,168.7]` vs `y=[140.1,155.3]`) — a double-projected copy of the same
  quad, offset by a few pixels, leaving an uncovered sliver between them where the black clear color shows
  through. This is a genuine violation of the project's own "ONE native render path, decoupled"
  architecture goal (`[[one-native-render-path-decoupled]]`) — two orchestrators are unknowingly sharing
  (and double-driving) the same scene-table submission.
  **Fix (not yet applied — needs a decision, not a quick patch):** determine which ONE of the two call sites
  should own scene-table (`0x800F2418`) submission during ordinary field gameplay and stop the other from
  calling `ov_field_entity_render` when SOP field-mode is active for this frame (e.g. gate
  `engine_render_walk.cpp`'s scene-table call on the SAME "is SOP field-mode driving this frame" condition
  sop.cpp already knows, or vice versa) — do NOT just suppress one blindly without confirming it doesn't
  regress the OTHER scenario each path was presumably added for (cutscenes / non-SOP field states). Re-run
  the `sbs oracle` A/B pixel scan at this same repro spot after the fix — the crack should vanish and A
  should match B in that x-range.
- **cause (3, root-caused via live debug-server inspection, non-destructive, on the user's own paused session):**
  `FUN_8007E9C8` (the PSX fade-rect builder, native reimpl already exists as `ov_8007E9C8`
  game/render/gpu_lib.cpp:75 but is ORPHAN) is called from 24 guest sites across 8 recompiled shard files:
  `ov_sop_shard_0`(3), `ov_game_shard_0`(6)+`ov_game_shard_1`(1), `ov_a06_shard_0`(3)+`ov_a06_shard_1`(5),
  `ov_a08_shard_1`(1), `ov_a0l_shard_1`(5). Of these, SOP (sop.cpp, all 3) and GAME's door/area-transition
  FUN_80107AFC (engine_stage.cpp, all 3) and FUN_80106B98 case-10 (engine_stage.cpp, 1) are natively wired to
  `engine_fade_set` already — 7/24 done. The GAME render-submit dispatcher FUN_8010810C (1 call) and two more
  GAME node-handlers FUN_80108EBC/FUN_80108E58 (not yet RE'd) are NOT yet native (`tools/codemap.py --addr`
  confirms all three: NO native owner). The a06/a08/a0l overlay call sites (13 total) are in AREA-SPECIFIC
  overlay .BIN code (outside MAIN.EXE's text range — `tools/disas.py` can't reach them, only the recompiled
  generated/ C shows them) and are ENTIRELY unowned (their enclosing functions, e.g. `0x80117AAC` in a06, have
  NO native code at all per codemap). BUT: several of these enclosing functions are small (~70 lines),
  self-contained per-NODE fade state machines operating on a generic node pointer (state byte at node+6,
  countdown at node+64) — the SAME shape recurs at `0x80117AAC`(a06) and similar addresses in a08/a0l/game
  shard_1, strongly suggesting ONE shared utility function got compiled into each overlay separately (a
  standard PSX overlay-linking pattern), not 13 independent area-specific machines. Porting THIS ONE pattern
  (RE once, reimplement once, wire at each address) is much more tractable than "port each area."
  A specific reproduction was captured live: paused mid field-area-load (`sm[0x48]=2` RUNNING → `sm[0x4a]=1`
  FIELD → `sm[0x4c]=2` → `ov_field_run` case 0/pool-init), the frame's classified scene showed `poly=0 rect=0
  fill=0` (nothing drew) yet the display showed raw texture-atlas noise — `present()`
  (runtime/recomp/gpu_gpu.cpp) unconditionally blits the WHOLE live VRAM buffer and samples the display rect,
  so any texture/asset upload landing in that rect during a state with no held fade bleeds straight through.
  This may also be entangled with the ALREADY-DOCUMENTED open frontier bug just above ("2D-poly overlays...
  on the field 2D-only walk") since a fade rect is exactly the kind of 2D poly that gets dropped/misclassified
  there.
- **fix (3, partially implemented, 2026-07-01):** GAME's `ov_a0l_shard_1` fade sequencer (all 5 calls, guest
  `FUN_8010957C` / `ov_a0l_gen_8010957C` reached via `ov_field_run` sm[0x4e]==0xb) is now natively owned as
  `ov_scene_fade_seq` (engine_stage.cpp) — a 6-step per-node ramp/delay SM on the FIXED global node
  `0x800E8008`. GAME's `FUN_8010810C` render-submit dispatcher's pause-menu page-1 dim-fade sub-branch (task
  byte @0x6B==1: unconditional flat-gray non-ramping fade before falling to still-recomp menu draw
  `FUN_801084F8`) is now `ov_game_submit_810c` — the other 11 dispatcher pages stay recomp (`d0` fallback).
  **OPEN in `ov_scene_fade_seq`:** guest `FUN_8007E9C8`'s 3rd arg (a2) is `0`/`1` at this call site (every
  other known site always passed a2==4); `engine_fade_set`'s 2-arg signature has no parameter for it, so what
  a2 controls here is unresolved and the fade blend mode may not exactly match PSX until dug up. NOT yet
  live-verified (needs reaching the a0l area + the pause menu in a running session).
  Remaining unowned: (a) RE + natively port the generic per-node fade SM pattern shared by a06/a08 (reference
  instance: a06 `0x80117AAC`, generated/ov_a06_shard_0.c:6689-6757; a06 8 calls + a08 1 call) — verify GAME
  shard_1 `0x80108E58`/`0x80108EBC` isn't the same shape first. (b) Once fade rects are natively
  `engine_fade_set` everywhere, ALSO fix `present()`'s raw-VRAM-passthrough-during-empty-frames (hold the
  last composited frame or an explicit black instead of sampling live VRAM when nothing was drawn this frame)
  — user directive: build this as an explicit, faithful fade/transition state machine (matching what each
  real PSX transition already does), not an ad-hoc "cache last frame" heuristic.
- **verification note:** each area's port needs a LIVE reproduction (reach that specific area/overlay in
  gameplay) to RAM/scene-diff — this is NOT verifiable by screenshot alone (see the AO-outline miss above).
  Use the debug server (`tools/dbgclient.py`, `PSXPORT_DEBUG_SERVER=1`) to inspect a paused live session
  non-destructively (frame/stage/scene commands) rather than guessing from stills.
- **refs:** runtime/recomp/gpu_gpu.cpp:617-618 (ssao_pass/shadow_pass stubs), commit 9f1ab11 (shaders_vk
  deletion), game/render/gpu_lib.cpp:75 (ov_8007E9C8, orphan), game/scene/sop.cpp + engine_stage.cpp
  (existing engine_fade_set call sites), generated/ov_a06_shard_0.c:6689 (reference per-node fade SM),
  docs/findings/render.md "2D-poly overlays... open frontier" (above), tools/dbgclient.py (live inspection)

## Opening-cutscene narration renders nothing on the native FIELD path
- **symptom:** New Game → the opening story cutscene ("Tomba is living peacefully in the country when Zippo finds a mysterious...") draws NOTHING; the prior menu's stale VRAM shows through (looks like the menu "overstays"/garbles).
- **status:** fixed 10a07e0
- **cause:** the cutscene runs in the FIELD/GAME stage (0x8010637C). ov_draw_otag's field branch (game_tomba2.cpp) runs ov_scene_native (PC-native 3D world) and SKIPPED the PSX OT walk entirely — so ALL guest 2D (the narration glyph SPRITES the field submits to the OT) was dropped.
- **fix:** game_tomba2.cpp field branch now runs ov_scene_native THEN a 2D-only OT walk (g_ot_2d_only=1; gpu_dma2_linked_list; =0), queuing leftover 2D HUD sprites as RQ_HUD on top of the native world. gpu_native.cpp g_ot_2d_only mode DROPS all guest-OT polys (the field 3D world is native-owned; is3d is unreliable here — projprim is empty on the field path, so is3d==0 for every poly → keeping them re-emits the whole world as flat HUD = render-queue overflow + the free-roam crash) and keeps only 2D HUD sprites.
- **refs:** journal later-252; game_tomba2.cpp ov_draw_otag; gpu_native.cpp g_ot_2d_only

## 2D-poly overlays and world-billboard sprites on the field 2D-only walk (open frontier)
- **symptom:** in-field gradient/fade PANELS (polys) or world-billboard sprites may be missing or flat in the 2D-only field overlay pass.
- **status:** known-issue (frontier)
- **cause:** on the native field path projprim/obj_depth provenance is empty, so 2D polys can't be told from 3D-world polys (all is3d==0). g_ot_2d_only drops all polys to avoid re-emitting the world; world-billboard sprites aren't discriminated.
- **fix:** NOT yet solved. The cutscene text + common HUD are sprites and work. Don't "fix" by keeping field polys in the 2D walk — that re-introduces the render-queue overflow/crash (see above).
- **refs:** journal later-252; gpu_native.cpp g_ot_2d_only comment

## native field path renders ONLY the sky/sea backdrop — the 3D world (grass/house/tree/Tomba) is occluded
- **symptom:** in the GAME free-roam seaside field, the native render path (`ov_scene_native`, field-default in `ov_draw_otag`) shows the cyan sea + sky backdrop + 2D HUD but NO 3D world; `PSXPORT_RENDER_PSX=1` (PSX OT walk) renders the full correct scene (grass/house/tree/fence/crane/Tomba). Same on SDL_GPU and the old VK renderer (USER-confirmed).
- **status:** FIXED 2026-06-26 (texpage-provenance backdrop classification) — was the later-235..245 "sea on top" / render-ordering blocker, OPEN #1
- **cause:** ENGINE-SIDE render ordering, NOT a renderer regression — the SDL_GPU port reproduced the same depth-band behavior the VK path had. The decoupled native scene (`ov_scene_native`: backdrop tilemap RQ_BACKGROUND + terrain + entity lists at real depth) draws the backdrop but the world geometry does not land in front of it on this path. The PSX OT walk is correct because it uses PSX OT order. The render-queue band math in gpu_gpu.cpp is internally consistent (backdrop set_order_2d_bg≈0 far, world ord3d∈[0.0625,0.9375], HUD≈1 near; GREATER_OR_EQUAL + clear 0).
- **UPDATE (2026-06-26, SBS evidence — RULES OUT the "not queued" hypothesis):** the prior note guessed "the gap is in what `ov_scene_native` actually QUEUES for the world." That is WRONG. The SBS dump (`PSXPORT_SBS_MODE=both`, after the black-pane fix — see findings/sbs.md) traces the native core at free-roam emitting `worldquads=1299` → `batch tex=11382` (vs the PSX core tex=4497), yet the native pane still shows only sky/sea. So the world geometry IS walked, IS queued, AND DOES reach the geometry batch — it is RASTER-OCCLUDED by the backdrop, not missing. Next step is therefore the DEPTH/ORDER at raster (does the backdrop write depth that occludes the world? is the world ord3d landing behind set_order_2d_bg? is depth-test direction/clear wrong for these prims?), NOT the queue/emission. The SBS view is the tool: left native pane vs right PSX pane.
- **ROOT CAUSE (2026-06-26, SOLVED via SBS layer-isolation diags):** the seaside sky/sea backdrop is drawn TWICE on the native field. (1) `ov_scene_native`→`ov_bg_tilemap_native(0x800ed018)` draws it CORRECTLY as ~352 `RQ_BACKGROUND` quads (far band, behind world). (2) the GUEST background drawer (FUN_80115598) also builds its 16×16 sky/sea tiles into the OT; the field's 2D-only OT walk (`g_ot_2d_only`) then walks them. Those guest tiles are MIS-CLASSIFIED as `RQ_HUD` (nearest band, ord 0.9375–1.0) because each 16×16 tile is far below `bg_2d`'s ≥¾-screen coverage threshold AND `node_is_bg` provenance never fires — the provenance recorder `ov_bg_tilemap` (engine_submit.cpp:2036, calls `gpu_bg_range_add`) is DEAD CODE: it uses `rec_super_call`/`g_override_tgt` and was orphaned when the override system was removed (2026-06-22). With no provenance, the redundant guest tiles land in HUD and draw OVER the entire native 3D world → the field shows only sky/sea. This is the later-235..245 regression. PROOF (SBS dumps, all at free-roam): `PSXPORT_ONLYWORLD=1` → native pane shows the full correct world (grass/house/tree/Tomba) on black; `PSXPORT_NOHUD=1` → full correct world + the real native backdrop sliver; `PSXPORT_NOBG=1` → NO change (the occluder is not RQ_BACKGROUND); `debug rqhist` → per-frame bg=352/world=1001/hud=353. scratch/screenshots/sbs_ow.png, sbs_nohud.png.
- **fix:** restore backdrop provenance WITHOUT the removed override system: `ov_bg_tilemap_native` publishes the active backdrop texpage; the OT-walk sprite classifier treats a field sprite (`g_ot_2d_only`) sampling that texpage as `bg` → `RQ_BACKGROUND` → dropped (the native backdrop already owns it). Genuine HUD (banner/dialog, different texpage) is unaffected. Diags added this session: `PSXPORT_ONLYWORLD` / `PSXPORT_NOBG` / `PSXPORT_NOHUD` (layer-isolation, gpu_native.cpp gpu_emit_rq_item), `debug rqhist` (render_queue.cpp flush), `PSXPORT_PRIMAT="x,y,f0"` (frame-floor). Verify via `PSXPORT_SBS_MODE=both` dump: native (left) must match PSX (right), banner still present.
- **refs:** journal later-235..245, engine_submit.cpp ov_scene_native, game_tomba2.cpp:219 (field routing), gpu_native.cpp rq_emit_or_queue, gpu_gpu.cpp ord3d/set_order_2d_bg

## Intro-narration cutscene rendered wrong (void = sea, cliff banded) — native field path hijacked the SOP scenes
- **symptom:** the intro NARRATION (after New Game): the dark "void" beat ('Was she kidnapped?') drew the SEA + characters instead of a black void with a swirl EFFECT; the cliff beat's water looked banded/striped
- **status:** fixed (later-281) — verified vs the software-GPU oracle (void = purple swirl + Tomba + text; cliff = grassy cliff + clean sea), free-roam unchanged, gates green
- **cause:** the SOP intro narration runs in the GAME stage, so game_tomba2.cpp treated it as the walkable FIELD: it ran the native scene render (ov_scene_native = terrain+entity-list world) AND walked the guest OT in 2D-ONLY mode (g_ot_2d_only=1, dropping fills/backdrop/world prims). But the narration is a 2D-COMPOSITED cutscene whose WHOLE picture (full-screen fills, semi-transparent textured EFFECT quads, character sprites, sea tiles, text) is built by the dispatched PSX SOP code into the guest OT (oracle prim-trace proof). So the 2D-only filter DROPPED the cutscene's fills/effect (leaving ov_scene_native's stale field sea showing in the void), and the native 3D field fought the cutscene. The oracle (interpreter + software GPU, docs/oracle.md) proved the PSX renders the whole cutscene from its GP0 stream.
- **fix:** detect the narration by the loaded MODE overlay (the SOP overlay's first insn *(0x80109450)==0x3C021F80 — the same check ov_game_submode0 uses; sm[0x4a] is NOT reliable, free-roam settles back to sm[0x4a]==0). For the narration, walk the FULL guest OT (g_ot_2d_only=0) so the cutscene's 2D layer draws, and run the native 3D scene render (ov_scene_native) ONLY for the 3D-world beats — skip it for the dark VOID beat (SOP scene byte 0x800bf9b4==5, a pure 2D effect scene) so it doesn't draw a stale field/sea behind the swirl. game_tomba2.cpp ~L231.
- **refs:** later-281, engine/game_tomba2.cpp, docs/oracle.md, docs/narration-port.md, oracle prim-trace (PSXPORT_SELFTEST=oracle, g_oracle_prim_log)

## Intro-cutscene FREEZE + red-diagonal render corruption over idle frames — render walk overflows task0 stack into the task table
- **symptom:** after `newgame` (REPL) the opening runs but Tomba HANGS in the caught-on-fishing-line pose and never reaches the fisherman/house-fire dialog no matter how long you idle (`run`); at ~f3705 (frame counter, ≈f3737) the whole screen degrades into DARK-RED DIAGONAL garbage that grows worse each frame. Field STATE looks unchanged (scene byte 0x800bf9b4=0, MODE overlay 0x80109450=0x801138A4, stage GAME). Circle does NOT advance it (there is no dialog up — it's frozen BEFORE the dialog); only Start-`skip` (pulsing Start) forces past it.
- **status:** FIXED (later-284b). Removed the redundant PSX-render-underneath; `dv_restore_pre` alone keeps guest state correct. Cutscene now plays through (narration → cliff → free-roam field), no freeze, no red corruption. VERIFIED: oraclediff convergent through free-roam onset; screenshots fx_700/1000/1145 render clean. Exposed the NEXT frontier: `jal 0x80109450` recomp-MISS in free-roam (A00-resident MODE slot has a jump-table there, not a fn — see the sibling finding below).
- **cause:** the "PSX render UNDERNEATH" — engine/engine_stage.cpp:304-308 in ov_field_frame runs `d0(c,0x8003f9a8)` + `d0(c,0x8010810c)` EVERY field frame AFTER the native ov_render_frame, purely to keep guest OT/packets/scratchpad in the PSX-correct state that still-recomp content reads back (user 2026-06-24 scaffolding). That recompiled 0x8003f9a8 orchestrator runs on TASK0's guest stack (top = mem_r32(0x801fe008) = 0x801FEA00, only ~2KB above the task table at 0x801fe000, stride 0x70) and recurses deep: 0x8003F9A8 → 0x8003BB50 → ov_a00_gen_80122974 (an A00 node render fn) → 0x8003CCA4 → 0x8003CDD8 → 0x8003F698 → ov_a00_gen_80146478 (generated/ov_a00_shard_1.c, the A00 GT3/GT4 submitter, later-274). Most field frames fit ~2KB; the island intro-cutscene's ov_a00_gen_80122974 object pushes the guest SP down from 0x801FEA00 to 0x801FE038 (verified: `watch 0x801fe048 0x801fe04a` + PSXPORT_CW_BT → sp=0x801FE038), so ov_a00_gen_80146478's normal `sw r16,0x10(sp)` lands ON 0x801fe048 = task0 sm[0x48], writing r16 (17). sm[0x48]=17 is not a valid GAME top-state: ov_game_frame returns 0 ("unknown top state", `debug gframe`) → task0 drops to the game_coop PSX loop 0x801063F4, which only dispatches sm[0x48]∈{0,1,2} → it yields forever = the FREEZE (never reaches the fisherman/house-fire dialog). SECONDARY: the game_coop re-entry (native_boot.cpp ~L405-414) resets r16/r17/r18/r31 but NOT r29, so each frame FUN_80051f80's `addiu sp,-0x18` is re-applied and never unwound → task0 SP leaks 0x18/frame → after ~2500 frames overwrites live data = the growing red-diagonal corruption at ~f3705. NOTE the NATIVE render walk (ov_render_frame/ov_render_walk) is shallow and is NOT the culprit (`debug rwalk` = "NATIVE walk active"). The sm[0x48]=17 clobber ALSO occurs in the interp+softGPU harness (oraclediff core A) but there task0 isn't gating on it identically so it still progresses — proof the write-collision is the shared root, not a newgame-only artifact.
- **fix (DONE, later-284b):** DELETED the PSX-render-underneath (`d0(0x8003f9a8)` + `d0(0x8010810c)`) in ov_field_frame (engine_stage.cpp). It was REDUNDANT: `dv_restore_pre` (kept) already restores the FULL post-gameplay guest state (2MB RAM + scratchpad + GTE), undoing every guest write the native render made, so still-recomp content reads the correct PSX state WITHOUT any re-render. Nothing consumed the PSX-built OT/packets — the native display (ov_scene_native/ov_draw_otag) re-derives its transforms from node/entity data, not from leftover OT. The re-render's ONLY effects were (1) the deep-recursion task0-stack overflow that clobbered sm[0x48]=17 → freeze, and (2) the game_coop re-entry r29 SP-leak → red corruption; removing it kills BOTH. **The empirical finding that overturned the handoff's assumption:** the handoff said dv_snapshot/restore must ALSO be deleted (contingent on first making the render write-free); but removing ONLY the redundant re-render fixes the freeze/corruption with dv_restore RETAINED as the decoupling mechanism — PROVEN correct by oraclediff (native==oracle, only benign baseline bytes, through free-roam onset f1131). Making ov_render_frame write ZERO guest memory (native-float A00 object render, then delete the dv rewind for perf) remains a valid FOLLOW-UP, but was NOT required to fix this bug. Verified: no render-path write to sm[0x48]; oraclediff convergent; screenshots render clean narration→cliff→free-roam.
- **refs:** later-284, engine/engine_render_walk.cpp (ov_scene_native/submit_perobj_render), engine/engine_stage.cpp (ov_game_frame ret0), runtime/recomp/native_boot.cpp:405 (game_coop r29), generated/ov_a00_shard_1.c (ov_a00_gen_80146478), scratch/screenshots/repro3735.png. Diagnostics: `watch <lo> <hi>` + PSXPORT_CW_BT=1 (mem.cpp), `debug gframe` (engine_stage), `debug sched/yieldpc`, PSXPORT_SELFTEST=oraclediff progression capture (selftest.cpp).

## Free-roam recomp-MISS: `jal 0x80109450` with A00 resident — FIXED (later-286): recompiler fall-through-into-fragment guest-sp leak
- **symptom:** after the later-284b freeze fix, `newgame; run` plays narration→cliff→free-roam field, then aborts ~53 frames into free-roam (≈f1184, no input) at `[recomp-MISS] no recompiled fn for 0x80109450 (caller ra=0x801088B0)`.
- **status:** ✅ FIXED (later-286, commit pending). `newgame; run 6000` now holds free-roam steady (t0 st=2 s48=2) to the end with ZERO net sp leak and no miss. **BOTH prior diagnoses were WRONG:** later-284c blamed the A00 render recursion (falsified by later-285); later-285 then blamed a "gameplay object-graph recursion ~4× deeper than the oracle" (ALSO WRONG — it is not a recursion-DEPTH divergence at all).

### ROOT CAUSE (later-286) — a recompiler mis-emission LEAKED 0x28 of guest sp every field frame
- **The real defect:** the recompiler (`tools/recomp/emit.py`) DROPPED a control-flow edge. `emit_func` only chained a function that FALLS THROUGH into the next function (`hi`) when `hi in reentry` (a narrow special-case added for the GAME prologue); every other genuine fall-through got a bare `return;`. `gen_func_80022854` (a jump-table case fragment of the object-update loop 0x80022760, fires once/frame at free-roam) ends in `jal 0x80022190` and then FALLS THROUGH into the shared-frame epilogue fragment at `0x8002285c` (which does `addiu sp,+0x28; jr ra`). With the bare `return;`, that epilogue NEVER ran on this path → the 0x28 frame allocated by 0x80022760's prologue leaked **every frame**. The interpreter (oracle) follows the real fall-through, so it never leaked — which is exactly why the oracle's task0 sp stayed shallow (0x801FE7A8) while the native port's crept down 0x28/frame.
- **Why it manifested in the render + as the 0x80109450 miss:** the guest sp is persisted across frames in the task context (`task_ctx[i].r[29]`, native_boot.cpp), so the 0x28/frame leak ACCUMULATES. After ~50 free-roam frames the sp had bled from ~0x801FEA00 down to ~0x801FE0F8; the per-frame native render pass `ov_render_frame → ov_a00_gen_80122974 → 8003CCA4 → 8003CDD8 → 8003F698 → 0x80146478` (substrate, runs on task0's stack) then overflowed into the task table at 0x801FE04C, clobbering sm[0x4c]/[0x4e] → sm[0x4a]→0 → ov_game_frame ret 0 → game_coop → `jal 0x80109450` MISS. So the miss ADDRESS and the RENDER chain were both red herrings (later-284c/285's mistake); the origin is the linear per-frame sp leak in the GAMEPLAY object-update dispatch.
- **How it was found:** min-sp probe in ov_render_frame showed entry sp decreasing exactly 0x28/frame (a leak, not deep recursion); a net-sp probe around `ov_game_frame` then around each `ov_field_frame` sub-call pinned it to `d0(0x80022a80)`; a per-dispatch net-sp probe (`spwho`) pinned the deepest −40 to 0x80022760; disassembly + generated-code inspection found `gen_func_80022854` emitting `func_80022190(c); return;` (dropping the fall-through to `func_8002285C`).
- **The fix (2 parts):**
  1. `emit.py` `emit_func`: chain the fall-through whenever `hi in funcset and body_falls_through()` (drop the `hi in reentry` restriction). `body_falls_through()` returns False for a normal `jr ra`/`j` terminator, so natural function boundaries are unaffected — only a body whose last instruction actually reaches `hi` (normal insn / conditional branch / **jal**) chains. This is a general recompiler correctness fix (any mid-function-split shared-frame fragment).
  2. That fix let free-roam run long enough to surface a LATENT discovery gap (same class as later-272): `gen_func_8003CCA4`'s perobj-render `jr v0` dispatches a node's render-cmd handler `0x8003D8AC` — a jump-table case-label INSIDE `gen_func_8003D584`'s range, never emitted as a function entry → recomp-MISS via the switch default. Seeded `0x8003D5CC` + `0x8003D8AC` in `emit.py` EXTRA_SEEDS.
- **refs:** later-286. `tools/recomp/emit.py` (emit_func fall-through chain ~L514; EXTRA_SEEDS 0x8003D5CC/0x8003D8AC). Repro (was): `newgame; run 4000` aborted ~f1184; now runs clean. Bumped RECOMP_VERSION.

### CORRECTION (later-285) — the deep recursion is GAMEPLAY, not render; native recurses ~4× deeper than the oracle [SUPERSEDED by later-286 — see above; the "recursion depth" framing was WRONG, it was a linear per-frame sp LEAK]

### CORRECTION (later-285) — the deep recursion is GAMEPLAY, not render; native recurses ~4× deeper than the oracle
- **What later-284c got wrong:** it fingered the A00 per-object RENDER chain (ov_render_frame → render walks → ov_a00_gen_80122974 → 0x8003CCA4→…→0x80146478) as the overflow. It is NOT. Instrumentation (a min-sp probe in rec_dispatch / interp, `debug lowsp`) shows: (1) owning 0x80122974's 3D render native-float and routing the walk's RCASE_DEFAULT + rq_dispatch_case to it did NOT stop the crash; (2) `debug skip3d` skipping the WHOLE `ov_render_frame` orchestrator AND the submit `0x8010810c` STILL crashes at the same ~f1184 / same `jal 0x80109450` miss. So the deep recursion is NOT in the render pass. A scope flag (`g_in_scene_native`) confirms the deepest dispatches happen with `in_scene_native=0` — i.e. in task0's own guest execution, not the native display pass.
- **Where it actually is:** task0's FIELD-FRAME GAMEPLAY update (engine_stage.cpp ov_field_frame ~L286, `d0(c,0x80022a80)` and the sibling passes) recurses the OBJECT GRAPH via the indirect-call thunk 0x80022AB8 (`jalr v0`), ~10 levels deep, interleaving the object 2D-marker projector 0x8013DD34 (builds OT sprites during update) and the libgpu OT/GS-sort walker 0x80082D04↔0x80082734 (journal: "the ordering-table linked-list walker … FUN_80082d04 submits OTs"). The min sp reaches 0x801FE078; the recomp leaf frames below that reach ~0x801FE038 and a `sw rX,0x10(sp)` clobbers 0x801FE04C = task0 sm[0x4c]/[0x4e] → sub-machine corrupt → sm[0x4a]→0 → ov_game_frame ret 0 → game_coop → `jal 0x80109450` MISS (this downstream chain is unchanged from later-284c and still correct).
- **THE decisive datum — oracle vs native, SAME task0 layout:** `PSXPORT_SELFTEST=oracle PSXPORT_DEBUG=lowsp` runs the pure interpreter through the WHOLE opening to f4235 (free-roam A00, ovsig 0x801138A4) and its GLOBAL-MINIMUM task0 sp is **0x801FE7A8** (uses only ~0x258 bytes; deepest 0x80082738 recursion is ~2 levels). Task0 obj=0x801FE000, stack top=0x801FEA00 — IDENTICAL to the native port. The native port at the SAME free-roam scene reaches **0x801FE078** (~0x988 bytes, ~10 levels) — ~4× deeper. So the guest stack is NOT too small (the real game / oracle fit fine); the native port RECURSES TOO DEEP / uses too much guest stack per level. The overflow is a SYMPTOM of that divergence.
- **NEXT (the real fix, unstarted):** find WHY the native port's object-graph recursion is ~4× deeper than the oracle at the same free-roam frame. Two hypotheses to bisect: (A) a native override in the object-update recursion path LEAKS guest sp (`c->r[29] -= X` without restore — the same class of bug as the game_coop r29 leak fixed in a766217; candidates: ov_objwalk 0x8007a904, ov_disp_26c88 0x80026c88, behaviours in engine/beh_*.cpp, or 0x8013DD34's callers) → each level bloats the frame; (B) a native object-spawn/placement divergence (ov_place_objects / pool init) builds a longer/looping child chain than the oracle → more recursion LEVELS. Method: run native vs oracle to the same free-roam frame, dump the object graph (`ents`/`node`) + the recursion backtrace from both, diff. `oraclediff` is convergent through free-roam ONSET (~f1124) but the divergence bites ~f1181 — checkpoint the diff RIGHT THERE. Tools built this session: `debug lowsp` (min-sp tracker, currently REMOVED — re-add the ~6-line probe in overlay_router.cpp rec_dispatch + interp.cpp if needed).
- **root cause (later-284c, VERIFIED):** native runs A00 free-roam on the NATIVE path (ov_game_frame → ov_game_s48_2_frame → ov_field_run, sm[0x4a]=1/[0x4c]=2/[0x4e]=9) CORRECTLY and steady from f1124→f1180, byte-matching the ORACLE — which holds `sm[48]=2 [4a]=1 [4c]=2 [4e]=9, bf89c=2, e7e68=0` ROCK-STEADY forever (Tomba caught on the fishing line; proven via `PSXPORT_SELFTEST=oracle` + a temp sm-probe in run_oracle: it NEVER leaves s4e=9, and e7e68&8 the only case-9 advance trigger stays 0). At ~f1181 native's per-frame render (ov_render_frame → ov_render_walk_snapshot 0x8003bb50 → submit_render_walk_snapshot → rq_dispatch_case → **RCASE_DEFAULT 0x8003C29C → `rec_dispatch(node+24)`**) hits a DEEP A00 object renderer (the fishing-line-pull object: ov_a00_gen_80122974 → 0x8003CCA4 → 0x8003CDD8 → 0x8003F698 → ov_a00_gen_80146478) that RECURSES on task0's ~2.5KB guest stack (top 0x801FEA00, task table 0x801FE000). sp reaches 0x801FE038; a leaf `sw rX,0x10(sp)` (resident 0x8004798C/0x8004602C) clobbers 0x801FE04C = sm[0x4c]/[0x4e] (`watch 0x801fe04a 0x801fe050` shows word stores of spill values 0x800498F0/0x00000800 at sp=0x801FE038). That corrupts the sub-machine → sm[0x4a]→0; ov_game_frame then returns 0 (s48=2,s4a=0,SOP-not-loaded, `debug gframe`) → task hands to the recomp game_coop loop → recomp 0x80108784 → 0x8010882c → `jal 0x80109450` into A00's jump-table data → recomp-MISS. So the miss ADDRESS is incidental; the DEFECT is the deep A00-object render overflowing task0's tiny guest stack — the SAME deep chain later-284b removed from the PSX-underhood, still present in the NATIVE walk via rq_dispatch_case RCASE_DEFAULT. This is why the freeze fix advanced to free-roam but no further: it removed ONE caller of the deep chain (the underhood), not the native walk's.
- **A00 0x80109450 is a jump-table not a fn** (why the miss address is unrecompiled): MODE-slot sig at 0x80108F9C == A00's `0a000000aca31080`; A00.BIN offset 0x4B4 (=0x80109450) is all `0x801138A4` words (default-handler ptr table). SOP.BIN has a real fn there (`lui v0,0x1f80; …` = ov_sop_gen_80109450). GAME.BIN 0x8010882c's hardcoded `jal 0x80109450` (word 0x0C042514) is a SOP-mode path; it should NEVER run under A00 — and doesn't, until the stack-overflow corruption forces s4a=0.
- **fix (was TODO, now KNOWN-INSUFFICIENT):** ~~own the A00 per-object render native-FLOAT so the render runs on the C stack~~ — later-285 proved the render is NOT the overflow source (skipping it entirely still crashes). The A00 render native-float ownership was prototyped this session (native ov_a00_node_render: submit_perobj_render for the 3D model + faithful 0x8013DD34 marshalling) and REVERTED — correct-by-RE but tangential + visually unverified. Real fix = the DIVERGENCE hunt in the correction block above (native recurses ~4× deeper than the oracle in the GAMEPLAY object-update). Do NOT bandaid (enlarge task0 stack / scratch render stack — banned).
- **refs:** later-284b/284c/285. engine/engine_render_walk.cpp (ov_render_walk_snapshot:511, submit_render_walk_snapshot, rq_dispatch_case, RCASE_DEFAULT 0x8003C29C, submit_perobj_render:215), generated/ov_a00_shard_1.c, generated/ov_sop_shard_0.c, overlay_table.c:34/60. Repro: `newgame; run 4000` aborts ~f1184. Diagnostics: `watch 0x801fe04a 0x801fe050` (task-table spill), `debug gframe` (ret0 s4a=0), `PSXPORT_SELFTEST=oracle` (oracle holds s4e=9 steady).

## 0x8013DD48 is a GTE cull/midpoint leaf (sibling of the already-excluded 0x8013DD34) — LEAVE PSX
- **symptom / task framing:** flagged as an un-owned "HOT (8 native callers)" address in the
  0x8012xxxx-0x8013xxxx behavior/AI region, assumed to be per-area event-dispatch game logic worth
  owning (per "no engine-vs-content fence").
- **status:** RE'd, then EXCLUDED — this is a render/GTE hardware leaf, not game logic. Do not port.
- **RE (Ghidra headless on `scratch/bin/tomba2/ram_derail2.bin`, the A00-resident dump):** the fn body
  computes a MIDPOINT between two vec3 pointers (s0, s1 — averages each of x/y/z: `(a+b)>>1` with
  round-toward-zero via the sign-extend trick), writes it to scratchpad `0x1F8000C0..C4`, then issues
  real `cop2`/GTE opcodes (RTPS-style perspective transform) against that scratchpad block, and finishes
  with a GTE flag-register check (`lw v0, 0x1F800080`) that drives a clip/depth branch. This is EXACTLY
  the GTE-compose family already identified and intentionally left PSX at the sibling address
  **0x8013DD34** (docs/port-progress.md "later-283": "0x8013DD34 cull/bound... intentionally left PSX
  (transcribing GTE compose is banned by the RENDER directive)") — 0x8013DD48 sits 0x14 bytes after it
  in the same object-2D-marker-projector code (also referenced as a recursion-depth suspect in the
  later-285/286 stack-leak investigation above, "0x8013DD34's callers").
- **decision:** per CLAUDE.md's render directive ("Never read/honor/reproduce... GTE output... A native
  fn that reproduces PSX instructions/packets byte-for-byte is PSX-simulation"), this is a hardware GTE
  leaf, not portable game logic. LEAVE PSX (rec_dispatch, unchanged) — same call as 0x8013DD34.
- **correction to the RE-first process:** address-range heuristics ("0x8012xxxx-0x8013xxxx = behavior
  region") are not a substitute for RE — always disassemble before classifying. Two of this cluster's
  three OTHER addresses (0x8012866C, 0x8012E168) also turned out to have non-trivial structure; see
  docs/findings/scene.md "un-owned entity-behavior register-implicit leaves" for those.
- **refs:** docs/port-progress.md later-283 (0x8013DD34 precedent); scratch/decomp (Ghidra project
  `tomba2_derail2`, `scratch/bin/tomba2/ram_derail2.bin`).

## `NodeXform`'s 6 methods missing guest-stack-frame mirror — reproducible f117-class SBS residual (2026-07-08)

- **symptom:** after owning `Render::perObjRenderDispatch`/`billboardCompose1`/`billboardCompose2`/
  `billboardEmit` (game/render/perobj_billboard.cpp, commit c6a780f), SBS full mode showed a
  reproducible residual at 0x801FE8E4..0x801FE8EF around f117-f118, converging by f157 and staying
  clean for 8000+ further frames every run.
- **root cause:** `NodeXform`'s 6 methods (game/render/node_xform.cpp) — `build`/`buildWithOffset`
  (0x80051844/0x800518FC, 32 B frame), `propagate` (0x80051128, 56 B), `propagateRotmat`
  (0x80051300, 40 B), `propagateAxis` (0x80051464, 48 B), `buildAxis` (0x80051C8C, 32 B) — were landed
  in `d0eb6f9` BEFORE `37594c8` mandated guest-stack frame mirroring, and NONE of them touch `c->r[29]`
  at all despite every one having a real recomp frame (verified against `generated/shard_*.c`
  prologues: each descends r29 and spills live r16../ra at RE'd offsets, then restores on return).
  Last-writer trace at the divergent address showed core B (recomp) writing via
  `NodeXform::propagateAxis`'s own compiled spill, while core A (native) wrote via a totally
  unrelated function (`GraphicsBind::installSceneRecord` / other still-substrate leaves) — i.e. on
  the native side that stack address belonged to a DIFFERENT function's frame than on the recomp
  side, because propagateAxis (and siblings) never staked out their own frame there, letting whatever
  else was running at that moment "own" the address instead.
- **fix (part 1, frame descent):** added 6 named RAII frame structs (`BuildFrame`, `BuildAxisFrame`,
  `PropagateRotmatFrame`, `PropagateAxisFrame`, `PropagateFrame`) to node_xform.cpp, each spilling the
  LIVE `c->r[16..23]`/`c->r[31]` values at the RE'd offsets on construction and restoring on
  destruction — same pattern as `Cull::performBaseCullFramed`. This fixed the SP trajectory but left
  2 residual `sbs-div` at f117/f157 (0x801FE8E4..EF).
- **fix (part 2, register faithfulness) — the actual close to TRUE 0-diff:** the last-writer map
  (extended to also dump `sp`) showed BOTH cores hit 0x801FE8E8 at the identical sp=0x801FE8D8 — so
  the frames were sp-faithful; the divergence was the VALUE spilled. A per-write UPPROBE dump named it:
  the recomp bodies load `node` into CALLEE-SAVED registers (`gen_func_80051C8C buildAxis: r16=node,
  r17=node+152`) and then TAIL-CALL a nested NodeXform fn (`propagateAxis`) whose prologue spills the
  caller's live `r16..r22` into its own frame. On the recomp side that spill = `node` (0x800FD010); on
  the native side the C++ body uses local variables and never updates `c->r[16]`, so it spilled a
  stale 0x1000. Fix: each OUTER NodeXform fn making a nested NodeXform call now sets its callee-saved
  node/scratch registers to the RE'd recomp values right after the frame descent (build/buildWithOffset:
  `r16=SCR_M, r17=node, r18=SCR_R`; buildAxis: `r16=node, r17=node+152`). The nested callee then spills
  the correct bytes; the frame RAII still restores the caller's incoming values on exit.
- **why the scope is bounded:** the Math leaves (rotmat 0x80085480 / matMul 0x80084110 / rotpair
  0x80085050 etc.) are FRAMELESS — they never descend sp and only use r2..r15/r24/r25, so they never
  spill a callee-saved register. The ONLY nested calls that spill callee-saved regs are the
  NodeXform->NodeXform tail calls (build/buildWithOffset->propagate, buildAxis->propagateAxis), so
  fixing register state in those 3 outer functions is complete.
- **result:** `PSXPORT_SBS_MODE=full` autonav, headless, `isDeadStackScratch` deleted:
  `grep -cE 'sbs-div|VIOLATION'` = **0** through f7440. TRUE zero-diff, no masks, no exclusions.
- **lesson:** frame-descent mirroring gets sp right but NOT register state. When a stack-spill byte
  still diverges after mirroring the frame, check whether the spilled value is a CALLEE-SAVED register
  the recomp loaded (node/scratch-ptr passed through r16..r23) and a nested callee is spilling it — the
  native body must maintain those registers, not just the sp. The extended last-writer `sp` field
  (both cores same sp => it's register state, not sp) is the tell.
- **refs:** commit landing this fix (see git log, "render: register-faithful NodeXform nested spills");
  docs/findings/animation.md (sibling investigation, same day, for `Animation::attach`).
