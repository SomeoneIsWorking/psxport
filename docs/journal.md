# Debug / progress journal

## later-290 — wire + SBS-verify ActorTomba::frameTick (FUN_8005950C); root-cause the f158 register-faithfulness divergence
Wired the wide-RE draft of Tomba's per-frame G-block driver onto the live path (EngineOverrides;
the only core-A reacher is the native `frameStartTickFaithful`'s `default: rec_dispatch(c,
0x8005950Cu)`, the sole direct `func_8005950C` caller being the substrate `gen_func_80059D28`
which is core-B only — so no `shard_set_override` dual-wire). The 5 RE'd-but-untrusted sub-callees
(turnBiasCompute / outerTransitionGate / outerTransitionCommit / assetReady / resetLoadGate) stay
dispatched to the SUBSTRATE from frameTick — line-by-line vs generated/shard_4.c found real bugs in
the first two drafts (turnBiasCompute 0x800-threshold branches swapped; frameTick's own case-1
skipClear had the 0x800BF80F condition inverted, fixed). Each sub-callee gets its own verify+wire
pass. Every dispatch sets the gen jal-site r31 constant first so substrate callees that spill ra
byte-match core B.
- ROOT CAUSE of the first divergence (the real work this session): the first wiring diverged hard
  at lockstep f158 — `[sbs-div] 0x801FE8DE..E0 A=80 1F B=00 00`, last-writer Animation::step
  (gen_func_80076D68) spilling the caller's r17. Per the user directive ("don't stash/bisect, use
  Ghidra or compare against recomp"), root-caused by reading the oracle gen_func_8005950C
  (generated/shard_4.c:7624) line-by-line rather than bisection. Cause was INTRA-BODY register
  faithfulness: gen keeps r17/r18 live across the case-1/4/7 callees (r17=ECF54_saved/
  r18=E7E68_saved in cases 1,4 [gen 7648/7650, 7693/7695]; r17=sub/r18=1 in case 7 [gen
  7736/7737]), and those case callees read the values via their OWN prologue spills (func_800597AC
  spills r18, func_80053FDC spills r17, func_80076D68 spills both). The draft used C++ locals for
  frameTick's own logic and never wrote c->r[17]/c->r[18] — so substrate callees spilled stale
  caller values on core A (0x1F80xxxx scratchpad ptr) vs the saved/sub values on core B
  (0x0000xxxx zero-extended flag). Fix: write c->r[17]/c->r[18] at the same points gen does. This
  is a distinct flavor of the register-faithfulness family already in docs/findings/animation.md
  (NodeXform/attach frame-mirror): the FRAME prologue was already correct; the gap was mid-body
  callee-saved assignments held live across callees. Recorded there + a general audit rule.
- VERIFY: PSXPORT_SBS_MODE=full + PSXPORT_SBS_AUTONAV=1, headless: 0 sbs-div / 0 VIOLATION through
  f15600+ (150 s wall-clock; pre-fix diverged at f158 every run).

## later-289 — own the two PURE-engine ov_field_frame children native (FUN_80025588 / FUN_8004fe84)
Top-down descent per the minimize-recomp frontier (later-287): owned the two remaining ov_field_frame
direct children that make NO overlay/content calls, so they are cleanly A/B-verifiable.
- `ov_scene_25588` (FUN_80025588, game/scene/engine_stage.cpp) — the field EVENT/COMMAND-FIFO state
  machine (struct @0x800ed058): 3-state top switch; state 0 arms (snapshot list head 0x800ecf58, run
  setup 0x80024e00) and falls through; active body drains a 2-array byte FIFO (id @+0x16 / kind @+0x1c,
  count @+0x15, head-idx @+0x14), dispatching entry 0 via 0x80040aa4 then 0x80074bf8, then per GAME phase
  0x800bf870 runs 0x800251f0 (default) or a light-toggle (phase 2/7) or nothing (phase 3/20); always ends
  0x80077b5c. 10 leaf callees stay substrate.
- `ov_scene_4fe84` (FUN_8004fe84) — the 2-phase scene/render-list builder driver (struct @0x800bf548):
  phase 0 arms (snapshot list ptr 0x800ecf64 → +0x2b0/+0x2b4/+0x2b8), phase 1 runs sub-state base[1]
  (0x8004f430/f474/f514/f6d0), then sets/clears bit0 of flag @0x800bf822.
Wired both into ov_field_frame AND ov_field_frame_x (replaced the `d0(...)` calls). ov_field_frame is now
6 native / 9 direct substrate children.
VERIFY: `newgame; run 1500; dumpram` native vs `git stash` substrate → main RAM 0-diff AND scratchpad
0-diff; `newgame; run 4000` → 0 recomp-MISS.
WORKFLOW FIX: codemap.py was mapping the new natives to the wrong guest address because a pre-existing
orphan doc-comment (the "handler 0x801088d8" block, no function under it) merged into the new comment
block; a blank-line separator fixes it. Recorded in docs/findings/tooling.md.

## later-288 — restructure finish: deleted the 2 dead grab-bag files (native_misc, peripheral_misc)
USER directive: no grab-bag files in the codebase, ever. After the engine/->game/ consolidation, two
`*misc*` grab-bags remained: `game/core/native_misc.cpp` and `runtime/recomp/peripheral_misc.cpp`. PROVEN
100% DEAD — every function is `static`/unreferenced and the only exported symbol (`games_native_path_init`,
called from game_tomba2.cpp) ran only empty stubs; the build LINKS and is A/B RAM+scratchpad 0-diff at f1500
WITHOUT them. They were orphaned reference reimplementations abandoned when the override system was removed
(2026-06-22) — reachable only via the deleted `rec_set_override` table. Deleted both + the empty
games_native_path_init wiring. Recoverable from git if a caller is later ported and wants the leaf.
DEAD-END NOTES preserved from native_misc's disabled-reimpl TODOs (these were reimplemented and FAILED A/B
vs the recomp body — do not re-derive without fixing the noted cause):
  - FUN_80097540: A/B 2 bytes off — return-selection edge case (a0==-1/-2 path).
  - FUN_800752B4: A/B 167 bytes — stride-12 band-classify table @0x800BE238, wrong records.
  - FUN_80090160: A/B 80 bytes — varint stream consumer, wrong accumulation.
  - FUN_80094C10: A/B 15 bytes — fixed-point mixer/pan, reciprocal-magic divides.
  - FUN_80077FB0: A/B 4 bytes — 16-bit integer sqrt, wrong result (cascades into 0x800E806x).
Also extracted the interactive REPL driver (repl_btn/wav/xadump + native_repl_read + the g_nav_* auto-drive
state) OUT of native_boot.cpp (1569->1298 ln) into `runtime/recomp/repl.cpp` (+ `repl.h` interface), and the
rc0-4 guest-call helpers into shared `runtime/recomp/guest_call.h`. native_boot.cpp is now boot + scheduler,
not boot + scheduler + REPL. Verified: build clean, REPL functional (ents/dumpram run), A/B RAM+scratchpad
0-diff at f1500. No grab-bag files remain (verified: no *misc*/*util*/*common*/native_path*/native_dl* under
game/ or runtime/). Restructure COMPLETE.

## later-287 — MINIMIZE-RECOMP frontier: `recdep` dependency meter + own 2 more ov_field_frame children native
USER direction (2026-07-01): reduce recomp dependency to as little as possible. Built the `recdep` diagnostic
(`PSXPORT_DEBUG=recdep`, overlay_router.cpp + config.md) — histograms every substrate fn rec_dispatch routes to,
dumps top-40 at exit. Free-roam runs 410 unique substrate fns; the distribution is top-heavy: #1 is rand
0x8009A450 @ 172K calls (86/frame), then the cull cluster (already native) + libgpu 0x80082/83xxx + matrix
0x80051794 + the A00 object handlers 0x8013xxxx (~5/frame each). The lever is TOP-DOWN ownership: own
ov_field_frame's direct substrate children, wiring their leaf calls to native subsystem impls so the hot leaves
get captured as parents go native. Owned 2 more of ov_field_frame's 9 children native (engine_tomba2.cpp):
ov_list_walk_69b28 (FUN_80069b28, 2nd object-list walk head 0x800f2738) + ov_arr8_dispatch_26368 (FUN_80026368,
8-slot array 0x80100400 stride 0x4c, jumptable 0x8009d314) — both route methods via
dispatch_native_behavior|rec_dispatch (no sp munging). A/B RAM+scratchpad 0-diff at f1500 (native vs `git stash`
substrate baseline). Now 4 native / 9 substrate children. Next: the pure engine ones 0x80025588 / 0x8004fe84.

## later-286 — free-roam recomp-MISS FIXED: recompiler dropped a fall-through edge → 0x28/frame guest-sp leak (later-284c AND later-285 both wrong)
The free-roam crash (`newgame; run 4000` → abort ~f1184 at `[recomp-MISS] 0x80109450`) is FIXED. Both prior
diagnoses were falsified: later-284c blamed the A00 render recursion; later-285 blamed a "gameplay object-graph
recursion ~4× deeper than the oracle." Neither was a recursion-depth issue at all. **Root cause: the recompiler
(`emit.py`) DROPPED a control-flow edge.** `emit_func` only chained a function that FALLS THROUGH into the next
function (`hi`) when `hi in reentry` (a narrow GAME-prologue special-case); every other genuine fall-through got a
bare `return;`. `gen_func_80022854` (a jump-table case fragment of the object-update loop 0x80022760, fires
once/frame at free-roam) ends in `jal 0x80022190` then FALLS THROUGH into the shared-frame epilogue at 0x8002285c
(`addiu sp,+0x28; jr ra`); the bare `return;` skipped that epilogue → the 0x28 frame 0x80022760's prologue
allocated leaked EVERY frame. Guest sp is persisted across frames in the task ctx, so it accumulated: after ~50
frames sp bled from ~0x801FEA00 to ~0x801FE0F8, and the per-frame native render pass (ov_render_frame →
ov_a00_gen_80122974 → …→ 0x80146478, substrate on task0's stack) then overflowed into the task table at
0x801FE04C → sm corrupt → ov_game_frame ret 0 → game_coop → jal 0x80109450 MISS. The interpreter follows the real
fall-through so the oracle never leaked (its task0 sp stayed at 0x801FE7A8 — the datum later-285 misread as
"recurses less deep"). Found via a min-sp probe (entry sp −0x28/frame = leak not recursion), then net-sp bisect to
d0(0x80022a80) → 0x80022760 → gen_func_80022854. **Fix:** (1) `emit.py` chains the fall-through whenever
`hi in funcset and body_falls_through()` (general recompiler correctness fix; natural `jr ra`/`j` boundaries
unaffected). (2) That surfaced a latent later-272-class discovery gap — FUN_8003CCA4's perobj `jr v0` dispatches a
node handler 0x8003D8AC (a case-label inside FUN_8003D584, no function entry) → seeded 0x8003D5CC/0x8003D8AC.
Verified: `newgame; run 6000` holds free-roam steady, zero net sp leak, clean exit. See findings/render.md.

## later-275 — narration recomp-MISS 0x8010BF54 fixed (overlay-ID re-validation + dangling-render-pointer guard); next blocker = A00 objects render pre-model-attach
The narration cutscene (full intro, NOT skipped — `newgame` then `run`, no Start) hit `recomp-MISS
0x8010BF54`. TWO distinct substrate bugs, both fixed; then a THIRD (a real render-timing bug) is exposed.
1) **Overlay mis-identification (overlay_router.cpp).** `resident_overlay()` trusted the load-time identity
   cache (`resident_ov[slot]`) without re-checking it against live RAM. During the SOP intro the MODE slot
   genuinely holds SOP (live base bytes match sig_sop exactly), but A00 had been NOTED into that slot earlier
   at GAME setup, so the cache said A00. The SOP narration renderer 0x8010BF54 thus routed to A00's switch
   (mid-function there, no entry) → fail-fast. FIX: validate the cached identity by a 32-byte signature
   memcmp against live RAM; on mismatch, re-scan for the overlay actually resident and refresh the cache;
   fall back to the cache only when nothing matches (the header-mutation case the cache was built for).
2) **Dangling overlay render pointer (engine_render_walk.cpp).** Once A00 *does* load over the MODE slot
   (the field transition), the leftover SOP narration node (0x800ED9E8, type 32, default-case renderer
   node+24 = 0x8010BF54) is a DANGLING pointer (0x8010BF54 is mid-function in A00). The master render walk
   dispatched it → miss. FIX: new router query `rec_addr_has_entry(c,addr)` (uses the per-overlay
   `ov_<tag>_func_index`, now exposed on `RecOverlay.idx` via emit.py — RECOMP_VERSION 2026-07-01.1); the
   walk's default case SKIPS a node whose render-fn is not a real entry of the resident module. The engine
   owns its render visibility and refuses to execute a render pointer into an evicted overlay. (This is a
   GUARD; the deeper fix is tearing the stale narration node out of the render list at the SOP→field
   transition — not yet done.)
3) **NEXT BLOCKER (real render-timing bug, NOT substrate).** With (1)+(2), the narration advances past
   0x8010BF54 and the master walk completes — then the per-object entity loop in `ov_scene_native` overflows
   the render queue: `[rq] FATAL: render queue full (65536)`. ROOT CAUSE (pinned via a count-sanity probe in
   native_gt3gt4): node 0x800FBB70 (an A00 area object) renders with a geomblk pointing into the AREA-DATA
   slot 0x8018A000 holding UNRELOCATED pointers (e.g. geomblk 0x8018A034 = 0x8018AA14, read as 43540 tris).
   I.e. `ov_scene_native` renders A00 entity objects DURING the area-init transition frame, BEFORE their
   models are attached — model-attach (FUN_80077b38) runs from per-object BEHAVIOR handlers during the
   object-update frame (ov_field_frame), not at placement (ov_field_run case 0). On the case-0 init frame the
   objects exist but `cmd+0x40` still holds the raw area-data pointer. AUTO_SKIP (Start pressed) does not hit
   it (skips the narration's auto-transition). Verified: AUTO_SKIP 0-miss + free-roam renders 105 objs, and
   SELFTEST=startgame PASS, all unchanged by (1)+(2).
   **FIXED (TDD):** added `PSXPORT_SELFTEST=narration` (native shipping path, plays the un-skipped intro
   through the transition; asserts no overflow + the GAME loop keeps running) — RED, then GREEN. The fix is an
   AREA-INIT render suppression in `ov_scene_native`: skip the field-scene render on the GAME field-area-
   machine object-placement init frame (stage GAME, sm[0x48]==2, sm[0x4a]==1, sm[0x4e]==0 = ov_field_run
   case-0, pre-attach). Gated on the PERSISTENT task0 GAME state machine (0x801fe00c/048/04a/04e), NOT a
   per-frame latch — a latch set by the render-build pass is unreliable because the GAME loop runs in a
   coroutine whose step need not fall in the same native_step_frame as the display pass (an earlier latch
   attempt blanked free-roam for exactly this reason). The backdrop still draws on the init frame; this
   matches the real game not drawing the field during the area transition. Verified GREEN: narration selftest
   PASS (loop +2467/2500), `newgame; run 3000` 0-crash, AUTO_SKIP 0-miss, free-roam 105 objs, startgame PASS.
   REMAINING (user-reported, needs eyeball): the intro shows TWO fade-ins (narration text fade, then black,
   then field fade) — confirm whether that double-fade is faithful to the original.

## later-274 — narration miss 0x80146478 FIXED (native walk owns the unowned node cases) + render-walk split out of engine_submit.cpp
Two things. (1) The narration-cutscene recomp-MISS 0x80146478 (later-273) is fixed the PC-game way. The live
backtrace was `ov_scene_native → ov_render_walk → gen_func_8003C048 (super-call) → 8003CCA4 → 8003CDD8 →
8003F698 → MISS`, NOT the predicted 0x8010A0E0 chain: the NATIVE `submit_render_walk` FELL BACK to
`rec_super_call(0x8003C048)` because the narration scene has a render-list node of TYPE 16 the native walk
didn't own. The whole-body super-call then ran the recompiled per-object chain down to the per-AREA dispatch
8003F698 → A00's 0x80146478 (overlay not resident during the data-only SOP intro) → abort. The native
per-object path (`submit_perobj_flush → native_gt3gt4`) bypasses 8003F698, so the only thing forcing PSX was
the one unowned node type. FIX: own the remaining simple master-walk cases natively — the render-nothing skip
cases (types 9-14,21-31 → 0x8003C2AC) and the special-effect leaf cases (types 16-19 → leaves
0x8003C2D4/464/5F8/788, which only call resident-MAIN library fns, never 8003F698 / an overlay). type-4 (per
later-239) and type-20 (overlay-dependent leaf 0x8011be5c) keep falling back. Verified: 0x80146478 gone;
AUTO_SKIP free-roam 0-miss; emit 14/14; SELFTEST=startgame PASS. The narration now advances to a SEPARATE
later miss `0x8010BF54` (SOP-overlay routing — next frontier; the SOP overlay HAS ov_sop_func_8010BF54 but the
router didn't route it).
(2) WORKFLOW/structure (user directive: "don't cram all the code in engine_submit.cpp, build a PC game file
structure"). engine_submit.cpp (2274 lines, ~5 subsystems) was split: the render-list WALK subsystem
(ov_scene_native + the master/snapshot/aux list walks + per-object render/flush + native backdrop) moved to
`engine/engine_render_walk.cpp` (sibling of engine_render.cpp, the orchestrator that already fwd-declares
them). Shared internals (PktSpanSession, cur_render_node/obj_world_ord/fps60_bb_node inline helpers,
native_gt3gt4 + ov_field_entity_render decls) moved to `engine/render_internal.h`. engine_submit.cpp now holds
the geometry-SUBMIT subsystem (poly submitters, render-cmd dispatcher, transform/matrix orchestration). Pure
mechanical move — all gates above pass unchanged.

## later-270 — headless SPU/XA advance so audio-gated game logic progresses; selftest runs the intro clip
The strong selftest (later-269) exposed the NEXT layer: after the GAME loop runs, entering the first area
starts an intro XA voice clip [66515..75523] and the field waits `while(*(0x801fe0e0)!=0)` for it to finish —
but headless the clip never advanced, so the wait hung. ROOT CAUSE: `xa_decode_next_sector` is driven only by
`CDC_GetCDAudioSample` (per output sample, inside `spu_update`), and `spu_audio_frame` early-returned when no
output consumer (SDL device / WAV) was active — conflating OUTPUT gating with STREAM-CLOCK gating. FIX: advance
the SPU+XA whenever `xa_stream_is_active()` (discard the samples); keep OUTPUT gated. One spu_update == one
video frame == correct realtime-equivalent pacing; windowed/shipping always has a consumer so it's unchanged.
The clip now PLAYS + COMPLETES headless (this is the "present" headless-pump the cutscene needed — it also lets
ANY headless run progress through audio-gated logic). Selftest now asserts BOTH verified fixes (GAME loop runs
+ intro clip plays→ends). All gates clean.

DEEPER LIMIT FOUND (NOT fixed, tracked): running the FULL recompiled field-mode under coroutines doesn't fully
progress past the intro cutscene — the outer loop counter stalls and the field-mode (overlay 0x80109450) parks
in a deep sub-wait. This is the SBS DIAGNOSTIC full-PSX path; the SHIPPING NATIVE field runs fine (4000 frames
clean). Next session: capture the GAME coro's GUEST BACKTRACE at the freeze (the mem_r32(sp+16) waitloop
heuristic is unreliable on inner frames) and compare to ov_sop_field_mode. See docs/findings/sbs.md.

## later-269 — full-PSX Start→field FREEZE fixed: recompiler now flows a split stage prologue into its loop (TDD)
User clarified Issue 1: full-PSX (SBS core B) RESPONDS to Start (starts the game) then FREEZES in the field —
and asked for TDD. Built `PSXPORT_SELFTEST=startgame` (runtime/recomp/selftest.cpp): boots one psx_fallback
core, mashes Start, asserts it reaches GAME free-roam (sm[0x48]==2) AND the GAME loop runs (*(0x1F800198)
advances). RED: the field froze at sm[0x48]=0 — the GAME coro `start`ed once (entry 0x8010637C) then `done=1`
with no ov_switch yield. **Root cause (recompiler):** the GAME stage fn is a PROLOGUE 0x8010637C that FALLS
THROUGH into its cooperative LOOP at 0x801063F4; both are seeded as separate fns (so native game_coop can
re-enter the loop top per frame), but emit_func emitted a bare `return;` at every fn end — so func_8010637C
returned after the prologue instead of flowing into the loop, and the full-PSX coro reaped the task as done →
field never ran. **Fix:** emit_func now emits a TAIL-CALL to the next fn on genuine fall-through (last insn not
`j`/`jr`), SCOPED to deliberate re-entry seeds (OVERLAY_EXTRA_SEEDS) — a global version regressed the native
field (executed dead fall-through regions in area overlays that jal undiscovered fns → recomp-MISS 0x80124448).
func_8010637C now ends `ov_game_func_801063F4(c); return;`. GREEN. All gates pass (emit 12/12, test_coro,
native plain + AUTO_SKIP 0-miss, SBS both no-crash, selftest). RECOMP_VERSION → 2026-06-30.10 (full regen).
NOTE: the selftest does NOT require the loop to advance forever — entering the first area starts a long
one-shot voice clip and the field's `while(*(0x801fe0e0)!=0)` wait legitimately pauses the loop until the clip
ends; clip progress is REAL-TIME audio-driven (CDC_GetCDAudioSample), which headless logic-frames outrun. That
audio-gated cutscene is NOT a freeze (with audio it resolves). This is also the likely shape of Issue 2 ("field
frozen, music keeps playing") — same audio-gated-wait family, to confirm on the user's machine. See
docs/findings/sbs.md.

## later-268 — dead libetc VSync counter revived (recomp timebase) → SBS core-B title-freeze partial fix
Investigating the two freezes (scratch/handoff_two_freezes.md). **Root cause found for the timebase half:**
the libetc VSync counter `DAT_800abde0` was NEVER advancing — `ov_vsync`/`frame_tick` (timing.cpp) are dead
code (VSync 0x80085900 is TRAPPED by sync_overrides, so they never run) and nothing else bumped it. It sat at
0 forever. Native paths reimplement their own pacing and ignore it, but RECOMP code (the full-PSX SBS core B,
and any still-recomp leaf) reads `0x800abde0` to pace animations/timers → frozen. FIX: `timing_frame_tick()`
bumps `timing.vblank` + writes `0x800abde0` once per native frame, called at the top of `native_step_frame`
for ALL cores. Verified: counter now increments (was 0); native plain + AUTO_SKIP field 0-miss; emit 12/12 +
test_coro pass; SBS both no crash. **Still open (Issue 1):** whether the timebase fix alone unfreezes core
B's title is UNCONFIRMED — headless SBS renders BLACK at the menu so it can't be verified here; needs WINDOWED
SBS (user). DEAD-END NOTE: I tried reading the demo SM via raw `0x801fe048` and saw 7/7, but that read is
UNRELIABLE — the demo SM_PTR is `*(0x1f800138)` in SCRATCHPAD and the debug server is BLIND to scratchpad
(reads main RAM literally; in the GAME stage the same trap showed garbage 25712 vs the real `sm[48]=2`). So
the earlier "s7 phase=7 divergence" guess is NOT trustworthy; the next session needs the REAL SM_PTR (a
per-frame log on the psx_fallback path, or a scratchpad-aware probe) before any per-substate claim. **Issue 2 (native field freeze on macOS):** NOT
reproducible on Linux — area warp (0→1, 138 nodes) + AUTO_SKIP field both run 0-miss; needs the user's macOS
env or a manual-play repro. See docs/findings/sbs.md.

## later-264b — overlay router by LOAD-TIME identity → SBS both/gameplay run clean (full-PSX alive)
After the coro fix (later-264) got full-PSX past the mid-function-resume wall, SBS mode=gameplay/both hit
a router miss at 0x801063F4 (GAME loop top), caller ra=0x801063F4 (core A's native game_coop resume). The
diagnostic I added to overlay_router.cpp showed the cause: the resident-overlay identification compared the
raw GAME.BIN first-32-bytes signature against live RAM, but the game MUTATES its header pointer table at
runtime — GAME +0x08 swapped from 0x80106510 (raw) to 0x80106470 (live), 14/16 byte match → no overlay
matched → fail-fast. (This is a latent bug in the NATIVE path too; it just wasn't hit in my test runs.)
Fix: record the resident overlay at LOAD time, when the freshly-written image still matches its signature,
into a per-core map SchedulerState::resident_ov[slot] (overlay_note_load, called from the CD loaders
ov_cd_loadfile/ov_cd_async_read in cd_override.cpp). The router (resident_overlay) routes by that IDENTITY,
falling back to the live signature scan only when a slot has no noted load. Robust to runtime header
mutation. Result: native gates stay 0-miss (plain + AUTO_SKIP field DEMO→GAME); PSXPORT_SBS_MODE=gameplay
AND =both now boot BOTH cores START→DEMO→GAME with 0 recomp-MISS and no crash/hang (the full-PSX reference
core is alive again). Tools: tests 12/12 + test_coro all-pass. Finding promoted to docs/findings/sbs.md.

## later-263 — ensure_recomp.py: one hash-checked recomp step; attract-path misses resolved by all-overlay build
Two things from the handoff (scratch/handoff_ensure_recomp_and_attract.md), both done.
(1) USER directive — `tools/ensure_recomp.py` is now the SINGLE recomp-provisioning step. It resolves the
disc (CLI > $PSXPORT_TOMBA2_DISC > .env > *.chd, mirroring run.sh), extracts MAIN.EXE + SCUS_944.54 + every
overlay (6 stage + 22 A0* area), runs emit.py, and VERIFIES the generated set against a SHA-256 of the
INPUTS (MAIN.EXE + stub + each .BIN + emit.py/decode.py/psexe.py) stored at generated/.recomp.hash. Hash
match + complete generated set ⇒ no-op; else re-emit and rewrite the hash. So every machine builds
byte-identical recomp from the same disc — which is exactly the determinism the per-box-miss bug needed.
run.sh §3 collapsed from the extract-loop + emit-staleness block to one `python3 tools/ensure_recomp.py`
call (discdump still built by run.sh, passed via $PSXPORT_DISCDUMP). Env: PSXPORT_FORCE_RECOMP=1 forces
re-emit; PSXPORT_DISCDUMP overrides the binary.
(2) The attract-path misses (0x800739AC mine / 0x800810F0 USER's macOS) were artifacts of the incomplete
per-box build (later-262). With all A0* overlays extracted + the hash-checked build, NO misses remain on my
box: plain headless 1500 frames = 0 miss (stays in DEMO attract menu, no field load); AUTO_SKIP field =
0 miss; and MASHING START through boot (REPL `tap start` + `run`, USER's suggested repro) drives
DEMO → GAME → field with 0 miss and RENDERS the field (Tomba on the green island over water,
scratch/screenshots/mash_field.png). The per-box divergence is gone because both boxes now build identical
recomp. Build clean, ensure_recomp skip/emit paths both verified.

## later-245 (2026-06-24) — scenenative made FIELD-DEFAULT (invisible-Tomba fixed); RENDER ARCH mapped + the 2D gap
USER (./run.sh): invisible Tomba (occluded), missing 2D (pickups, attack weapon), atlas judders on camera
turn at 60fps, cutscene fades missing, some Z-fighting (a pot's red top), and spurious `[miss N] addr
0x00000002`. "Just fill the gaps."
- **Invisible Tomba = the gate.** ./run.sh used the DEFAULT path; scenenative was behind a debug flag. Default
  walks the PSX OT, which re-adds FLAT (is3d=0) duplicates of the world that sort on top → objects look
  occluded. FIX (pushed): ov_draw_otag routes the FIELD (GAME stage, mem_r32(0x801FE00C)==0x8010637C) through
  ov_scene_native by default; other stages still PSX-walk; `renderpsx on` forces PSX; `debug scenenative`
  force-on elsewhere. Title/menu verified unchanged.
- **RENDER ARCHITECTURE (mapped this session — the key to the 2D gaps):**
  - `ov_render_frame` (engine_stage.cpp:284, per field frame, BEFORE ov_draw_otag) already NATIVE-draws the
    OBJECTS (Tomba/door/see-saw/pots/swine) into the render queue (submit_perobj_flush→native_gt3gt4→
    gpu_draw_world_quad), AND builds the PSX OT (the 2D passes + ground + special-object renderers it
    rec_dispatches emit OT packets). It does NOT native-draw the terrain/ground.
  - The render queue persists across both (RenderQueue auto-resets on first push after flush; one rq_flush at
    end of ov_draw_otag). So `ov_scene_native` RE-walking the same walks DOUBLE-DRAWS the objects (harmless —
    identical opaque pixels — but wasteful, and would double-blend semis). PROVEN with `debug bgonly` (scene
    skips walks+entity-lists, only backdrop): objects still render (from ov_render_frame), terrain GONE (sea
    backdrop shows through) → ov_scene_native is the SOLE terrain/ground renderer (ov_render_walk→ov_terrain +
    ov_field_entity_render(0x800F2418)).
  - **THE 2D GAP:** pickups, the attack weapon, cutscene FADES, HUD/dialog are drawn by the still-PSX passes
    (2D sprite band 0x80025d98 = op-0x65; the fade/transition passes; special per-type renderers reached by
    rec_dispatch in the walks). They emit ONLY PSX OT packets — never native-drawn — so scenenative (skips the
    OT walk) drops them. Walking the full OT is NOT the answer: it re-adds the flat is3d=0 object dupes that
    occlude (verified earlier as scenenativehud: native Tomba covered).
- **PLAN to fill the 2D gaps (next):** run the specific 2D-content passes (0x80025d98 sprite band; the fade
  pass) into an ISOLATED OT (swap the OT head *0x800ed8c8+12, run pass, walk only its links, restore) so only
  genuine 2D (pickups/weapon/fades/HUD) is queued — NOT the flat object dupes. Classify as RQ_OVERLAY/RQ_HUD
  (engine owns layer). OR port those renderers to native draw. NEEDS on-screen content to verify (no weapon/
  pickup/fade in the AUTO_SKIP intro free-roam) → user-side eyeball or a save with a weapon equipped.
- **Z-fighting (pot red top) + 60fps atlas judder on turn:** separate; the pot z-fight is native depth
  precision/coplanar (need the close-up to repro); the atlas judder is fps60.cpp's RQ_BACKGROUND
  half-median translation failing on camera ROTATION (tiles wrap → fingerprint mismatch → median oscillates).
  Both are USER-eyeball-verified (motion/close-up) — can't self-verify headless at 1x/30fps.
- `[miss] addr 0x00000002`: a jalr/computed-jump to target 2 (a < 0x10000 → falls past the interp window to
  rec_dispatch_miss). Not reproduced in AUTO_SKIP free-roam at rest or with tap square/circle/triangle/cross;
  triggered by the user's specific action (likely the attack/weapon renderer derailing). Tie to the weapon 2D
  port. Add a guest-ra + callring dump when rec_dispatch_miss gets addr<0x10000 to localize.

## later-244 (2026-06-24) — BACKDROP ported native (sky + parallax hills); decoupled scene now complete at the seaside field
Finished the Phase-1 decoupled native render gap from later-243: the only black area (sky + distant parallax
hills) is now drawn natively. ATTRIBUTION (empirical, not the later-243 guess): walked the still-skipped passes
with `PSXPORT_SKIPPASS=<addr>` under `debug scenenative scenenativehud`. Skipping **0x8003df04** removed the
ENTIRE backdrop; skipping 0x8004fd30 (the flat 320×240 fill) changed NOTHING (it is fully covered → invisible
this field). So the backdrop = 0x8003df04, which is a 16-state jump table @0x80014fc0 keyed on
`mem_r8(0x800bf870)` (gated by `mem_r8(0x800bf873)==0`). At the seaside field state=0 → 0x8003df74 →
**FUN_80115598** — i.e. the backdrop IS the known scrolling-TILEMAP drawer (§F's ~49% hot lever), NOT "absent
in this field" as later-243 claimed (the old signature search keyed on a texpage this field doesn't use).
- **FUN_80115598 RE'd** (overlay; disas via scratch/dual/mdis.py). Struct @a0=0x800ed018: +0x04 tpage, +0x06
  clut-base, +0x10 W(=36), +0x11 H(=72), +0x14 tilemap ptr (u16[H][W] @0x801F0FA4), +0x28 scrollX, +0x2a
  scrollY. Iterates a wraparound visible window (22 cols × 16 rows of 16×16 tiles). Tile u16: [0:3]=atlas col,
  [4:7]=atlas row, [8:11]=clut sub-index → U=(t&0xF)<<4, V=(t&0xF0)+8, clut=clutbase+((t&0xF00)>>2). Texpage
  0x0E = 4bpp @ VRAM(896,0); applied once via a GP0(0xE1) prim in the PSX body.
- **PORT** (`ov_bg_tilemap_native`, engine_submit.cpp): TRANSCRIBE the integer wrap/scroll/index math (scene
  data — what tile goes where), EMIT native RQ_BACKGROUND 2D quads via `rq_push_2d_quad` (raw texture, behind
  the world) instead of GP0 sprite packets / OT links. Wired into `ov_scene_native` ahead of the world walks,
  gated by the same 0x800bf873==0 / state==0 guards (only state 0 owned so far; other fields' drawers still
  black until ported — frontier). +352 native quads (22×16) at the seaside.
- **VERIFIED headless** (AUTO_SKIP free-roam, scenenative): sky/clouds/sea-horizon/distant-hills now render and
  MATCH PSX vanilla (scratch/dual/bd_cmp.png); native world (Tomba/see-saw/pots/swine/water) intact at real
  depth; before/after diff (bd_ba.png) shows the ONLY delta is the backdrop filling the former black — no
  regression. Stable + parallax-correct under walk-right motion (bd_scroll.png / bd_walk2.png).
- NEXT (unchanged frontier): native 2D HUD into ov_scene_native, then retire the PSX OT walk here and UNGATE
  (make scenenative the behavior, drop the debug flag), then confirm 60fps/ires/lighting all ride this one
  path. Other fields' backdrop states (jump-table entries ≠0: 0x8013f0dc/0x80136f28/0x80141a94/… ) still PSX.

## later-221 (2026-06-24) — LOAD GAME freeze FIXED: DEMO substate s4 owned native (memcard slot browser)
USER REPORT: native ./run.sh FREEZES when selecting LOAD GAME from the title (PSX fallback PSXPORT_GATE=1
works). ROOT CAUSE: the DEMO per-frame dispatcher `ov_demo_frame` (engine_demo.cpp) handled s0/1/2/3/5/6/7
but **s4 hit the `default` case = silent no-op spin** (FAIL-FAST violation). Repro: title (s2) RIGHT (cursor
item1=sm[0x68]=1) + Cross -> sm[0x48]=4 -> default -> freeze.
- RE (Ghidra headless on a DEMO-overlay RAM dump, scratch/ghidra + scratch/decomp): s4 = 0x80106580 runs the
  LOAD sub-machine 0x8007bf20(0,0) then routes on sm[0x6b]. The load sub-machine's case-0 disc load
  FUN_80045558(1) = FUN_80045080(0x8018a000, idx=1) = the async indexed reader FUN_8001dc40 -> loads the
  LOAD-MENU OVERLAY to 0x8018a000; that overlay's FUN_8018fa88/fbcc is the actual memcard slot browser
  (reached via the resident UI driver FUN_8007be18). The async reader never completes in our no-IRQ runtime
  (same class as the s0 loaders).
- FIX (engine_demo.cpp `demo_frame_s4` + `load_machine_s4`): reimplement 0x8007bf20 native+SYNC — case-0 disc
  read via cd_dc40_sync (+ set 0x1f80019b done), rec_dispatch the page-open FUN_800750d8 and the browser
  driver FUN_8007be18 (its memcard frame R/W is already sync+instant via the BIOS B0/A0 card HLE,
  memcard.cpp — so the browser does NOT yield). Wired `case 4` in ov_demo_frame.
- GOTCHA (caused a follow-up bug the USER caught: "exiting load replays OP FMV, skipping it loops back to
  load"): the root dispatcher 0x801062E4 prologue loads **s2=1, s1=2, s3=3** (NOT s1=1/s2=2). So 0x80106580's
  `sh s1,0x48` writes sm[0x48]=**2** (TITLE), and the load-ok path writes sm[0x48]=5 + *0x1f800134=**1**. My
  first cut assumed s1=1 -> routed cancel to sm[0x48]=1 (s1 = the opening-movie machine) -> OP FMV replayed,
  and the skip-edge then re-confirmed back into load. Fixed: cancel (sm[0x6b]==1/2) -> sm[0x48]=2 title;
  load-ok (sm[0x6b]==7) -> sm[0x48]=5 GAME, *0x1f800134=1.
- VERIFIED (headless): title -> Load -> renders the real PS1 card screen ("Load / Select slot", MEMORY CARD
  slot 1/2, X:OK ○:Return △:Exit; 88% non-black, scratch/screenshots/loadgame.png) with NO freeze/derail;
  Circle (Return) -> back to title (s4->s2), no movie replay; newgame unaffected (reaches GAME 0x8010637C).
  Load->GAME (sm[0x6b]==7) routing transcribed from disasm; needs a save on the card to exercise live.
- Memcard ops are ALREADY sync+instant (USER directive confirmed): host-backed 128KB image, _card_status
  always reports complete, wired live via the BIOS trampoline (hle.cpp card_hle_a0/b0) — not the dead
  override table.
- TOOLING: established a Ghidra-headless decompile pipeline for the front-end — import the DEMO-overlay RAM
  dump (base 0x80000000) -> analyze -> DecompDump.py per-function clean C (scratch/decomp/*.c). Far faster
  than hand-tracing the menu state machines. Reusable for the rest of the front-end port.

## OPEN BUGS (user-reported, not yet fixed) — work after the current task
- **2D game objects lost their Z / depth ordering (USER, 2026-06-20).** Collectable 2D sprites (e.g. apples)
  in the vanilla game have a Z: they occlude correctly — render BEHIND closer geometry and IN FRONT of
  farther geometry. Our renderer does not replicate this; the 2D objects ignore depth. Likely needs RE +
  port of the game's OBJECT PLACEMENT and specifically how it places/depths 2D objects (their per-object Z
  → real depth in our queue, instead of the flat 2D layer band). Related: the engine-owned render queue
  (`RQ_OM_2D_*` vs `RQ_OM_DEPTH`) — 2D collectables probably need a real view-Z, not the 2D FG/BG band.
  Tie-in to the broader "own 2D from scene data" M4 work.
- **Gameplay fade only covers the ATLAS region (USER, 2026-06-20).** A gameplay fade (the screen
  fade-to/from-black or color overlay) only covers the texture-atlas area of VRAM, not the full displayed
  framebuffer — so the fade is wrong/partial on screen. Likely the fade overlay prim's screen rect (or the
  region our renderer applies it to) is keyed to the wrong VRAM extent. Investigate the fade prim
  classification / the 2D overlay band vs the actual display region (ties into render-ownership + the
  earlier fade work: see [[tomba2-fade-flash-solved]]).
- **Teal terrain band at the field bottom (USER, 2026-06-23).** The intro/field shows a TEAL diamond-
  checker band at the screen bottom where GREEN terrain should be — "something is culling it." LEAD: the
  PC-native terrain render is ORPHANED — `ov_terrain`/`terrain_render_pc` (engine_submit.cpp:1227 +
  native_terrain.cpp) is NOT wired (`[ov_terrain]` debug never fires), so terrain renders via the RECOMP
  path 0x8002AB5C (the old "water/garbage at the field edge" class, later-157), OR a 2D BG draws over the
  near terrain. FIX = own terrain render PC-native (wire terrain_render_pc once its caller is native) +
  give the engine the right near depth/order. Independent of the gameplay-loop work. See gfx-debug skill.
- ~~SCEA license screen was black~~ **FIXED PC-native (later-179, below).**

## later-220 (2026-06-23) — IN-GAME MUSIC: wrong VAB bank + wrong tone-start (corrects later-219's slot[0x26] claim)
USER: "no dialogue music; the area-load is the culprit." Gated the native area-load (REPL `native areaload
off`) — NOT it: the recomp libsnd path is dead (keyon/seqplay = 0 in every config), so in-game music is 100%
the native engine. Real root cause is the native engine's VAB/tone handling, three bugs:
1. **Wrong VAB bank.** The area bundle (guest 0x80182000) has TWO `pBAV` VABs: bank0 @+0x26b4 (ps=4) and
   bank1 @+0x38d4 (ps=18). `music_list.c` bound bank0 for every song. The songs program-change (0xCn) to
   programs 1,2,4,5,7,8,13,15 (verified live with a temporary per-note program trace) — those exist only in
   bank1. Fixed: bind bank1 (AREA_VAB_OFF 0x38d4; AREA_SEP_END 0x26b4 still bounds the SEP scan).
2. **Live program ignored.** `seq_note_on` forced `slot[0x26]` (=program 0) for ALL notes "because programs
   9..17 dropped against ps=4" (later-219). That was a workaround for bug #1, not a fix — it collapsed every
   instrument to program 0 (the SFX-tone sound). Fixed: use the live 0xCn `s->prog[ch]`, fall back to
   prog_set only when the bound VAB lacks it (keeps the sound-test ps<=2 TOMBA2.SND VABs audible). later-219's
   "slot[0x26] is right, 0xCn is wrong" is FALSIFIED — the inverse is true once bank1 is bound.
3. **Wrong tone-start.** `na_prog_tone_start` summed per-program tone COUNTS (packed layout), but the table is
   FIXED 16 slots/program (na_vab_open_at's own `vagtab = tonetab + ps*16*32` proves it). Agreed only for
   program 0 — so it was masked while #2 forced program 0; honoring the live program exposed it (every note
   resolved to the wrong slots -> 0 tones -> dropped). Fixed: `return p*16`.
RESULT (objective, measured via a temporary per-note trace): field song 8 now keys 104 voices, **0 dropped**,
across 4 instruments (programs 1/2/4/15) vs all-program-0 before. Sound test unchanged (ps<=2 -> fallback).
USER verifies by ear. Diag left in tree: `PSXPORT_DEBUG=bgmreq` (game's sound_play_bgm/stop trigger — never
fires in normal field play, confirming the area trigger FUN_8007566c path is what drives BGM). OPEN: per-song
bank selection is currently hardcoded to bank1 — if some jingles are authored for bank0, add a song->bank map.

## later-219 (2026-06-23) — NATIVE audio engine LANDED: sound test + IN-GAME field music (libsnd replaced)
Built the PC-native music engine + RmlUi Sound Test, and wired it to score the field (commits up to
5dd61ba). The native engine REPLACES the silent libsnd sequenced-music path entirely.
- SHARED SYNTH CORE `engine/audio/native_audio.{h,c}` = snd_render.c's VAB/ADPCM/ADSR/SEP synth,
  de-globalized (operates on a byte buffer + explicit offsets; per-NaSeq VAG cache). tools/snd_render.c
  now #includes + drives it (offline + in-game are ONE codebase = the unification).
- REAL-TIME PLAYER `engine/audio/native_music.{h,c}` (play/stop/render/active), mixed into the SPU sink
  (spu_audio.c) before SDL/WAV; silent when idle.
- CATALOGUE `engine/audio/music_list.{h,c}`: the 10 TOMBA2.SND songs + `music_list_play_area()` for the
  LIVE area bundle. REPL `musictest <n>|stop` (native_boot.cpp) + RmlUi "Sound" tab (menu.rml +
  imgui_overlay.cpp action handler).
- **ROOT-CAUSE FIX (corrects later-218 / handoff §5b dispute):** tone selection uses the per-SEQUENCE
  program-set selector **slot[0x26] (=program 0)**, NOT the live 0xCn MIDI program. DATA-PROVEN: the area
  VABs have only 4 programs but the sequences emit 0xCn programs 9..17 -> those don't index ProgAttr ->
  EVERY note was dropped (the universal-silence the handoff blamed on "wrong VAB"). With slot[0x26] all 10
  area seqs AND all 10 TOMBA2.SND songs synth audibly. The handoff's "0xCn drives tone selection"
  correction was WRONG; spec §5b (slot 0x26) is right. The "songs 0-3/9 need area VABs" gap was this synth
  bug, not missing data.
- IN-GAME: `field_bgm_director()` in game_tomba2.cpp's ov_frame_update — while the GAME stage (0x8010637C)
  is active + the area bundle (guest 0x80182000: 10 SEPs + field VAB @+0x26b4) is loaded, it starts the
  native area theme (default seq 8; `PSXPORT_FIELD_SONG=<0..9>` to audition), restarts on drain, stops on
  leaving the field. NB the no-override arch means the recomp BGM-start (ov_sound_play_bgm) is DEAD (PSX
  never calls PC) — the director is the single audible path; do NOT try to hook ov_sound_play_bgm.
- DEAD END recorded: 0x80104C28 ("playmask" 0xC3FF) is STATIC config (same at DEMO + field), not a
  per-slot play flag — useless as a BGM trigger. The field never sets song@0x800bed80 (FFFF throughout)
  because it plays seq slots via SsSeqPlay directly, not FUN_80074BF8.
- VERIFIED headless: field auto-starts "area BGM song 8", sustained ~rms 2000; idle/post-stop = rms 0.
  USER confirms the right field theme by ear (windowed) + auditions via the Sound tab / PSXPORT_FIELD_SONG.

## later-218 (2026-06-23) — SEQUENCED MUSIC IS SILENT IN THE PORT: root-caused; audio-engine UNIFICATION directed
USER: sequencer effects/music (incl. dialog music) won't play (windowed/macOS), though SFX + XA do.
Root-caused via the game's own libsnd as an oracle (new diagnostics: `PSXPORT_DEBUG=keyon` traces
every libsnd voice keyon 0x800939A0; `=banksel` traces the slot[0x26] setter 0x8008e390; REPL
`seqsolo <i>` plays one sequence in isolation):
- **Every sequencer keyon fails.** The note channels' per-channel VAB id `slot[0x26]` = 0xFF (-1), so
  the VAB-select 0x800962b0 rejects it → no voice. The bank-select handler 0x8008e390 never runs for
  note channels. **REFINED:** the VAB ADPCM bodies ARE in SPU RAM (`PSXPORT_DEBUG=spu`: two DMA writes
  46144 + 45664 = 91808 words = the two area VABs). Forcing a valid bank at keyon makes the tone-scan
  0x80095C40 succeed (vab-select OK) but STILL no audible voice → the per-tone→SPU-address setup (done
  at sequence-open / SsVabOpenHead time, tied to the same -1 binding) is also broken, so keyed voices
  read silence. Binding fails at OPEN time (likely ordering: sequences opened with vab=-1 before the
  VABs are registered, OR SsVabOpenHead returns -1 in our port). NO in-game reference exists.
  The dialog music (song 4) additionally needs bank 8, which this area never loads at all.
- **Data divergence found.** The game loads a compact area BGM bundle to 0x80182000 = 10 SEPs
  (byte-identical to TOMBA2.SND's first 10) + 2 AREA-specific VABs (bank0 ps=4 @+0x26b4, bank1 ps=18
  @+0x38d4). These area VABs are NOT in TOMBA2.SND (VABs at 0x51800, ps≤2) — so the offline snd_render
  (which read TOMBA2.SND's VABs) used the WRONG instruments (sounded like SFX). Extracted to
  scratch/bin/snd/AREA_BGM.bin; snd_render gained `raw <seqOff> <vabOff>` + per-channel prog tone
  selection (was forcing prog 0 — the spec's "prog-change=vol/pan only" was WRONG; 0xCn sets the
  tone-selection PROGRAM via slot[0x37]) + an 0xF0 parser fix (libsnd SEP is NOT MIDI: events are
  [status][N data][delta-varlen]; 0xF0 type 0x2F=end else 3-byte tempo).
- **USER DIRECTIVE — UNIFY:** offline export and in-game audio must be ONE native engine (they diverge:
  in-game=libsnd+Beetle SPU, offline=snd_render, + different data). Build a single CD-sourced loader +
  native synth, used offline AND in-game, retiring the (broken, silent) libsnd music path. Plan +
  full RE in scratch/handoff_audio_unify.md (the native-engine bible scratch/native_audio_spec.md §5b/§6
  are CORRECTED there). NOT YET FIXED in-game — this is the active work.

## later-217 (2026-06-23) — GAMEPLAY-START FLOW MAPPED + native boundary CORRECTED; audio synth+sequencer built offline
Two parallel threads this session.

**(A) Gameplay-start flow — where native ends, and the path into gameplay (map: `scratch/gameplay_start_flow_re.md`).**
- **Native ownership ENDS at `ov_game_stage_main`'s PROLOGUE (GAME.BIN 0x8010637C).** It runs the prologue
  native then `rec_coro_redirect`s to the GUEST loop body 0x801063F4; from there the per-frame loop, the
  sm[0x48] dispatch, the sub-mode-0 bridge 0x8010882c, the SOP field-mode machine 0x80109450, the area
  loads, and all per-frame gameplay systems run as RECOMP (`rec_coro_run`).
- **STALE-MARKER CORRECTION:** the native `ov_game_s48_0/1/2` + `ov_game_s4c` handlers and the hundreds of
  later-188..208 gameplay natives (cull/spawn/collision/object-walk) are **ORPHANED** — the address-keyed
  override table that reached them was removed (later-212, 2026-06-22). `stage_scan_overlay` is a no-op;
  the handler defs are `(void)`-cast (engine_stage.cpp:188). They are correct ready-to-wire reference
  bodies, not live code. port-progress §C "✅ owned" markers updated accordingly.
- **The gameplay-start state flow (recomp, verified live `newgame`):** GAME sm[0x48]=2 → sm[0x4e] 0→1 (the
  0x8010882c bridge: input-reset 0x8005082c + per-frame `jal 0x80109450`) → SOP sm[0x50] 0(LOAD)→1(FADE-IN)
  →2(GAMEPLAY) reached by ~frame 61. SOP map `scratch/sop_mode_re.md`, area loads `scratch/level_layout_re.md`.
- **NEXT — ORDER MATTERS (critical constraint found this session):** the GAME native-loop conversion is
  BLOCKED on first owning SOP state-0's area load SYNCHRONOUSLY. SOP state 0 spawns the load as a
  cooperative slot-1 task and yields on 1f80019b; a native per-frame loop that `rec_dispatch`es SOP would
  break that handshake (yield → frame-done guard, not slot-1 service; SOP restarts state-0 each frame and
  re-spawns the load) → regression. Revised order: **(0)** own `LAB_80109164` (0x80109164) as a native
  sync disc read writing 1f80019b=1 (no task spawn), **(1)** own SOP machine 0x80109450, **(2)** own the
  bridge 0x8010882c, **(3)** convert the GAME loop to native per-frame (mirror demo_native). Verify each:
  newgame → sm[0x50]=2, no derail. Full trap write-up in `scratch/gameplay_start_flow_re.md`.

**(A.2) later-217c — GAME stage CONVERTED to a NATIVE PER-FRAME LOOP; SOP field-mode machine OWNED.**
Built on 217b (native sync area-load). The GAME stage no longer coro-redirects into the guest loop —
it runs as a native per-frame task (`game_native` in native_scheduler_step, mirroring `demo_native`):
- `ov_game_stage_prologue` (split out of ov_game_stage_main) + `ov_game_frame` each frame (sm[0x48]
  dispatch → the re-wired native `ov_game_s48_0/1/2_frame` + frame-counter bump = the cooperative yield).
- GAME→SOP bridge 0x8010882c → `ov_game_submode0` (native): input-reset + the per-frame SOP call.
- **SOP field-mode machine 0x80109450 → `ov_sop_field_mode`** (engine/sop.cpp): the full sm[0x50]
  LOAD→FADE-IN→GAMEPLAY→FADE-OUT→RESET switch native. State-0 calls `native_sop_area_load` INLINE
  (NOT FUN_80044bd4, which clears 1f80019b + spawns the slot-1 task + yields — fatal to re-enter per
  frame), spawns the 3 scene objects from the SOP overlay tables @0x8010c98c, dispatches the BG/init
  callees. Heavy per-frame work (FUN_801092b4, per-scene handlers) stays rec_dispatched.
- VERIFIED: newgame → sm[0x50] 0→1→2 (gameplay) by frame 60 (one frame faster than the cooperative
  baseline — load is inline), 1f80019b=1, stable 300 frames, zero derail/hang/caught-yield, render
  99.9% non-black (USER eyeball pending). NB game.h gained `game_native[3]` → needs `build_port.sh all`.
- NEXT: descend FUN_801092b4 (entity update/render, Tomba update) to re-wire the orphaned
  cull/spawn/collision/object-walk natives; then the area-transition path.

**later-217e/f — skip→field REGRESSION found + fixed with a cooperative fallback (native start preserved).**
Driving past the intro (`skip 400`) DERAILED: the native per-frame loop rec_dispatched the area machine
(sm[0x4c] 0x80106478) + transition sub-modes (sm[0x4a]!=0) + the non-SOP field overlay (0x80109450 swaps to
0x801138A4), all of which YIELD DEEP — fatal in the per-frame model. The cooperative parent (6c659d1)
reaches the field fine (A/B confirmed), so it was MY regression. Per the user (full-ownership, don't revert):
`ov_game_frame` now returns 1 only for OWNED states (area-init + the SOP-intro sm[0x4a]==0 path w/ SOP
loaded) and 0 otherwise; the scheduler hands the GAME task back to the cooperative guest loop (resume
0x801063F4) for the rest. Native intro preserved; field reachable, no derail. **The remaining work = own the
area machine sm[0x4c] + the non-SOP field overlay (make their loads sync, same as native_sop_area_load) so
the fallback shrinks to nothing = FULL native gameplay-start.** Handoff: scratch/handoff_native_gameplay.md.

**later-217g — FIELD AREA MACHINE owned native; the cooperative fallback for the field is GONE.**
Closed the 217e/f gap. CORRECTION to the handoff: the live walkable-field machine is GAME.BIN **0x801088d8**
(s48_2 handler[1], reached at sm[0x4a]==1), NOT 0x80106478 (handler[2]/sm[0x4a]==2, which `skip 400` never
enters). Owned `ov_game_submode1` (engine_stage.cpp): switch on sm[0x4c] (table @0x80106334, 7 states).
- State-0 (0x80108918) does FUN_8005245c (sound/CD setup, sync leaf) + the cooperative area-DATA load
  FUN_80044bd4(0x800452c0,*0x800bf870,0,2), then FALLS THROUGH into state-1 (0x8010893c, sm[0x4c]=next-state
  table[area]) — both run in one frame. Replaced the spawn-and-wait with **`native_transition_area_load`**
  (engine/sop.cpp): a faithful native transcription of the load body 0x800452c0 (quick path + main path:
  FUN_8001cf2c, the next-area file load via FUN_80045080, BGM trigger FUN_8007566c, ov_load_texgroup, the
  area-asset overlay DMA via FUN_8001dc40, collision grid FUN_80045258, the ecf58 reloc-patch loop), with
  the FUN_80051fb4 task-completes + the slot-2-settle / audio-busy yield loops DROPPED (no-ops under our
  synchronous CD/audio runtime). rec_dispatches the leaf callees (all sync).
- States 2..6 rec_dispatch the field RUNNING sub-machine (0x80106b98/70b4/7230/766c/7790, a 12-way sm[0x4e]
  dispatch); a transitive jal-graph scan (197+ fns each, treating CD/task/audio-busy as leaves) found NO
  FUN_80051f80 in them → yield-free, safe to rec_dispatch per-frame.
- `ov_game_frame` now returns 1 for sm[0x4a]==1 (and the SOP-intro sm[0x4a]==0) → the **`[sched] GAME ->
  cooperative guest loop` message no longer fires at all** for the field.
- KEY RE BUG (cost the most): 0x800452c0's `sb v0,0x800bf870` at 0x800453d8 is in the **jal DELAY SLOT** of
  `jal 0x80045080` → it stores the *prior* v0 (= sm[0x6e], the area index), NOT FUN_80045080's return. I
  first stored the return (a 168-byte CD-load size) → FUN_8007566c's `jr v0` jump-table (@0x80016c7c, indexed
  by *0x800bf870-5) read garbage → DERAIL to 0xFF800004. Reading delay slots correctly is load-bearing.
- VERIFIED (headless REPL): `skip 400` → field reaches sm[0x4a]=1 sm[0x4c]=2 overlay 0x801138A4, sm[0x4e]
  cycles 0/9/10/7/8/6/1 EXACTLY as the cooperative baseline (6c659d1), stable 200 frames; clean `newgame`
  120 frames unaffected; ZERO cooperative-fallback / caught-yield / derail / fault in any run. Added a gated
  `yieldpc` diagnostic channel to ov_switch (logs the longjmp caller ra — how I found the FUN_80051fb4
  task-complete was the yield source). Files: engine/engine_stage.cpp, engine/sop.cpp, native_boot.cpp.

**later-217h — FIELD RUNNING sub-machine 0x80106b98 dispatcher owned native (`ov_field_run`).**
Descended one level into the field frame. `ov_field_run` (engine_stage.cpp) owns the 12-way sm[0x4e]
jump-table dispatch (table @0x8010626c) that ov_game_submode1's sm[0x4c]==2 case used to rec_dispatch
wholesale. Replicates the guest prologue's stack frame (`addiu sp,-0x18; sw ra,0x14; sw s0,0x10`) and
rec_dispatches the selected state at its entry — all 12 states fall into the SHARED epilogue 0x801070a4
(`lw ra,0x14(sp); lw s0,0x10(sp); jr ra`), which restores ra to the rec_dispatch sentinel and returns;
the native side then restores the caller regs. VERIFIED: skip 400 → sm[0x4e] cycles 0/9/10/7/8/6/1 EXACTLY
as the cooperative + 217g baseline, 200 frames, zero derail. The state bodies (0x80106bdc..0x80107098)
remain rec_dispatch leaves; their callees (FUN_80072a78 object-placement etc.) are the next descent.

**later-217i — FIELD running sub-machine STATE BODIES + the field per-frame update owned native.**
Replaced the dispatch-only ov_field_run with a FULL native transcription of all 12 states (authoritative
decomp: scratch/decomp/game/80106b98.c — it cleared up the hand-RE ambiguity, e.g. case-1's `DAT_1f800236`
and the explicit fall-throughs case 2→3, case 4→1). The running states now call **`ov_field_frame`** — a
native transcription of the field per-frame update 0x80108b0c (bump frame counters; if not paused run the
11-call gameplay-update block; conditionally 0x8003f9a8; then render-submit 0x8010810c + 0x80077d8c +
per-frame area update 0x80075a80 — yield-free, 1021-fn jal scan). Heavy leaf callees (object-walk 0x8007a904,
display 0x80026c88, FUN_80072a78 placement, …) stay rec_dispatch leaves. VERIFIED: skip 400 → sm[0x4e]
cycles 0/9/10/7/8/6/1 EXACTLY as baseline and rests at sm[0x4e]=1; 300 frames + clean newgame 150 frames,
ZERO derail/fault/fallback. NB: orphan natives ov_objwalk/ov_disp_26c88 exist but are `static` in other TUs,
so ov_field_frame still rec_dispatches the GUEST bodies — wiring them as direct calls (de-static + header)
is a follow-up. Files: engine/engine_stage.cpp.

**later-217j — orphan natives ov_objwalk + ov_disp_26c88 wired into the live field frame (direct calls).**
The 217i follow-up: de-static'd `ov_objwalk` (engine_tomba2.cpp, native FUN_8007a904 object-list walk +
widescreen margin flush) and called it (plus the already-non-static `ov_disp_26c88`, native FUN_80026c88)
DIRECTLY from `ov_field_frame` instead of rec_dispatching the guest bodies. These were ORPHAN natives
(reachable only by the removed override table); they now run on the live gameplay frame. VERIFIED: skip 400
→ `[engine] objwalk` fires (110→152 nodes over 300 frames), sm[0x4e] sequence unchanged vs baseline, zero
derail/fault. Files: engine/engine_stage.cpp, engine/engine_tomba2.cpp.

**(B) Native audio engine — offline synth + sequencer (continues later-216; tool `tools/snd_render.c`).**
- Corrected the ToneAttr parse (ground-truthed from raw bytes): adsr1@0x10, adsr2@0x12, prog@0x14,
  vag@0x16, + note-range min@6/max@7 (the spec had adsr off by 2).
- **Voice synth** (Beetle `CalcVCDelta`/`SPU_RunEnvelope` ADSR verbatim + note→pitch + vol/pan + ADPCM
  loop points): `tone` cmd renders a clean enveloped note (sustain held, release on keyoff). **SEP
  sequencer** (`song` cmd): MIDI running-status interpreter + tempo→samples/tick + 24-voice alloc; all 10
  container sequences render without derailing; output levels match Beetle refs (rms ~2530 vs intro 2538).
- **Program→tone mapping RE'd + APPLIED** (the blocker, now resolved): SEQ program is NOT a direct
  ProgAttr index. Tone selection uses **slot[0x26]** = the SEQ byte at file offset 0x0F (consumed at
  SsSeqOpen 0x8008E390), which is **0x00 for every Tomba!2 seq → VAB program 0**; the runtime 0xCn
  prog-change feeds only vol/pan. baseTone = ProgAttr[prog]+8 (on-disc 0xFF placeholder → libsnd rewrites
  to the program's tone base at load; offline tool uses `prog_tone_start(prog)`). Multi-voice tone scan
  (all in-range matches). Also corrected the **SEP framing to [event][delta]** (delta FOLLOWS each event;
  stream starts at 0x10 after the 0x0F selector byte) — the `p%ps` probe is GONE. VERIFIED musical: seq4
  (14.07s ≈ intro.wav 14.20s) renders rms~1444 with a wide-range VAB. Residual loudness gap = song→VAB
  selection (spec §6 TODO), not the map. Spec: `scratch/native_audio_spec.md` §5b.

## later-208: OWN DEMO front-end substate s7 (attract-demo launch) native — ov_demo_s7_phase REGISTERED
Owned the last reachable DEMO front-end deep-yielder, **s7 0x80106668** (engine/engine_demo.cpp,
ov_demo_s7_phase on the phase machine **0x80106C24**), via the same coro-redirect handshake as s0/s2/s3/s6.
The override body was already written + RE'd in a prior session but UNREGISTERED ("could not be VERIFIED to
fire"). This session **reached s7, verified it, and registered it.**

**KEY DISCOVERY — how to reach s7 (the prior "needs New-Game confirm" assumption was WRONG).** s7 is the
ATTRACT-demo auto-launch, reached by letting the **title intro timer expire**, not by confirming a menu
option. Recipe (headless REPL): `run 45` (boot to DEMO), `tap 4008` once (the title s2 confirm-edge advances
s2->s3, the title-screen menu page), then `run ~455` — s3's intro/hold timer `sm[0x5a]` (inits 450) counts
down once per frame and at expiry the front-end AUTO-advances `sm[0x48]->7` with NO further input. (Blind
menu taps only ever reach s2/s3/s5, never s7 — that's why prior sessions couldn't hit it; the s3 confirm
returns outcome 2 -> s5 leave-demo, and there is no v0==1 confirm path in the title machine. The s7 path is
the timer auto-launch.) Disasm of 0x80106ac4 (s3 machine) confirmed: after the timer, edge 0x4008 returns 2
(not 1); 0x2000 returns 3; the s7-producing outcome-1 is the timer expiry.

**Verification (all on the live headless port, override registered = the behavior):**
- Disasm-faithfulness re-checked against a fresh menu RAM dump (ram_menu.bin): the trampoline `jal 0x80106C24`
  at 0x80106668 falls into TAIL_NONE 0x80106670 (jal sets ra=0x80106670); the phase prologue sp-=40 / save
  s0@24,ra@32,s1@28 / s1=1 / phase select on sm[0x4a] (beq phase1@0x80106d4c / phase0@0x80106c74 /
  phase2@0x80106dfc / else epilogue@0x80106e14); phase2 = `jal 0x80074bc4` (SYNC) ; *0x1f80019a=0 ;
  sm[0x48]=0 ; epilogue `jr ra`. ALL match the override exactly.
- **FIRES at every reachable phase** (`demo` log): phase0 (sm[0x4a]=0) once at the launch frame; phase1
  (sm[0x4a]=1) continuously through the attract play loop (sm[0x6e] wrapped to 1 by phase0, sm[0x5a] counting
  down from ~26216). The later-170 never-fired trap is cleared.
- **A/B 0-diff (the existing-substate gate):** full-RAM + scratchpad `dumpram` at a steady phase1 frame
  (override-ON) vs a guest-baseline dump captured at the identical drive sequence = **scratchpad 0-diff;
  main-RAM diff = exactly 2 bytes** at 0x1FE05A (a single saved-context halfword in the top-of-RAM coroutine
  stack page, native 0x659E vs guest 0x02BA, surrounded by zeros) — the DOCUMENTED coro-redirect
  saved-ra/sentinel artifact (same artifact class as the other coro-redirect substates), never game data.
- **phase0+phase1 ran clean over 2000+ frames** (no bad opcode / no task-0 death — the prologue-frame
  replication lets the deep in-body yields longjmp/resume in-context and the body's `jr ra` returns to the
  trampoline). **phase2 verified by injection** (poke sm[0x4a]=2 while in s7): the override took the phase2
  branch, dispatched the SYNC teardown 0x80074bc4, set sm[0x48]=0 (front-end restart), and the front-end
  cleanly re-ran s0->s1 — no crash. (Reaching phase2 naturally needs ~26000 frames = the full attract play
  out, so injection is the practical gate for the all-SYNC teardown path.)
- **No regression:** the `newgame`->`skip 500` path still reaches the GAME stage 0x8010637C (seaside field)
  cleanly — s7 only fires on the attract path, never on newgame.

**s4/s5 stay GUEST (final, re-confirmed, NOT a TODO).** s4 0x80106580: its only engine logic is the sm[0x6b]
branch at 0x8010658C, reached by `jr ra` from the deep yielder 0x8007bf20 -> UNREACHABLE by an override (the
table is consulted only on jal/j/jalr/computed-jr; a `jr ra` RETURN does not consult it — interp_flat
453-483). s5 0x801065DC: whole body is `jal 0x80052078(2)` leave-demo + tail yield, nothing to own. **The
DEMO front-end is now owned to the limit the override mechanism allows** (s0/s1/s2/s3/s6/s7 native; s4/s5
structurally guest). Updated engine_demo.cpp (registered + reach recipe), docs/engine_re.md, docs/port-
progress.md §C.

## later-207: OWN per-area BAV effect/animation CEL LOADER FUN_80096590 native (ov_bav_load)
First **asset-loading subsystem** owned end-to-end (engine/engine_bav.cpp). `FUN_80096590` loads an area's
effect cels (the seaside field loads 3 at entry: descriptors 0x801846b4 / 0x801858d4 / 0x801886f4). It
allocates one of 16 cel SLOTS, parses the BAV descriptor's 16-byte cel-record table + UV table, sums the
per-frame VRAM sizes, calls a caller-supplied VRAM allocator/upload **callback** (a2 = FUN_800964b4 → the
allocator FUN_800977c0), then writes per-frame tpage/clut halfwords into the cel records (rec+12 even / +14
odd) and latches the slot-keyed cel-system globals. Caller chain: area-loader FUN_800753D4 → wrapper
FUN_80096480 (prefills the callback in a2) → FUN_80096590.

OWNED native: the full control flow + the descriptor parse + EVERY cel-global write (slot state table
0x80105D18, refcount 0x80105D70, data/desc/UV-base 0x80105C10/C50/C98 [index = slot*4], 64/128 clamp
0x80105CDA, 0x80105CF0, size 0x80105D30, VRAM base 0x80105D78, lock 0x800AC638) + the two trivial lock
helpers (FUN_80099478 test / FUN_80099450 set, single-word ops on 0x800AC638, inlined). The VRAM
allocator/upload callback stays PSX via rec_dispatch in the recomp's exact order (its free-list is in
tracked RAM 0x800AC5xx/6xx → returns the same VRAM base in an A/B re-run; verified D78 native==oracle).
Full RE (descriptor struct, cel record, globals, callees, control flow) in engine_re.md "BAV cel loader".

VERIFIED with the `bavload` full RAM+scratchpad+v0 A/B gate vs rec_super_call (native → snapshot+rollback →
recomp body → diff, excluding the top-of-RAM stack window [sp-0x800, sp) per the grid/scriptvm family
rationale): **0 mismatches over all 3 area-entry cel loads** (a spawn-time loader, called once per cel at
area init, not per-frame — so 3 is the natural exercise count, same posture as child40410's 28 spawns).
Drive: `debug bavload; newgame; skip 650; (walk)` — fires on the seaside area entry.

3 GOTCHAs, each caught BY the A/B gate (the whole reason the gate exists):
1. **Lock semantics inverted.** FUN_80099450(1) ACQUIRES via a `sw zero` in the a0==1 branch's DELAY SLOT
   → writes 0 (busy), not 1; FUN_80099450(0) RELEASES → 1 (free). 1=FREE / 0=BUSY. Guard FUN_80099478 =
   `(lock^1)!=0`. First mismatch was lock-only (native left it busy on a path the recomp released).
2. **kind-shift inverted.** `if (*(desc+4) < 5)` uses `<<2` (the `sll v1,2` is the bne-taken delay slot);
   `>=5` uses `<<3`. Had it backwards → every size was 2× off (D30 exactly doubled).
3. **C10/C98 index = slot*4, not field18>>14.** The `sll s2,16` feeding the `sra ,14` is a beq DELAY SLOT,
   so the index = `((int16)slot<<16)>>14` = slot*4. I misread the sra's source as field18 → wrong slot wrote.

Build/infra note: a parallel worktree agent shared scratch/obj + scratch/bin (the scratch symlink), so its
in-flight game_tomba2.cpp link-clobbered my binary. Fix = give the worktree a PRIVATE scratch (own obj/bin)
and symlink only the read-only input dir scratch/bin/tomba2 (MAIN.EXE + SCUS_944.54 + START.BIN etc.) — the
missing SCUS_944.54 was why the loader silently never fired for a stretch. (Confirms the docs gotcha:
parallel agents need separate OBJDIR/EXE, not a shared scratch.)

## later-206: OWN per-object DISPATCHER LOOP FUN_80026C88 native (ov_disp_26c88) — §F resident leaf
The §F-flagged primary target after later-205. `FUN_80026C88` is a per-object DISPATCHER LOOP over the
40-entry, 64-byte-stride object table at `0x800ec188`. **No args, void return. NO GTE, NO render packets —
pure control flow.** disas / gen_func_80026C88:
```
  s2 = 0x800ad52c  (handler fn-ptr table, stride 4)
  s0 = 0x800ec188  (object table, stride 64)
  for i in [0,40), obj += 64:
    if (lbu obj[0]) == 0: continue        ; obj[0] = active byte
    idx = lbu obj[1]                       ; obj[1] = handler index
    fn  = *(s2 + idx*4)                     ; load handler fn-ptr
    a0  = obj; (*fn)()                      ; tail-call handler(obj)
```
The dispatcher itself writes NOTHING to memory (the recomp body only saves/restores s0/s1/s2/ra in its
own 32-byte stack frame, which the native body never touches). EVERY side effect lives inside the
dispatched handlers, kept PSX via `rec_dispatch` (each handler honors its own owned override identically in
the super-call path). Control flow + the table/object address math owned native; handlers dispatched.

Verified with the full RAM+scratchpad A/B gate `disp26c88` (native run → snapshot+rollback → rec_super_call
→ diff): **0-diff over 800+ live field calls, 0 mismatches** (`debug disp26c88`, newgame → skip 650 →
press right 250 → press left 250). It FIRES per-frame in the reachable seaside GAME stage (the counter
ticks 50/100/…/800+). Same-family gate exclusion `[sp-0x800, sp)`: the dispatched handlers run in BOTH
passes and leave transient residue in their own stack frames below entry sp (no native frame there) + this
fn's own 32-byte frame is dead below sp on return (sp ~0x1FExxx, RAM end 0x200000 — far above ALL game
data; a real divergence would alter persistent state). Gate-off run reaches GAME(0x8010637C) and walks
normally. Registered in game_tomba2.cpp alongside the other dispatcher-loop overrides. (No new .cpp — edit
to game_tomba2.cpp only, so no run.sh / build_port.sh SRC change.)

NEXT clean §F targets: `FUN_8003F024` (the flagged fallback — same per-object-dispatcher shape, verify the
same way; disas it FIRST to confirm no GTE/GP0 and that it fires); then the remaining transform-cluster
consumers of `FUN_80051128`. AVOID `FUN_80027A4C` (16% but GTE/GP0 packet submitter, render-boundary).

## later-204: OWN sm40558 STATE-1 sub-behavior FUN_8003FD10 native (ov_osc_fd10) — first descent into the state-machine's hot active-behavior callees
Natural descent from later-203 (FUN_80040558 owned): STATE 1 (the ~11000×/run hot active-behavior path)
dispatches into the obj[5] jump table JT1[6] @0x80015300. JT1[0] = `FUN_8003FD10`, a per-object OSCILLATE /
FRAME-TOGGLE sub-behavior (a counter-driven 2-frame sprite/offset oscillator on a child node). **a0 = obj,
void return. NO GTE, NO render packets — pure object/scratchpad memory ops + ONE dispatched callee
(0x8009A450 = ov_rand, already owned).** A 3-way micro state-machine on the phase byte obj[6]:
- **obj[6]==0** (@fd40): if obj[43]==0 return; else obj[6]=1, obj[43]=0, obj[64](sh)=16.
- **obj[6]==1** (@fd64): if obj[43]!=0 { obj[43]=0; obj[64]=16 }; then @fd7c: cnt=obj[64](lhu); cnt--;
  obj[64]=cnt; if (int16)cnt==-1 obj[6]-- (the `addiu v0,v0,-1` adds v1=-1); then @fdb0:
  r=(*(u16*)0x1F80017C)&1; node=*(obj+0xC0); node[2](sh)=r*6; rr=ov_rand(); node[0](sh)=((rr&3)-2)*6.
- **obj[6] other** (@fdf0): return.
**Owned native:** all control flow + every obj/scratchpad/node memory access. ov_rand stays PSX via
rec_dispatch (honors its own override identically in the super-call path). GOTCHAs (every one a delay-slot
hazard, all caught by the A/B): (1) the `sh v1,2(node)` at 0x8003fdd0 is in the ov_rand jal DELAY SLOT —
node and v1=r*6 are computed BEFORE the call (node loaded @0x8003fdc4), the store uses the pre-call values;
(2) the obj[6]-- at @fdac only fires on the cnt==-1 branch (the `addu v0,v0,v1` with v1=-1); (3) node[2]/[0]
are halfword stores of v0*6 = (v0*3)<<1.
**`fd10` gate** (game_tomba2.cpp, sm40558 template): full RAM (0x200000) + scratchpad (0x400) A/B vs
rec_super_call — native runs once, RAM/scratch/regs rolled back, super_call runs, byte-compare. **0-diff
over 11000+ live field calls** (newgame + skip 650 + press right 250 + press left 250 — this is a hot
state-1 sub-behavior, so 11000 is the natural exercise count; it covers the obj[6]==0/1 phases, the
counter-decrement/wrap, and the ov_rand-driven node offset). Same-family gate exclusion [sp-0x800, sp)
(the dispatched ov_rand runs in BOTH passes + this fn's 24-byte frame is dead below entry sp on return;
sp ~0x1FE9xx, far above all game data). Lazy cfg_dbg gate so the REPL `debug fd10` works. Live (gate-off)
run reaches the GAME stage and walks normally. Registered in game_tomba2.cpp (no new file → no run.sh /
build_port.sh SRC change). **NEXT in the descent:** the other JT1 entries (0x8003FED8/FFCC/4022C/40390)
and the obj[94] @7e0 common-block callees — same posture; 0x80040390 is the next-cleanest (gated on obj[41],
2 dispatched callees, no GTE).

## later-198: OWN the per-object ANIMATION-SEQUENCE VM stepper FUN_80076D68 native (ov_anim_vm_76d68)
Frontier §F resident leaf (3.6% field interp time, no GTE, a freq leader on motion scenes). First port of
the animation-keyframe VM — a content state machine, same methodology as scriptvm/pad931c0 (later-196/197).
**a0 = anim object.** Two object fields drive it: s0+0x0E (h) = the per-frame COUNTDOWN/duration (low12) +
a 0x1000 freeze flag; s0+0x38 = the CURSOR (word ptr) into an 8-byte-stride keyframe stream — each keyframe
has a tag/payload halfword at +6 and a jump pointer at +8. ctrl=s0[0x0E] is read ONCE at entry.
- **low12(ctrl) > 1 → DELAY** (still counting this frame): the cursor's SIGN BIT is a flag. cur<0 = freeze
  in place (h=(low12−1)|(ctrl&0x1000), ret 0). cur≥0 = apply the current frame (jal 0x80075f0c, a1=low12−1)
  then h=(low12−1)|(post-call s0[0x0E]&0x1000), ret 2.
- **low12(ctrl) == 1 → STEP** (frame elapsed): dispatch on [cur+6]&0xc000 — 0x0000 advance cur+=8;
  0x4000/0xc000 FOLLOW the jump cur=[cur+8]; 0x8000 terminal/hold (h=[cur+6]&0xfff, no advance). After the
  advance/follow, reload the new frame's duration into h via jal 0x80076904, and if the LOADED frame has the
  0x2000 "execute" bit, run the executor jal 0x80075ff8 keyed by the new tag. (When frozen — ctrl&0x1000 —
  the cursor is NOT advanced in any block.)
- **Owned native:** all control flow + the s0[0x0E]/s0[0x38] reads & writes. The 3 callees (0x80075f0c
  per-frame applier, 0x80076904 frame loader, 0x80075ff8 frame executor) stay PSX via rec_dispatch (each
  honors any later override identically in the super-call path).
- **`animvm` gate** (game_tomba2.cpp, scriptvm template): full RAM (0x200000) + scratchpad (0x400) A/B vs
  rec_super_call — native runs once, RAM/scratch/regs rolled back, super_call runs, byte-compare + v0;
  exclude the fn's own 40-byte stack frame [sp−40,sp). **0-diff over 4000+ live calls** across right/left/
  up/down movement (exercises advance + follow-jump + freeze + the executor sub-calls). Lazy cfg_dbg gate
  (re-checked each call) so the REPL `debug animvm` works (the fn can run before the channel is set).
- **4 delay-slot/arg-register GOTCHAs, every one caught by the A/B (the gate working):** (1) the cur<0 path
  writes `(low12−1)+(ctrl&0x1000)`, NOT a literal +2 — the +0x1000 is the bltz delay slot `andi v0,a0,0x1000`
  (a0=entry ctrl). (2) 0x80075f0c needs **a1=low12−1** — it ORs the KSEG0 bit (0x80000000) into the cursor
  iff (int16)a1==1; calling it with only a0 left the cursor's high byte clear (mismatch at obj+0x3b = the
  cursor's top byte). (3) T4000/TC000 FOLLOW [cur+8] when NOT frozen — the asm writes a transient cur+8 that
  is immediately overwritten by [cur+8]; I first modeled it as the inverse (cur+8 not-frozen / follow when
  frozen). (4) the DELAY apply path RE-READS s0[0x0E] AFTER 0x80075f0c (the applier may modify it) for the
  freeze-bit OR — using the entry ctrl's bit was wrong.
Build/verify done in the agent worktree (parent `generated/`+`build/` symlinked, MAIN.EXE + obj cache copied,
`.env` copied, `vendor/beetle-psx` submodule init'd — no code change to build scripts). Field run perf gain
expected ~3.6% (the bucket leaves the hot-list); re-profile to confirm if needed (not re-profiled this pass).

## later-182: OWN the DEMO / front-end MENU substate machine PC-native (s1/s2/s3/s6) — `engine/engine_demo.cpp`
Frontier item 2 (docs/port-progress.md). Mirrors engine_stage.cpp's GAME-stage ownership for the DEMO
overlay (base 0x80106228, aliases GAME.BIN — distinguished by the root prologue sig 0x801062E4==0x27bdffd0).
The root dispatcher 0x801062E4 (prologue + per-frame loop reading sm[0x48], table @0x8010622C `jr v0`, single
yield in the TAIL) runs as task-0's coroutine; each substate body is an inline block reached by the loop's
computed `jr v0` and exited by `j <TAIL>`. We do NOT override the root — the loop's `jr v0` into an owned body
address fires the override (interp fires overrides on computed jumps, interp.cpp:481), exactly the ov_game_s4c
shape: native work + coro-redirect to the guest TAIL.
- **OWNED: s1 (0x8010641C), s2 (0x80106464), s3 (0x801064E8), s6 (0x801065EC)** — the substates whose only
  sub-call is SYNCHRONOUS. They rec_dispatch the inner machine (0x80106f80/0x8010696c/0x80106ac4/0x8007b45c +
  0x8001cf2c/0x80106824/0x80106690/0x800750d8), then own the transition LOGIC native and redirect to the TAIL.
- **NOT owned yet: s0/s4/s5/s7** — DEEP-YIELDING (s0 loaders 0x80045080/0x80044bd4; s4 0x8007bf20; s5
  stage-restart 0x80052078; s7 loader 0x80106c24 all reach FUN_80051f80). A plain rec_dispatch of a deep
  yielder longjmps out and destroys the override C frame → kills task 0 (later-169). They stay guest until
  reworked with the coro-redirect-INTO-the-yielder handshake. Leaving them guest is safe: the guest root loop
  dispatches them normally; only the owned addresses divert.
- **NEW TOOL `tools/yield_reach.py`**: scans a 2MB KSEG0 RAM dump, follows direct `jal` recursively from a
  fn, reports whether FUN_80051f80 (the yield) is reachable → tells you SYNC (safe to rec_dispatch) vs YIELDS
  (needs handshake). Direct-jal only, so "no yield" is a lower bound; it prints indirect-call blind spots.
- **`dumpram` now also writes a `.spad` sidecar** (the 1 KB scratchpad 0x1F800000) — the main-RAM A/B diff was
  BLIND to scratchpad, where several DEMO flags live (0x1f80019a/19d/134/198).
- **A/B GATE PASSED.** override-on vs override-off (PSXPORT_DEMO_OFF, a TEMPORARY verify toggle, now removed)
  at a steady menu frame via REPL `run 150`: main-RAM diff = ONE word at 0x801FE9CC (task-0's saved-ra stack
  slot: ON=0xDEAD0000 CORO_SENTINEL vs OFF=0x80106424 guest return-PC — the inherent coro-redirect mechanism
  artifact, dead stack residue, not engine/content state) and **scratchpad 0-diff**. All sm fields + live
  state byte-identical. (Confirmed it's the root frame: 0x801FE9B0..BC = the saved s0/s1/s2/s3 regs
  1F800000/2/1/3.) This matches the engine_stage ownership result class (0-diff modulo the override's stack
  bookkeeping).

## later-183: own the engine SUBSYSTEM init FUN_800520e0 orchestration PC-native (eng_init_subsystems) — frontier item 3
Init-prefix function dispatched once at boot (native_boot.cpp, was `rc0(c, 0x800520e0)`). RE (2 parallel
Explore agents over ram_menu.bin, also mapped FUN_80075130) confirmed it is FULLY SYNCHRONOUS and touches
NO GPU/DMA/SPU/GTE — pure engine-state: 6 direct flag writes (*0x800bf4fa=0xffff; *0x800ecf4a/4c/4d/4e/4f=0)
then 4 subsystem-init callees (8007b328 entity-pool, 80088b00 allocator/dispatch-table[a0=0x800bf4f8,
a1=0x800bf51a], 80086620 mode-ctrl(1), 80087a60 input). Owned the ORCHESTRATION + the 6 writes native in
`engine/engine_init.cpp` (`eng_init_subsystems`, mirrors eng_init_framestate); the 4 callees stay dispatched
(each is SYNC with no effective indirect call at init — 80086620's gated jalr needs counters >0x95, =0 here).
**A/B GATE PASSED** (boot frame 50, REPL run 50): main-RAM 0-diff AND scratchpad 0-diff vs the guest dispatch.
NEXT = descend into the 4 callees one by one (entity-pool init connects to the engine's entity ownership).
FUN_80075130 (font/text) DEFERRED: 8/14 callees are LIBGPU with indirect calls → the later-182b nested-
dispatch divergence risk; own only its 3 engine-state callees (FUN_800963a0/80096370/800752b4) + memsets later.

## later-182b: DEAD-END — owning the DEMO ROOT dispatcher prologue (ov_demo_root) introduces a GP0 env-packet divergence; REVERTED
Tried to own the DEMO root 0x801062E4 prologue native (mirror ov_game_stage_main): reproduce the
register/flag init, rec_dispatch the two SYNC setup calls (0x800810f0, 0x8005082c), coro-redirect to the
loop body @0x80106388. The scheduler DOES fire an entry override on a fresh task entry (native_boot.cpp:134),
so the mechanism works. BUT the A/B gate (override-on vs -off, REPL run 150) showed a **5-byte divergence**
(GP0 command byte 0xE0 vs 0x00) in the draw-environment packet pools at 0x800A59xx / 0x800EA0xx — while the
substate sub-fns dispatched 0-diff. ROOT CAUSE: 0x800810f0 builds the PSX draw-env via an INDIRECT libgpu
call (`jalr v0[8]` off the double-buffer struct *0x800a5998, with a {0,0,320,512} desc + black color), and
running 0x800810f0 as a NESTED rec_dispatch from the override is NOT equivalent to running it in the task
coroutine context. Scratchpad was 0-diff (incl. buffer parity 0x1f800135 + frame ctr 0x1f800198), so this is
NOT a frame-phase artifact — it's a genuine output difference from the nested-dispatched libgpu init.
DECISION: REVERTED (no unverified override ships). The root prologue is low-value PSX disp-env plumbing the
boundary says the engine shouldn't reproduce; owning it cleanly would need 0x800810f0 to run IN-CONTEXT
(coro-redirect into it), which conflicts with its two-sync-call-then-loop structure. The valuable part — the
substate transition LOGIC (s1/s2/s3/s6) — is owned and verified 0-diff (later-182). LESSON for the deep
yielders: a SYNC sub-fn that itself makes an INDIRECT libgpu/hardware call may not be safely rec_dispatch'd
from an override; check for jalr-to-overridden-target, prefer coro-redirect-in-context for those.

## later-179: SCEA license screen — render it PC-NATIVE (composite the text into the framebuffer) + own its duration
**The SCEA "Sony Computer Entertainment America Presents" screen now displays** (fades in to crisp white,
holds, then proceeds to the FMVs/menu). USER directive (2026-06-20): stop skipping/being walled by PSX —
make it PC-native; no need to be faithful to the PSX boot.

**Two problems, both fixed:**
1. **It handed off after ~10 frames (near-black flash).** The boot stub (SCUS_944.54, interpreted) draws SCEA
   with a count-driven fade (rgb +2/frame, full at count ~100) and LoadExec's MAIN when its CdRead retry
   loop succeeds. `ov_stub_cdread` returned instant success → the retry loop (which IS the on-screen hold)
   collapsed, so it cut away at rgb~18. FIX: the ENGINE owns the splash DURATION — `ov_stub_cdread` reports
   "busy" until the stub vblank count reaches `SCEA_SPLASH_VBLANKS` (default 160; PSXPORT_SCEA_HOLD overrides,
   0 = old instant), so SCEA plays its full fade-in + hold (windowed: the count tracks REAL vblanks, ~2.7s).
2. **The stub's textured rects don't rasterize through our hi-res VK path** — the text asset is uploaded to
   VRAM (4bpp page @ (832,256), CLUT @ (880,511)) but the sprites never landed in the framebuffer (proven:
   PSXPORT_VK_FULLSHOT showed the source texture present but the framebuffer + scratch-FB black; the prims
   ENUMERATE but don't draw). Rather than debug the stub→VK pipeline, we render SCEA PC-native exactly like
   the offline exporter: `GpuState::scea_splash_composite(fade)` (gpu_native.cpp) decodes the 4bpp+CLUT text
   from VRAM and writes it straight into the displayed framebuffer s_vram (0,0), modulated by the fade level;
   `ov_stub_vsync` calls it each present with fade = count*2. Present is `blit_src(s_vram,…)` so a direct
   s_vram write is guaranteed to show. (Transparency = a 0x0000 CLUT entry, NOT index 0 — index 0 here is the
   white fill; testing idx==0 first painted only the dark outline.)

**Proven offline first** (the "same fix" the user asked to apply): decoded the 3 SCEA rects' texpage/CLUT/UV
from the captured GP0 and reconstructed the full "Sony Computer Entertainment America Presents" image from a
VRAM dump (scratch/screenshots/scea_export.png) — confirming the asset is intact and it's purely a
render/compositing gap. The sprite layout baked into scea_splash_composite is from that decode.
VERIFY: `PSXPORT_VK_SHOT` headless — fades in (grey→white) and holds; title + field unaffected.
**NOTE (PC-native end state):** the stub still runs (it uploads the asset + LoadExec's MAIN). The eventual
fully-native path decodes SCEA straight from SCUS_944.54 and drops the interpreted stub entirely; the
hi-res-2D-sprite VK-rasterization drop is also a real bug worth fixing for other 2D (see OPEN BUGS).

## later-178: FIX the title/menu BG render — later-172's linear pool walk scrambled the E1-texpage→sprite order
**The title (DEMO stage 0x801062E4) now renders correctly** (TOMBA!2 logo + brick + characters + New Game/
Load Game — matches `scratch/screenshots/fl_06.ppm`). The field still renders correctly (no regression).
USER-directed ("aim for this first").

**later-177b's root-cause was WRONG — FALSIFIED (see below).** It blamed a hardcoded FIELD pool base in the
walk, missed by the title's "own pool." That is not it: the OT genuinely holds only ~5 prims on the title
(independent `PSXPORT_SCENEDUMP` OT-walk confirms it), and reverting to the OT walk draws the SAME 5 prims —
so the *pool base* was never the issue, and disabling the later-177 loader override left the title black too
(later-177 is exonerated). The 5 prims are not a "drop": they are the FULL title (2 full-screen background
sprites + 3 menu-text polys). The bug was that **the 2 background sprites rendered BLACK.**

**Real root cause — confirmed mechanically.** The title's 2 background sprites are op 0x65 (RAW textured rect:
color ignored, texture used directly), each immediately preceded in the OT by its OWN E1 DR_TPAGE. Decoded
the OT vs our walk from a live RAM dump (`tools/disas` + a hand OT-walker on `scratch/bin/title_ram.bin`):
- **OT LINK order (= guest DRAW order):** `E1(768,256) → sprite1(x=256,64w) ; E1(640,256) → sprite0(x=0,256w)`
  — a 2D OT links its nodes in REVERSE allocation order (classic prepend-to-bucket-head).
- **later-172's LINEAR PACKET-POOL walk (memory/ascending-address order):** `sprite0, E1(640), sprite1,
  E1(768)` — it DECOUPLED every E1 texpage from its sprite. So sprite0 sampled a STALE texpage (whatever the
  previous frame's last GP0 left) and rendered BLACK; sprite1 got sprite0's E1 (640,256) instead of its own
  (768,256). The captured GP0 stream (`PSXPORT_GPUTRACE`) matched the broken pool order exactly.
- **Why the FIELD was unaffected:** its prims carry their texpage inline (POLY_GT vertex words) and the depth
  buffer owns occlusion, so memory-vs-link order is invisible there. later-172 was verified ONLY on the field
  → the false premise "memory order ≡ draw order, the engine re-sorts anyway" shipped. It is FALSE for 2D:
  GP0 state commands (E1 texpage / E2 texwindow) are ORDER-DEPENDENT and must be applied in DRAW order,
  because each 2D sprite binds the texpage of the E1 that PRECEDES it.

**FIX (gpu_native.cpp `gpu_dma2_linked_list`):** restore OT LINK-ORDER enumeration (revert later-172's linear
pool scan; removed the now-dead `s_pool_drawn`). This is NOT honoring PSX *visibility* order — the engine
still OWNS what's on top: 3D world prims carry real per-vertex depth (the depth buffer decides occlusion,
order-independent); 2D prims use enumeration order, which (until the 2D drawers are owned at SOURCE — the M4
work) IS the guest draw order, the only correct enumeration for *replaying* guest GP0. Link order is needed
purely to reconstruct correct GPU STATE (texpage) per prim. The "sea-bg-on-top" class (a 3D ordering bug) is
unaffected — that lives in the depth buffer, not here.

**VERIFY:** title VK present = full logo/brick/characters/menu (`PSXPORT_VK_SHOT=540`, headless); field VK
present = village byte-for-the-eye identical to the oracle gameplay shot, both via the VK path (the raw
`PSXPORT_VRAMDUMP` shows the SW `s_vram`, which is BLACK for 2D screens because the textured sprites render
through the VK rasterizer, not SW — a trap that made later-177/177b read "framebuffer black" off the wrong
buffer). The gpu_differ replay harness is currently broken (build.sh references the deleted native_dl.c) —
left as-is; not needed here (this was a missing-state bug in the enumeration, not a rasterizer-fidelity bug).

## later-177: ASSET PIPELINE ownership — texture DECODE proven PC-owned; loader ORCHESTRATION is the gap (USER directive)
**USER directive (2026-06-20), supersedes the camera frontier:** port top-down boot→gameplay; the asset
pipeline (load→decode→render of textures AND models) must be FULLY PC-owned so assets can be reconstructed
with the emulator OFF. The acceptance test: reconstruct a known atlas **bit-identical**. Target reference =
the **sea backdrop, native 288×576** (user-supplied 576×1152 = 2× it; sky gradient + clouds + horizon +
tiled cyan water). **Order: fully own the pipeline FIRST, then match 100%** (user).

**Pipeline state (what runs in the texture build):**
- ✅ file load `ov_cd_loadfile` (0x8001DB8C) · ✅ LZ decompress `lz_decompress`/FUN_80044D8C ·
  ✅ group unpack `ov_unpack_group`/FUN_80044E84 · ✅ VRAM upload `ov_upload_image`/FUN_80081218 ·
  ✅ CLUT/bit-depth DECODE (`GpuState::tex_export`, new `PSXPORT_TEXEXPORT`; same logic as sample_tex).
- ✅ **post-step `FUN_80080f6c`** per image — RE'd: it is `FUN_80083364(0)`, the libgs frame **DrawSync**
  (waits for the GPU OT-DMA to drain + GPU idle by polling GPUSTAT @0x800a5ab4 bit 0x01000000 / @0x800a5aa8
  bit 0x04000000). It is ALSO the main-loop per-frame DrawSync, so it must NOT be globally overridden (a
  global no-op stalls ALL presentation — observed: gpu present count froze < 200 while native frames hit
  460). The UNPACK call site, though, is a between-uploads GPU sync that is meaningless for our SYNCHRONOUS
  native VRAM upload (no async DMA to wait on), so the unpack loop now OWNS it as a skip (game_tomba2.cpp,
  gated only by diag `PSXPORT_DEBUG=unpacksync`). A/B-verified: VRAM byte-identical (present f100, post-menu-
  load), main-RAM identical at native f455 except 14 bytes of DEAD STACK below sp (decode = the post-step's
  own saved ra 0x80080FC4 + saved 0x800Axxxx regs — transient call-frame scratch, not game state).
- ✅ **the per-group ORCHESTRATION `FUN_80044F58`** — owned PC-native as `ov_load_texgroup` (game_tomba2.cpp).
  RE'd the whole loader: the current task picks a set via task[0x6D]=mode / task[0x6E]=set, then (1) CD-loads a
  2KB HEADER from sector (filebase0=*0x800BE0F0)+set [+4/26 bias in mode 2 from the 0x800BFE56 bitmask] to
  0x800EF478, (2) CD-loads the compressed ARCHIVE from sector (filebase1=*0x800BE0F8)+(hdr[0]>>11), len
  hdr[1]-hdr[0], to the FIXED staging buffer 0x8018A000 (descriptor table in first 0x800 bytes, packed data
  after — so the "group table" IS the loaded file), (3) UNPACKs it (owned), (4) copies a 42-word per-set
  metadata table hdr+0x100 -> 0x800FB170 (content reads it back), (5) mode==0 only: sets _DAT_1f80019b=1 +
  runs the task terminal-yield FUN_80051FB4 (this streams one group/frame). Reimplemented native, calling the
  owned leaf fns (ov_cd_loadfile via 0x8001DC40, ov_unpack_group) + the scheduler yield; NOT transcribed.
  A/B-gated (main-RAM diff at native f455): ON-vs-OFF differs by only 8 bytes of DEAD STACK (the gen frame's
  saved s0/ra spill slots my C path doesn't write) — the non-zero diff confirms the override runs; VRAM
  byte-identical (present f100). The 5 seaside archive loads (10/2/4/2/6 images) compose the multi-page VRAM
  backdrop; observe with PSXPORT_DEBUG=unpack.
- ✅ **OFFLINE bit-identical RECONSTRUCTION (the user's acceptance test) — PROVEN.** `tools/tex_reconstruct.c`
  (standalone C, NO PSX execution) rebuilds the seaside VRAM atlas from the raw compressed archives alone:
  it mirrors ov_unpack_group + lz_decompress on flat buffers (descriptor walk + the MAIN.EXE @0x800153C8
  offset table baked in: mode 1 = -1 byte RLE, modes 2..7 = row-relative factor*stride) and places each
  decompressed stride×field image at its VRAM (x,y). Fed the 5 seaside archives (dumped via PSXPORT_UNPACKDUMP,
  now full-size to RAM end) and compared to the live PSXPORT_VRAMDUMP f455: **353034 / 353216 touched VRAM
  words bit-identical (99.95%)**. The 182 residual words are NOT decode errors — they are 16-pixel-wide
  PALETTE strips the running game color-cycles per frame: the (1008,191) 16×65 CLUT block (the sea palette,
  x1008-1023/y210-255 = 146 words), a bottom CLUT strip at x688-703/y509-511 (30 words), and 6 sprite-cell
  words — all runtime-animated state, by definition not in the static archive. **The static image pipeline
  load→decompress→unpack→VRAM-place is reproduced 100% bit-exact with the emulator OFF.** Run:
  `cc -O2 -o scratch/bin/tex_reconstruct tools/tex_reconstruct.c` then
  `tex_reconstruct <archive...> out.bin --cmp live_vram.bin`.

- ☐ **the on-screen 288×576 sea BACKDROP composition** — the multi-page VRAM layout the 5 loads build (pages
  at VRAM y=0 and y=256, x in {320,448,512,576,640,832,896,1008}) is now fully owned + offline-reproducible;
  remaining is how the ENGINE composes/scrolls those pages into the on-screen backdrop (render ordering, the
  engine's own 2D layer/sort — ties into the broader render-ownership work, NOT the asset pipeline which is done).

**Proven this session (committed 2ce8b00):** `PSXPORT_TEXEXPORT=<frame>` exports each background atlas via
OUR decoder (no PSX in the decode). Verified 3: menu text atlas (4bpp, "NewGame LoadGame OPTIONS StartGame"),
menu char-art bg (8bpp, Tomba!2 logo+chars), sea EFFECTS atlas tp=(320,256) clut=(1008,255) 4bpp (water
sparkle/foam/rainbow sprites). NOTE the sea 288×576 BACKDROP (sky+clouds+water) is a DIFFERENT asset than
the effects page — found by reaching seaside f450 (AUTO_GAMEPLAY); its source/layout is TBD (loader RE).

**Title/menu BG render is BROKEN (regression) — separate from decode:** the full title (nav3.png: logo+brick+
characters) does NOT composite in the current build; at f440 only ~5 prims submit (text atlas + char-art
strip + 3 menu polys), the 352-tile background is NOT submitted, and VRAM framebuffer regions (0,0)/(0,256)
are black. Textures ARE in VRAM and decode fine, so this is a RENDER/submission drop (suspect: packet-pool
enumeration after the OT-walk retirement later-172), not a decode bug. To "export the menu image" the user
wants, the title must render correctly under PC ownership first.



## later-177b: ~~ROOT-CAUSE of the title/menu BG render regression — the pool-walk hardcodes the FIELD pool~~ FALSIFIED
**FALSIFIED by later-178 (above) — do NOT act on this entry.** The diagnosis below (hardcoded field pool base
0x800bfe68 / stale write-ptr; "362 prims live in the DEMO pool") is WRONG. The OT holds only ~5 prims on the
title (not 362 — that number conflated the `pool` diag's linear-walk counter with OT nodes), and the title's
real failure was 2 background SPRITES rendering BLACK due to scrambled E1-texpage ordering, NOT a different
pool base. Kept for the record; superseded entirely by later-178. Original (wrong) text follows:

The title (DEMO stage 0x801062E4) submits only ~5 prims; its 352-tile background is dropped (confirmed:
`PSXPORT_DEBUG=gpu` shows "5 prims, 39 gp0words" on every title frame). NOT a decode bug (textures are in
VRAM, decode fine). **Root cause: the later-172 packet-pool walk (`GpuState::gpu_dma2_linked_list`,
gpu_native.cpp ~1522) HARDCODES the field/GAME overlay's pool — base `0x800bfe68`, write-ptr `*0x800bf544`.**
The DEMO/title overlay uses its OWN render pool: `PSXPORT_DEBUG=pool` on a title frame shows DrawOTag handed
**madr=0x800EA0D8 with 362 OT nodes** (+ a second madr=0x800EA0A4, 1 node) — the title's 362 prims live in
the DEMO pool around 0x800EA000, NOT the hardcoded field range 0xBFE68..0xC1564. So the pool-walk reads the
wrong (field) pool — which holds only ~5 stale/menu prims on the title — and misses the title's real content.
later-172 was verified ONLY on a field frame, where the hardcoded address is correct, so this regressed every
non-field (2D menu/title) overlay.

**CONFIRMED mechanism = (A): the title's prims are in a DIFFERENT pool; the hardcoded field pool is STALE.**
Proof: on every title frame the field pool write-ptr `*0x800bf544` is **static at 0xC1564** (unchanged frame to
frame) — the title never writes the field pool; 0xC1564 is leftover from when the field last ran. The linear
walk reads [0xBFE68,0xC1564) of stale field bytes (≈5 still-parseable prims after partial overwrite), then sets
s_pool_drawn=0xC1564 and, since phi never moves, draws 0 from the pool on all subsequent frames. The ~5 prims
that DO show on the title are DIRECT GP0 (menu text/char-art), not pool prims. The title's 362 real prims live
in the DEMO overlay's own pool, reachable via the OT root madr=0x800EA0D8 (region 0x800EA0xx).
**FIX — keep later-172's no-OT design (do NOT revert to honoring the guest OT order — boundary directive: the
engine owns ordering).** VERIFY with gfx-debug: `PSXPORT_DEBUG=gpu` title prim count should jump 5 → ~360+, and
the title composites the full TOMBA!2 logo/brick/characters (reference scratch/screenshots/nav3.ppm). This is the
blocker for the user's "export the menu image". The asset pipeline (load/decode/unpack/orchestration) is already
fully owned + offline-reconstructed (later-177) — this is a separate RENDER-submission bug.

## later-176: own the per-frame camera DISTANCE / ZOOM solver `FUN_8006d2ac` native (engine_camera.cpp)
First sub-fn of the camera mode orchestrator `FUN_8006e0f0`. RE'd via `tools/disas.py` (a 13-entry jump table
@0x800168d4 dispatching the TARGET source on `G[+0x164]`, plus a `cam[+0x72]&2` fast path, the look-point math
via libgte rcos/rsin/isqrt/ratan2, and a cam[+0x14] distance smoother that steps ±65536/frame toward ±0x280000
or snaps in the near band). Reimplemented PC-native as `ov_cam_dist_solve`: own the control flow + 32-bit arithmetic
(MIPS mult-lo via `mlo`, arithmetic shifts), CALL the libgte trig via `rec_dispatch`. Writes are MAIN-RAM cam
fields (cam[+0x08]/[+0x10]/[+0x14]/[+0x22]/[+0x58]). Gated by a per-call comparator `ov_cam_dist_solve_verify`
(`PSXPORT_DEBUG=camverify`): snapshot cam+scratchpad, run native, restore, run the recomp oracle (rec_interp),
diff every word of the cam struct. **0-diff over 1800+ calls** on the free-roam MOTION scene with continuous
varied movement (the static idle field is A==B and can't exercise it).

Two bugs caught by the gate, both worth recording:
- **Inverted `blez`**: the far-path `(cur>0?0:cur)−65536` step uses `blez` which BRANCHES (skips the zeroing)
  when cur≤0 — I had zeroed when cur≤0 (backwards). Fixed to zero only when cur>0.
- **Missed branch DELAY SLOT** (the subtle one): the far/near split `beq v0,zero,0x8006d5b8` has delay slot
  `slti v0,angd,2048`, which executes on the TAKEN (near) branch and OVERWRITES v0. So the NEAR-path store test
  `beq v0,zero,0x8006d5c4` is really testing `angd<2048` → when true it falls through to `subu s0,zero,s0`
  (NEGATE). My near path just stored +s0d; it must store `(angd<2048) ? −s0d : s0d`. The static disasm reading
  looked airtight as +s0d, producing a "logically impossible" oracle output. **Diagnosis tool that cracked it:**
  a trig-call SPY (override rsin/rcos/isqrt/ratan2, record fn+args+result for the native run vs the oracle run,
  diff) — it proved s0d was BIT-IDENTICAL between native and oracle, so the divergence had to be in the final
  branch, not the math. Harness self-tested oracle-vs-oracle = 0 first. Lesson: when "same inputs → different
  output" seems impossible, suspect a delay slot that mutates the very register a following branch tests.

NEXT (frontier): remaining `FUN_8006e0f0` sub-fns 0x8006d654, 0x8006c80c, 0x8006dcf4, 0x8006d02c, 0x8006e010,
then collapse the orchestrator. See docs/port-progress.md.

## later-175: own the per-frame camera ROTATION / LOOK-AT builder `FUN_8006e464` native (engine_camera.cpp)
Owned the camera look-at/pitch builder — a big multi-mode function (entry **0x8006e464**, NOT 0x8006e6a8
which is one jump-table case). `ov_cam_rotbuild` (engine/engine_camera.cpp). It picks a camera MODE via two
jump tables (table1 @0x80016994 on `G[+0x164]`; table2 @0x800169cc on `((G[+0x61]&0xff)>>4)-1`; G=0x800e7e80)
plus a `cam[+0x72]&0x40` "mode-A" and a `&2` path, computes a heading ANGLE, then the COMMON look-at tail
@0x8006e7c8: look point = `center(0x1f800160/164) ± rcos/rsin(angle)·radius>>12`; `yaw=ratan2(-dz,dx)`;
`dist=isqrt(dx²+dz²)`; `0x1f8000d0 += rcos(yaw)·dist>>1`, `0x1f8000d8 -= rsin(yaw)·dist>>1`. Owns control
flow + arithmetic native, CALLS libgte rsin/rcos/ratan2/isqrt via rec_dispatch (NOT reproducing their LUTs).
- **Two bugs, both caught by the gate (not by eyeballing):** (1) RADIUS is set as `s2 = -mem_r16(0x1f8000ee)`
  in the **DELAY SLOT @e4d8**, which executes UNCONDITIONALLY before the mode dispatch — I first mis-modeled it
  as an ancestor-supplied live `r[18]` (=1), giving radius 1 instead of ~1748. (2) The recomp **sext16's s2**
  right before each mult; without that, `-ee` read as u16 (63786) gave radius −63786 instead of +1750. Correct:
  `radius = (int16_t)(-(int32_t)mem_r16(0x1f8000ee))`; the e518 path overrides to `(int16_t)(-ee-600)`.
- **GATE — output is SCRATCHPAD (0x1f8000d0/d8), which the main-RAM A/B dump is BLIND to** (the CLAUDE.md
  scratchpad caveat in practice). Built a per-call comparator `PSXPORT_DEBUG=camverify`: snapshot the full
  1KB scratchpad + reg file, run native, capture its writes, restore, run the recomp **oracle**
  (`rec_interp(0x8006e464)`, bypasses the override), compare 0x1f8000d0/d8 + cam[+0x8c]/[+0x66]. Self-tested
  the harness (oracle-vs-oracle = 0 diff) before trusting it. **0 mismatches across 3 scenes** with d0
  ACTIVELY accumulating (continuous-movement walk script — a STOPPED scene is degenerate A==B and a false gate;
  this corrected the handoff's assumption that a plain `AUTO_WALK=r` exercises rotation).
- Diagnostic tap `isqrt(a0)` tagged by run made the divergence obvious: native passed `isqrt(3059017)` (dist
  1748) while the oracle passed `isqrt(0)` (lookpoint exactly on the camera) → pointed straight at the radius.
- NEXT: the per-mode orchestrators `FUN_8006e0f0/e228/e3f4` (call the owned smoothers + this builder).

## later-174: own the per-frame camera X/Z FOLLOW native (engine/engine_camera.cpp) — first verified on motion
Resumed engine porting (user: "go back to porting the game"). Owned the camera-follow's X/Z tracker
`FUN_8006d960` (0x8006d960) PC-native — the per-frame function that rate-limits the camera position toward
the target object. **`ov_cam_track_xz` (engine/engine_camera.cpp), registered in games_tomba2_init.**
- **RE (tools/disas.py 0x8006d960 — disas.py is FUNCTION-scoped, stops at jr ra):** `a1` = camera-target obj
  (=0x800E8010 at runtime). Two axes: X accum `0x1f8000dc` (int `0x1f8000de`) toward `a1[+0]/[+2]`; Z accum
  `0x1f8000e4` (int `0x1f8000e6`) toward `a1[+8]/[+a]`. Each axis: `delta = targetInt − curInt` (low-16
  sign-extended); if `|delta|<=10` SNAP accum=target32, else `accum += clamp(delta,±6144)<<13` via the shared
  rate-limiter `FUN_8006ce74`(delta,maxstep)=clamp(±maxstep). Returns `v0 = 1` iff BOTH axes settled.
- **Reimplemented faithfully (it's a CONTENT interface — the still-PSX cull FUN_8007712c reads the camera
  fields, so it MUST RAM-match, like the terrain content-interface writes later-158).** Not the render path,
  so no "don't transcribe GTE/packets" tension; this is integer camera STATE the cull consumes.
- **VERIFIED mechanically on the free-roam MOTION scene** (`AUTO_SKIP=500 AUTO_WALK=r`, the scene built this
  session; the static idle field is A==B and can't exercise camera motion): **gameplay RAM 0-diff @ f650 AND
  f900** vs the override-OFF build (A/B toggle + PSXPORT_RAMDUMP + cmp -l → 0). And the override FIRES
  (`PSXPORT_DEBUG=cam`: "ov_cam_track_xz FIRED a1=0x800E8010") — per the later-170 lesson, confirmed it's
  actually invoked, not a faithful no-op passing the gate. New diagnostic `PSXPORT_DEBUG=cam` = per-frame
  camera pos + the fire log.
- **Y sibling done too:** `FUN_8006da54` (0x8006da54) = Y-axis smoother (accum 0x1f8000e0, maxstep 5632),
  owned as `ov_cam_track_y`; also FIRES (a1=0x800E8010) + RAM 0-diff @ f650/f900. So camera POSITION (X/Y/Z)
  is now native.
- **NEXT:** the matrix-build fn (the 0x8006e3c4/e89c/e8bc cluster, uses libgte RotMatrix etc. → writes the
  camera ROTATION matrix 0x1f8000f8) — own it the same way to complete the camera-update system. The libgte
  helpers must be reproduced faithfully (the matrix is the render/cull interface). engine_re.md "Camera" map.

## later-173: free-roam nav SOLVED headless (toward verifying the staged ov_game_s4c) — area-exit still open
Pursued the handoff's option 2: drive headless to an in-game AREA transition (`sm[0x4a]==2`) so the staged
`ov_game_s4c` (sm[0x4c] area machine, 0x80106478, later-169) can be A/B-verified and registered. Outcome:
**the free-roam-reachability half is SOLVED; the area-exit half remains open** (it needs vision or deeper RE).
- **Free-roam IS reachable headless (was driving-the-game.md §5's open blocker).** The "auto-appearing menu"
  was OVER-pulled Start: `AUTO_GAMEPLAY` releases input at f328 while the fisherman DIALOG cutscene is still
  up, so Tomba never becomes controllable. Fix = `PSXPORT_AUTO_SKIP=500` (keep pulsing Start THROUGH the
  dialog) then `PSXPORT_AUTO_WALK=<dir>`. Tomba then WALKS — `PSXPORT_DEBUG=nav` (new diagnostic, logs camera
  `_DAT_1f8000d2/d6/da` + pause page `task+0x6B` every 30f) shows holding right pans cam-X 3270→5330, left
  4012→3991. This is a deterministic **free-roam MOTION scene** (the idle field is static A==B; this isn't).
- **Seaside start area geometry:** purely HORIZONTAL (Up/Down = no motion, cam Z fixed 2352); hard walls at
  cam-X ≈ 3991/5330. `PSXPORT_AUTO_JUMP` (pulse Cross = jump while walking, added) doesn't reach the exit.
- **OBSERVABILITY FIX (user pushed on this — "it's bad that you can't tell").** My first nav probe read the
  pause page off `*(0x1f800138)+0x6B` = the CURRENT-task pointer, not the menu task, so its "pausePage" /
  "Cross opens a menu" readings were GARBAGE — a falsified claim. Replaced it with `PSXPORT_DEBUG=state`, which
  dumps all 3 cooperative-task slots (state@+0, entry@+0xc at 0x801fe000+i*0x70) on change. RELIABLE result:
  **NO pause menu ever spawns** (0 menu tasks across AUTO_GAMEPLAY/+SKIP/+JUMP; no new task after f178; s0 =
  GAME stage throughout). **Cross is just JUMP.** Screenshots (PSXPORT_VK_SHOTSEQ) confirm the green-village
  field with Tomba and no menu. Lesson: read state off the RIGHT struct and confirm with a second channel
  (task list + screenshot), don't infer game state from one unvalidated field.
- **Area transition NOT reached.** Walking into either wall keeps `sm[0x4a]==1` (no transition). Added a
  `PSXPORT_DEBUG=stage` log of `sm[0x4a]`/`sm[0x4c]` transitions + an `ov_game_s4c` ENTER log. Confirmed
  **0x80106478 is NEVER entered** on the field NOR during the boot area-load (0 hits) — on the field `sm[0x4c]`
  is driven by the steady handler 0x801088d8, not this machine. So `ov_game_s4c` stays UNREGISTERED (no
  unverified override ships; registration site documents why). Verifying it needs visual steering to the
  seaside exit, or RE of the exit trigger inside 0x801088d8 (GAME.BIN overlay).
- **Net:** diagnostics + the AUTO_SKIP+AUTO_WALK free-roam recipe are durable wins (unblock a class of
  motion-scene verification — e.g. camera-follow / animation ownership). Task #1 (reach sm[0x4a]==2) and #2
  (verify ov_game_s4c) remain open, now precisely characterized.

## later-172: RETIRE the guest OT read — the engine drives rendering from its OWN packet-pool enumeration
Render-queue plan M3 (the "stop reading the PSX ordering table" half). The frame draw no longer reads the
guest OT at all — a PSX render intricacy is gone, the PC way.
- **RE (PSXPORT_DEBUG=poolwalk, retired after):** at a field frame the main DrawOTag walked **2476 OT nodes
  = ~2048 EMPTY ordering buckets + 426 actual prims**. Those same 426 prims live contiguously in the linear
  packet pool [0x800bfe68, *0x800bf544) as tagged nodes ([tag word: GP0-word count in the high byte][len GP0
  words]); a linear pool scan parses EXACTLY to the write-ptr. So the OT was pure PSX-ordering scaffolding
  around prims the pool already holds cleanly.
- **Change (gpu_native.cpp gpu_dma2_linked_list):** replaced the OT next-pointer chain traversal with a
  **packet-pool range walk**. Each DrawOTag draws the pool prims built SINCE the previous DrawOTag —
  `[s_pool_drawn, pool_hi)` (new GpuState member, reset with the pool in native_step_frame) — which partitions
  prims by draw pass WITHOUT reading any OT/next-pointer. Each prim's GP0 words are replayed through gpu_gp0
  exactly as before (same s_cur_node provenance, same RQ_BACKGROUND/WORLD/HUD classification into the engine
  queue, which re-sorts by Layer — so enumeration ORDER is irrelevant). Nothing consults the guest OT (the
  PSXPORT_DEBUG=ot inspector is the only remaining OT read, gated/diagnostic).
- **VERIFIED mechanically (the render is what I ported, so verify the render):** PSXPORT_PRIMDUMP=650 prim
  set is BYTE-IDENTICAL between the OT-walk baseline and the pool-walk — 413 prims (34 poly + 379 sprite),
  0 diff in the sorted (order-independent) prim signatures incl. the bg/layer classification. Run completes,
  field reaches+renders. Render-only change (no guest-RAM writes), so the content interface is untouched.
- **Still PSX (not this milestone):** the guest 2D drawers still BUILD the GP0 packets in the pool (I
  enumerate their output, not the OT). Owning those drawers at SOURCE — render 2D from scene data, not by
  replaying their GP0 — is the remaining M4 work; then the pool read goes too. But the OT-as-source is gone.

## later-171: retire ALL behavior gating; native-GTE replica tried & REVERTED (PSX mimicry) — user directive
User directive (2026-06-20): "no gating; don't worry about breaking; END GOAL = remove the interpreter AND
external deps like Beetle entirely" — BUT "we don't want to MIMIC the PSX, we want to PORT the game to PC"
(so remove Beetle by porting its callers, NOT by reimplementing its hardware).
**VERIFICATION SCOPE (user, 2026-06-20):** NEVER visually verify. Verify MECHANICALLY (RAM/state probes,
oracle diff, gpu_differ) — but ONLY the parts you actually PORTED. Do NOT verify or debug "still PSX"
(un-ported) things: we EXPECT them to not work correctly (e.g. the reported SCEA screen / reversed cutscene
fades are un-ported PSX paths — leave them; they get FIXED BY PORTING, not by debugging the emulated path).
- **De-gated (f30c74b):** games_tomba2_init now registers the native engine path UNCONDITIONALLY — removed
  PSXPORT_FAITHFUL + every *_RECOMP / NO_TICK/CULL/MENU/FLUSH/DISP/WALK/TERRAIN/XFORM / TERRAIN_FAITHFUL /
  RECOMP_OBJWALK opt-out. Deleted the orphaned submit_terrain PSX-transcription oracle. native_depth keeps
  only PSXPORT_FAITHFUL_DEPTH as a depth-diff DIAGNOSTIC. ONE behavior = the PC game. (entity walk
  FUN_8007a904 was already owned — engine_tomba2.cpp ov_objwalk; now ungated.)
- **DEAD END (reverted) — bit-replicating the GTE natively was WRONG (PSX mimicry).** I started a native
  GTE (gte_op_native: NCLIP/AVSZ3/AVSZ4 as faithful transcriptions of gte.c, FLAGS/checksum bit-matched, a
  PSXPORT_GTE_VERIFY shadow-diff harness). The user corrected it: **we don't want to MIMIC the PSX, we want
  to PORT the game to PC.** A native GTE byte-identical to Beetle's gte.c just swaps one PSX-hardware emulator
  for another — it reproduces GTE output, exactly what CLAUDE.md forbids ("do NOT reproduce GTE output", "NO
  gte_op for render", "the engine must NOT deal with PSX intricacies"). It does NOT advance the port. CORRECT
  framing: the GTE/Beetle dependency disappears by **porting its CALLERS to PC-native math**, not by
  re-emulating the coprocessor. Render already does this right (proj_native_vertex — float matrices, real
  depth, the engine's own culling; no gte_op for render); the remaining gte_op calls (incl. NCLIP=backface,
  AVSZ=OT-depth — both PSX render intricacies the PC engine replaces with its own depth buffer + visibility)
  go away as those callers are ported. The GTE stays as the retained PSX-content math service (collision/
  physics) until that content is ported. REVERTED gte_op back to GTE_Instruction. **Lesson: "remove Beetle"
  ≠ "reimplement Beetle's hardware" — it means port the callers so the hardware emulation is never invoked.**

## later-170: own the cooperative STAGE TRANSITION `FUN_80052078` native (engine/engine_level.cpp)
Owned the stage/area transition primitive `FUN_80052078(stageIdx)` (MAIN.EXE 0x80052078) PC-native — the
engine function that loads the next stage's overlay and RESTARTS task 0 at its new entry. Frontier moves
from "the transition runs as interpreted guest code" to "the engine owns the stage handoff; PSX drives only
the overlay/level CONTENT it loads."
- **RE (tools/disas.py 0x80052078):** prologue `sp-=0x18; sw s0,0x10(sp); sw ra,0x14(sp)`; `a1=stageIdx`;
  `a0=mem[0x1f800138]+0xc` (task's entry-pointer slot); `jal 0x800450bc` = eng_load_stage(task+0xc, stage)
  (already native, later-162); then `task[0]=3` (RESTART at new entry) + `task[0x6f]=0`; then the THREAD
  PLUMBING `EnterCriticalSection(0x80080890) / CloseThread(task[4],0x80080870) / ExitCriticalSection
  (0x800808a0) / ChangeThread(0xff000000, 0x80080880)`; epilogue `lw ra; lw s0; jr ra; sp+=0x18`.
- **Reimplemented, NOT transcribed:** the four thread/critical-section calls only stop+switch the task;
  the native cooperative scheduler (native_scheduler_step) replaces all of it — **setting task state=3 IS
  the transition** ("restart fresh at the new entry") and the terminal `ChangeThread` IS the existing
  native yield primitive `ov_switch` (0x80080880). The thread calls are RAM- and IRQ-flag-NEUTRAL in the
  native model (Enter+Exit cancel; Close/Change only set v0), so dropping them is faithful to the content
  interface. So `eng_stage_transition` = mirror prologue stack writes (byte-faithful) → eng_load_stage →
  task[0]=3, task[0x6f]=0 → restore s0/sp/ra → `rec_dispatch(0x80080880)` (ov_switch).
- **TERMINAL yield, so NO coro-redirect handshake needed** (unlike the RUNNING dispatcher, later-169). The
  task never resumes into this fn — it restarts at state 3 — so a plain override that does the work then
  ends the run is correct. `ov_switch` matches the PSX per context: mid-game (in a task run, in_stage==1)
  it longjmps to the scheduler and the fn never returns (== ChangeThread suspending the task); at BOOT the
  START→DEMO→GAME init transitions run via `rc0(FUN_800499e8)` with in_stage==0, where ov_switch is a
  no-op return and the fn returns to FUN_800499e8, which continues — exactly as the stubbed thread layer
  did. Either way the scheduler then restarts task 0 (state 3) at the new stage entry.
- **VERIFIED:** registered in games_tomba2_init (`!faith`, next to ov_load_stage). The field reaches GAME
  (frame 50 stage=GAME) and runs stable to f1000+ with task 0 alive — which REQUIRES the START→DEMO→GAME
  transitions to all run through this override. **Gameplay RAM 0-diff @ f650 AND f1000** vs the pre-change
  (interpreted-FUN_80052078) build (git-stash A/B + `PSXPORT_RAMDUMP_FRAME` + `cmp -l` → 0).
- **ALSO owned task-0 INITIAL ENTRY `FUN_800499e8` (0x800499e8) native (`ov_task0_boot`, same file).** The
  engine's first-level bootstrap: resolve `\BIN\START.BIN;1` (name@0x80015458) via the CD directory
  (FUN_8008b8f0 — platform, called not reimplemented), decode MSF→LBA (FUN_8008a110), write stage[0]
  (LBA,size) to 0x800be1e0/e4, then `FUN_80052078(0)` (the native transition). Called once at boot via
  rc0(FUN_800499e8), in_stage==0 (FUN_80052078's terminal yield no-op returns there). **VERIFIED RAM 0-diff
  @ f650 AND f1000** vs HEAD (interpreted-FUN_800499e8). Two stack-faithfulness fixes mattered (the gate
  caught both as 20-byte task-stack diffs): (1) set ra to each `jal`'s post-link addr (jal+8) before each
  `rec_dispatch` so callees save the same ra to their frames; (2) the prologue `sw s0,0x28(sp)` saves the
  INCOMING s0 BEFORE `s0=name` — saving `name` instead leaked the name ptr into 5 task-stack frames.
- **ALSO owned the GAME stage TOP-LEVEL ENTRY PROLOGUE `0x8010637C` native (`ov_game_stage_main`,
  engine_stage.cpp).** Task-0's stage driver = a one-time prologue then `{dispatch sm[0x48]; bump
  DAT_1f800198; yield}` forever. The prologue is pure register/memory init of the stage SM (no yield, no
  jal): set s0=s1=0x1f800000, s2=1, sp-=0x20, save incoming s0/s1/s2/ra to the stack, reset
  0x1f800206/236/234/19a/198 + 0x800be0e4, set `sm[0x48]=mem_r8(0x1f800134)` + sm[0x4a..0x50]=0, then
  `rec_coro_redirect(0x801063F4)` into the guest loop body IN-CONTEXT (deep yields unchanged). Registered
  AUTO in stage_scan_overlay. **VERIFIED RAM 0-diff @ f650 AND f1000.** This is the entry point for owning
  the loop ITSELF; the loop body + handler dispatch (all 3 handlers already native) + the FUN_80051f80 yield
  still run as the resumed task-0 coroutine.
- **FIX — the prologue override was DEAD; made it LIVE via task-entry-override firing.** Caught by re-
  examining the mechanism: `native_scheduler_step` enters a fresh task via `rec_coro_run(c, entry)` →
  `interp_flat` starts interpreting AT the entry pc, and interp_flat only fires overrides on a control
  transfer INTO an address (never at its start pc), and nothing `jal`s a task entry — so `ov_game_stage_main`
  (registered at the task entry 0x8010637C) NEVER fired. The 0-diff "verification" was fooled by the
  override being FAITHFUL (interpreted prologue == native prologue). FIX (native_boot.cpp): on a FRESH entry
  (not a resume — a resume's saved pc is deep guest code that must be interpreted), look up the entry
  override and invoke it as a native call, then continue interp_flat at its `coro_redirect_pc` (or ra). Now
  `ov_game_stage_main` ACTUALLY runs (confirmed: `PSXPORT_DEBUG=stage` prints "ov_game_stage_main: prologue
  run") AND RAM 0-diff @ f650/f1000 holds. This is also the general mechanism for owning ANY task entry.
  LESSON: a faithful override registered where it's never invoked passes the A/B gate while doing nothing —
  always confirm the override FIRES (debug log / a deliberate divergence), not just that RAM matches.
- **NEXT:** (a) Own the loop BODY/yield (native_scheduler_step "one stage-iter per frame") — the real
  obstacle is the byte-faithful content-interface gate: the handlers' saved ra/stack are tied to the guest
  loop trampoline, and deep-yield resume needs interp_flat. Delicate core-scheduler change; A/B-gate every
  step. (b) Drive a deterministic IN-GAME area transition → register + verify the staged `ov_game_s4c`
  (sm[0x4c] area machine) — BLOCKED on reaching free-roam headless (driving-the-game.md §5).

## later-169: cooperative-yield handshake (`rec_coro_redirect`) — own the GAME RUNNING dispatcher native
Built the handshake later-168 flagged as the gating blocker, and used it to OWN the GAME running dispatcher
`sm[0x48]==2` (0x80108784). The frontier moves from "all sub-handlers run as guest code" to "the engine owns
the running sub-mode selection natively, content runs in-context."
- **The KEY structural fact (re-RE):** GAME.BIN holds exactly ONE `jal FUN_80051f80` (the yield), @0x80106468
  in the TOP-LEVEL loop (0x8010637C), AFTER the `sm[0x48]` dispatch returns. The `sm[0x4a]`/`sm[0x4c]`
  sub-handlers do NOT yield in the overlay — but they call RESIDENT MAIN.EXE fns (`0x8007xxxx`) that yield
  deep (asset-load waits across frames). So the running dispatcher's callee CAN yield deep, which is exactly
  why a `rec_dispatch` override killed task 0 (later-168): the nested `rec_interp`+`CORO_SENTINEL` C frame is
  destroyed by the deep yield's longjmp, so the resume mis-reads the return as task-end (st=2→0 @f53).
- **The fix — `rec_coro_redirect(c, target)` (core.h `Core::coro_redirect_pc`, interp.cpp):** a native
  override does its work, sets `coro_redirect_pc`, and RETURNS; the flat interp's next control transfer then
  jumps to `target` IN-CONTEXT (same `interp_flat` / task run) instead of to `r[31]`. No nested interpreter,
  no second sentinel — so a deep yield inside `target` longjmps to the scheduler and resumes correctly, and
  `target` eventually returns up the guest chain. Consumed-and-cleared at the transfer (`coro_next_pc`), so it
  never leaks. This is the GENERAL cooperative-yield handshake (also unlocks FUN_80052078/FUN_800499e8).
- **`ov_game_s48_2` (engine/engine_stage.cpp), now registered:** native prologue (`sp-=0x18; sw ra`), read
  `sm[0x4a]`, select the handler from the engine's 6-entry table, set `ra = 0x801087CC + s4a*0x10` (the guest
  `j 0x8010881c` trampoline — byte-identical saved-ra so the A/B gate stays clean), and
  `rec_coro_redirect` to the handler. The handler runs in-context (deep yields fine) and returns through the
  guest epilogue → the task loop, exactly as the PSX path.
- **VERIFIED:** field reached at f328 with task 0 ALIVE (st=2, s48=2 running) and stable through f1000+ (vs
  task 0 dying @f53 with the naive override). **Gameplay RAM 0-diff @ f650 AND f1000** vs the pre-change
  (guest-interpreted) build (`PSXPORT_RAMDUMP_FRAME` + `cmp -l` → 0). Build deterministic.
- **Staged (not registered): `ov_game_s4c`** — the sm[0x4c] AREA machine (0x80106478, 9-state load/intro/
  play). RE'd + implemented with the same handshake (native prologue + synchronous `jal 0x80075a80` —
  verified yield-free by a 57-fn transitive jal-graph scan — + 9-way state select + redirect). BUT it is
  reached only via sm[0x4a]==2 (area LOAD/TRANSITION); the headless idle field runs entirely in sm[0x4a]
  0/1 (steady play: handler 0x801088d8 does NOT call 0x80106478), so s4a==2 is never exercised and the
  A/B gate can't verify it. Left unregistered (no unverified behavior ships) until a deterministic area-
  transition test path exists.
- **NEXT:** (a) get a deterministic area-transition headless path (drive Tomba through an area boundary, or
  own FUN_80052078 to script one) → then register+verify `ov_game_s4c`. (b) Own the cooperative stage-
  transition `FUN_80052078` + file-resolve `FUN_800499e8` with the handshake (engine_re init table flags
  both). (c) Push toward owning the top-level loop / yield itself (native_scheduler_step "one iter/frame").

## later-168: GAME stage state machine — RE'd in full; own area-INIT native; running loop BLOCKED by yield
Pushing the relentless-ownership frontier into a NON-render engine system (handoff's default target = the
GAME stage state machine, the per-area scene/update driver). Full RE + a verified partial port + a decisive
architectural finding.
- **RE'd the whole GAME-stage machine** (overlay `\BIN\GAME.BIN` @0x80106228, task-0 entry 0x8010637C; map
  in engine_re.md "GAME stage state machine"). It's a 3-level nested SM keyed on task-obj fields
  `sm[0x48]` (top: 0=INIT/1=RESUME-INIT/2=RUNNING) → `sm[0x4a]` (6-way running sub-mode) → `sm[0x4c]`
  (9-state area load/intro/play machine). The entry is a COOPERATIVE TASK: infinite loop, one
  `FUN_80051f80(1)` yield per frame.
- **OWNED native (engine/engine_stage.cpp):** the two area-INIT handlers `sm[0x48]==0` (0x801086e0) and
  `==1` (0x80108720), registered AUTO when GAME.BIN loads (signature scan @ the overlay-load hook, mirrors
  the M3 submit/tilemap scan). Each mirrors the original's exact guest writes + dispatches its SYNCHRONOUS
  resident setup fns (FUN_8007a8e0/b38c/b3f4). **Verified: gameplay RAM 0-diff @ field f650 AND f1000** vs
  the pre-change build; build deterministic (0-diff across runs); the field reaches/runs normally.
- **DECISIVE FINDING — the RUNNING loop CANNOT be owned by override+rec_dispatch (cooperative-yield
  blocker).** First attempt owned all three sm[0x48] handlers incl. the RUNNING dispatcher 0x80108784,
  which dispatches the sm[0x4a]/sm[0x4c] sub-handlers. Result: 675k-byte RAM divergence @f650. Localized by
  bracketing dumps (f80=0, f100=8473 root, f140=671k explosion) + a per-frame task-state trace
  (`PSXPORT_DEBUG=schedf`, new diagnostic): **task 0 dies (st=2→0) at f53** in the override build, stays
  alive in baseline. ROOT CAUSE: the area sub-handlers YIELD; `rec_dispatch` runs them in a NESTED
  `rec_interp` with its own `CORO_SENTINEL`, so when a deep yield longjmps to the scheduler and the task
  resumes, the PSX return chain unwinds back to that nested sentinel → `rec_coro_run` reads it as "the task
  returned" → marks task 0 free → the game runs with NO stage driver → divergence snowballs. This is the
  same cooperative-scheduler-longjmp handshake the engine_re init table flags for
  FUN_80052078/FUN_800499e8. Fix = drop 0x80108784 from the scan (kept in-code as `ov_game_s48_2`, the
  next-step reference with the correct sub-handler table, intentionally unregistered).
- **NEXT (gating piece):** build the cooperative-yield handshake — a native-override path that survives a
  deep yield/resume (so a native stage handler can call a yielding sub-handler without the nested-sentinel
  task-death). That unlocks native ownership of the GAME running loop AND of FUN_80052078/FUN_800499e8.
  Tooling added this session: `PSXPORT_DEBUG=schedf` (per-frame task0/1/2 state + sm[0x48/4a/4c/5c] trace),
  `PSXPORT_DEBUG=stage` (GAME-stage ownership log). A/B gate recipe: build with/without, dump RAM @ a fixed
  field frame (`PSXPORT_RAMDUMP_FRAME` + `PSXPORT_RAMDUMP`), `cmp -l` (0 = good); bracket frames to localize.

## later-167: M3 — own the 2D BACKGROUND layer by PROVENANCE (replaces the bg_2d coverage guess)
Implemented the M3 layering fix later-166b called for: classify the field's tiled backdrop by WHO drew it,
not per-prim size. RE first (field, `PSXPORT_AUTO_GAMEPLAY`, f650, RAM dump `PSXPORT_RAMDUMP_FRAME`):
- **The 352 op-7C tiles are the field BACKDROP, proven two independent ways.** (1) Provenance: a `PSXPORT_WWATCH`
  on the first tile packet (pool node 0x800BFF30) pinned the writer to an interpreted overlay drawer; disasm
  of the field RAM dump decoded it as a scrolling-tilemap renderer **FUN_80115598** (reads cols/rows@+16/+17,
  scroll@+40/+42, U-base@+6, clut@+4, mapdata ptr@+20; builds 16×16 textured-rect packets into the pool).
  (2) Draw order: running the full guest OT (all ownership reverted) + `PSXPORT_POLYDUMP=650` shows the 352
  op-7C tiles are drawn FIRST (prims 1–352), THEN world geom (op 3C/34/3E), THEN HUD sprites/text (op 65 at
  font texpage, last) — i.e. tiles are painted behind the world. So tilemap = backdrop is sound, not a guess.
- **Mechanism (engine_submit.cpp `ov_bg_tilemap` + `engine_scan_overlay`):** the backdrop drawer's signature
  is the unique tile command-word build `lui a1,0x7d80 ; ori a1,a1,0x8080` (1 occurrence in 2 MB). The
  scan-on-load registers a BRACKET override at each matching entry (a field has SEVERAL parallax layers —
  found two: 0x8010C26C + 0x80115598). The override reads the packet-pool ptr (*0x800BF544) before/after
  super-calling the real body, and records the produced pool span as RQ_BACKGROUND
  (`gpu_bg_range_add`→`GpuState::node_is_bg`). The OT-walk 2D classifier is now
  `node_is_bg(node) || bg_2d(...)` — provenance wins (any tile size), coverage stays only as the fallback
  for scenes whose backdrop drawer isn't owned yet (menus/title full-screen sprite). To super-call the EXACT
  intercepted entry from one shared override fn, the interp now exposes `g_override_tgt` (the override target
  addr, set right before the override runs).
- **VERIFIED:** attribution — all 352 tiles classify RQ_BACKGROUND, the 26 op-65 + 23 op-2D + 11 op-2F HUD/
  text stay RQ_HUD (`PSXPORT_PRIMDUMP=650` histogram). Content interface — gameplay **RAM 0-diff @ f650**
  vs the pre-change build (the override only super-calls the original body + host-side bookkeeping; no guest
  write changes). Visual = user (`./run.sh`): backdrop behind world, HUD on top.
- **STILL OPEN (M3 tail / M4):** this owns the LAYER decision; the backdrop geometry still flows through the
  guest drawer + the OT walk. Retiring the OT walk as the frame driver + a native background renderer (push
  tiles straight to RQ_BACKGROUND, no guest packet) + deleting `bg_2d` is the remaining step. The HUD/text
  (op 65/2D/2F) are still classified by the bg_2d fallback (all correctly HUD here, but own them at source
  next to finish M3).

  **Two follow-on approaches RULED OUT this session (record so they're not re-walked):**
  1. **Native transcription of the tilemap drawer = NO.** The per-drawer screen-centering constants are
     baked as code IMMEDIATES, not in the descriptor: drawer 0x80115598 centres on (160,120), the 2nd
     parallax drawer 0x8010C26C on (144,120) (captured live: scrollX=0 → tile0 X=8 ⇒ centre 144). A native
     reimpl would have to import per-overlay magic constants — banned, and it's pure-mechanism transcription
     the engine rule forbids. (Descriptor decode IS in later-167/engine_re if ever needed: cols@+0x10
     rows@+0x11 scroll@+0x28/+0x2a clutBase@+0x06 mapdata@+0x14; tile = 16×16, U=(idx<<4)&0xF0
     V=(idx&0xF0)+8 clut=clutBase+((idx&0xF00)>>2).)
  2. **Per-drawer provenance for HUD/text = impractical.** Unlike the single backdrop drawer, the field HUD
     has ~10+ heterogeneous sources (writer-trace via PSXPORT_WWATCH on HUD nodes): resident font 0x8007E8xx,
     overlay UI 0x8013DFxx / 0x8010C4xx, item icons 0x8002668C, + the AddPrim linkers 0x8003CBF0/CC04/CC10
     and sprite linker 0x80078F28/3C/48. Bracketing each is a sprawl with no payoff (HUD already classifies
     correctly via the fallback).
  3. **AddPrim-chokepoint capture = ALSO NO (decisive).** There is NO generic AddPrim function to hook —
     linking is INLINE in every drawer. 0x8003CBF0 disassembles as mid-function inline prim emission:
     `a0=*0x800bf544 (pool); a1=*0x800ed8c8 + slot*4 (OT entry); *(a0)=*(a1)|0x09000000; *(a1)=a0` then
     copies the prim's words from the builder into the pool. The `|0x0900` (POLY_GT3 tag len) is baked per
     site; the sprite/quad/tile drawers each have their own inline copy+link. So "the linker" is a code
     PATTERN scattered across all drawers, not a chokepoint. Trapping it everywhere is far more invasive
     than any benefit.

  **CONCLUSION (evidence-backed boundary): the OT-walk-as-ENUMERATOR is the right architecture; keep it.**
  Draw-ORDER authority — the directive's actual concern ("engine decides what occludes what, not the PSX
  OT") — is ALREADY owned: the render queue re-sorts by (engine layer, then submission seq) and the OT's
  chain order is DISCARDED (plan M1 said the OT is read "only to enumerate leftover guest prims; its order
  is discarded"). `gpu_dma2_linked_list` now serves solely as the convergence point that ENUMERATES the
  engine-built 2D packets (it's also where env E1/E3 texpage/clut/draw-area state is sequenced into each
  prim's queue snapshot). Removing that read would require trapping inline link-emission in every drawer for
  ZERO observable change (order already re-decided, layers already provenance/coverage-classified). That is
  cost without benefit — explicitly the kind of mechanism-shuffle to avoid. So M3's ordering goal is met;
  the residual OT *enumeration* stays as a benign engine mechanism, NOT a PSX ordering decision. Further
  high-value "PSX only drives content" ownership lies in NON-render engine systems (stage state machine,
  main loop, menus, asset/level loading), which are RAM-gateable against the oracle — that's the frontier
  to push next, not the OT enumeration.

## later-166b: M3 root finding — the bg_2d coverage heuristic is blind to TILED backgrounds (provenance needed)
Scoping M3 (own the 2D layer at source). Dumped the green-field 2D prims (`PSXPORT_PRIMDUMP=650`,
present f650, `PSXPORT_AUTO_GAMEPLAY`). The field background is **NOT one full-screen sprite** — it is a
**grid of 352 op-0x7C 16×16 textured tiles** (texpage `tp=(896,0)` clut=(1008,250), stepping 16px in x/y
from (-20,-13), filling the screen). The `bg_2d` classifier (gpu_native.cpp:55) calls a prim a backdrop
only if it covers ≥¾ of the display in BOTH axes — so every 16×16 tile is below threshold → classified
**HUD (bg=0)**. CSV histogram for f650: 352 sprite/7C bg=0, 26 sprite/65 bg=0, 23 poly/2D bg=0, 11 poly/2F
bg=0, 1 sprite/62 bg=1(semi). i.e. essentially the ENTIRE 2D layer (incl. the tiled background) lands in
the HUD/near band, over the world. The coverage heuristic only ever works for a SINGLE full-screen backdrop
sprite; it cannot see a tiled one.

This is the structural reason M3 must classify by PROVENANCE, not coverage: the engine has to know "these
tiles are the background drawer's output" from WHERE they were submitted, not from how big each one is.
NEXT unit = tap the 2D submit SOURCE (the tiled-background drawer + the HUD/text drawer) and tag each with
an explicit RQ layer, replacing `bg_2d`. NOTE: `PSXPORT_POLYDUMP`'s `node=` for these is the GPU
packet-pool address (0x800BFxxx, +0x10/prim), NOT the source entity/handler — provenance needs a tap at
the draw call, not the packet. CAUTION (not yet verified visually, user's call): whether this tiled layer
currently renders correctly is unknown — do NOT blind-flip it to BACKGROUND, since if the tilemap is meant
ON TOP forcing it behind would hide it. Get the user's visual ground truth (or a source tap that proves
which layer it IS) before changing the layering.

## later-166: state reconciliation — field WORLD geometry is 100% engine-owned; M4 effectively done, M3 is what's left
The handoff (`scratch/handoff.md`) and the committed docs (de8ea8e `engine_re.md`, journal later-165)
named **`0x80027768` as "the next ownership target"** and claimed **"~70% of field world polys carry no
native depth."** Both are STALE — the same class of stale-handoff error later-165 caught for M2. Verified,
not assumed (grep + runtime measurement at the green field, `PSXPORT_AUTO_GAMEPLAY`, present f650):

- **`0x80027768` is already owned + committed** — `ov_submit_poly_gt4_bp` (engine/engine_submit.cpp:315),
  registered `game_tomba2.cpp:424`. A prior session re-RE'd an already-owned function and wrote it up as
  open work (lost-track-of-existing-work, the exact failure CLAUDE.md warns about).
- **The dominant field submitter is already native via the scan.** `PSXPORT_DEBUG=pdisp` shows 59/76
  dispatches/frame are dispatcher **mode 0** → overlay renderer `0x80146478`. That looks like a "fallback,"
  but `PSXPORT_DEBUG=submit` shows the scan-on-load owns it: "own overlay CALLER @ 0x80146478", and the
  GT3/GT4 submitters it calls (`0x801465EC`/`0x801467BC`) are scan-owned native → push to the render queue
  as `RQ_WORLD` with real depth. pdisp counts the dispatch *wrapper*, not the inner submit = a red herring.
- **Only 2D leaks through the OT walk now.** `PSXPORT_DEBUG=ndepth` op-histogram at the field: the sole
  OT-walk leftover is **op 0x2D/0x2F flat-textured quads** (~34/frame). `PSXPORT_POLYDUMP=650`: they are
  small (8–12px) axis-aligned screen-space rects at the font/UI texpage `tp=(960,256)` with per-glyph
  colors = **HUD text/UI**, genuinely 2D. The gouraud world ops (0x3C/0x34) the stale note listed as leaking
  are GONE (owned). So **M4 — "100% world geometry carries real depth" — is effectively achieved for this
  scene.** No more world submit VARIANTS to own here.
- **Retired metrics warning:** `ndepth` real-depth/2D counters and the value-keyed `projprim` depth ring
  measure the OLD attach path the render queue (later-164) superseded; they read ~0 even when world depth is
  correct. Don't gate on them — measure the queue.

**What's actually left = M3:** the leftover 2D (HUD/text FT4 quads + the screen-space background/water
sprites) still reach the renderer via `gpu_dma2_linked_list` walking the guest OT, and their BACKGROUND-vs-
HUD layer is chosen by the `bg_2d` screen-coverage heuristic (gpu_native.cpp:48), not by what the prim IS.
Full ownership = capture those 2D submits at their SOURCE with an explicit engine layer, stop driving the
frame from the OT walk, delete `bg_2d`. Docs corrected (engine_re.md OPEN section). No code change this
entry — a measurement/reconciliation milestone so the next unit (M3) starts from the true state.

## 2026-06-17 — chan4 area music "loops ~18 s early" FIXED (spurious interleaved EOF)
User report: the gameplay area music after the fisherman cutscene (chan4 XA, clip [84515..97979]
loop=1, = 89.76 s) loops noticeably earlier than it should (~90 s expected). RE'd via offline +
runtime diagnostics:
- **Data/loop point is CORRECT.** Standalone `tools/xa_wavdump` (no game boot; opens the CHD and
  decodes any chan/[start..end] with the same xa_decode_sector as the port) renders the chan4 song:
  89.76 s to end 97979, and the user confirmed ~90 s is right. So NOT an end_lba/data error.
- **Steady-state audio is realtime** (PSXPORT_AUDIO_RATE meter: ~44100 samples/s, no sustained queue
  drops). So NOT a global playback-rate drift either.
- **ROOT CAUSE (runtime):** `xa_decode_next_sector` terminated the stream on ANY EOF submode bit
  (raw[18] & 0x80), including EOF on a NON-matching interleaved sector. The [84515..97979] range has
  a non-audio sector at **LBA 95338** carrying EOF (an unrelated file/channel's end-of-file). That
  killed the chan4 music ~18 s early (95338-84515 = 10823 of 13464 sectors ~= 72 s), and the
  dialog-coord resume (`cd_override.c` `xa_dialog_coord`) restarted it from the top -> "loops early".
  Caught live: a repeating `[xa] EOF (non-audio) @ LBA 95338` -> `xa_active=0` -> `[xa] PLAY clip chan=4`
  cycle in the field.
- **FIX (`xa_stream.c`):** a BOUNDED clip (`s_end_lba != 0`) ends strictly at `end_lba` (already the
  `s_lba > s_end_lba` loop) - EOF markers from other interleaved channels inside the range are ignored.
  EOF still terminates an OPEN-ENDED stream (`s_end_lba == 0`); `xa_stream_start` now clears
  `s_end_lba`/`s_loop` so a stale clip end can't make an open-ended stream look bounded.
- **VERIFIED:** runtime now logs `[xa] LOOP chan=4 ... span=13464 sectors` (full clip), no 95338 stop;
  a captured field SPU WAV autocorrelates to a **90.02 s** loop period (was ~72 s).
- **Tooling added (reusable):** `tools/xa_wavdump.c` (+`build_xa_wavdump.sh`) offline XA extractor;
  `PSXPORT_AUTO_GAMEPLAY=1` state-gated navigator in native_boot.c (pulses **Start** - the fisherman
  dialog cutscene advances on Start edges and otherwise hard-stalls headless at sm[4e=9]; releases
  input once chan4 area music loops continuously = field) + `pad_repl_release()`; `[xa] LOOP` log;
  `PSXPORT_AUDIO_RATE` production-rate meter.

## 2026-06-17 (later-88) — HW renderer M0–M2 + M2b: Vulkan present + GPU VRAM image + triangle rasterizer; finding: Tomba2 is texture-dominated.
Built the Vulkan/MoltenVK renderer foundation (approved plan; runtime/recomp/gpu_gpu.c, PSXPORT_VK=1):
- **M0**: SDL_Vulkan swapchain + fullscreen-quad present (SW still rasterizes s_vram, VK presents it).
- **M1**: VRAM as a device image (R16_UINT 1024x512 = 1555); present samples it + unpacks in-shader.
- **M2**: GPU triangle pipeline (flat/gouraud) drawing into the VRAM image; readback. Self-test
  PSXPORT_VK_TRITEST=1 PASSES (flat fill 0x001f exact, gouraud interpolated, clear correct).
- **M2b**: tee UNTEXTURED polys from gp0_exec into gpu_gpu_draw_tri (absolute VRAM coords) + a per-frame
  VK-vs-SW diff (PSXPORT_VK_DIFF=frame: upload SW VRAM as bg, draw VK tris on top, readback, count
  mismatches, dump scratch/screenshots/vk_diff.ppm).
- **FINDING:** gameplay frames have **zero untextured polys** (f2500/f4500 both "no untextured tris").
  Tomba2 draws virtually everything TEXTURED. So M2 coverage on this game is ~nil — the untextured
  rasterizer is a validated foundation, but **M3 (textured tris + CLUT-in-shader + semi-transparency)
  is the milestone that actually renders Tomba2**. Pivot the renderer effort there next.
- Verified: builds/link with -lvulkan; headless unaffected (VK only with a window); windowed VK up
  (1280x720, RADV); present + self-test + diff-harness all run clean. SW path + default untouched.

## 2026-06-16 (later-87) — ROOT-CAUSED the WIDE60 "tripled weapon icon": the re-rasterized in-between is LOSSY. Interim fix: present the real frame when not interpolating.
User: the tripling happens ONLY with PSXPORT_WIDE60=1 (the port), and is the only thing broken.
Diagnosed via the offline A/in-between/B dump at the field scene (f4720, spiky-ball enemies in water):
- **A (prev real) = 1 ball, B (cur real) = 1 ball, but the in-between (s_interp) = 3 balls.** Prim count
  ~1325 = one frame (NOT multi-frame accumulation). Ball-region prims are all `obj=0` (untagged, per
  later-86), so the synth snaps everything — yet the in-between still differs from VRAM.
- **ROOT CAUSE:** `wide60_synthesize` rebuilds the in-between by re-rasterizing only the captured GP0
  poly/sprite/line subset into a cleared buffer. It does NOT reproduce occluders the real frame uses
  (semi-transparent water over the balls, fills, uncaptured ops, exact blend/draw-order/mask), so
  objects that are hidden/dimmed in the true frame REAPPEAR in the in-between → spurious extra copies.
  The "capture display list + re-rasterize" approach is fundamentally lossy. (Generalizes the later-81
  "uncaptured prim types" caveat.) The correct fix is the native renderer (own every draw + occlusion).
- **INTERIM FIX (engine/wide60.c):** `wide60_synthesize` returns the #prims actually interpolated;
  `wide60_present` blits the LOSSY s_interp only when that's >0, else presents the REAL current frame
  (back buffer A, then real front B). Since interpolation is blocked (obj tags unavailable → moved=0
  always), WIDE60 now shows real frames = NO tripling, at the cost of no true in-between until the
  native renderer supplies object-tagged draws. Headless unaffected (faithful branch). NEEDS live check.

## 2026-06-16 (later-86) — Phase 4 object interpolation: reworked to node-pointer identity; BLOCKED by decoupled render pass (key finding). Falls back to safe all-snap.
Replaced wide60's GTE-fingerprint matcher with **per-object 2D screen-translation keyed by the entity's
pool-slot pointer** (the stable identity from later-84): each captured poly tagged with `Prim.obj`
(= `g_current_object`), per-object screen centroid matched across frames by pointer, prims translated to
the midpoint (`engine/wide60.c` ocen_* + native-walk `call_handler` sets g_current_object=node around
each handler). The translation math is correct and can't smear/dupe (rigid per-object move, gated).
- **BLOCKED (measured, decisive):** `rtp/frame ≈ 3268` but **`rtp_with_obj = 0`** — the render-time GTE
  RTPS fire in a **separate pass with NO object context**; neither the cull dispatcher (LOD math only,
  no RTPS) nor the `FUN_8007a904` handler walk brackets the drawn geometry. So `Prim.obj` stays 0 →
  everything snaps. (Generalizes later-82's "g_current_object stays 0".) Current behavior: clean
  all-snap (no smear/flicker/dupes; 60fps cadence shows each real frame ~twice = judder, not true
  in-between). Opt-in (PSXPORT_WIDE60); default faithful path untouched.
- **UNBLOCK paths:** (a) RE the render pass and set object context where per-object RTPS happens; or
  (b) **get object-tagged draws from the planned native (VK/MoltenVK) renderer** — it knows which entity
  each draw belongs to, so interpolation falls out. (b) is cleaner and aligns with the next track.
- grid_get sentinel fixed (was returning 0xFFFFFFFF for obj-0 cells, which masked this in the old
  approach — "join 92-99%" was joining everything to one blob). Now returns 0 → honest snap.

## 2026-06-16 (later-85) — Phase 1: native entity-list walk LANDED (FUN_8007a904), default-on, oracle bit-identical.
First native engine-layer function: `engine/engine_tomba2.c` reimplements the per-frame object driver
`FUN_8007a904` in native C — walks both entity lists (heads 0x800fb168/0x800f2624) via next@+0x24,
clears render_flag@+1, calls each node's handler@+0x1c via `rec_dispatch` (gameplay STAYS PSX). `next`
read before the handler runs (handler may unlink the node) and held in a host local. Second list head
re-read fresh after list 1 (matches the recomp reload).
- **Verified faithful:** VRAM bit-identical (1 MB `cmp` PASS) native-walk vs recomp body at frames 4000
  AND 4720 of real gameplay; default(native)==recomp-fallback at f4000. Visits 110-157 nodes/frame.
- **Default-on** (the native engine owns the walk); `PSXPORT_RECOMP_OBJWALK=1` restores the recomp body
  as the oracle. `PSXPORT_ENGINE_DBG=1` logs node counts. This is the seam the user wanted: native
  engine driving PSX gameplay handlers in guest memory. Next: snapshot per-node pos for interpolation
  (the obj-pointer-keyed approach from later-84), then native cull/render submission.

## 2026-06-16 (later-84) — DIRECTION: Tomba2Engine native-engine port. Entity list RE'd + runtime-validated. Repo reorg + 72 GB cleanup.
User redirected the project: reimplement Tomba!2's **engine layer** in native C (gameplay logic STAYS
recompiled PSX in guest memory; the native engine reads entity structs from guest RAM). Repo reframed
as **Tomba2Engine**; N64Recomp-style framework/game split, boundary-first in-tree (engine/ = game,
runtime/recomp/ = common PSX platform → future psxport submodule). See CLAUDE.md, docs/engine_re.md,
plan <local-notes>/plans/fancy-tinkering-kite.md, memory [[tomba2engine-native-engine-pivot]].

### Engine RE — the entity system (Phase 0 done)
- **Entity list = two doubly-linked lists** of 0xD0 pool nodes; heads `DAT_800fb168`/`DAT_800f2624`.
  Node: render_flag@+1, state@+4, type@+0xc, **handler fn-ptr@+0x1c**, prev@+0x20, **next@+0x24**,
  pos@+0x2e/32/36. Walk = **`FUN_8007a904`**: per list, follow next@+0x24, clear +1, call handler@+0x1c(node).
  Found by searching gameplay RAM dumps for the handler address bytes (handlers are per-object fn ptrs,
  not a type-indexed table — that's why static xref found nothing).
- **Runtime-validated** (`PSXPORT_OBJLOG=1`, field scene): the cull/object path FIRES in gameplay
  (212k calls/4700 frames) — **CORRECTS the later-82 claim "cull doesn't fire"**, which was DEMO-only.
  Objects carry real 3D world positions keyed by their pool-slot pointer (type 02/03,
  e.g. obj=800fc5c0 pos=(4750,-1500,5000)). ≥2 pools observed (strides 0x88 and 0xD0).
- **Interpolation insight (supersedes the GTE-fingerprint approach):** the **object pointer is a stable
  cross-frame identity** (pool slot) with a real position — snapshot per-object pos each logic frame and
  interpolate that, instead of fingerprinting GTE transforms. This is the Phase-4 path once the native
  entity walk (Phase 1) owns the list. (wide60.c per-prim gating fix `b2de253` stays meanwhile.)

### Cleanup
tools/clean.sh (allowlist of regenerable dumps; preserves RE assets/state/bios/bin/obj) — freed ~72 GB.
Dropped a stray tracked 16 MB Beetle savestate (`hold`).

## 2026-06-16 (later-83) — wide60 reprojection: FIXED user-reported "terrible" live output (smearing + flicker + TRIPLED weapon HUD icon). Root cause = global screen-XY remap with no per-prim object gating. Tooling-proven.
User ran `PSXPORT_WIDE60=1 ./run.sh` on the later-82 reprojection code: "terrible" — stretched/smeared
polys, flicker, and the bottom weapon-icon HUD drawn DOUBLED/TRIPLED. (Diagnosed via tooling per user
directive, not by eyeballing — see the new sdbg counters.)

### ROOT CAUSE (confirmed, `PSXPORT_WIDE60_SDBG` per-frame counters in wide60_synthesize)
`wide60_build_remap` builds a **global old-SXY → interp-SXY hash table**; `wide60_synthesize` remapped
**any** captured prim vertex whose packet (x,y) hit a key — with NO notion of whether that prim is a
3D/GTE object. PSX sprites/rects/lines + CPU-projected polys are screen-space (never RTP'd), but their
coords can COLLIDE with a real 3D vertex's SXY in the table, so they got dragged to interpolated
positions. Measured on gameplay (f471+, every frame): `sprite_remap` 1–4/frame (= the tripled HUD icon),
`line_remap` 1–2, and **poly partial-remap 4–103/frame, varying wildly** (= stretched tris = smearing;
the frame-to-frame variation = flicker). The old per-prim object join (`Prim.obj` via the grid) was
computed at capture but UNUSED by the transform-based synth — that's the regression.

### FIX (wide60.c wide60_synthesize) — ALL-OR-NOTHING per primitive
- Sprites (nv==1) + lines (nv==2): **always snap**, never consult the remap (they're always 2D).
- Polygons: remap only if **EVERY** vertex resolved in the table (whole prim belongs to one matched+
  interpolated object); else snap the WHOLE prim. No mixed/partial vertices → no stretch. A CPU-projected
  2D poly won't have all corners coincide with distinct 3D SXYs → snaps. Snapped prims = 30fps fallback.
- VERIFIED by tooling: `sprite_remap=0 line_remap=0` across all 5943 frames of the headless run (was
  1–4/frame). Offline f2500 dump (`PSXPORT_WIDE60_SYNTH=2500`) unchanged-good: in-between complete (bg,
  rails, DEMO text, orbs), character at midpoint, no holes. Faithful path untouched (synth is WIDE60-only).
- **Kept as committed tooling:** `PSXPORT_WIDE60_SDBG=1` runs the synth every headless frame and prints
  per-frame remap-correctness counts (poly full/partial/none, sprite/line remaps, collisions-snapped).
- OPEN / next: user must eyeball LIVE (`PSXPORT_WIDE60=1 ./run.sh`) — the confirmed artifact sources are
  gone, but residual judder is possible since ~10–30% of 3D polys/frame fall back to snap (partial
  coverage: their object was gated by the TR-teleport gate, or they span objects). If too many 3D polys
  snap, investigate WHY coverage is partial (gate too tight? per-vertex object association needed?).

## 2026-06-16 (later-82) — wide60 60fps RE-ARCHITECTED to the per-game GTE-transform tier (user-directed; screen-space plane interp REJECTED). Native graphical-object capture + cross-frame matching VERIFIED.
**User rejected screen-space primitive interpolation** ("you are doing this blindly; reverse engineer the
game's camera and objects and interpolate those, not individual planes"; then "port the game's graphical
objects into PC native such as camera and models, then interpolate them"). Did the RE:

### RE findings (PSXPORT_W60_GTEDBG instrumentation, demo scene f2500)
- The **0x8007712C cull dispatcher does NOT fire** in these scenes — `g_current_object` stays 0. So the
  earlier "object join 92-99%" (later-57) was joining EVERYTHING to one sentinel = a single mega-blob —
  THE ROOT CAUSE of the smearing in the screen-space approach. **Falsifies the object-join premise.**
- The real object identity is the **GTE TRANSFORM**: each run of RTPS/RTPT sharing one (R = rotation
  matrix CR0-4, TR = translation CR5-7 = the model-view, camera baked in) is one object. ~34-121 distinct
  transforms/frame: identity-rotation+large-TR = world props/background; varied-rotation+small-TR clusters
  = the character (one transform per animated bone, 30-70 RTPS each); one 1535-RTPS group = the terrain.
- Camera has **identity rotation** here (2.5D); motion is in TR (e.g. TRZ 2624→2656 between f2500/f2501).
  Projection consts OFX/OFY/H constant. Local input verts (DR0/DR1) are **model-space, frame-invariant**.

### Architecture (the per-game tier, per CLAUDE.md)
Group geometry by GTE transform = native object. Cross-frame **identity = local-vertex fingerprint** (mesh
invariant; only transform moves). **Interpolate the transform** (R + TR) at t=0.5; **re-project** the local
verts through the real Beetle GTE → old-SXY→new-SXY map → remap B's captured prim vertices. Unmapped prims
(CPU-projected terrain edges / 2D HUD) snap = 30fps. Perspective-correct ⇒ no smears, no gaps.

### VERIFIED foundation (headless, gameplay f500-f5000) — `wide60.c` XObj capture
`wide60_rtp`→`xobj_rtp` groups RTPS by transform into native `XObj{R,TR,fingerprint,nrtps}`, double-buffered
A/B. **objects/frame = 24-121; cross-frame match by fingerprint = 87-94%; TR delta avg 70-940, max ~5700**
(interpolatable). 0-object frames = FMV/load (no interp). Faithful path unchanged.

### Next (this is the active work)
Reprojection: save GTE state → for each matched object write interp(R_A,R_B)/interp(TR_A,TR_B) → RTPS each
local vert → build SXY remap → restore GTE → render in-between by remapping prim verts. **The screen-space
synth (wide60_synthesize lerp, prim matcher, per-object decide, shape gate) is SUPERSEDED** — kept only
until reprojection lands, then removed.

## 2026-06-16 (later-81) — wide60 60fps: in-between SYNTHESIZER + live present, rebuilt to the user's mandated architecture. PLAYABLE windowed (awaiting user flicker check). Also: fit-to-screen 4:3 display scaling.
**User correction (a first cut that rendered the in-between INTO the game's back VRAM buffer was
rejected as "terrible").** The 60fps layer must NOT write VRAM — it sits ON TOP of the PC renderer.
Rebuilt per [[wide60-60fps-architecture]]:
- **Separate framebuffer.** Interp is rasterized into `s_interp` (gpu_native.c), NOT VRAM. A write-target
  redirect `s_fb_base` (used by `put_px_b`) points at s_interp only during synth; `sample_tex` always
  reads VRAM, so textures come from the live atlas. `gpu_w60_begin_interp` clears the target region +
  sets env; `gpu_w60_draw_poly/sprite` reuse the SAME `tri()`/`raster_sprite()` path (extracted a shared
  `raster_sprite` helper from gp0_exec). `gpu_present` split into `gpu_present_ex(do_blit)` so wide60 runs
  bookkeeping without the front blit.
- **Cadence = 1 frame behind.** Per 30fps logic frame present TWO frames: the PREVIOUS real frame (intact
  in the back VRAM buffer, blitted read-only) then the INTERP frame (s_interp). Current is held → becomes
  "previous" next frame. Stream: A, lerp(A,B), B, lerp(B,C), C… = 60fps. `wide60_present()` owns it; gated
  to windowed + animating (geom>0) + actually double-buffering (front_y flips), else one faithful present.
  Pacing: `gpu_pace_subframe(2)` twice (the shared accumulator advances one logic frame, so audio stays
  realtime); faithful path unchanged at `gpu_pace_subframe(1)`.
- **Interp = object-vertex lerp at t=0.5** (matched within a displacement gate, default 48px L1; else snap);
  UV/color/state kept from B (avoids UV-parity smear). Full list redrawn over a cleared region → background
  + HUD reproduced, no holes (verified via the offline dump).
- **Display scaling (`present_window`→`blit_src`):** fit output to the screen at fixed **4:3** with
  letterbox/pillarbox bars — never stretch 2D/FMV. Default borderless **fullscreen-desktop** ("adapt to
  screen"); `PSXPORT_WINDOWED=1` = resizable window; ESC/close quits; linear upscale filter.

### Verified
- **Faithful path byte-identical** with PSXPORT_WIDE60 off (VRAM f3000 `cmp` PASS) — the `put_px_b`→`fb()`
  and `raster_sprite` extraction are behavior-preserving.
- **Offline dump** (`PSXPORT_WIDE60_SYNTH=2500`): the interp frame in s_interp is COMPLETE (full bg, rails,
  DEMO text) with the character at the interpolated midpoint between A and B. scratch/screenshots/w60_*.png.
- Headless WIDE60 run completes (else-branch single present, no crash).

### OPEN / next (NEEDS THE USER's eyes — windowed)
- **Play + flicker check:** `PSXPORT_WIDE60=1 ./run.sh`. Watch for smoothness + ZERO flicker. Do not
  declare done on headless alone.
- Known approximations to revisit if artifacts show: per-prim clip not captured (uses full front-buffer
  clip); lines (0x40–0x5F) / fills (0x02) not captured → if a scene draws bg via those they'd be missing in
  the interp (clear-to-black shows through). The DEMO scene was complete with poly+sprite only.

## 2026-06-16 (later-80) — wide60 60fps: full primitive capture + cross-frame MATCHER built & displacement-verified (the handoff's stated next milestone). Foundation for the in-between synthesizer.
Continued the 60fps tier from later-57 (object→primitive join). Built milestones 3 (full capture) and
the matcher, all in `runtime/recomp/wide60.c` + read-only taps in `gpu_native.c gp0_exec` (gated PSXPORT_WIDE60).

### Mechanism
- **Full primitive capture (PrimFrame A/B).** Every completed GP0 poly (`wide60_cap_poly`) and
  sprite/rect (`wide60_cap_sprite`) is teed into the current frame's `Prim[]` buffer: op, nv, per-vertex
  (x,y packet-coords / u,v / rgb), E5 draw-offset, texpage, color-mode, CLUT, and the joined obj id
  (`grid_get` of the lead vertex; sprites/lines = obj 0 → snap). Double-buffered A(prev)/B(cur), swapped
  in `wide60_frame_commit`. PRIM_MAX=8192 (measured ≤~1400/frame, with headroom; overflow flagged).
- **Matcher (`wide60_match`, run per logic frame).** Key = **obj + op + texpage + color-mode + CLUT**;
  greedy "first unused A prim with equal key, in A draw order" → pairs the k-th same-key prim in B with
  the k-th in A. `s_match[i]` = A index or −1 (snap). obj 0 never matches.

### Verified (headless, gameplay f500–f5000)
- **poly-join 92–99%** (reconfirms later-57). **lerp match 49–96%** of all prims (the rest = sprites/HUD/
  terrain that snap by design + joined polys whose mesh genuinely changed). **Median cross-frame
  displacement of matched pairs = 2–9 px** — i.e. matches are genuinely the same primitive one logic
  frame apart, exactly what's interpolatable. (p90 saturates the 15px histogram ceiling = the fast-motion
  tail the synth's displacement gate will snap.)
- **KEY-CHOICE dead end (don't revisit):** dropping texpage/CLUT and keying on obj+op alone (relying on
  draw-order ordinal) is WORSE — median displacement jumps to 15+px because per-tri LOD/cull shifts the
  ordinals. The "CLUT high halves alternate by parity, don't key on them" warning applied to the OBJECT-LESS
  offline fingerprint matcher; with obj as the anchor, texpage/CLUT *refine* the match (verified by the
  displacement histogram). Richer key kept.
- **Faithful path safe:** VRAM at f3000 **byte-identical** with PSXPORT_WIDE60 ON vs OFF (`cmp` PASS) —
  capture/match only READS, never touches s_vram.

### Next (remaining, needs the USER's eyes for flicker)
The in-between SYNTHESIZER + present ordering: replay B's captured list rasterizing lerp(A,B,0.5) for
matched prims (snap the rest, plus a displacement gate), skip VRAM-upload ops on in-betweens, present
in-between then B, host-pace 60 Hz. No-flicker is TOP priority — do NOT declare done on headless alone.

## 2026-06-16 (later-79) — FIXED (user-verified): "ingame music plays over the dialog" in the prologue. Cause: the looping ingame-music XA (chan4) is started by the gameplay state machine, which on real HW only fires AFTER the CD-paced scene load — by then the dialog owns the audio. Our INSTANT CD reads fire it during the dialog, so it overlaps the dialog tone. Fix (per user directive — keep instant CD, mod the CD-speed-dependent behavior in PC code): suppress the looping ingame-music XA while a dialog tone is the current song, resume it after. Identifications corrected: **song 4-7 = sequenced dialog tones** (user-ID: 4=regular, 6=worry), **chan4 XA = ingame/area music**, **chan22 XA = dialog voice**, **chan7 XA = prologue narration**. FOLLOW-UPS FIXED in later-79b/79c: the resumed music starting at full volume (now fades in via the game's own CD-volume ramp) and a ~1-frame music "blip" at the dialog start (later-79b cut synchronously; later-79c found the fade-in still leaked one FULL-volume frame and fixed it by flushing the SPU CDVOL register on the snap — USER-VERIFIED PERFECT). STILL OPEN: fade-OUT entering indoors is abrupt (not yet captured — see later-79c).

### later-79 CORRECTION of the mid-investigation notes below
The measurement notes in this entry are accurate as DATA but two interpretations in them are WRONG and are corrected here:
1. "song 4 = field BGM that plays through the dialog" — FALSE. song 4 is the **regular dialog tone**; it's *supposed* to play during the dialog. The thing wrongly overlapping it is the **chan4 XA ingame music**, a different SPU path.
2. "holding DAT_801fe0e0 / the gate fix is refuted" — the gate IS the mechanism: the ingame music is started by FUN_8005f2f0/FUN_800624b4 (gameplay handlers) at the FUN_80074f24 call (ram_f1000_all.c:40267/41983), and those handlers are held by `if (DAT_801fe0e0 != 0) … return`. Instant loads let the handler reach the music-start during a gate gap. (We did NOT implement a raw gate-hold — see the FIX — but the gate was the right lever to reason about.)
3. The "model CD latency" fix was **rejected by the user**: instant CD reads are a required PC feature (PC disk speed, not CD speed); every CD-speed-dependent function gets MODDED in PC code instead.

### later-79 — oracle A/B of the prologue→fisherman timeline (the decisive measurement)
Drove BOTH cores to the prologue narration and sampled `0x800bed80` (current song) + `0x801fe0e0`
(voice task-2 gate) frame-by-frame. **Oracle menu nav fix:** REPL `tap` does NOT register at the
title menu, but **held press works** — `press Cross / run 30 / release Cross` selected NewGame then
StartGame (updates docs/diff-driver.md "known gap"). Reusable oracle state saved at the prologue:
`scratch/state/prologue.sav`.

ORACLE timeline (ground truth):
- prologue narration (chan7): gate `0x801fe0e0`=1 **continuously** ~f6360→~f8460 (~2100 frames), song=FFFF.
- narration ends → gate→0 at ~f8520, song still FFFF.
- **~300-frame pause, gate stays 0**, then song→**04** at ~f8820 (field BGM starts WHILE gate=0).
- fisherman "Aaaaahhh!" scream + dialog: gate **toggles** 1/0 per voice line from ~f9300 onward
  (f9300-9360 up, 9420-9660 down, 9720-10080 up, …) — and **song stays 04 the entire time**.

=> In the real game the sequenced field BGM (song 4) **correctly plays underneath the whole fisherman
dialog**; it is NOT gated off during the cutscene. song 4 even starts BEFORE the first dialog line, in
a gate-0 window after the narration. **This REFUTES later-78e's plan** (hold `DAT_801fe0e0` nonzero to
delay song 4 — that would make the port DIVERGE from the oracle). song 4 is not gated by the voice task.

PORT timeline (same scene, native frames): narration gate=2 ~f267→~f1360 (~1093f, ≈HALF the oracle's),
then at f1390 gate→0 and **within 4 frames** song4 + chan4-loop fire together (f1394), chan22 scream
~f1640. The oracle's ~300f post-narration pause is collapsed to ~4f, so song4 and the cutscene start
simultaneously — what the user hears as "song 4 during the cutscene."

### later-79 ROOT CAUSE (confirmed with PSXPORT_CD_VERBOSE)
At narration-end the game loads the fisherman scene: a burst of large `FUN_8001db8c`/`FUN_8001dc40`
loadfiles — 285096 + 188416 + 231424 + 448512 B … (~1.4 MB) — issued right at f1390. The port serves
these **synchronously and instantly** (cd_override.c `ov_cd_loadfile`/`ov_cd_async_read`), so the whole
load finishes in ~4 frames; the game then starts song4 the instant the load-done flag is set. In the
oracle the same ~1.4 MB takes real CD time (~5 s ≈ ~300 frames at 2× ≈ the measured f8520→f8820 gap).
**The game uses CD load latency as implicit pacing; the port's zero-latency CD I/O removes it.** This is
an architectural consequence of the "no CD emulation / synchronous reads" design, not a localized bug.
(The narration being ~2× too short is a related, separate pacing gap — likely XA/stream consumption rate
— and is NOT yet root-caused; the post-narration collapse above is the dominant audible effect.)

### later-79 — RE of the trigger + the implemented fix
- **What starts the ingame music:** `FUN_80074f24(DAT_800bf870)` (→ `FUN_800750d8`/`FUN_8001d364` → `FUN_8001d2a8(chan4,[84515..97979],loop=1)`), called from the gameplay handlers `FUN_8005f2f0` (case 6, :40267) and `FUN_800624b4` (:41983). Those handlers are held by `if (DAT_801fe0e0 != 0) { FUN_8001cf2c(); return; }`. The dialog routine `FUN_801464c0` (overlay, 0x80146xxx — INTERPRETED, not in the Ghidra decomp; disassembled live from a RAM dump with capstone) is just the **voice player** (line index → channel → `FUN_8001d2a8`); it does not stop the music.
- **Oracle ground truth (cold-boot WAV; savestate XA does NOT replay):** narration (chan7) → **~8 s silence** (real CD load) → dialog tone + voice, with **NO ingame music** (chan4 cross-corr ≈0.087 vs dialog-tone 0.31; the 8 s silence rules out a looping chan4). So the ingame music is correctly absent during the prologue dialog. `scratch/wav/oracle_scene.wav`, segments in `scratch/wav/music/`.
- **The mod (cd_override.c + xa_stream.c + native_boot.c):** a looping XA clip = ingame/area music; one-shot clips = voice/narration. While a dialog tone is the current song (`0x800bed80` in 4..7), `ov_voice_play` defers a looping clip (remembers it, doesn't start it / doesn't set the gate) and `xa_dialog_coord` (per-frame, from native_scheduler_step) stops any looping XA that's sounding; once the dialog tone clears and the XA stream is free it resumes the remembered ingame-music clip. Voice clips are untouched. USER-VERIFIED correct (dialog now = tone + voice only; ingame music returns after).
- **Music identification library** (for future audio bugs): `scratch/wav/music/` — `seq_00..13.wav` (sequenced songs, rendered via new REPL `bgm N` + `wav PATH`) and `xa_chan{4,7,22}_*.wav` (XA tracks, via new REPL `xadump chan lba PATH`). chan4=ingame music, song4=regular dialog, song6=worry dialog (user-identified).
- **OPEN (next):** resumed ingame music starts at full volume; the real game fades it in from 0 (another instant-CD timing casualty — the volume ramp). Investigate the XA/CD volume fade and mod it PC-side. → DONE in later-79b.

### later-79b — fade-in + the 1-frame "blip", both PC-side (register-verified)
Two follow-ups from later-79, fixed in the same instant-CD-mod spirit.

**The fade mechanism (RE'd):** the game fades XA loudness via the **CD-volume** SpuCommonAttr (mask
`0xc0` = CDVOLL/CDVOLR → SPU regs 0x1B0/0x1B2 → Beetle `spu.c` scales the XA by `CDVol>>15`, spu.c:1400).
Per-frame `FUN_80075824(&DAT_800be1f8)` ramps a fade **current** `DAT_800be224` toward a **target**
`DAT_800be222` by 0x100/frame (×2 if `DAT_1f800137==2`), scaled by master `DAT_800be220`; `FUN_80075cec`
sets the target (neg arg snaps both); the scene-transition fades are `FUN_80026470` (out, target 0) /
`FUN_800264bc` (in: snap to ~0, target 0x47ff). Verified live (PSXPORT_CDVOL_DBG, new gated log in spu.c):
in gameplay CDVol sits flat at 25484 (= master, current `DAT_800be224`=0x7fff), i.e. the ramp runs but is
already at target — so a (re)started clip plays at full, no fade. The dialog VOICE (chan22) is the SAME XA
stream and is ALSO scaled by CDVol, so the fade may only be applied when the music is the sole XA user.

**Fade-in fix (cd_override.c `music_fade_in`):** on ingame-music (re)start (`ov_voice_play` loop path +
`xa_dialog_coord` resume) snap the fade-current `DAT_800be224`=0 and leave the target; the game's own
per-frame ramp climbs it back (~128f ≈ 2.1 s) = a faithful fade-in using the vanilla mechanism/rate.
Verified: at chan4 start CDVol drops 25480→**198** (first ramp step from 0) and climbs.

**1-frame "blip" fix (native_boot.c `ov_bgm_start` → cd_override.c `xa_music_cut_if_dialog`):** the
per-frame `xa_dialog_coord` stops the looping music one frame AFTER the audio is mixed (frame loop:
`rc0(0x800788ac)` audio mix at line 378 runs BEFORE `native_scheduler_step`/`xa_dialog_coord` at 379-380),
so a dialog-tone start leaked ~1 frame of music. Now the BGM-start override (always-on; logging still
gated by PSXPORT_BGMDBG) cuts the looping XA **synchronously** the instant a dialog-tone song (4-7) is
written. Combined with the fade-in, any residual sub-frame leak is at ~0.6 % volume (inaudible).

**Not a regression:** the "house is on fire" (song 6) dialog stalls headless at `DAT_801fe0e0`=2 — but
**baseline (stashed) stalls identically**, so it's the pre-existing open BGM-timing bug (commit a42289d),
not from this change. The post-dialog resume couldn't be observed headless (dialog stall); the resume uses
the identical `music_fade_in` path. Pending: user ear-check via `./run.sh`.

### later-79c — later-79b fade-in was INCOMPLETE: it still leaked one FULL-volume frame (user ear-confirmed). Root cause found via frame-by-frame trace; fixed by flushing the SPU CDVOL register on the snap. Now USER-VERIFIED PERFECT.
later-79b snapped only the *guest* fade-current `DAT_800be224`=0. But the game's per-frame ramp
`FUN_80075824` writes the SPU CDVOL **register** (0x1F801DB0/DB2 → Beetle spu.c case 0x30/0x32, `CDVol[]`)
from `cur` exactly **once per frame**, and on the music-(re)start frame it had ALREADY run with the old
(full) `cur` *before* `music_fade_in` fired — so the started XA mixed at full for that one frame, then
dropped and climbed. That leading full frame = the user's "1-frame blip" and "starts loud then drops to
zero then climbs."
- **Diagnosis tooling (gated, kept):** `xa_audio_trace()` (cd_override.c, PSXPORT_XA_DBG) logs the fade
  vars + XA lifecycle per frame at `pre`/`post`/`coord` points (native_boot.c, around `rc0(0x800788ac)`);
  `[xamix]` (spu.c, PSXPORT_XAMIX_DBG, env cached) logs the CDVOL actually applied to live XA samples.
  The trace nailed it: at chan4 start `[xamix] CDVolL=25480` (full) for the start frame, then 198/198…
- **Fix (cd_override.c `music_fade_in`):** in addition to `DAT_800be224`=0, write the SPU CDVOL register
  to 0 directly — `mem_w16(0x1f801db0,0); mem_w16(0x1f801db2,0)` (routes io_write→spu_write→SPU_Write).
  This mutes the start frame's mix; next frame the game's ramp rewrites CDVOL from cur=0 and climbs.
  `ov_voice_play` runs mid-tick (after `FUN_80075824`), so the register write lands before that frame's
  SPU mix. **Verified:** start frame now mixes at `CDVolL=0` (was 25480), then 198/795/1591… climbing;
  **user ear-confirmed "absolutely perfect, to the last minute detail."**
- **STILL OPEN — fade-OUT (entering indoors) is abrupt.** Not yet captured: in both traces the music
  stayed at full through the transition and the game never set CDVOL target→0, then the stream was
  STOPped at full `cur` and immediately restarted (loop/scene re-trigger), so no fade-out path was
  exercised. Need a clean "enter indoors" capture to see whether the game arms `FUN_80026470` (target 0)
  and the stream is killed before the ramp reaches 0 (→ defer `xa_stream_stop` until cur≈0), or the game
  never fades here at all. Don't fix blind.

## 2026-06-16 (later 78) — FIXED later-77: in-game XA-ADPCM streaming implemented natively (prologue BGM + voice now audible, WAV-confirmed). Fisherman-scream "first note repeats / cutscene won't advance" ROOT-CAUSED to FUN_8001cfc8's faked GetlocL position wait; interim stopgap in place, native engine-port planned.
**New native subsystem `runtime/recomp/xa_stream.c`** (in build: added to run.sh + tools/build_port.sh).
Decodes CD-XA from the ReadS-streamed sectors and feeds the SPU CD-audio input via
`CDC_GetCDAudioSample()` (Beetle spu.c calls it per 44.1kHz sample, scaled by the game's `CDVol`,
gated on `SPUControl` bit0 — both game-set). The silence stub that used to live in spu_beetle.c is
gone; xa_stream.c owns that symbol now. Decode is **pull-driven** (decode-on-consumption → self-paces
to realtime, advances the disc LBA exactly as audio is consumed). Resamples XA 37800/18900 → 44100 with
a fractional phase accumulator + linear interp. `xa_decode_sector()` (mednafen-parity) is reused from
native_fmv.c. Debug: `PSXPORT_XA_DBG=1` (events) / `=2` (per-sector). `PSXPORT_CDCMD_DBG=1` logs both
CD-command wrappers.

### How the game drives in-game XA (observed via PSXPORT_CDCMD_DBG, both wrappers)
TWO CD-command wrappers; cutscene XA uses the engine-streaming one:
- libcd `CdControl` = `FUN_8008AC34` → `ov_cd_command`. Boot/menu (Setmode 0x80/0xC0, a short clip).
- **engine streaming = `FUN_8001CE90` → `ov_cd_cmd_stream`. Cutscene voice/BGM goes HERE:**
  Setmode **0xC8** (Speed|ADPCM|SF-filter), Setfilter file=1/chan=N, Setloc→LBA, ReadS. Both wrappers
  now route Setmode/Setfilter/Setloc/ReadS/Pause to xa_stream. Prologue narration = file1/chan7 @ LBA
  66515; fisherman-scene voice clips = file1/chan4 @ 84515, chan22 @ 12771, etc. (interleaved, every
  8th sector is the selected channel). VERIFIED: prologue WAV has real BGM+voice from ~t=8s (RMS
  1700-4400) where it was silent before; xa_stream decodes continuously, LBA advances, no loop in the
  engine itself.

### Fisherman "AAAGH repeats / cutscene stuck" — ROOT CAUSE (RE'd FUN_8001cfc8, the engine streaming reader, task slot 2)
`FUN_8001cfc8` plays a clip [start..end] on file1/chan(task+0x66): Setloc start, ReadS, then **polls
GetlocL (cmd 0x10) and yields each frame WHILE head <= task+0x58 (clip end)**; when head > end it pauses
(`FUN_8001ce90(9)`) + `DAT_800be0e4=0` + ends the task (advancing the cutscene), OR if task+0x67==1 loops
the clip. Our `ov_cd_cmd_stream` faked GetlocL as a STATIC position (clip start) → `head <= end` always
true → the wait never ends → cutscene never advances, clip runs on / re-triggers ("first note over and
over"). The user nailed it: that voice line gates cutscene advancement.
- Task params (RE'd): `FUN_8001d2a8(chan, start_lba, end_lba, flags)` writes them DIRECTLY into the
  slot-2 task struct — `DAT_801fe134/138/146/147` ARE task2 (`0x801fe0e0`) `+0x54`(start)/`+0x58`(end)/
  `+0x66`(chan)/`+0x67`(loop). Helpers: `FUN_8008a00c` LBA→BCD MSF (msf=lba+150); `FUN_8008a110` BCD
  MSF→LBA(−150); `FUN_8001ceb0(0xC8,..)` ensures Setmode 0xC8; `FUN_8001cf00(0/1)` CD-event enable.
- **Interim stopgap (committed):** `ov_cd_cmd_stream` GetlocL now reports `xa_stream_play_lba()` (the
  native engine's advancing read head) while streaming → the wait terminates, clips play once and the
  scene advances. Marked `// STOPGAP` in cd_override.c.

### later-78e — XA audio ORACLE-VERIFIED; OPEN: gameplay BGM starts during the fisherman cutscene
XA in-game audio now works AND is oracle-verified: drove the oracle (cold boot, Start to skip FMVs,
Cross to select StartGame) to the prologue narration ("Tomba is living peacefully…") and captured its
audio (scratch/wav/oracle_cold.wav, narration ~263-291s). Extracted both narration segments and ran
tools/audio_differ/compare.py diff vs the port (scratch/wav/port_cdfix.wav, narration ~15-23s):
**corr=0.954, RMS ratio 0.894** — the music+VO content MATCHES the oracle. (Two bugs fixed to get here:
mono-XA buffer overflow [later-78d], and the dropped CD->SPU enable [FUN_8001cf00(1)] that left
SPUControl bit0=0 so the decoded XA was discarded by spu.c.) User confirms: fisherman dialog + other
audio fixed.

OPEN (user report): the **sequenced gameplay BGM starts too early — during the fisherman cutscene**,
should wait until it ends. Observed in port: `FUN_80074BF8(idx=4)` fires at ~f1554 (0x800bed80→4) while
the cutscene's XA (chan4 loop music + chan22 voice lines) is still playing. The gameplay-vs-cutscene
gate is `DAT_801fe0e0 != 0` (voice-task-alive): handlers FUN_8005f2f0 (case1 @40208) and FUN_800624b4
(case2 @41958) do `if (DAT_801fe0e0 != 0) { FUN_8001cf2c(); return; }` — i.e. don't run gameplay while a
voice clip plays. Our native voice port (later-78c) sets DAT_801fe0e0=2 only WHILE a clip is mid-play and
0 otherwise, so between clips / before the first clip the gameplay slips through and starts song 4.
User's framing: "cutscenes stop gameplay, maybe that stop didn't work."
NEXT (needs oracle A/B): watch when the oracle writes 0x800bed80 (song 4) relative to the cutscene, and
what keeps gameplay blocked across the WHOLE cutscene in the real game (candidate: the chan4 XA loop keeps
task slot 2 alive continuously; our single-ring player drops it when a chan22 voice line replaces chan4
and doesn't resume → DAT_801fe0e0 gaps). Likely fix: hold the cutscene "voice/stream active" state for the
whole cutscene (don't 0 it between lines), or resume the looped chan4 music after each voice line. Confirm
the exact gating against the oracle before changing — do NOT guess (user rule: verify vs oracle).

### later-78d — BUG: mono XA overflowed the decode buffers (voice was silent/garbled)
User: no voice in the WAV / "sounds the same as when XA was broken". Per-sector log showed the
chan22 voice = **mono 18900Hz → n=4032 frames/sector** (mono puts ALL units on one channel: 18*8*28),
but the decode buffers were sized for 2016 STEREO frames: `xa_decode_sector`'s internal
`ch[2][2016+8]` and the callers' `pcm[2016*2]` / `xa_pcm[2016*2]`. The 4032-frame mono sector
overflowed them → corrupted state (seen as `wr=678508593` garbage right after a mono sector) → no
coherent voice. FIX: size all three for the mono max (4032): `ch[2][4032+8]`, `pcm[4032*2]`,
`xa_pcm[4032*2]` (native_fmv.c + xa_stream.c). After: wr increments 4032,8064,12096… cleanly.
(Stereo 37800 FMV audio never hit this; mono voice did.)

### later-78c — FIXED: voice-streaming engine ported native (drop the FUN_8001cfc8 task)
Implemented the native port. Verified interactively (drive like a player → prologue → hands-off):
voice lines now ADVANCE one-by-one (chan22 clips 12771→15875+, each new line starts fresh) and
task-2 state tracks clip completion, instead of sticking on the first clip forever.
- ROOT CAUSE confirmed live: read task-2 fields mid-stall — they said clip = LBA 13923..14243 chan22,
  but xa_stream was still playing the OLD clip (12771). The dialog re-registered slot 2 with a new
  clip, but the native scheduler resumed the STALE FUN_8001cfc8 coroutine (it keys fresh-vs-resume on
  g_task_started, not on the entry/re-register), so the new clip's Setloc/ReadS never fired → old
  stream's head never reached the new end (14243) → `while (DAT_801fe0e0 != 0)` hung → "AAAGH repeats".
- FIX (engine→PC): override `FUN_8001d2a8` (the voice/BGM clip player all by-index APIs funnel through)
  → `xa_stream_play(chan,start,end,loop)` (idempotent per clip → re-issuing the same line can't reset
  the ring); override `FUN_8001cf2c` → `xa_stream_stop`. We OWN task slot 2: `native_scheduler_step`
  skips the now-unused FUN_8001cfc8 coroutine and writes the task-2 state byte (2 while
  `xa_stream_voice_busy()`, 0 when the clip's head passes its end) so the cutscene wait advances.
  Clip end/loop handled in xa_decode_next_sector. GetlocL stopgap reverted (that poll no longer runs).
- Slot 2 is exclusively FUN_8001cfc8, registered ONLY by FUN_8001d2a8 (verified) → safe to own.
- NOTE: XA is a single stream (one channel to SPU at a time) — BGM (chan4, loop) and voice (chan22)
  can't sound simultaneously; the game interleaves them by switching the clip, which our single-ring
  player matches. Pending user ear-check on the captured WAV (scratch/wav/fish_native_clean.wav).

### later-78b — the dialog/voice chain RE'd (stopgap did NOT fix fisherman; user confirms)
The voice line gates cutscene advancement via the **voice TASK**, not a flag:
- Dialog/cutscene script (recomp, e.g. FUN_80043a40) plays a voice line by index via the engine
  voice APIs: **FUN_8001d71c / FUN_8001d364 / FUN_8001d41c / FUN_8001d0e0** → all funnel to
  **FUN_8001d2a8(chan, start_lba, end_lba, flags)** which sets task-2 fields + `DAT_800be0e4` and
  (re)registers task slot 2 (FUN_80051f14 → entry FUN_8001cfc8, state=2). Clip channel/LBA come from
  tables PTR_DAT_8001005c.. + `_DAT_1f800220/224`.
- **The cutscene waits `while (DAT_801fe0e0 != 0)`** (ram_f1000_all.c:24287, 40208, 41958).
  `DAT_801fe0e0` = task slot 2's state byte (0x801fe000 + 2*0x70). It advances ONLY when the voice
  task ENDS (state→0). FUN_8001cf2c = stop voice (DAT_800be0e4=0 + CD pause).
- Suspect (not yet reproduced headless): our native scheduler keys fresh-vs-resume on `g_task_started`,
  NOT on the task ENTRY changing. FUN_80051f14 re-registers slot 2 (state=2, NEW entry) on each
  voice-play; if the slot's previous coroutine context is still "started", the scheduler RESUMES the old
  pc instead of entering the new entry → wrong clip / never-ends → cutscene stuck / "first note repeats".
- REPRO GAP: fast-forward nav (tap x / tap start) manually advances the dialog and MASKS the bug; only 4
  XA STARTs seen headless, clips progress. Need to reach the fisherman scream and let it AUTO-advance
  (no input). Ask the user how to reliably trigger it, or compare vs oracle at that exact beat.

### NEXT — port the voice-streaming engine to native C (engine→PC; gameplay/script stays on recomp)
Port FUN_8001d2a8 (+ the by-index APIs) and FUN_8001cf2c to drive xa_stream DIRECTLY and DROP the task-2
coroutine FUN_8001cfc8 entirely (no scheduler interaction → no re-register fragility, no GetlocL fake):
- xa_stream: add play(chan,start,end,loop) [idempotent if same clip already active → no ring reset/
  repeat] + busy() [1 while play_lba<=end & active].
- Override FUN_8001d2a8 → xa_stream_play; FUN_8001cf2c → xa_stream_stop. DROP spawning task slot 2.
- The cutscene polls `DAT_801fe0e0 != 0` (task-2 state): since we no longer run task 2, MAINTAIN that
  byte natively = (xa_stream_busy() ? running : 0) so the existing recomp wait still works. (Set it on
  play, clear it when the clip finishes.) Verify clip plays once + scene auto-advances; compare vs oracle.
Old single-function plan (kept for reference):
Per the locked architecture (engine on PC, gameplay on recomp/interp), the streaming reader is ENGINE
and should be native, eliminating the faked GetlocL poll. Plan:
1. Add a **native task-stepper** path to `native_scheduler_step` (native_boot.c): when a slot's entry
   is a known native engine task (0x8001cfc8), call a native stepper instead of `rec_coro_run`; treat
   its return as YIELD (set task state 1, re-armed by FUN_800506d0) or DONE (state 0).
2. Native `cd_stream_task` state machine using xa_stream: read chan/start/end/loop from the task
   struct; setfilter(1,chan)+setloc(start)+start; each frame check abort (`DAT_800be0e4 & 0x10`) and
   `xa play_lba > end` → done (pause, `DAT_800be0e4=0`, end) or loop (restart). Add a by-LBA
   `xa_stream_setloc` variant (we have the LBA, not BCD). Skip the PSX CD-event plumbing (FUN_8001cf00
   etc.) — xa_stream is self-contained — but replicate `task+0x6f=2` and the DAT_800be0e4 side effects.
3. Verify parity vs the stopgap build (clip plays once, scene advances) and vs the oracle.

## 2026-06-16 (later 77) — ROOT CAUSE (oracle-confirmed): the port has NO in-game XA-ADPCM (CD-streamed) audio. That's BOTH the opening-cutscene BGM AND the missing voice acting. Sequenced BGM (gameplay) works; XA does not.
Got the oracle to the SAME prologue scene (cold boot + tap Start/Cross navigates New Game; the stale
title.sav does NOT take input — its frontio/SIO state is stale, cold boot is the reliable path) and
diffed: at the prologue narration the **oracle ALSO has song=0xFF / no active libsnd slot** (tools/bgm.py
on scratch/bin/orc_prologue.bin). So the prologue's audio is NOT sequenced → it's **XA-ADPCM CD audio**.
- `runtime/recomp/cdc_native.c`: CD `SetFilter`(0x0D)/`Play`(0x03)/`Setmode`(0x0E) just ACK; `ReadN/ReadS`
  only `load_sector()` data files. There is **no XA-ADPCM decode → SPU CD-audio input** path at all.
  `disc.c` even says "(XA/STR) is a later front-end concern." FMVs get XA via native_fmv.c, but in-game
  streamed audio (cutscene BGM + dialog voice) is unimplemented.
- So the user's two symptoms — opening-cutscene "Tomba is living peacefully" BGM missing AND dialog voice
  acting missing — are the SAME gap: in-game XA streaming.

### Oracle navigation FIXED (for the tooling): cold boot, not title.sav
Injected input DOES reach the emulated pad (verified: PSXPORT_INPUTDBG in vendor input.c logs
`player0 type=1 buttons=…`). The stale title.sav just ignores it. Cold-boot + tap Start (skip FMVs) +
Cross navigates to New Game reliably. main.cpp watchdog disarmed under -repl so the oracle idles at the
prompt. tools/drive.py drives it.

### NEXT — implement in-game XA-ADPCM streaming (the fix)
Decode XA-ADPCM (Mode2/Form2, 2324B, subheader submode/coding at raw[18/19] — framing already noted in
disc.c) from the CD sectors the game ReadS-streams with the XA filter set (CdlSetfilter + Setmode XA
bit), and mix into the SPU's CD-audio input (SPU CD volume / the mednafen spu.c CD input path used for
CDDA/XA). Gate on the SetFilter/file+channel. Verify vs the oracle at the prologue (audible BGM + voice).

## 2026-06-16 (later 76) — Missing BGM PINNED to the OPENING PROLOGUE cutscene (post-New-Game narration). Its BGM-start never fires in our port (zero writes to the song byte 0x800bed80 across the whole prologue); the oracle holds song 4 throughout. Title=correctly silent; gameplay BGM (song 2/3) works. Built interactive driver + BGM inspector tools; oracle now drivable (watchdog fix).
User ground truth (corrected this session): the **title/menu** has NO BGM and that's CORRECT (the user
had confused it with the unimplemented memory-card/Load page). The **gameplay** field BGM (song 2/3,
FUN_80025588 via stage handlers) WORKS. The bug is the **opening prologue cutscene** that plays right
after New Game (narration: "Tomba is living peacefully…", "Tabby disappeared", "Is she safe?", over
village→cliff→fisherman scenes). It must have BGM and ours is silent.

### Evidence (interactive, clean navigation — NOT forced-Start spam)
- Drove the native port like a player via tools/drive.py (tap x to confirm NewGame→StartGame, then
  hands-off). Watched 0x800bed78 (sound-cmd queue count) + 0x800bed80 (current song): across the whole
  prologue, **0x800bed80 is NEVER written** (stays 0xFF). Only volume bytes 0x800bed7c-7f and the
  0x800bed82 flag are touched. So the prologue's BGM-start (FUN_80074BF8) is never reached.
- Oracle reference = scratch/state/newgame.sav (the fisherman cliff scene, part of the same prologue):
  0x800bed80 = **4**, held steady 600+ frames with NO re-writes → the oracle starts song 4 once at
  prologue start and sustains it. (tools/drive.py oracle + tools/bgm.py.)
- Earlier FORCE_BUTTONS=FFF7 runs DID fire song 4 at f166 — that was an artifact of Start-spam advancing
  scene state differently (Start in gameplay = pause = stops BGM). Disregard those; clean nav is the
  truth: prologue BGM never triggers.

### Mechanism recap (still true, later-75)
BGM machinery works: FUN_80074BF8(idx) sets 0x800bed80 + SsSeqPlay; SsSeqCalled ticks; SEQ→SPU produces
audio. So this is purely a TRIGGER problem: the prologue cutscene never issues its "play song 4" command.

### CORRECTION (user): the narration cards are a DISTINCT silent scene — song 4 is a SEPARATE scene
My earlier "song 4 starts late" was WRONG (user corrected). What happened: I let the silent narration
("Tomba is living peacefully" / "kidnapped?" cards) run ~39 s; it then ADVANCED into the *separate*
fisherman/Tomba-in-tree scene, which correctly plays song 4. Those are different scenes. The narration
cards are their OWN scene and must have their OWN BGM (NOT song 4) — and it's missing/silent. Do not
conflate the two: the fisherman BGM (song 4) working says nothing about the narration cards' BGM.
- The song-4 trigger FUN_80074590(0x72) lives at 0x8011a120 — but that overlay is NOT loaded during the
  narration (0x8011a118 = zeros in scratch/bin/native_prologue.bin); it loads with the game scene at f1174.
- So either (a) the real game starts the narration's BGM via a DIFFERENT (earlier) trigger we don't fire,
  or (b) our narration phase is far too long / stalled (~39 s) and runs before the map+BGM load, whereas
  in the oracle the map/BGM is up during the (shorter) narration. Need the oracle AT the narration cards
  to decide — and that's blocked on oracle menu navigation (below).

### BLOCKER to resolve first: oracle menu navigation
Injected input (REPL tap/press, g_repl_buttons set & confirmed) does NOT reach the game in the oracle:
the title menu cursor (0x801fe068) doesn't move and the candidate pad buffer (0x800bf4f8) stays 0xFFFF.
Likely the OpenBIOS pad-poll path isn't seeing InputStateCb (or the game's real pad buffer is elsewhere).
Until fixed, can't drive the oracle from title→NewGame to the narration for a clean diff. Fixing this is
the highest-leverage next step (unblocks "always compare vs oracle"). Workaround refs: newgame.sav is
POST-narration only.

### NEXT — find + port the prologue/narration BGM trigger (compare vs oracle)
The prologue scene loads (narration renders) but its BGM-start isn't issued. Candidate: the prologue
map/scene-load BGM selector or the cutscene/event script's music cue runs in the oracle but is skipped
by our native boot/scheduler (native_boot.c hand-codes the frame loop + replaces FUN_80051e60). Find in
the oracle exactly what calls FUN_80074BF8 for song 4 at prologue start (need the oracle AT prologue
start — blocked: oracle title-menu input via REPL tap does NOT navigate the menu, cause TBD; cold-boot
through FMVs is slow). Then check whether our native scheduler runs that code path / its precondition.

### Tooling built/fixed this session (use these; extend as needed)
- **tools/drive.py** — persistent INTERACTIVE driver for native|oracle via a FIFO (O_RDWR keeps it open).
  `start [native|oracle] [headed] [ENV=val…]`, `send "cmd" …`, `out [N]`, `stop`. Drive like a player,
  observe between commands. Native: FMVs off + BGMDBG by default. `headed` opens a window (SDL; needs a
  display — windowing in this sandbox didn't show for the user, headless is the norm).
- **tools/bgm.py** — BGM/libsnd state inspector for ANY 2MB RAM dump (oracle snapshots OR our dumps):
  `dump`/`slots`/`callers`. The reliable way to diff BGM state across cores/scenes.
- **native REPL**: added `dumpram <path>`; fixed `shot`/`dumpram` path truncation (was %31s).
- **PSXPORT_FORCE_STOP_AT=N** (pad_input.c): cease forced input at frame N (reach a scene via Start, then
  go hands-off so Start-pause doesn't poison the capture).
- **Oracle watchdog fix** (main.cpp): with -repl, the global idle SIGALRM watchdog killed the process at
  the blocking prompt — now disarmed for -repl (run/tap self-arm). The oracle is now interactively drivable.
- Gotcha: oracle REPL tap/press does NOT move the title menu (input not reaching it); root cause TBD.

## 2026-06-16 (later 75) — BGM: later-74 FALSIFIED. The libsnd sequencer ACTIVATES BGM, the read pointer ADVANCES, and it PRODUCES AUDIO (WAV-confirmed). Not "frozen/never activated." The real issue is SCENE-SPECIFIC: some scenes' BGM is started+sustained (fisherman intro = audible), others (menu / steady gameplay) are not started or get stopped. Bug moved upstream to scene/stage BGM-trigger logic, NOT the SPU/sequencer.
**later-74's whole premise is wrong** and so is memory [[track-game-state-not-output]]'s "songs OPEN but never ACTIVATED." Evidence, all from the native port (PSXPORT_BGMDBG diag in native_boot.c: overrides on FUN_80074BF8/FUN_80074E48 + a per-frame slot-tick probe):

### How BGM actually works here (RE'd this session)
- **FUN_80074BF8(idx)** = "play BGM #idx": stores idx to the current-song byte **0x800bed80**, looks up
  seq table @0x800b6e68 (idx*8 → {seq_no, loopcount}), calls **SsSeqPlay** (public 0x80090560 → worker
  0x800905e0; sets the per-seq struct ptrs + **+0x98 bit0**). idx 0xFF = "no song".
- **FUN_80074E48** = "stop current BGM": if 0x800bed80 != -1, SsSeqStop then sets 0x800bed80 = 0xFFFF.
  FUN_80074BC4 (callers all overlay/stage code) is "stop-all-sound" → calls it on scene transitions.
- **FUN_80075A80** = per-frame sound-command service (callers = stage handlers 0x80106480..0x80108d4c):
  processes a command queue; a request byte @0x800be22a drives start/stop of BGM.
- SsSeqCalled (0x80090BD0) advances any slot with +0x98 bit0 set. Slot struct = (lw 0x80104c30) +
  i*0xB0; flag @ +0x98; read ptr @ +0x00, SEQ base @ +0x04. Active BGM slot for the newgame scene = **slot 4**.

### Proof the chain works (native port, PSXPORT_NO_FMV=1, FORCE_BUTTONS=FFF7)
- f166 BGM_START(idx=4) fires (ra=0x80074608). **slot 4 read ptr ADVANCES** every frame: base
  0x8018245C → +84 over f166-225 (logged by the [bgmtick] probe). f260 BGM_START(idx=2): slot 2 read
  ptr advances +167. So sequences DO activate and tick.
- **WAV capture (scratch/wav/bgm_nofmv.wav, PSXPORT_WAV, FMVs OFF) has real signal**: RMS 3000-4700 at
  t≈5.5-7.5s (= f166-225, the idx-4 BGM) and t≈9s (idx-2). So the SEQ→SPU→mix path produces audio.
  (Reaching scenes via FORCE_BUTTONS=FFF7 (pulse Start) — and disabling intro FMVs with PSXPORT_NO_FMV=1
   so the capture/frame-timing isn't the FMV player. Without NO_FMV you're capturing the FMVs, not the game.)
- Oracle reference (newgame.sav, fisherman scene): 0x800bed80 = **4**, stable 1500+ frames, no rewrites
  → the oracle holds BGM 4 at that steady scene. Native at f166 ALSO starts BGM 4 (matches).

### What's actually wrong (the narrowed target)
The intro/fisherman BGM plays in our port (audio confirmed) — consistent with the user hearing it. The
silent cases are **specific scenes**: the **menu** and **steady gameplay** (grassy field). There, either
the stage's BGM-start (FUN_80074BF8 via a stage handler) never fires, or a spurious stop/scene-state
divergence kills it. The 0/1 start/stop oscillation I first saw was an ARTIFACT of forced-Start spam
(Start = pause menu, which stops BGM); not a real bug. NEXT: reach the menu and steady gameplay with NO
spurious input (REPL, minimal taps, no pause), confirm whether their BGM-start fires, and if not, find the
stage-handler precondition our native scheduler/boot path doesn't satisfy. Compare the per-frame command
queue @0x800be22a / stage handler calls of FUN_80075A80 vs the oracle at the same scene.

### Gotchas / tooling this session
- **PSXPORT_NO_FMV=1** to skip intro FMVs (else screenshots/WAV/frame-timing are the native_fmv player).
- Oracle REPL `tap`/`press` did NOT advance the title menu from title.sav nor newgame.sav this session
  (input not reaching the menu, or those states idle waiting on stalled CD stream) — could not navigate
  the oracle live; used the RAM-dump snapshots (scratch/bin/tomba2/state/{demo,newgame,title}.bin) and
  newgame.sav idle instead. Native FORCE_BUTTONS=FFF7 DOES drive menus.
- PSXPORT_BGMDBG=1: logs every BGM start/stop with caller ra + the per-frame [bgmtick] read-ptr probe.

## 2026-06-16 (later 74) — BGM: USER-CONFIRMED it's the SPU music MIX (not the output path). Sequenced BGM songs are OPEN but never ACTIVATED — the per-sequence play flag (struct+0x98 bit0) is clear, so SsSeqCalled skips them (read ptr frozen). Jingle/SFX work. Built interactive REPL + watchpoints + screenshots to drive both cores.
Tooling built (user request): native-port REPL (PSXPORT_REPL, FIFO-driven, persistent) mirroring the
oracle's `-repl`; commands run/r/rw/w/w8/watch/unwatch/hits/press/release/tap/regs/seq/shot/stage;
runtime memory watchpoints (mem_set_watch, reports writer PC); `shot <path>` (gpu_native_shot → PPM);
tools/repl2.py drives native|oracle|both. Oracle gained `watch`/`unwatch`.

### Headless samples → USER GROUND TRUTH (scratch/wav/{menu,navigated}_sample.wav)
Generated headless SPU captures; user listened: **NO sequenced BGM** in menu or gameplay (only the
fisherman FMV music + a quest jingle + SFX/footsteps/pause-menu). So headless == windowed: the SPU
music MIX lacks BGM → it is NOT a windowed-output-path bug. The jingle + SFX DO play → the
sequencer/SPU CAN make sound; only the BGM SONGS are silent.

### Root-cause localization (via the REPL at the menu)
- The menu BGM sequence (struct 0 @0x800BE3D8) is OPEN with a real SEQ data ptr (0x80182380, in the
  verified-correct level-data region) and L/R volumes set — but its read pointers DO NOT ADVANCE over
  200 frames (frozen). 
- SsSeqCalled (0x80090BD0) only advances a sequence if **struct+0x98 bit0** (the active/play flag) is
  set. struct 0's flag @0x800BE470 = **0x00000000** (clear) → skipped → frozen → silent.
- So the BGM songs are allocated/opened (playmask 0xC3FF, 14 open — identical to the oracle) but never
  ACTIVATED: the per-sequence play flag is never set (SsSeqPlay/activation not firing for them), while
  the jingle/SFX sequences DO get activated.

### NEXT (the fix)
Find what SETS struct+0x98 bit0 (SsSeqPlay-equivalent) for a BGM song and why our port never calls it
for the menu/gameplay BGM (the jingle/SFX path works). Drive the oracle to a steady-BGM scene
(read structs 7-13 play flags too — slots 0-6 were clear at the pig scene), find which struct is the
active BGM there (flag bit0 set), then watch that flag's writer PC in the oracle (= the activator),
and check why our port's path doesn't reach it. Likely the scene/menu BGM-start code our boot path
skips, or an SsSeqPlay precondition. Tools ready: REPL `watch` on struct+0x98 in both cores.

## 2026-06-15 (later 73) — BGM: the libsnd sequencer INFRASTRUCTURE is correct (state matches the oracle); dialogue issue RETRACTED. Missing BGM is in finer per-sequence state, needs a GAME-stage sequence-table compare vs the oracle.
User refined the symptom: Menu (no BGM) → Story cutscene (no BGM) → **Fisherman (BGM plays)** →
Gameplay (no BGM). Only the fisherman has BGM = it's FMV/XA (native_fmv), everything else is the
libsnd SEQUENCER → so ALL sequenced BGM is silent, one common cause ("fix menu, fix the rest").
**Dialogue is a NON-ISSUE** (user retracted): the "missing dialogue" was the fisherman-cutscene
dialog; the user's spam-Start runs just got stuck on it; vanilla Start skips it. Drop it.

### Method (user directive): track GAME STATE, not output (no WAV/RMS — that's [[track-game-state-not-output]])
SsSeqCalled (0x80090BD0) reads the libsnd state: **0x801054B0** = open-seq count, **0x80104C28** =
playing bitmask, **0x80104C30** = sequence table, **0x800AC424** = tick mode, **0x800AC42C** =
SsSeqCalled ptr. Probe: PSXPORT_SEQDBG (native_boot.c). Reach GAME headless via
PSXPORT_FORCE_BUTTONS=FFF7 (pulse Start; active-low mask) — confirmed reaches GAME stage.

### Finding: sequencer infra is CORRECT — not the bug
- OURS over START→DEMO→GAME: open=14, playmask=0xC3FF, tickmode=5, seqfn=0x80090BD0 — STABLE the
  whole run (only changes at f0→f3 during boot).
- ORACLE @ green-field DEMO (oracle_ram_7000): **identical** — open=14, playmask=0xC3FF, tickmode=5,
  seqfn=0x80090BD0.
⟹ The sequencer is set up and "playing" identically to the oracle. BUT the DEMO is correctly SILENT
in both with this same 0xC3FF — so playmask is TOO COARSE to indicate audible BGM (14 slots stay
allocated; per-scene the game swaps the SEQ song / ramps per-slot volume). The missing BGM is in the
FINER state: which SEQ song / volume each slot plays, or whether the VAB (instrument samples) is in
SPU RAM. later-54's headless "music" (RMS) was likely these always-allocated slots, not real per-scene BGM.

### NEXT
RE the sequence struct (table @0x80104C30, stride from SsSeqCalled's loop) → per-slot {SEQ data ptr,
volume, current song}. Dump it from OURS at GAME (FORCE_BUTTONS=FFF7) AND the ORACLE at GAME
(-inputscript pulse-Start), diff → find which song/volume/VAB differs. Also verify the VAB reached SPU
RAM (the game's SsVabTransfer → SPU data-port/DMA). The native port has NO loadstate, so GAME must be
reached via input each time (oracle has -loadstate, but the oracle plays BGM correctly so it can't
show OUR bug — only serves as the reference state to diff against).

## 2026-06-15 (later 72) — GRAPHICS CORRUPTION FIXED (user-verified). Root cause: our GPU never handled VARIABLE-LENGTH POLY-LINES — it consumed a fixed 3/4 words, drifting the whole GP0 parse so a later data word decoded as an atlas-clobbering VRAM copy.
THE BUG WAS IN OUR PC GPU after all (gpu_native.c gp0_len / FIFO). gpu_differ misled me (later-59/71):
its captured INITIAL VRAM at f3265 was ALREADY clobbered by the prior frame, so both renderers
"reproduced" the garbage — it never tested a clean start.

### Root cause (offline OT analysis, scratch/otanalyze.py)
Walking the real ordering table from its root (0x800EC114) and parsing with CORRECT lengths: the drift
originates at OT node 0x800DDF00, command 0x5EF8F8F8 = **op 0x5E, a gouraud poly-line** (4 vertices =
9 words, = the node's count). PSX poly-lines (line group 0x40-0x5F with bit 0x08; 0x48-0x4F mono,
0x58-0x5F gouraud) are VARIABLE length — a vertex list terminated by a word with
(w & 0xF000F000)==0x50005000 (0x55555555). Our gp0_len returned a fixed 4 ("poly-line term not
modeled"), consuming 4 of 9 words → parse drifts by 5 → ~3000 words later a data word (0x8040333D)
lands at a command boundary and is executed as a GP0 0x80 VRAM->VRAM copy onto the atlas (later-69/70).

### Fix (gpu_native.c)
- gp0_len: 0x80 (VRAM->VRAM copy) is 4 words, not 3 (was grouped with A0/C0 headers — also a real bug).
- gpu_gp0: detect poly-lines at command start and ACCUMULATE words until the terminator (vertex-start
  slots: gouraud even idx>=4, mono idx>=3), then render. s_fifo grown 16->256 (poly-lines are long).
- gp0_exec line branch rewritten to draw N-1 segments over the full vertex list (single lines = 2
  verts = 1 segment, unchanged).
**USER-VERIFIED: graphics are fine now** (green-field/dungeon/gameplay all render correctly).

### Remaining issues (user ground-truth, next)
- Missing BGM almost everywhere (only the fisherman "pull Tomba from water" cutscene BGM plays); menu
  BGM also missing (earlier "menu has no BGM = vanilla" was WRONG).
- Missing dialogue at the start of gameplay (Tomba & Zippo dialogue doesn't play).
- User hypothesis: we may be initializing/running in DEMO mode (would explain missing dialogue, and
  possibly the BGM triggers). Investigate a DEMO-vs-GAME mode flag.

## 2026-06-15 (later 71) — CONFIRMED the clobber is a corrupted ORDERING-TABLE entry in RAM, NOT our GPU. gpu_differ at the ACTUAL clobber frame (f3265): replaying the same word stream through OUR renderer AND Beetle both clobber the atlas identically. So the bad VRAM-copy is in the display-list words from RAM; the fix is upstream (whatever builds/corrupts the OT).
gpu_differ (later-59) had only ever tested f3360/f5000, never the f3265 clobber. Tested it now:
- PSXPORT_GPUTRACE="3265:…" captures the 10815-word GP0 stream fed at f3265 (post-DMA-traversal).
- replay_ours vs `wide60rt -gpureplay` of that SAME stream: atlas (320,0) 92% same, (576,0) 100% same
  (the 8% is the known UV-round/dither residual, later-44). BOTH renderers execute the copy and clobber
  the atlas. ⟹ the bad 0x80 copy is genuinely in the word stream from RAM; our GPU/parser is faithful
  (re-confirmed). The clobber word 0x8040333D lives at 0x800D8FD4 as a DATA word (last word of a
  gouraud-tri, immediately followed by a 0x090D8044 OT header) — i.e. the OT stream is structured so a
  data word lands at a command boundary and is executed as a MoveImage.

### Where this leaves the fix
The display list (ordering table) our interpreted game-logic/HLE builds at ~f3265 is corrupted (a
node count / next-pointer / buffer is wrong, so the DMA feeds a misaligned stream that decodes a data
word as an atlas-clobbering VRAM copy). The OT lives in 0x800Cxxxx-0x800Exxxx, exactly the dynamic
region that diverges from the oracle (later-60). This is upstream of the GPU entirely.

### NEXT (unavoidable: from-boot OT-corruption hunt)
The transplant masks accumulation, so go from-boot. Find what writes the bad OT: PSXPORT_CW watchpoint
on the OT buffer around f3265 (filter to the offending node/words), or sequence-diff the OT-build path
(ClearOTagR @0x80081458 / AddPrim / the draw-enqueue) vs the oracle. Identify the routine that
mis-builds the list (a wrong word-count in a primitive, a stale double-buffer OT index DAT_1f800135,
or an HLE memcpy/alloc that overruns the OT) and PC-own/fix it. NO bandaid (do not just skip the copy).
Tooling: gpu_differ now builds again (replay_ours.c defines g_ram); PSXPORT_TEXWATCH logs node+words;
PSXPORT_CLOBBERDUMP dumps RAM at the clobber.

## 2026-06-15 (later 70) — the clobbering copy is a MALFORMED OT NODE (garbage words read as a VRAM copy). Bad words come from the OT in RAM, not our GPU parser. Root cause = a bad OT node our port builds and the oracle doesn't.
The f3265 atlas-clobbering 80copy (later-69) is node **0x800D4B9C**, raw words
**8040333D, 34383838, 005A0040, 38FF322E**. A real libgpu MoveImage cmd word is 0x80000000; this is
0x8040333D with ASCII-ish operands (0x34383838="8884", 0x2E=".", 0x3D="=") ⟹ these are DATA/garbage
words being read as a GP0 0x80 VRAM→VRAM copy (which lands on the atlas: dest (64,90) 558x255).
gpu_differ (later-59) already showed Beetle reproduces our garbage from our own captured GP0 WORDS, so
our GPU parser is fine — **the bad words are in the OT (game RAM)**, fed to GP0 by the DMA. So our port
builds (or corrupts) a bad OT node at ~f3265 that the oracle does not (the OT region 0x800Cxxxx-0x800Exxxx
is exactly where our RAM diverges from the oracle, later-60).

### Reconciles every prior result
- VRAM-only transplant clean (later-65): the clobber (f3265 ≈ present 3265) happened BEFORE the
  transplant (logic 1680 ≈ present 4267); the transplant replaced the clobbered atlas with clean and the
  one-time clobber didn't recur ⟹ stayed clean. RAM-only no effect: the VRAM damage was already done and
  RAM transplant doesn't repaint VRAM.
- Decompressor/load/upload byte-perfect (later-69): the atlas WAS correct (f2591 == oracle 100%); the
  clobber is the only corruption.

### The real bug, restated
A malformed OT node (0x800D4B9C) appears in our display list around f3265 and decodes as a spurious
VRAM copy that overwrites the atlas. It is upstream of the GPU — in the interpreted game logic / HLE
that builds or owns that OT buffer (a wrong word-count, a stale/garbage pointer, or a buffer the OT
node refs that got clobbered). This is the SAME class as the long-standing "dynamic region
0x800C-0x800E diverges from oracle."

### NEXT (the from-boot divergence hunt — the call-sequence harness the user approved)
The transplant masks accumulation, so go from-boot: find the FIRST frame the OT (or its source struct)
diverges from the oracle, via a sequence-keyed differential (log the OT-build / AddPrim / the function
that emits node 0x800D4B9C's command, sequence-numbered, in both engines; diff). Identify the
instruction/HLE that writes the bad word. Watch 0x800D4B9C-area stores around f3265 (PSXPORT_CW /
oracle PSXPORT_WATCHW). The owning routine, once found, gets PC-owned per the user directive.

## 2026-06-15 (later 69) — DECOMPRESSOR/LOAD/UPLOAD ALL CORRECT. The atlas is decompressed byte-perfect then CLOBBERED by a VRAM→VRAM copy (80copy dest=(64,90) 558x255 @f3265). The corruption is a spurious/mistimed VRAM copy, not the asset pipeline.
Big correction to later-66/67/68 (the reused-scratch made the input look wrong; it is not).

### The decompressor is FAITHFUL — atlas is correct right after unpack
- The oracle does the SAME load+unpack sequence we do (oracle calltrace via PSXPORT_CALLTRACE +
  psxport_hooks.cpp loader logging): same LBAs unpacked (6613/6625/6717/6774), 2020 loaded-not-unpacked
  — IDENTICAL to ours. So NOT a sequencing bug; 2020 is not the atlas.
- The decompressor offset table @0x800153C8 is byte-identical ours vs oracle.
- **Texpage (320,0) right after our 6625 unpack (present f2591) is 100.0% identical to the oracle**
  (oracle_vram_7000). So our decompress+upload of the atlas is byte-perfect. (later-66/67's "input
  52.9%/wrong" was the reused-scratch 0x18A000 ghost — the LIVE input matches the disc 100%.)

### …then it gets CLOBBERED
- Our (320,0) at present f4267 is only 4.7% == its f2591 self ⟹ overwritten between f2591 and f4267.
- PSXPORT_TEXWATCH="320,0,512,256": exactly ONE write hits the atlas in that window —
  **f3265 80copy src=(56,56) dest=(64,90) 558x255** (a VRAM→VRAM block copy). dest (64,90)+558x255
  spans x64-622 y90-345, swallowing the atlas texpages (320,0)-(512,256). This single copy accounts
  for the whole (320,0) change.

### So the real bug
A VRAM→VRAM copy overwrites the already-correct atlas. The decompressor/CD-read/upload are all
RULED OUT (byte-perfect). The copy either (a) shouldn't run / has wrong coords/size in our port, or
(b) is mistimed (ordering vs the atlas load — sync-vs-async CD), or (c) its source (56,56) holds
garbage in our port. The oracle's atlas survives at f7000, so the oracle either doesn't do this copy,
does it before the atlas load, or to different coords.

### NEXT
Capture the oracle's GP0 0x80 (VRAM→VRAM) copies (hook GPU in wide60rt, or gpudump) around the
green-field demo and compare to ours: same copy? same coords/size? same timing relative to the atlas
unpack? Then find what issues our f3265 copy (GP0 0x80 origin in the OT / a libgpu MoveImage) and why
it lands on the atlas. Tools added: psxport_hooks.cpp loader/unpack calltrace (oracle), the early/late
VRAM compare recipe (PSXPORT_VRAMDUMP at two frames), PSXPORT_TEXWATCH.

## 2026-06-15 (later 68) — mapping the loader/streaming dispatcher; two load paths found. Need the ORACLE's load/unpack LBA sequence to pin the correct atlas (reused-scratch ambiguity). User directive: PORT the loader+streaming to PC-native.
Caller (ra) logging on ov_cd_loadfile + ov_unpack_group: the load+unpack path that fills the atlas is
**FUN_80044f58** (loadfile call ra=0x80045008 i.e. the jal at 0x80045000→FUN_8001dc40; unpack
ra=0x8004501C i.e. jal 0x80044e84 @0x80045014, anchor 0x1fd000, then a 42-word descriptor copy to
0x800fb178/0x8010-4e90). Our run unpacks ONLY 6625→(320,0) and 6774→(576,0) via this path. The
LBA-2020 load comes from a DIFFERENT site (ra=0x80045430 = jal FUN_8001dc40 @0x80045428) and is
followed by 0x80045558/0x80045258 processing — NOT the FUN_80044E84 unpacker.

### Open ambiguity (must resolve before fixing)
"Oracle atlas = LBA 2020" (later-67) was inferred from oracle 0x18A000 @f7000 = LBA 2021, but 0x18A000
is REUSED scratch — at f7000 it may just hold the LAST load (2020), while the live atlas was
decompressed from an EARLIER load since overwritten. So I do NOT yet know the correct atlas source.
Two live hypotheses: (a) demo-SEQUENCE divergence — our port streams/unpacks DIFFERENT areas
(6625/6774) than the oracle by the same scene point (our sync CD vs async timing, or skipped-intro
demo state); (b) a later load+unpack the oracle does and we miss. Both are streaming-sequence bugs.

### Plan (user: port the loader+streaming to PC-native)
1. Instrument the ORACLE (wide60rt, psxport hooks) to log every loadfile (FUN_8001db8c/dc40) and
   unpack (FUN_80044E84) with LBA/dest/size, run to the green-field hut, and DIFF the sequence vs ours
   (scratch/logs/ra.log) → find the first divergence (which area/atlas, when).
2. Then PC-own the loader+unpack sequencing (it's asset-streaming infra, not game logic) so the right
   atlas is unpacked deterministically when its load completes — instead of relying on the async
   IRQ/state-machine our no-IRQ port desyncs. Verify: green-field renders clean with NO transplant.

## 2026-06-15 (later 67) — ROOT CAUSE: the correct atlas (LBA 2020) is LOADED but NEVER UNPACKED. Our texpages keep a PRIOR area's atlas. Load→unpack streaming desync (synchronous native CD, missing async completion trigger).
Nailed it (continuing later-66). The oracle's green-field atlas decompresses from **disc LBA 2021**
(discscan on oracle_ram_7000 @0x18A800 = 100% consecutive from LBA 2021; i.e. ov_loadfile of LBA 2020
table + 2021 data). We DO load LBA 2020 (cd.log: "loadfile 448512 B @ LBA 2020 -> 0x8018A000").

### The smoking gun (one run, CD_VERBOSE + UNPACKLOG + UPLOADLOG)
Across the WHOLE attract run there are exactly **two** atlas (count=10) unpacks, both from the WRONG
source, and the atlas texpages are uploaded exactly twice:
- LBA 6625 → unpack → upload (320,0) 192x256  (f2590)
- LBA 6774 → unpack → upload (576,0) 256x256  (f3054)
- **LBA 2020 (the correct atlas) is loaded (line ~2421) and then NO count=10 unpack ever follows it.**
So our VRAM atlas at (320,0)/(576,0) holds the decompressed LBA 6625/6774 textures (a PRIOR area),
never the hut atlas (LBA 2020). The oracle unpacks LBA 2020 by its frame 7000; we never do. Our
logic-frame 1681 ≈ oracle 7000 (same area, level data identical), so we SHOULD have unpacked it by
then. Same disc data (our LBA-2020 load == oracle's LBA-2021 input), same decompressor ⟹ if we
unpacked it we'd get the clean atlas.

### Root cause class: CD-streaming / load→unpack sequencing desync
The atlas unpack is triggered by game logic AFTER the load completes. Our native CD is SYNCHRONOUS
(ov_loadfile / ov_cd_read complete instantly) and we deliver NO preemptive IRQ; the real game streams
asynchronously and sequences load→unpack via completion callbacks / a state machine (cf. the CD
streaming contract notes, FUN_8001d7c4 per-sector IRQ callback that our no-IRQ runtime never fires).
The instant-vs-async timing desyncs that state machine, so the geometry advances to the hut scene
while its atlas is loaded-but-unpacked (or the unpack trigger is missed entirely). This is consistent
with the user's "accumulates as you play" — each new area whose atlas-unpack trigger is missed keeps a
stale/wrong atlas; effect sprites etc. degrade as more areas stream.

### NEXT (the fix)
Find what triggers the count=10 atlas unpack (FUN_80044E84) after a loadfile in the real game — the
event/callback/flag the game polls — and ensure it fires in our port when the corresponding load
completes (deliver the completion event, or drive the unpack from the load-complete path). Verify:
after the fix, a count=10 unpack from LBA 2020 should appear and upload to (320,0)/(576,0), and the
green-field render should match the oracle WITHOUT any transplant. Trace via the loadfile→unpack
caller chain (who calls FUN_80044E84, gated on which flag) and the LBA-2020 loadfile's caller.

## 2026-06-15 (later 66) — corruption traced to the ATLAS LOAD: decompressor input is the WRONG DATA for the scene, though our CD read is faithful-to-disc (consecutive 100%). The atlas load targets the wrong disc location.
Continuing later-65 (corruption = VRAM atlas content). Traced the atlas pipeline backward.

### The atlas is decompressed from 0x8018A000 by the unpacker (FUN_80044E84)
PSXPORT_UNPACKLOG/UNPACKDUMP (games_tomba2.c): the green-field atlas is a count=10 unpack group at
table 0x8018A000, data at +0x800, decompressed to the 0x1Exxxx scratch then uploaded to the texpages
(576,0 256x256, 320,0 192x256, …). 0x8018A000 is REUSED scratch (5 unpacks/run: c2,c10,c2,c10,c2,
filled by ov_loadfile from LBAs 6613/6625/6717/6774/2020 — so static LBA compares there are unreliable;
capture LIVE in the override instead).

### The decompressor INPUT is wrong, but our READ is faithful to the disc
- Same-scene RAM compare (ours logic-frame 1681 vs oracle frame 7000, same area — level data
  0x158000/0x182000 == oracle 100%): the compressed-atlas region **0x18A000 is only 52.9% == oracle.**
- BUT our LIVE unpacker input (captured in the override, scratch/raw/unpackdump/unpack_*_c10.bin)
  matches the disc **CONSECUTIVELY 99.99%/100%** (data@+0x800 == disc LBA 6626 / 6775; tool
  scratch/bin/discscan: finds the best first-sector LBA then measures consecutive match). ⟹ our
  ov_loadfile reads exactly the disc's consecutive bytes — **the CD read is faithful** (re-confirms
  later-60; submode 0x08 is just this disc's data convention, NOT XA interleave — the verified-good
  level load at LBA 1908 is also submode 0x08).

### So we load faithful-but-WRONG data
The data at LBA 6625/6775 is read correctly but it is NOT the atlas this scene needs: the oracle's
atlas (oracle_vram_7000) renders OUR geometry CLEANLY (later-65 VRAM-only transplant), and we are in
the SAME area (level data identical), so the correct atlas for our scene == the oracle's. We loaded a
different one. ⟹ the bug is the **LBA/file our atlas loadfile targets** (FUN_8001db8c a1=lba) — wrong
disc location for this area's atlas — OR a demo/sequence divergence selecting a different atlas. NOTE
the dynamic game-state region (0x800C0000+) diverges from the oracle (different demo MOMENT, same
area), so a sequence/timing divergence that changes WHICH atlas is live is plausible and must be
checked.

### NEXT
Instrument the oracle's ov_loadfile-equivalent (FUN_8001db8c @0x8001db8c) to log dest/lba/size, run to
the green-field hut scene, and diff the load sequence vs ours (scratch/logs/cd.log). Find where our
atlas LBA (or the area-load order) first diverges from the oracle's, then trace WHY (the LBA is
computed by interpreted game logic from a directory/manifest — a wrong directory entry, wrong file
handle, or an earlier state divergence). Tools added: PSXPORT_UNPACKLOG/UNPACKDUMP (games_tomba2.c),
scratch/bin/discscan (find a buffer's disc LBA + consecutive match).

## 2026-06-15 (later 65) — CORRUPTION ISOLATED TO VRAM TEXTURE CONTENT via a state-transplant harness. RAM + per-frame logic + per-frame CLUTs are all CORRECT; the scene-load atlas textures baked into VRAM are wrong.
Built a transplant harness (PSXPORT_TRANSPLANT="frame:ramfile:vramfile", native_boot.c +
gpu_native_load_vram) that drops the ORACLE's clean green-field state into our RUNNING port at a
logic-frame boundary and CONTINUES. The user predicted "transplant will mask the bug" — correct, and
that masking IS the diagnostic.

### Oracle dumps (green-field, frame 7000): scratch/raw/oracle_ram_7000.bin + oracle_vram_7000.bin
(oracle: PSXPORT_RAMDUMP=7000:.. + PSXPORT_VRAMDUMP=7000:.. ; wide60rt <disc> -bios scratch/bios -frames 7100)

### Results (transplant at our logic frame 1680; effect visible from present f4267 — boot turbo makes
the logic→present map nonlinear, so find the transplant point by first-diff vs baseline, not by math)
- **Full transplant (RAM+VRAM) → CLEAN.** Our per-frame logic renders a perfect green-field (Tomba on
  grass) and STAYS clean for 78+ frames. ⟹ the per-frame rendering/logic is CORRECT; it does not
  corrupt good state.
- **RAM-only transplant → byte-identical to the no-transplant baseline (ZERO render effect).** The
  game RAM state is NOT the differentiator (our logic re-derives the same scene; transplanted RAM
  doesn't rebuild the already-built VRAM). ⟹ game RAM state is fine.
- **VRAM-only transplant → CLEAN (same as full).** Replacing ONLY our VRAM with the oracle's fixes the
  render completely.
⟹ **The corruption is entirely in our VRAM TEXTURE CONTENT.** And since the VRAM-only run KEPT our RAM
yet the per-frame 16x1 CLUT re-uploads (5315/run, sourced from our RAM 0x801FCDC0) did NOT re-corrupt
the clean VRAM over 78 frames, **the per-frame CLUTs are also correct.** The wrong data is the
SCENE-LOAD ATLAS textures (the big 256x256/192x256/… uploads), baked into VRAM once at load.

### So the bug is in the scene-load texture pipeline: decompress → upload
Upload is faithful (later-63, native). So either the DECOMPRESSED output is wrong, or textures are
placed at wrong VRAM coords, or the descriptor that drives it is wrong. NOTE this re-opens the
decompressor: later-61 only proved native==recomp==interp (consistency across OUR engines), never
correctness vs the oracle — and the "clean-C is faithfulness-independent" argument only covers
load-delay (now ruled out), not other subtle bugs or wrong INPUT addressing. NEXT: diff our green-field
VRAM ATLAS region (camera-independent, loaded once) against the oracle's to see WHICH texpages are
corrupt, then trace which decompress/upload call produced them. (Atlas is scene-static so this compare
is alignment-SAFE, unlike the framebuffer.)

## 2026-06-15 (later 64) — LOAD-DELAY RULED OUT as the corruption cause (measured, not assumed). Zero genuine load-delay hazards in the executed rendering code. New tooling: PSXPORT_LDHAZARD detector + PSXPORT_VRAMDUMP.
The prime remaining hypothesis (interp.c:13 "no load-delay slot") was that the interpreter's
omission of the R3000 load-delay slot makes game-logic draw-param code compute wrong UVs/CLUT/geom.
**Tested it directly — it is NOT the cause.**

### Detector (PSXPORT_LDHAZARD, interp.c) — counts REAL divergences
A load-delay divergence is: instruction I-1 loads a GPR, and the very next executed instruction I
reads that GPR (so our no-delay model gives the new value, hardware/oracle gives the old). The
detector follows TRUE execution order, INCLUDING jump/branch delay slots (a load in a delay slot is
checked against the branch TARGET, not the memory-next word). It excludes the benign lwl/lwr
unaligned-merge idiom (our no-delay model already merges those correctly: lwl commits, lwr merges).
- **Runtime, full attract incl. green-field (f3360) AND dungeon (f5000), 2600 logic frames: 0
  genuine hazards.** The overlays (gameplay logic, rec_coro_run) have NONE.
- **Static scan of all of MAIN.EXE (scratch/ldscan.py): 44 raw hits, ALL in the DATA region**
  (addresses ≥0x800A0000; highest recompiled function is 0x8009D06C) — pointer/asset tables misread
  as code, not real instructions. Zero in actual code.
- Why: the SN/GCC toolchain fills every load-delay slot (nop or an independent op), so compiler
  output never reads a load target in the next slot. Only hand-asm would, and the lwl/lwr pairs
  (the only load→next-reads-target cases that DO occur) are handled correctly already.
⟹ Adding faithful load-delay would be effort + regression risk for a NON-cause. Do not implement it
to "fix" the corruption. (It's still legit CPU correctness if we go interpreter-only, but it won't
change a pixel here.) RULED OUT — do not re-chase.

### Also this session
- **PSXPORT_VRAMDUMP="frame:path"** added to gpu_native.c (raw 1024x512x16), matching the oracle's
  same-named knob (main.cpp). Cross-engine VRAM diff at green-field (ours f3360 vs oracle f7000):
  atlas region differs, but that is the alignment ghost (different demo moment) — ours actually has
  MORE nonzero than the oracle in the atlas; NO black/missing-upload cells (cellcmp.py). Inconclusive
  for the bug (the documented alignment block), but rules out "a texture is missing from VRAM".
- Confirmed (POLYDUMP f3360 + f3720): every textured poly's texpage lands inside an uploaded atlas
  region (x≥320) → texpage selection is CORRECT. Some per-frame CLUTs are real palettes (1008,255),
  some are uniform 0x1084 (800,198 / 496,488) — uniform-palette = suspect but not yet pinned.

### Where the bug stands (elimination)
Ruled out: rasterizer, CD-read, XA, unaligned-SWR, raw-load, decompressor, **upload library**
(later-63), **load-delay** (this entry). Texpages correct; atlas content correct by argument
(native decompressor is faithfulness-independent yet byte-identical to recomp/interp). Remaining:
wrong UVs / wrong CLUT-INDEX per poly / wrong per-frame CLUT CONTENT / wrong vertex geometry —
all computed by interpreted game logic — OR a wrong/missing HLE the render path relies on. The
decisive tool is still a scene-aligned oracle lockstep (find FIRST RAM divergence via a shared
logic-frame counter); cross-moment VRAM/RAM diff stays blocked.

## 2026-06-15 (later 63) — GPU UPLOAD LIBRARY now PC-native (user directive: "PC-native GPU, not faithful"). RULES OUT the upload library as the corruption source — fully native upload reproduces the SAME garbage byte-near-identically.
User directive (corrects later-62's "faithfully port the vtable"): the GPU library should be **PC-native,
not a faithful recomp**. So instead of byte-translating the libgs vtable, I replaced the upload entry
point with a native VRAM blit and tested whether the corruption changes.

### What was done
- **FUN_80081218 (0x80081218) → `ov_upload_image` (games_tomba2.c) + `gpu_native_load_image`
  (gpu_native.c).** Empirically RE'd (PSXPORT_UL_PROBE + the A0 UPLOADLOG, exact match): a0 =
  descriptor { x:s16@0, y:s16@2, w:s16@4, h:s16@6 }, a1 = source = w*h contiguous 16-bit pixels,
  row-major. The recomp body ENQUEUES into the GsSortObject ring @0x800A5AC8 (head/tail @5AC8/5ACC,
  mod 0x40), DMA'd later as a GP0 0xA0 packet. **It is the SINGLE chokepoint for BOTH the scene-load
  texture atlas (256x256/192x256/128x256… into the character texpages) AND every per-frame 16x1 CLUT
  — 5315 CLUT + ~25 atlas calls per attract run.** Native impl writes the rect straight to s_vram and
  does NOT enqueue (later ring flush/sync no-ops over the empty ring). A/B: PSXPORT_LZ_RECOMP=1 keeps
  the recomp upload library. The unpacker (ov_unpack_group) routes its uploads here too → the whole
  decompress→upload asset path is now PC-owned.

### RESULT — upload library RULED OUT as the corruption source
A/B over 5187 presented frames (native vs PSXPORT_LZ_RECOMP=1), `cmp` each PPM:
- **5038 byte-identical; 149 differ, all in the green-field demo f3270–3775, by ~0.4% (≈300px/76800).**
- The differing pixels are a CLUT upload-TIMING offset (native writes immediately at the enqueue call;
  recomp defers to the ring flush — likely a 1-frame CLUT lag, so native is plausibly *more* correct).
- **The corruption is UNCHANGED.** f03360 (striped grass/black blocks/garbled char) and f03720
  (magenta+RGB-noise striped pillars) are visually identical native vs recomp; f03720 is byte-identical.
  ⟹ a fully PC-native upload reproduces the same garbage ⟹ **the upload library is not the cause.**

### So where is the bug now (narrowed)
Source texture data in RAM is correct (later-60: ==oracle 100%); decompressor byte-perfect
(later-61); upload faithful (this entry); rasterizer faithful (later-59). The garbage is
"uninitialized VRAM sampled as texture" (magenta + RGB noise). With correct textures + correct
upload + correct raster, that leaves the **DRAW STREAM PARAMETERS** (UVs / texpage / CLUT-index /
geometry) computed by the **interpreted gameplay overlay** — i.e. either wrong draw params, or
MISSING uploads (the overlay logic that decides what/where to upload computed wrong). Prime suspect
remains the interpreter's faithful-first quirks (no load-delay slot, add==addu — interp.c:13) that
both our engines share but the oracle does not; the gameplay overlays run THROUGH this flat
interpreter (interp.c:3). NOTE the decompressor ran clean through that same interpreter, so the quirk
is path-specific (geometry/UV code may have load-delay deps the codec does not). NEXT: determine
whether our VRAM atlas is correct vs oracle at the green-field scene (static, scene-alignable) — if
yes, the bug is in draw params (interpreter arithmetic), if no, in upload placement.

## 2026-06-15 (later 62) — porting non-gameplay subsystems to native (user goal). Decompressor + texture-unpacker now PC-owned & verified byte-identical. Next subsystem mapped: the libgs-style gfx/upload vtable.
Per the user's directive ("have anything EXCEPT gameplay logic PC-owned"), porting the asset/gfx
library out of recomp+interp into native C, one subsystem at a time, A/B-verified vs the recomp body
(PSXPORT_LZ_RECOMP=1) until the gameplay 2D-sprite corruption surfaces/resolves.

### Done this session (committed, each byte-identical to recomp over the full attract run)
- **LZ image decompressor** FUN_80044D8C → `ov_lz_decompress`/`lz_decompress` (games_tomba2.c).
- **Texture-group unpacker** FUN_80044E84 → `ov_unpack_group`. Reads descriptor table (count +
  12-byte entries {stride@+4, field@+6, srclen@+8}; packed source 0x800 past the table); per entry
  dst = anchor − 2*stride*field, native-decompress, then FUN_80081218 (upload) + FUN_80080f6c.
  NOTE: both ports are behaviour-neutral so far (frames identical) — they don't fix the corruption;
  they're steps toward full PC ownership. The recomp is faithful, so the divergence is elsewhere.

### NEXT subsystem mapped: the gfx/upload library (libgs/GsSortObject-style vtable)
- The gfx context object is the STATIC struct at **0x800A5958** (pointer cached at 0x800A5998; state
  byte at 0x800A59A2, =0/1/2 selects handlers). Its function-pointer fields (read from a RAM dump):
  - obj+0x08 = **0x80082D04** ← the actual upload method (FUN_80081218 calls this with
    a0=mem_r32(obj+0x20)=0x80082734, a1=descriptor, a2=8, a3=dst).
  - obj+0x0C = 0x80082504, obj+0x1C = 0x80082970, obj+0x20 = 0x80082734, obj+0x3C = 0x80083364
    (used by FUN_80080f6c).
- FUN_80081218 also calls FUN_80080fd4(template=0x8001BF2C, descriptor) first — a 3-way state
  machine on 0x800A59A2 → handlers 0x80081010 / 0x8008109c / 0x800810e0.
- To PC-own the upload path: port FUN_80081218 + FUN_80080fd4 + FUN_80080f6c and the method
  0x80082D04 (and whatever it dispatches). This is the prime remaining non-gameplay subsystem and the
  most likely home of the corruption (or an HLE/recomp quirk it relies on). Tooling: PSXPORT_TEXWATCH
  (VRAM-rect upload trace), PSXPORT_CW (RAM store watchpoint), PSXPORT_UPLOADLOG.

### Obstacle to differential debugging (documented, do not re-fight)
Cross-engine RAM/VRAM compare is blocked by frame-alignment: our native-boot frame numbering ≠ the
oracle's, and the attract demo's dynamic CLUT/area cycling means our f3340 ≠ oracle f7000 for dynamic
state (static level data DOES align 100%, but front-buffer/CLUT do not → 0% at arbitrary frames).
So verify ports by A/B (native vs recomp, same engine) + visual, not by oracle RAM diff.

## 2026-06-15 (later 61) — DECOMPRESSOR RULED OUT (recomp==interp==native byte-identical); 0x801FCDC0 is transient scratch not the live CLUT; loaded data is disc-correct. New direction (user): port ALL non-gameplay-logic to native.
Chased the per-frame CLUT at 0x801FCDC0. Findings, including a falsified hypothesis (kept honest):

### What the corruption looks like (user ground-truth, windowed + headless repro)
2D effect sprites corrupt and ACCUMULATE: gems → water splashes → eventually "busted" (magenta
polys + RGB-noise stripes = uninitialized memory sampled as texture). 3D chars/terrain often clean;
it's the 2D sprite/CLUT path. Reproduces headless: f5000 dungeon demo = character is a rainbow blob,
2D counter garbled (`scratch/frames/view_05000.png`). "Corrupted differently each time" windowed =
OS-threading races (user: don't chase determinism; headless IS deterministic).

### FALSIFIED hypothesis (do NOT repeat): "decompressor recomp-vs-interp divergence"
`gen_func_80044D8C` is the LZ image decompressor (2D predictors: offset[i]=base+2*factor*stride from
the static table @0x800153C8; ctrl byte → len=ctrl>>3, mode=ctrl&7; mode0=literal, else back-ref).
I caught it (PSXPORT_CW host-backtrace store watchpoint, added to mem.c) writing ZEROS to 0x801FCDC0
via the flat interpreter, while the recompiled path wrote the correct CLUT — looked like a
recomp-vs-interp divergence. **It is NOT.** Rendered frames are BYTE-IDENTICAL across the entire
5947-frame attract run whether the decompressor runs native, recompiled, or flat-interpreted
(`scratch/frames/{after,before}` cmp: 5947 same / 0 differ). The "zeros at 0x1FCDC0" is a TRANSIENT
decompression-scratch leftover — the unpacker `gen_func_80044E84` sets dst = anchor(0x1FD000) −
2*stride*field, decompresses each texture into that (intentionally overlapping) scratch, then
immediately uploads it to VRAM via `0x80081218` (vtable dispatch through the gfx object @0x800A5998).
The LIVE CLUTs/textures are in VRAM, not RAM — comparing 0x1FCDC0 in a RAM dump was an alignment ghost.

### Ruled out / verified this session
- **Decompressor** (recomp==interp==native, byte-identical over the whole run). NOT the cause.
- **Loaded compressed source is disc-correct**: scratch 0x8018A000 at f3340 == disc LBA 2636 100%
  (tool `scratch/bin/srccmp` — links disc.c, compares a RAM-dump region to disc sectors).
- 0x80158000 / 0x80182000 static level data == oracle 100% (scene base aligns). 0x8018A000 is a
  REUSED per-area scratch (loaded many times from different LBAs) → cross-frame RAM compare there is
  meaningless (dynamic; our demo-moment ≠ oracle's for the dynamic CLUT cycling).

### Changes kept (verified, aligned with the new goal)
- **LZ decompressor is now PC-owned** (`ov_lz_decompress` in games_tomba2.c; A/B via
  PSXPORT_LZ_RECOMP=1). Verified byte-identical to recomp over the full run. First step of the goal,
  but does NOT fix the corruption by itself.
- **interp.c rec_coro_run**: tail-`j` (op 0x02) now routes through coro_native_call, so native
  overrides / BIOS vectors fire on tail-jumps too (were bypassed — only jal/jalr/jr checked).

### Direction (user): "have anything EXCEPT gameplay logic PC-owned"
Port the texture/gfx/upload library to native C and keep going until the corruption surfaces or
resolves. NEXT native targets: unpacker 0x80044E84, upload 0x80081218, the gfx object @0x800A5998
(libgs/libgpu-style; method table at obj+8/+1c/+20, obj+0x3c). Root cause still OPEN — likely in the
gfx/upload library, or a shared recomp+interp faithful-first quirk (no load-delay slot, add==addu)
that both engines share but the oracle doesn't.

## 2026-06-15 (later 60) — corruption RULED-OUT list grows: NOT unaligned-SWR (latent SWR bug found+fixed but never hit), NOT CD-read, NOT XA-interleave. Corruption is in the gameplay texture UPLOAD/VRAM-content path; data loads correctly.
Continued from later-59. The corruption is in the VRAM texture atlas content (gpu_differ replays from
the trace's INITIAL VRAM and both renderers reproduce the garbage ⇒ it's baked into VRAM by prior
uploads, not per-frame rasterization).

### Found + fixed a REAL but LATENT bug: `mem_swr` preserve-mask (mem.c)
`mem_swr`'s keep-mask was `0x00FFFFFFu >> (32-sh)` — wrong; it zeroed the low bytes SWR must PRESERVE
on every unaligned store (verified vs canonical LE tables, `scratch/bin/uatest`: k=1/2/3 gave
e.g. `ADBEEF00` vs correct `ADBEEF88`). Fixed to `0xFFFFFFFFu >> (32-sh)`. LWL/LWR/SWL verified
CORRECT. BUT: **instrumenting (PSXPORT_UASWR_DBG) shows unaligned SWR is hit ZERO times in the demo**,
and post-fix demo frames are **byte-identical** to pre-fix (`cmp` f3345/f3386/f5000/f4600) — so this
fix does NOT change the corruption. Kept anyway (real correctness fix; a future unaligned SWR would
corrupt). NOT the cause.

### Ruled out (do not re-chase)
- **Rasterizer** (later-59: gpu_differ front-buffer IDENTICAL to Beetle on f3360+f5000).
- **CD data read**: reads happen (big level loads 188K/333K/448K → staging 0x8018A000/82000/58000;
  single-sector stream → 0x800EF478). Sectors at the gameplay LBAs are all **Mode2 Form1 DATA**
  (submode 0x08), consecutive — so consecutive-sector reading is correct; loaded bytes = the disc
  bytes the oracle also reads. Tool: `scratch/bin/secprobe` (links disc.c, prints submode per LBA).
- **XA interleaving** of the texture region: falsified (no Form2 audio sectors interleaved there).
- **Unaligned SWR**: never hit (above).
Staging RAM at a gameplay frame looks like real data (89–94% nonzero, high uniqueness) — but proving
it correct needs a SAME-SCENE compare vs the oracle (level data is static once loaded, so that
compare is alignment-insensitive — the decisive next test).

### RESULT of the same-scene RAM compare: LOADED DATA IS BYTE-PERFECT (load/decompress ruled out)
Dumped recomp RAM at a green-field gameplay frame and oracle RAM at its green-field frame (7000), then
compared. The texture/level-data staging regions **0x80158000 / 0x8018A000 / 0x80182000 match the
oracle at 100.0%** (768 KB byte-identical across two different engines ⇒ same scene AND correct data).
**So the loaded+decompressed texture data in RAM is correct; load/decompress is NOT the bug.** Tools:
recomp `PSXPORT_RAMDUMP_FRAME=N PSXPORT_RAMDUMP=path`; oracle `PSXPORT_RAMDUMP=frame:path`.

Full 2 MB aligned diff: only the **dynamic object/game-state region 0x800C0000–0x800E0000 (~68–77%)**
and low kernel RAM (0x0–0x10000, 22%) diverge — but that is almost certainly **frame-WITHIN-scene**
difference (the demo animates; I can align to the scene via static data, not to the exact frame), not
corruption. So the RAM diff is inconclusive for the corruption itself.

### Upload path mapped (new tooling: PSXPORT_UPLOADLOG in gpu_native.c — logs A0 dest/dims + DMA src)
- Intro/title (CLEAN): per-frame **full-frame 320×240 CPU→VRAM uploads to (0,0)** from 0x8001FB9C
  (pre-rendered frames — that's why the intro path is clean; no per-poly texturing).
- Green-field gameplay: **textures uploaded ONCE at scene-load** (f3054) to the atlas texpages the
  character samples — e.g. dest (576,0) 256×256, (896,0) 128×256, (832,0) 64×256 — streamed from a
  **high-RAM DMA staging area 0x801Fxxxx** (0x801FC800/FC600/FC580…). Then **CLUTs (16×1) re-uploaded
  EVERY gameplay frame** from **0x801FCDC0** to the exact CLUT slots the character uses
  ((1008,227),(1008,255),(800,190),(816,205)…).
- CRUCIAL: the texture/CLUT DMA source (0x801Fxxxx) is a **DIFFERENT region** from the raw level-data
  staging (0x80158000/8A000/82000) that matched the oracle 100%. So the raw load is correct, but the
  **processing step that fills the 0x801Fxxxx DMA buffer (decompress / CLUT build)** is unverified and
  is now the prime suspect — it runs in the interpreted gameplay overlay (DEMO.BIN/GAME.BIN).

### Next (root cause — still open)
Verify the 0x801Fxxxx DMA-staging contents (texture pixels + the per-frame CLUT at 0x801FCDC0) against
the oracle. Needs EXACT-frame alignment (this buffer is rebuilt per frame / per load), not just
scene-alignment — find the game's logic-frame counter (a RAM word incrementing once/logic-frame,
identical in both engines until divergence) and dump both at the same value, OR break on the
fill routine. Then diff the staged texture/CLUT bytes: a mismatch pins the mistranslated
processing instruction. (mem_swr fix is committed but is NOT the cause; rasterizer, CD-read,
XA-interleave, unaligned-SWR, and raw-load all ruled out.)

## 2026-06-15 (later 59) — GAMEPLAY CORRUPTION ISOLATED: it is NOT the rasterizer — the recompiled CPU/HLE produces a PROGRESSIVELY-CORRUPT GP0 stream (bad RAM-sourced texture/CLUT data). Beetle reproduces our garbage byte-near-identically; the oracle running the real game is clean.
User (windowed, ground truth): in-game graphics "grow more and more garbage as you play" — garbage
blocks, missing sprites, melted geometry; fishing-rod teleports in the non-FMV intro cutscene. Also
asked to **sync native vs oracle at attract** and observe. (Audio: attract has NO BGM in BOTH native
and oracle = normal; only the real game has gameplay BGM — that's the SEPARATE streamed-CD-audio gap,
later-54, untouched here. Menu-cursor tempo (later-58) USER-CONFIRMED correct.)

### Repro (headless attract — corrects the handoff's "attract renders coherently")
The attract **gameplay DEMO** reproduces it; the early intro (SCEA, character cutscenes, TOMBA 2
title) is coherent, but once the DEMO starts the scene is corrupt and **worsens over frames**.
- Native recomp port (`scratch/bin/tomba2_port`), headless 2500 logic frames → ~5087 presents:
  green-field demo lightly wrong by f~3345 (striped ground, garbled character, a black block),
  dungeon demo HEAVILY garbled by f5000 (character = vertical magenta/yellow rainbow bar).
  `scratch/logs/native_progression.png`, `native_fishing.png`, `native2_late.png`,
  `native2_f05000_full.png`.
- Oracle (`wide60rt`, full Beetle interpreter on the same disc) renders the SAME demo scene **clean**
  — smooth grass, clean sprites, no stripes/blocks: `scratch/logs/oracle_007000.png`. So the real
  game's draw stream is correct; ours is not.

### Decisive isolation (gpu_differ on captured GP0 traces — `tools/gpu_differ/`)
Captured our live GP0 stream at f3360 and f5000 (`PSXPORT_GPUTRACE`), replayed each through BOTH our
rasterizer (`replay_ours`) and Beetle (`wide60rt -gpureplay`), diffed VRAM:
- f3360: front buffer **IDENTICAL**, back buffer 165 px small-Δ (UV-round/dither residual, later-44).
- f5000 (heavily corrupted live): front buffer **IDENTICAL**, back buffer 248 px small-Δ only.
- Rendering both replays to PNG vs the LIVE frame: **all three identical, INCLUDING the garbled
  character** (`scratch/logs/replay_vs_live_5000.png`). Beetle faithfully reproduces our garbage from
  our own GP0 words.
⟹ **The rasterizer is correct.** The corruption is in the GP0 word stream itself, which is built from
main RAM (`gpu_dma2_*` read via `mem_r32`). The draw commands are structurally fine (POLYDUMP f5000:
textured gouraud tris, sane texpage/CLUT/geometry for the character) → the corruption is in the
**texture/CLUT DATA in VRAM**, uploaded from RAM. It **degrades over time** ⟹ progressive **RAM
corruption by the recompiled MAIN.EXE / native HLE** (a recompiler mistranslation or a missing/wrong
HLE the game relies on for memory/texture management). This is upstream of the GPU entirely.

### Tooling fixed
`tools/gpu_differ/replay_ours.c`: added stubs for `wide60_join_poly`/`ws_sx_dump` (added to
gpu_native.c by later-55/57) so the standalone differ links again. `tools/gpu_differ/build.sh` now
builds clean.

### Refinement (user ground-truth, 2026-06-15 — corrects "starts clean")
The gameplay demo is **corrupt from its FIRST frame** (f3345 already striped/garbled/black-block), not
"starts clean then degrades" — it's corrupt the instant gameplay textures come into use, then worsens.
Dividing line confirmed by capture: **SCEA screen, character close-up cutscenes, and the TOMBA 2 title
all render CLEAN**; only the **3D gameplay demos** corrupt (green-field AND dungeon both corrupt from
their first frames — `scratch/logs/transition.png` shows clean title f4150–4400 → already-corrupt
dungeon f4450+). User also reports the corruption hits **everything roughly together** (player +
terrain + sprites), not one sprite-load path first ⟹ **wholesale** corruption of a shared
texture/VRAM region, not a single sprite pipeline. Clean 2D screens use big flat sprites / few
textures; corrupt scenes are the 3D gameplay path (many textured polys + streamed sprite uploads),
which runs largely from the recompiled **DEMO.BIN/GAME.BIN overlays** — a prime suspect (overlay
recompilation or the gameplay texture-upload path). Whether corruption resets on scene load = TBD.

### Next (root cause — NOT yet found)
Need a **differential RAM trace** vs the oracle to find the FIRST divergence (frame + address), then
map to the mistranslated instruction / missing HLE. No lockstep recomp-vs-oracle RAM harness exists
yet; the hard part is determinism/alignment (different engines: recomp C vs mednafen interpreter —
timers/RNG/uninit RAM differ benignly). Build that next. Do NOT re-chase the rasterizer — it is ruled
out, bit-near-exact vs Beetle on real captured gameplay (f3360 + f5000).

## 2026-06-15 (later 58) — AUDIO TEMPO FIX: per-vblank work was running 1× per logic frame instead of quota× → audio at HALF real-time tempo windowed (user heard the menu-cursor tick too slow). later-54's "tick once" reasoning corrected.
User (windowed, ground truth): "menu cursor movement audio is playing at wrong tick … should be
faster tick." Diagnosis confirmed it's a general half-tempo bug, not menu-specific.

### Root cause (corrects later-54)
`spu_audio_frame()` advances the SPU by exactly ONE 1/60 s field (one vblank). later-54 ticked the
libsnd sequencer ONCE per `ov_frame_update` to "match" that one field. But **one `ov_frame_update` is
one LOGIC frame**, which spans `DAT_1f800235` (=quota=2 for Tomba2's 30 fps) vblanks of wall time. So
the per-vblank work — BOTH the sequencer tick AND the SPU field advance — was running 1× when it must
run quota× to hold the hardware 60 Hz rate in real time. Result: windowed (paced to quota/60 s per
frame) the SPU was fed at half rate and the sequencer ticked at 30 Hz → **everything at half tempo**.
The headless WAV HID this: its timeline is field-count, not wall-clock, so 1 tick : 1 field is still
60:60 = correct-sounding (why later-54's WAV check passed and the user OK'd that capture).

### Fix (`games_tomba2.c` ov_frame_update)
Run the per-vblank work `quota` times per logic frame: `for (v=0; v<quota; v++) { seq_tick;
spu_audio_frame(); }`. Adaptive — a true-60fps scene (quota=1) ticks once. Keeps the tick:field ratio
(hence BGM tempo) unchanged; fixes real-time playback. PSXPORT_T2_NOSEQTICK still opts out the tick.

### Verified (headless)
- WAV for 3200 frames is now **106.67 s** (was 53.33 s, later-54) = 2 fields/frame for quota=2 — the
  SPU now advances at real-time rate. tick:field ratio unchanged ⇒ BGM tempo preserved.
- **VRAM at f3000 byte-identical** to the pre-change baseline — the change is audio-isolated, no
  rendering regression. (Sequencer ticked 2× changes only audio RAM/SPU state, not the GPU path.)
- **Tempo correctness in real-time needs the USER's ears (windowed).** The headless WAV cannot certify
  wall-clock tempo (that's exactly what masked the bug). User to confirm the cursor tick now sounds right.

## 2026-06-15 (later 57) — 60fps: OBJECT→PRIMITIVE JOIN validated on the native path (92–99% of gameplay polys tag to an object). The design's #1 open risk is retired.
Built milestone 2 of the 60fps tier: the object-identity join the matcher is founded on.

### Mechanism (all in `wide60.c` + thin taps, gated PSXPORT_WIDE60)
- `games_tomba2.c` `ov_object_cull` overrides the per-object cull/LOD dispatcher **0x8007712C**
  (a0=object*, once/logic-frame per live drawable): sets `g_current_object = a0`, super-calls
  `gen_func_8007712C` unchanged, restores. Registered only when wide60 is on.
- `gte_beetle.c` `gte_op` RTP tap → `wide60_rtp(op)`: for each SXY the GTE just pushed (DR14 for
  RTPS, DR12/13/14 for RTPT) it stamps an **SXY→object grid** (epoch-stamped 1024×512, no per-frame
  memset) with `g_current_object`. So every projected vertex carries the object whose cull-subtree
  projected it.
- `gpu_native.c` `gp0_exec` polygon tap → `wide60_join_poly(v0.x,v0.y)`: joins each drawn poly's
  lead vertex to a captured SXY (±2px). Packet vertex coords are pre-draw-offset = the same space as
  the SXY-FIFO (confirmed), so the join is direct.

### Result (headless gameplay to f5000, `scratch/logs/wide60_join.log`)
**poly-join = 92–99% across gameplay** (f1000 98.1%, f2500 99.0%, f5000 98.1%; dips to ~77% on a
menu/transition frame). I.e. nearly all drawn 3D geometry is object-matchable; the unjoined remainder
is CPU-projected terrain / 2D HUD that snaps by design. This is the native-path confirmation the doc
asked for (emulator-era was 97%-within-2px). Caveat: a high join RATE proves viability, not that each
join picks the *correct* object vs a coincidental nearby SXY — within-object disambiguation
(draw-order + prim type + texpage + CLUT/uv-low) is the matcher's job (next milestone).

### Faithful-path safety
PSXPORT_WIDE60 unset → cull override not registered, all taps no-op; VRAM at f3000 byte-identical to
the pre-wide60 baseline (`cmp` PASS).

### Next (60fps, remaining)
Per-frame primitive capture (full display list A/B), the matcher (object-id key → token key → snap),
the transform-lerp+reproject (or screen-XY-lerp fallback) synthesizer, `gpu_replay_list` for the
in-between, and 60 Hz host present. No-flicker rule: unmatched geometry held at frame A.

## 2026-06-15 (later 56) — 60fps tier started: MEASURED Tomba2 = 30 fps (was only "believed"). Rate detector live in the port, gated, faithful path byte-identical.
Pivoted from widescreen (blocked, later-55) to the 60fps interpolation tier
(`docs/wide60_recomp_60fps.md`). First milestone = actually measure the logic rate.

### New file `runtime/recomp/wide60.c` (gated PSXPORT_WIDE60; additive)
Owns the wide60 capture/rate-detector/(future)matcher+synthesizer. Milestone-1 content:
- `wide60_geom_xy()` — folds the GTE's projected SXY (DR12/13/14 after RTPS/RTPT, tapped in
  `gte_beetle.c` `gte_op`) into a per-frame FNV-1a fingerprint. SXY output is **parity-invariant**
  (depends only on logic inputs, not the double-buffer draw target) — the right cadence signal.
- `wide60_frame_commit()` — per-logic-frame fence, called from `games_tomba2.c` `ov_frame_update`
  (the proven once-per-logic-iteration chokepoint). Feeds the lrate_proto detector: counts
  consecutive `ov_frame_update` calls with an identical projected-geometry set; modal run-length =
  content-change period. A frame with zero GTE ops (FMV/menu) folds a constant = HOLD (no spurious change).

### Measured (headless to f5500, gameplay; `scratch/logs/wide60_rate.log`)
**modal-period = 1 call/change, engine vblank quota (DAT_1f800235) = 2 vbl/frame → content ~30.0 fps.**
Votes unanimously period-1 (p1=3601, p2=p3=p4=0) — every logic iteration produces a fresh projected
set. So Tomba2's logic IS 30 fps and each frame is intended for 2 vblanks. **For 60 fps: synthesize
exactly ONE in-between per logic frame** (lerp transforms at t=0.5, reproject). This retires the
long-standing "Tomba2 framerate unverified — measure, don't assume" (CLAUDE.md).

### Faithful-path safety
With PSXPORT_WIDE60 unset, all taps are no-ops; VRAM at f3000 is **byte-identical** to the
pre-change baseline (`cmp` PASS). The 0.000% raster / audio fix are untouched.

### Next (60fps, remaining)
Per-object transform capture (tag RTP ops via the `0x8007712C` dispatcher → object id), primitive
capture/match by object id in `gpu_native.c`, the lerp+reproject synthesizer, and `gpu_replay_list`
to rasterize in-betweens. Then host-pace 60 Hz present. No-flicker rule: unmatched geometry held at
frame A, never half-interpolated.

## 2026-06-15 (later 55) — WIDESCREEN blocker found: VRAM is packed (textures abut the 320-wide FB) → the design doc's "wider buffer, square pixels, no stretch" is architecturally impossible. Measured the GTE projects ~14% of verts past the 4:3 edges.
Started the wide60 widescreen tier (handoff: do widescreen first). Built two RE tools and ran them
against real gameplay; the result overturns the central premise of `docs/wide60_recomp_widescreen.md`.

### Tools built (permanent)
- `tools/vram_png.py` — dump a raw 1024×512 16bpp VRAM (`PSXPORT_VRAMDUMP_AT`) to PNG, optional
  magenta outline of the displayed region. Used to read the live VRAM layout.
- `gte_beetle.c` `ws_sx_record`/`ws_sx_dump` (gated `PSXPORT_WS_SXHIST`) — histogram the screen-X the
  GTE writes to SXY-FIFO per RTPS/RTPT, dumped every 500 frames from `gpu_present`. Measures how much
  geometry the GTE projects outside the 320-wide window.

### What the GTE projects (gameplay f3000–5500, `scratch/logs/sxhist.log`)
Display window is [0,320). Of all projected vertices: **~10–15% land at SX<0, ~24–36% at SX≥320.**
Per-bucket (f5000): near-left band `[-64,0)` ≈ 3%, near-right band `[320,384)` ≈ 11%. So the GTE
DOES project substantial world geometry just past the 4:3 edges (with spikes at the saturation
extremes = behind-camera / off-screen rejects). Wide geometry is *computed* — the open question was
only whether it's drawable.

### The blocker (decisive — `scratch/screenshots/vram_f5000.png`)
The VRAM is **fully packed**: two 320-wide framebuffers stacked at (0,0) and (0,256) (~224 tall;
display flips between them — vramscan), and the **texture/CLUT atlas fills x≥320 immediately to the
right of the FBs**, across all rows. The only free VRAM is the thin vertical bands rows 224–255 /
480–511 — horizontal, not usable to widen a FB. Therefore:
- **You cannot widen the framebuffer to 427 in place** — x∈[320,427) is the live texture atlas;
  drawing world geometry there corrupts the textures it then samples.
- **The doc §5 "present a WS_WIDTH-wide region centered on the display origin" is wrong** — copying
  VRAM [s_disp_x−53 .. +374] copies the texture atlas as the right third of the screen (garbage).
- **The doc §1 IR1×0.75 multiply is ALSO wrong for a square present** — it squishes content 0.75×
  horizontally; presented 1:1 that's a thin/squished world (confirmed by first-principles + the
  prototype's own numbers). The 0.75 multiply only makes sense paired with a downstream 1.333×
  STRETCH (the Beetle hack) — which the doc explicitly set out to avoid.

### What real hor+ widescreen actually requires here (none are an autonomous win)
1. **Squish-stretch (Beetle hack, the only one that fits packed VRAM):** keep the 320 FB, apply the
   IR1×0.75 multiply (more world squished into 320), present-STRETCH 320→16:9. Fits VRAM, standard
   PSX-widescreen technique. Quality = native-h-res stretched (acceptable, not "better-than-Beetle").
   STILL needs the cull-cone widen to populate the new edge bands.
2. **Separate wide FB backing store (what the doc imagined "owning the rasterizer" buys):** render FB
   draws into our own 640-wide off-screen buffer at recentered coords while textures stay in s_vram.
   Feasible (we own gpu_native.c) but doubles rasterization and must mirror FB read-backs; and STILL
   needs the cull-cone widen.
3. **Relocate the texture atlas to free FB columns:** rewrite every texpage/CLUT coord the game
   programs — deep, fragile per-game RE.

**Common unavoidable dependency: the per-game CULL-CONE widen.** The six `slti` sites (§2 of the doc)
gate draw-enqueue in *world/camera* space, so the squish/recenter does NOT bring culled edge geometry
back — without widening them the new edge bands stay empty regardless of present strategy. And cull
widening is the exact change that previously caused walk-through ghosts (needs the visibility/activation
split RE **and the user's eyes** to validate — not completable while the user is away).

### Decision
Widescreen is NOT an autonomous-completable win: it forces the squish-stretch approach (overturning the
doc) and is hard-blocked on a risky per-game cull RE that needs user validation. Recorded here; the doc
is corrected. **Pivoting this session to the 60 fps interpolation tier** (`wide60_recomp_60fps.md`),
which renders into the existing FB (no VRAM-packing wall), is gated/additive (faithful path stays
byte-identical), and has a proven RE basis — the tractable, verifiable deliverable. First 60fps
milestone: actually MEASURE Tomba2's logic rate (a long-standing project unknown) via the rate detector.

## 2026-06-15 (later 54) — AUDIO FIXED: native libsnd sequencer tick → port plays SPU-voice music (was all-zeros)
Implemented the later-53 fix (user picked "native sequencer tick"). The port now produces music.

### What it was (one more RE step past later-53)
- The libsnd setup is `FUN_80090750 = SsSetTickMode`; runtime globals (read from a loaded state via
  the REPL): tick mode `DAT_800ac424 = 5`, sequencer pointer `DAT_800ac42c = 0x80090BD0`
  (`SsSeqCalled`), user-cb `DAT_800ac430 = 0x80086288`, `DAT_800ac434 = 0`. The VBlank IRQ runs the
  tick **wrapper `FUN_800909c0`** = (optional user cb) + `(*SsSeqCalled)()`.
- **Neither `0x800909c0` nor `0x80090bd0` is emitted by the static recompiler** — they're only ever
  reached through the IRQ callback pointer, never a direct `jal`, so the indirect-call discovery
  never saw them (classic recomp coverage gap). They run fine through the hybrid interpreter.

### Fix (games_tomba2.c `ov_frame_update`)
Call `rec_dispatch(c, 0x800909C0)` once per `ov_frame_update` (→ interpreter, bit-identical, runs
the wrapper to its `jr ra`). This is "port the HW interrupt work to PC" (the busy-wait-porting
rule), NOT IRQ simulation. Guarded on the sequencer pointer `mem_r32(0x800AC42C)` being a sane code
address so we never call through null before `SsStart`. Opt out (A/B): `PSXPORT_T2_NOSEQTICK`.
- **Tick rate = ONCE per `ov_frame_update`, NOT VBLANK_QUOTA(=2) times.** `spu_audio_frame()`
  advances the SPU exactly one 1/60 s field per call (3200 frames = 53.33 s audio = 60 fields/s),
  and on hardware the sequencer ticks once per VBlank (60 Hz realtime) while the SPU plays
  realtime — so the faithful ratio is 1 tick per SPU field. The quota=2 is the game's 30 fps
  *display* pacing and is irrelevant to audio tempo vs SPU time; ticking twice plays music 2x fast
  (first cut did this — fixed).

### Verified (headless, `scratch/logs/ours_seqtick1x.log`, `scratch/wav/ours_seqtick1x.wav`)
- Port `PSXPORT_SPU_DBG`: per-note KON went from **~10 (init only) → thousands** (val=2000,1000,00CC,
  0010,0020,0003,…); `spu_render peak` 0 → 4691…21270 every frame.
- WAV: was **all-zeros**; now 53.33 s, **RMS ~4267, 94% non-silent, peak 21270** — same loudness
  band as the oracle's KON menu music (~3000, later-53). Reaches frame 3200 / gameplay, no crash.
- TEMPO is reasoned-correct (1 tick / SPU field = hardware 60 Hz). **User should confirm audibly**
  (windowed run, no `PSXPORT_NOAUDIO`) — the only thing a headless RMS check can't certify is the
  exact musical tempo/pitch.
- **No rendering regression:** full 1 MB VRAM dumped (`PSXPORT_VRAMDUMP_AT`) at f600 (menu) and
  f3000 (gameplay grass) is **byte-identical** with the sequencer tick ON vs `PSXPORT_T2_NOSEQTICK`
  — the fix is isolated to audio; geometry/raster (the 0.000% later-51/52 result) is untouched.

### Tooling hygiene
`vendor/beetle-psx spu.c` oracle SPU trace is now gated on `#ifdef __LIBRETRO__` (oracle-only) — the
same spu.c links into the port too, which has its own `spu_beetle.c` log; this stops double-logging
and keeps the port build free of the oracle-only `PSX_CPU` reference.

### Still open (smaller, separate)
FMV/menu CD audio through the SPU (`CDC_GetCDAudioSample` stub) is still silent on the SPU path, but
FMVs play via `native_fmv.c` (own device). If a scene wants CD-DA track 2 / XA mixed through the SPU
during gameplay, that remains a separate, smaller gap (later-53 candidate #1) — not the music bug.

## 2026-06-15 (later 53) — AUDIO root cause PINNED via oracle: music sequencer is TIMER-IRQ-driven; the port delivers no preemptive IRQ → silent. (corrects later-52's candidate order)
Built the missing oracle-audio tooling and used it to pin the silent-port root cause. The
answer corrects later-52's prioritisation: it is **candidate #3 (an IRQ-driven sequencer the
port never ticks)**, NOT candidate #1 (streamed CD music). Evidence, all cited below, gathered
with the new tooling.

### New tooling (committed)
- **Oracle WAV capture — `PSXPORT_WAV=path` in `runtime/main.cpp`** (`AudioBatchCb`): dumps the
  FULL-Beetle (`wide60rt`) mixed 44100 Hz stereo to WAV, headless, independent of `-play`/SDL.
  Same format the port's `spu_audio.c` writes, so `tools/audio_differ/compare.py` diffs them
  directly. This is the ground-truth reference the handoff said was needed.
- **Oracle SPU trace — `PSXPORT_SPU_DBG=1` in `vendor/beetle-psx/mednafen/psx/spu.c`** (fork):
  logs KON (with the interrupted CPU `BACKED_PC` of the writer), SPUCNT (enable/cdaudio/irq/xfer
  on change), and a per-10s `CDmix` counter (how many output samples actually had CD-audio
  enabled + nonzero). Mirrors the port's `PSXPORT_SPU_DBG`. Env-gated, harmless when unset.
- Port-side `PSXPORT_SPU_DBG` extended (`spu_beetle.c`): now also counts voice-region/KOFF/CDVol
  writes + periodic SUMMARY, and logs the SPUCNT CD-audio bit.

### What the oracle does (ground truth — `wide60rt`, real Beetle, boots Tomba2 normally)
Headless boot, Start held to skip logos (`scratch/inputs-skipintro-long.txt`), 9000 frames /150s
(`scratch/wav/oracle_boot_long.wav`, `scratch/logs/oracle_boot_long.log`):
- **0–100s = the opening FMVs**: heavy XA CD-audio. `CDmix` climbs to ~3.25M nonzero CD samples;
  SPUCNT briefly C001/C081 (cdaudio bit0=1) during playback. This is FMV/STR audio.
- **~120–160s = post-FMV menu/attract**: `CDmix` PLATEAUS (≈no new CD audio) yet per-10s RMS is
  ~2800–3200 at 78–99% non-silent (`scratch/win_rms.py`). So that audio is **SPU-VOICE music,
  not CD audio** — confirmed by 96 per-note KON events in this run.
- **KON writer PC histogram: 54×`pc=80050CE8`, 7×`80050CEC`, 6×`80050CE4`** — i.e. every per-note
  key-on is written while the CPU is parked in the **per-frame pace-dwell loop `0x80050CE4`**.
  That can only happen from a **preemptive interrupt handler** firing during the dwell.

### Why the port is silent (the mechanism)
- The game installs a custom IRQ dispatcher (`FUN_800016e4`) that services VBLANK, GPU, CDROM,
  DMA, **Timer0 (0xf0000005), Timer1/2 (0xf0000006), SIO, and SPU (0xf0000009)** event classes.
  The music sequencer runs inside one of the **Timer/SPU IRQ** handlers (it fires during the
  dwell — see the KON PCs above).
- The port models **only VBLANK** (`timing.c` delivers `0xF2000003/0xF0000001` once per `VSync`).
  It delivers **no Timer IRQ and no SPU IRQ**, and it **collapses the pace-dwell** itself
  (`games_tomba2.c ov_frame_update` sets the vblank counter so the dwell falls through). So the
  sequencer ISR has neither a trigger nor the dwell to fire in → **zero per-note KON → the SPU
  mixes silence** (verified: port `spu_render peak=0` every gameplay frame; only the all-voices
  init KON pattern + 162 KOFF housekeeping writes ever fire — `scratch/logs/audio_base.log`).
- **FMV audio is NOT this bug and already works**: the port decodes FMV XA natively in
  `native_fmv.c` through a *dedicated* SDL device, not the SPU mixer / `CDC_GetCDAudioSample`.
  So the FMVs (the most audio-rich part) play; the gap is the SPU-VOICE in-game/menu music.

### Disproven / corrected
- **later-52 candidate #1 (streamed CD music is the bulk gap) — WRONG ordering.** The non-FMV
  music is SPU-voice (KON), not CD audio. `CDC_GetCDAudioSample` being stubbed only costs FMV
  CD-audio-through-SPU (which the port routes around natively anyway). Wiring it would not bring
  back the menu/gameplay music.
- **VBlank callback `0x800506b4` is confirmed (disassembled, `generated/shard_6.c`) to be ONLY
  `lhu/addiu/sh` on the dwell counter `0x800E809C`** — a pure counter bump, NOT the sequencer.
  The journal was right about that; the sequencer is a *different*, Timer/SPU-IRQ handler.

### The proper fix (named; not yet implemented — scope decision for the user)
Make the port run Tomba2's sound-engine tick. Two routes:
1. **Preferred (matches the port's "port the busy-wait to PC, don't simulate the HW" rule):** find
   the sequencer tick entry (the libsnd `SsSeqCalled`-equivalent the Timer ISR calls) and invoke
   it from `ov_frame_update` at the timer's configured rate. Needs one more RE step (the EvCB
   registration for `0xf0000005/6` → callback addr) + the tick rate (so tempo is correct — naive
   once-per-frame would play slow).
2. **More faithful but bigger:** actually deliver the Timer (and SPU) IRQ events per frame at the
   modelled timer rate so the game's own dispatcher runs the ISR. This re-introduces preemptive
   IRQ delivery the port deliberately stripped — larger, with regression risk to the tuned HLE
   boot, so it needs sign-off.
Both need the timer rate to get tempo right; that is why this is a scoped subsystem task, not a
one-liner. Validate either against `oracle_boot_long.wav` (the menu-music window) via the differ.

## 2026-06-15 (later 52) — multi-frame GPU sweep (GPU is solid everywhere) + audio tooling + audio is SILENT (root-caused)
User: "you only compared the first few things… also add audio and audio compare tooling." Did both.

### GPU: validated across 8 scenes, not just one frame — no new structural bugs
Extended PSXPORT_GPUTRACE to capture a COMMA-SEPARATED frame LIST in one deterministic run
(`"600,1000,..,3150:prefix"` → `prefix_f<N>.bin`; single-frame exact-path form unchanged, differ
pipeline intact). Swept f600..f3150 (menu→gameplay), replayed each through ours + Beetle, diffed
both buffers. Result: **the drawn buffer is 0.000% (pixel-identical to the oracle) on ALL 8 frames**
(back buffer 0,256 when it holds the frame; the other buffer shows the same ≤0.4% sub-pixel
affine/edge floor from later-51). So after later-51 there are NO new structural GPU divergences
across scenes — the rasterizer matches the oracle everywhere sampled. (scratch/bin/sweep/, diff_all.sh.)

### Audio tooling added (user ask)
- **PSXPORT_WAV=path** (spu_audio.c): dumps the SPU's mixed 44100 Hz stereo output to WAV,
  INDEPENDENT of SDL — works headless / under PSXPORT_NOAUDIO (the mixer is advanced + drained
  regardless; refactored spu_audio_frame so the SPU runs when EITHER the SDL device OR a WAV
  capture is active). Header patched at exit, ~10 min cap.
- **tools/audio_differ/compare.py**: `stats` (duration, RMS/peak, silence %, first-audible — flags
  all-zeros) and `diff` (xcorr-align ours-vs-oracle then loudness/peak deltas; alignment-free like
  the GP0 differ since HLE vs full-emu don't sample-align). Pure stdlib.
- **PSXPORT_SPU_DBG=1** (spu_beetle.c): register-write count, SPUCNT (enable+xfer mode), KON masks,
  SPU-RAM DMA/data-port transfers, per-frame spu_render peak — to separate a mixing bug from a
  driver/IRQ problem.

### FINDING: the port is SILENT — VERIFIED facts + an HONEST (not-yet-pinned) root cause
A 55 s headless gameplay capture (incl. the f3000 grass scene) is **all zeros**. PSXPORT_SPU_DBG
shows the SPU itself is healthy: enabled (SPUCNT bit15=1), master volume 0x3FFF, SPU RAM uploaded
(359 DMA writes × 256 words ≈ 368 KB), voices keyed on at init. VERIFIED facts:
- Over 2500 frames only **5 KON writes, all the all-24-voices init pattern (0x188=FFFF/0x18A=00FF)
  — ZERO per-note key-ons**. The SPU mixes 735 zero samples/frame.
- Steady SPUCNT = C020 / C0A0: **SPU-IRQ-enable (bit6) = 0** and **CD-audio-enable (bit0) = 0**.
  So the sound path is NOT driven by the SPU IRQ (this DISPROVES my first guess that the stubbed
  `IRQ_Assert` was the cause) and CD audio is not steadily mixed through the SPU here either (bit0
  flips to 1 only briefly — C001/C0A1, a few times).
- The game's registered VBlank callback (0x800506B4, per games_tomba2.c) only increments the pacing
  counter — so the sequencer is NOT the VBlank callback either. (Don't chase "wire VSyncCallback".)
ROOT CAUSE NOT YET PINNED (deliberately not guessing / not bandaiding). Remaining candidates, in
order, for the next focused audio-RE session:
1. The music is likely STREAMED (XA-ADPCM / CD-DA) — that whole path (CD sector stream → SPU CD
   input / MDEC-audio) is stubbed: `CDC_GetCDAudioSample` returns silence. This is the most concrete
   known gap and probably the bulk of the missing audio.
2. SFX are SPU-sequenced; in the FORCED-INPUT (FFF7) attract state the game may simply trigger no
   SFX (no per-note KON because no SFX event), which would be partly EXPECTED, not a bug. Needs a
   run with real SFX-triggering input to tell "broken" from "nothing to play".
3. If a sequencer IS expected to run and doesn't, suspect a hardware Timer (RootCounter) IRQ tick we
   don't deliver — verify by checking Timer setup + whether a sound-update runs off it.
NOT attempted: any fix. Naming it per "no bandaids" rather than shipping a speculative call. GOTCHA
recorded: runs must go through run.sh (sets disc/BIOS/MAIN env); invoking scratch/bin/tomba2_port
directly does NOT boot the game, which briefly masked the SPU-RAM upload activity during triage.

## 2026-06-15 (later 51) — rasterizer fidelity: exact mednafen coverage + raw-texture poly bit → 2.57%→0.29% (a grass frame to 0.000%)
Continued the oracle GP0 differ chase (handoff scratch/handoff.md). Drove the GAME back-buffer
real-divergence (|Δ|>3, dither-filtered, region 0,256,320,240 on the f3000 banner frame) from the
later-45 residual **2.57% → 0.29%**, and a fresh live-captured grass frame to **0.000% (pixel-identical
to Beetle)**. Two root causes, both found via the differ + per-pixel PIXTRACE on both renderers, never
eyeballing:

### 1. Triangle coverage — ported mednafen's EXACT integer scanline edge-walk (the #1 handoff lead)
The absdiff was dominated by the "Go to the Burning House!" text-banner letter edges. PIXTRACE @64,308
(both renderers): our dark banner-board prim covered the pixel and overwrote the white letter; Beetle's
same prim stopped one pixel short. Our `tri()` used a half-space test keeping every pixel with all
edge-functions `wi>=0` — i.e. RIGHT/BOTTOM edges inclusive — so prims were one pixel "fat" and a later
prim mis-claimed a shared edge. A generic top-left fill rule got the direction right (2.57%→2.11%) but
not mednafen's exact sub-pixel endpoint rounding, so it over-fixed some edges and under-fixed others
(PIXTRACE @104,299 then showed ours MISSING a white pixel Beetle drew). Real fix: replicate Beetle/
mednafen's `DEFINE_DrawTriangle` verbatim — y-sort + core-vertex select, `MakePolyXFP`/`MakePolyXFPStep`
fixed-point edges, per-scanline span `[GetPolyXFP_Int(lc), GetPolyXFP_Int(rc))` (left/top inclusive,
right/bottom exclusive). Coverage now matches the oracle pixel-for-pixel. Kept our (already oracle-
validated) barycentric per-pixel shading by extracting it into `tri_px()` and driving it from the
ported walk (coverage and shading decoupled). **2.57% → 1.06%.**

### 2. Raw-texture polygons (GP0 cmd bit0 = texture-blend-disable) — the polygon path ignored it
The remaining residual had a systematic spike: 321 px at exactly |Δ|=9, all where Beetle output a
constant raw texel (e.g. 2E12 = (18,16,11)) while ours output a darker, dithered value. POLYDUMP @105,304
identified the prim: **op 0x2D** (textured quad, bit0 set). For polygons, bit0=1 means RAW TEXTURE —
output the texel verbatim, NO modulation by vertex/command color and NO dither. Beetle's TM0 template
does this; our polygon path always modulated (texel × color / 128) + dithered, so a raw texel 2E12 got
multiplied by the command color (168,72,31) → near-black. This is the SAME bit0 gating the sprite path
already honored (commit fb0c228); the polygon path was missing it. Fix (gpu_native.c): pass a `raw` flag
(`textured && (op&1)`) into `tri`/`tri_px`; when set, skip modulation and dither. **1.06% → 0.29%.**

### Residual = the affine/edge model floor (NOT chased further — diffuse, sub-pixel)
The leftover 0.29% (220 px on the banner frame) is diffuse single-pixel speckle, no structure. PIXTRACE
spot-checks: @8,359 the SAME prim samples a different texel (ours 1A12 vs Beetle 53DE) — affine-UV
sub-texel difference, our per-pixel barycentric UV vs Beetle's incremental fixed-point DDA can land on
adjacent texels at scattered pixels; @288,350 a 4th prim covers a boundary pixel in Beetle but not ours
(residual edge case). Eliminating these would require porting Beetle's per-pixel DDA interpolation
(CalcIDeltas + i_group stepping) wholesale for shading too — a large change for ~0.2%; left as the model
limit. The fresh live grass frame (no banner) is already 0.000%.

VERIFIED (oracle ground truth, headless): differ on f3000 banner frame 2.57%→0.29%; fresh live-captured
f3000 grass frame ours-vs-Beetle = **0.000% IDENTICAL**. Live port (scratch/bin/tomba2_port via run.sh)
rebuilt, ran headless to f3200, reaches GAME and renders — no crash/regression (the diff covers the whole
back buffer: grass/sprites/shadow all unregressed, all improved). Note: the live port is built by run.sh
(compiles runtime/recomp/*.c incl. gpu_native.c with the recompiled shards), NOT `make -C runtime` (that
Makefile's OBJS omits recomp/; it's a stale/secondary path). The differ's replay_ours uses gpu_native.c
standalone via tools/gpu_differ/build.sh.

## 2026-06-15 (later 50) — FIXED the menu-load flicker: only flip the double buffer when a frame drew
User: "flicker is not acceptable." Tooling (GPU_DUMP per-frame luma + GPU_LOG prim counts) pinned it:
during the ~17-frame menu-load gap after the FMVs the game produces NO draw prims (it's streaming in
a background image), but the native frame loop (native_boot.c) flipped the display double buffer
EVERY frame, alternating the display between the one buffer that has content and the still-black
other buffer → black/content/black/content flicker. In steady gameplay/menus every frame draws
(prims>0) so the flip is normal; only the load gap flickered.
- **Fix:** gate the buffer flip on `gpu_prims_since_present() > 0` (new accessor in gpu_native.c) —
  only flip to a buffer we actually drew this frame; otherwise HOLD the last completed front buffer.
  Same "never present an undrawn buffer" rule as the SCEA blink (later-46). During the load the
  display now holds the last good frame (the last OP FMV frame in the real flow) until the menu is
  drawn, instead of flickering.
- **Verified headless:** menu-load region went from alternating black/content to steady (no
  interspersed black), then the title menu renders cleanly; GAME stage still reached and renders the
  level — no regression to gameplay (which draws every frame).

## 2026-06-15 (later 49) — SCEA: Start-skip + stop the CD reads (kill the "CdRead: retry" loop)
User asks: SCEA skippable via Start; and SCEA should do NO CD reads — just cut to the LOGO FMV when
it ends. Both done (native_stub.c).
- **Start-skip:** ov_stub_vsync polls host input (pad_poll_sdl when windowed; pad_buttons() bit 0x0008
  = Start, active-low); on Start it does the MAIN.EXE hand-off immediately (load_exe_image +
  longjmp(g_stub_exit), same as ov_loadexec) — skipping the fades. PSXPORT_SCEA_SKIP forces it
  (headless/always); PSXPORT_NOSKIP disables. Verified: PSXPORT_SCEA_SKIP=1 hands off on the first
  VSync → straight to FMV/DEMO (also skips the CD-init retries).
- **No CD reads:** the "CdRead: retry..." spam (~20×) came from the stub's CdRead (0x8001BA64),
  whose caller (0x8001B89C) retries while the CD-status word (CD struct 0x80026BF8 +20 = 0x80026C0C)
  is negative — and our runtime serves the stub no CD data. RE'd it: caller `bgez`-checks that word;
  the original success path stores the VSync count there. Replaced CdRead with a no-op that writes a
  positive status (mimics success) so the caller stops retrying. The SCEA screen needs no CD data
  (FMVs play natively, MAIN is LoadExec'd from file). Verified headless: "CdRead: retry" count 20→0,
  SCEA still renders both lines correctly, reaches LOGO FMV → DEMO.
NEXT: the menu-load double-buffer flicker (firm "not acceptable") — fix the game-loop flip to not
present an undrawn buffer.

## 2026-06-15 (later 48) — SCEA fade pacing (real-vblank model) + the "headless still headed" bug
1. **PSXPORT_NOWINDOW didn't actually go headless.** present_window() gated the SDL window on mere
   getenv PRESENCE: `s_win_on = getenv("PSXPORT_GPU_WINDOW") ? 1 : 0`. But run.sh ALWAYS sets that
   var (to "0" when headless), and "0" is a non-NULL string → truthy → a window opened even under
   PSXPORT_NOWINDOW=1. Fix (gpu_native.c): check the VALUE (`atoi(w)!=0`), matching gpu_pace_frame.
   Now headless runs open no window / no SDL video init.
2. **SCEA fades/holds ran far too fast (headed).** The stub times its fades by the VBlank count
   (DAT_800267B4 / VSync return). ov_stub_vsync advanced that count by 1 per CALL, but the stub
   busy-polls VSync(-1) many times per frame, so the count raced ahead at the poll rate and
   count-timed fades blew through in a few real frames. Fix (native_stub.c): when windowed, use a
   real-vblank model — the count = elapsed real time × 60/1000 (exactly like the hardware VBlank IRQ,
   independent of poll rate), and a frame-wait (mode>=0) sleeps to the next 60 Hz boundary before
   presenting. Headless keeps the fast per-call path (tests stay fast). Verified headless: no
   regression (305 SCEA frames, smooth 57-step fade-in, reaches DEMO). The real-time fade SPEED is a
   windowed-only behavior — needs on-device confirmation (can't be timed headless).
NEXT: issue still open — menu-load flicker after skipping OP. Tooling (seq_check dump) shows the
DEMO menu-load presents ALTERNATING black/content frames (~f347-355: black,52,black,52,…) =
double-buffer flicker in the native game loop (ov_frame_update flips to an undrawn buffer during the
load). Same CLASS as the SCEA single-buffer blink (later-46) but in the GAME loop; fix there next.

## 2026-06-15 (later 47) — WIRED the intro FMVs: SCEA→Woopee→OP→Menu now plays
Next intro issue (user): FMVs didn't play — boot went SCEA→Menu instead of SCEA→Woopee→OP→Menu.
Tooling diagnosis: a headless stage-log run showed "time out in strNext()" in the DEMO stage — the
game's own STR streaming (StrPlayer/strNext) times out under our runtime (we don't feed CD-streamed
FMV sectors), so the movies were skipped to a black gap. The self-contained native FMV player
(native_fmv.c, native_fmv_play) was fully implemented + decode-verified but had ZERO callers — never
wired into the boot flow.
- **Fix (native_boot.c native_boot_run):** before crt0, play `MOVIE/LOGO.STR` (Whoopee Camp logo)
  then `MOVIE/OP.STR` (opening) via native_fmv_play(). Gated by PSXPORT_NO_FMV (set it for headless
  gameplay/differ tests — FMVs otherwise add frames and shift the global gpu-frame counter).
- **Verified via headless GPU_DUMP + analysis (not eyeballing):** LOGO decodes to the pink/yellow
  "Whoopee Camp" logo on white (first ~40 frames are its white fade-in, then content); OP decodes to
  coherent Tomba video; after the FMVs the game boots START→DEMO and renders the TITLE MENU ("TOMBA!2
  THE EVIL SWINE RETURN", New Game/Load Game). Full chain SCEA→Woopee→OP→Menu confirmed
  (scratch/screenshots/fmv_view_logo.png, fmv_view_op.png, seq_menu.png).
- GOTCHA: native_fmv_play paces to audio by default; PSXPORT_FMV_FPS=0 = uncapped (headless dumps);
  PSXPORT_FMV_MAXFRAMES=N caps frames (fast checks). Movies are Start-skippable.
- Residual (not chased): the DEMO attract still logs "time out in strNext()" (its own in-attract
  stream); harmless — the game proceeds to the menu. Separate from the intro FMVs.

## 2026-06-15 (later 46) — FIXED the SCEA screen: clipped "Presents" + unnatural blink
User flagged the intro sequence is wrong (SCEA→Woopee→OP→Menu; ours does SCEA→Menu, FMVs skipped)
and to fix issues IN ORDER, comparing via TOOLING vs the oracle (never eyeballing). First issue =
the SCEA "Sony Computer Entertainment America / Presents" screen. Two distinct bugs, both fixed:

1. **"Presents" line clipped + text mispositioned.** Tooling: dumped our SCEA VRAM
   (PSXPORT_VRAMDUMP_AT) and the oracle's (PSXPORT_VRAMDUMP) — BYTE-similar content; both lines
   present in VRAM at y≈205-258, centered for a 480-line display. Our render only showed line 1 at
   the bottom. Root cause: SCEA runs in **480i** (GP1(0x08)=0x27: interlace bit5 + VRes-480 bit2),
   GP1(0x07) vertical range = 234 scanlines, but in 480i the field is shown twice so the displayed
   VRAM height is 2×(y1-y0)≈468. Our gpu_gp1 set s_disp_h = y1-y0 = 234, clipping the bottom
   (y≈245-258 = "Presents"). Fix (gpu_native.c gpu_gp1): track interlace+VRes from GP1(0x08) and
   double s_disp_h in 480i. 240p stages (DEMO/GAME, GP1(0x08)=0x01) unaffected.
2. **Unnatural flicker.** Tooling: per-frame mean-luma of the GPU_DUMP showed every 4th frame fully
   BLACK (≈76/305) between text frames. Correlating GPU_LOG (per-present prim/word counts) with a
   new PSXPORT_VSYNCLOG: the stub's per-frame body is VSync(0)[frame boundary] → clear framebuffer →
   busy-poll VSync(-1)×3 (timing/CD) → draw → loop. Our ov_stub_vsync presented on EVERY VSync call,
   so the VSync(-1) poll fired right after the clear and BEFORE the redraw → presented the cleared
   (black) buffer. Real HW refreshes only at the VBlank frame boundary (VSync(0)), never mid-draw.
   Fix (native_stub.c ov_stub_vsync): present ONLY on mode>=0 (real frame-waits); mode<0 polls just
   advance the count (so loops still terminate) + pet the watchdog. After: black frames 76→5, and
   the 5 are only the fade-in (f1-3) / fade-out (f304-5) boundaries — no interspersed flicker; the
   brightness sequence is a smooth fade matching the oracle.
New gated debug tools: PSXPORT_GP1LOG (GP1 command log), PSXPORT_VSYNCLOG (stub VSync mode log).
NEXT (per user): FMVs don't play — SCEA→Menu instead of SCEA→Woopee→OP→Menu. A verified native FMV
player exists (native_fmv.c); the START-stage path that should play \Woopee\OP STR FMVs is skipped.

## 2026-06-15 (later 45) — FIXED the UV sampling residual: affine UV now round-to-nearest
Follow-on to later-44, using the differ to land its named #1 residual. PSX/Beetle sample the texel
NEAREST to the affine (u,v) — Beetle seeds its affine interpolant with a +0.5-texel bias
(`+(1<<(COORD_FBS-1))`, gpu_polygon.c) then truncates = round-to-nearest. Our `tri()` integer-divided
the barycentric UV sum and TRUNCATED, biasing every sample half a texel toward the origin and picking
a neighbouring texel at fractional coords. **Fix (gpu_native.c):** round the affine UV to nearest
(add half the divisor, sign-normalized since `aa` may be negative). Verified via the differ on the
f3000 grass frame: GAME back-buffer real-divergence (|Δ|>3, dither filtered) **17.0% → 2.57%**; total
(incl. dither) 64% → 42%. Live port rebuilt, reaches GAME, renders clean & sharper, no regression
(scratch/screenshots/differ_f3000/live_uv_full.png). REMAINING residual: the ~2.6% |Δ|>3 is now
diffuse (triangle-edge coverage: our top-left fill rule vs Beetle's), and the ~40% |Δ|≤3 is the
dither-matrix mismatch (our custom s_dither4 vs Beetle dither_table/DitherLUT) — next differ targets.

## 2026-06-15 (later 44) — BUILT the GP0 differ + FIXED the "shadow = black wedge" bug
Two deliverables (handoff scratch/handoff.md), both done & verified.

### 1. The cross-renderer GP0 differ (tools/gpu_differ/) — feeds IDENTICAL GP0 to both rasterizers
The whole point: our HLE port and the full-emulation oracle run at different timings/states, so you
CANNOT align by frame number (this trap defeated every earlier screenshot compare). Instead capture
one frame's GP0 word stream + its start-of-frame VRAM from our port, replay it through BOTH our
renderer and Beetle's from the same initial VRAM, and diff the output VRAM — any pixel difference is
then a pure rasterizer-fidelity difference (modulation/blend/dither/coverage), no alignment needed.
- **Capture** (our port): `PSXPORT_GPUTRACE="FRAME[:path]"` (gpu_native.c) writes file `GP0TRC01` =
  initial VRAM + exact GP0 word stream for that gpu frame.
- **Beetle replay**: `wide60rt -gpureplay TRACE OUT` (main.cpp + `GPU_ReplayBegin` in gpu.c) seeds
  Beetle VRAM, replays words via `GPU_WriteDMA`, dumps Beetle VRAM. Software/1x only.
- **Ours replay**: `tools/gpu_differ/build.sh` → `scratch/bin/replay_ours TRACE OUT [VX VY]` links
  gpu_native.c standalone (stub mem_r*). `+VX VY` with `PSXPORT_PROVAT=1` prints the prim our
  renderer used to write that pixel (via new `gpu_prov_dump`).
- **Diff**: `tools/gpu_differ/diff.py OURS BEETLE [--region X,Y,W,H] [--tol N] [--ppm DIR]`.
  `--tol N` filters per-channel |Δ|≤N (dither/rounding noise) so real bugs surface.
- **Per-pixel math trace** (both renderers): `PSXPORT_PIXTRACE="VX,VY"` dumps the sampled texel +
  interpolated vertex color + modulated output for every prim writing that pixel — `[pixtrace ours]`
  (gpu_native.c) and `[pixtrace beetle]` (gpu_polygon.c). This is the per-pixel ground-truth differ.
- GOTCHA: gpu_polygon.c/gpu_sprite.c are `#include`d into gpu.c; the Makefile does NOT track that
  dep, so editing them needs `touch vendor/beetle-psx/mednafen/psx/gpu.c` before `make -C runtime`.

### 2. The bug: gouraud-textured modulation used vertex-A's color FLAT (not interpolated)
- The "soft shadow" Tomba/items cast on the grass is NOT a semi-transparent blend (the handoff's
  premise was WRONG). It's a **gouraud-textured OPAQUE quad** (GP0 op 0x3C, semi=0) that darkens the
  ground texture by a dark-at-center / bright-at-edges vertex-color gradient (PROVAT in the replay →
  op=3C tex=1 semi=0; PIXTRACE confirmed).
- **Root cause (gpu_native.c `tri()`):** the textured-modulation path computed `texel * a.r / 128`
  using `a.r` = the FIRST vertex's color held CONSTANT across the whole primitive, instead of the
  per-pixel barycentric-INTERPOLATED color (which the untextured gouraud path already did right). So
  the whole shadow quad was flat-modulated by v0's dark color (32,32,32) → a uniform near-black
  wedge, instead of a smooth gradient with grass showing through at the bright end. PSX (and Beetle's
  `ModTexel`, fed the interpolated r,g,b) ALWAYS modulate a textured polygon by the interpolated
  vertex color (flat-shaded prims carry the command color in every vertex, so this covers them too).
- **The fix:** interpolate (cr,cg,cb) via the same barycentric weights and modulate by that; dropped
  the `if (shade)` gate so flat-textured polys also modulate by their (uniform) command color — both
  match Beetle. Saturation-clamp (later-42) preserved.
- **Verified via the differ (oracle ground truth, not arithmetic/eyeball):**
  - Visual: the black wedge is gone; ours now shows soft darkened grass = Beetle
    (scratch/screenshots/differ_f3000/shadow_ours_fixed.png vs shadow_beetle.png).
  - Quantified: back-buffer real-divergence (|Δ|>3, dither filtered) **41.4% → 17.0%**; shadow region
    **48.2% → 17.5%**.
  - Per-pixel: PIXTRACE on interior shadow pixels shows ours' interpolated modulation color now MATCHES
    Beetle's (@130,455 exact (165,165,174); @140,452 (164,165,183) vs (164,165,184)).
  - Live port (real gameplay) rebuilt, reaches GAME, renders clean (live_fixed_full.png); grass
    (later-42) and sprites (later-43) unregressed.
- **RESIDUAL (next lead, NOT this bug):** the remaining ~17% is dominated by **UV sub-texel sampling**
  — Beetle adds a +0.5 round-to-nearest bias to affine u/v before truncation (gpu_polygon.c ~L688/697)
  while our `sample_tex` truncates, so we sample a neighbouring texel at fractional coords (PIXTRACE:
  same screen pixel, ours texel 0942 vs Beetle 0901). Plus the dither matrix differs (ours custom
  s_dither4 vs Beetle dither_table/DitherLUT). Both are pervasive low-amplitude fidelity gaps; fix UV
  rounding first (likely the biggest remaining win), then dither, using the differ.

## 2026-06-15 (later 43) — FIXED: sprite tinting = textured rectangles ignored the command color
Follow-on to later-42 (user confirmed "sprite tinting has issues"). Textured rectangles/sprites
(GP0 0x60-0x7F) on PSX modulate the texel by the command color (texel*color/128, saturated, 0x80 =
neutral) — there is NO raw-texture rectangle variant. Our sprite path wrote the **raw texel** and
ignored (cr,cg,cb), so any non-neutral-tinted sprite rendered at full brightness / wrong hue.
- **Found via tooling, not guesswork.** POLYDUMP (sprite path) showed that at a GAME frame 352/355
  textured sprites are neutral (128,128,128 = passthrough, unaffected) and a few carry a real tint —
  e.g. three 32x24 HUD-ish item sprites with command color (39,248,0) (heavy green). PROVAT on one
  (display (157,207), f3060) showed the output pixel = 0xB929 (72,72,112) raw purple = the un-modulated
  texel, drawn by op=65 primcol=(39,248,0). A green-glowing item was rendering purple.
- **The fix (gpu_native.c sprite path):** modulate texel by (cr,cg,cb) with saturation, same rule as
  the polygon path. Neutral sprites (color 128) are unchanged (texel*128/128 = texel).
- **Verified via tooling + visual:** after the fix PROVAT on the same pixel = 0x8222 (16,136,0) =
  exactly texel*(39,248,0)/128 then 5-bit-quantized (21,139,0 -> 16,136,0). The item now renders green
  (scratch/screenshots/spritefix/f03060_hud.png — green spiky item; was blue/purple). Grass still clean
  (later-42 grass-garish stays 0), level still loads/plays/reaches GAME — no regression.
- Remaining GPU faithfulness notes (not bugs hit yet): mask-bit test on VRAM reads is not modeled;
  sprite/poly blend modes assume the standard set. Revisit if a specific effect looks wrong.

## 2026-06-15 (later 42) — FIXED: grass red-blocks = uint8 overflow in texture-color modulation
Root-caused and fixed the scattered garish blocks on the grass (the bug later-39/40/41 circled but
never landed). **It was our rasterizer all along — a saturation bug in gouraud-textured modulation.**
- **Root cause (gpu_native.c `tri()`):** `r = r * a.r / 128` etc. with `r,g,bl` as `uint8_t`. PSX
  modulation is `texel * vertexcolor / 128` **saturated to 0xFF**; doing it in `uint8_t` instead
  WRAPS mod 256. A bright texel channel under a bright (>128) vertex color overflows 255 and wraps to
  a small value — so a bright-green grass texel becomes red. Exact reproduction: green grass texel
  (136,192,24) × vertex color (187,187,197) → R=198, G=192·187/128=**280→wraps to 24**, B=36 →
  output (192,24,32) = 0x9078 = the exact red pixel observed. Scattered because only the brightest
  texel+brightest-vertex combos exceed 255.
- **The fix:** compute the modulation wide (int) and clamp each channel to 255. One-liner, the only
  modulation site (the untextured gouraud path interpolates within the vertex-color range, no
  overflow). Sprites (GP0 0x60-0x7F) don't modulate by command color in our path — a separate
  faithfulness gap, not this bug; left as-is.
- **Verified:** grass-region garish pixels (lower-right, away from the legit-colorful banner/Tomba/
  house) went **402 → 0** at f03000/f03255; the level renders clean green grass matching the oracle
  (scratch/screenshots/redfix/f03000.png — clean, "Go to the Burning House!" quest banner + Tomba +
  items all correct). Level still loads, reaches GAME, and plays (no regression).
- **How the provenance tool cracked it (after a session of dead ends):** added `PSXPORT_PROVAT=x,y[:frame]`
  — a per-pixel "last writer" buffer (gid + frame + prim metadata) stamped in `put_px_b`, queried in
  DISPLAY space at present time. That sidesteps the double-buffer offset that had defeated every prior
  pixel→prim correlation, and immediately showed: the red pixel was actively drawn (age 1 frame, not
  stale) by a 4bpp gouraud prim using the all-GREEN grass palette — which is impossible unless the
  arithmetic mangles a green texel. From there the overflow was a 2-minute check. Lesson: build the
  provenance tool FIRST next time a "which prim drew this pixel" question comes up.
- **Earlier-session wrong leads (now closed):** the clut-(880,507) quad is the DEMO intro subtitle
  (later-41, fine); the (240,0,0) prims were Tomba's own model (later-41b); the clut-(1008,25x)
  palette diffs are minor cyan-shade shifts (later-41b, not this bug). All red herrings — the one real
  bug was the modulation overflow.
- **Durable tooling added across the session:** PSXPORT_PROVAT (per-pixel prim provenance + stale
  detection), PSXPORT_POLYDUMP/POLYAT (poly+sprite dump with vertex colors, OT node, point filter),
  PSXPORT_VRAMDUMP_AT, PSXPORT_STAGETL, PSXPORT_RAMDUMP_FRAME, PSXPORT_WWATCH (+coro g_interp_pc),
  PSXPORT_TEXTDBG; oracle POLYWATCH range + POLYCLUT.

## 2026-06-15 (later 41b) — narrowing the real grass garbage (user confirmed both our frames are wrong)
Follow-up to later-41 after the user confirmed the DEMO frame (f02790) is ALSO wrong (only the oracle
is clean) — i.e. the bug is in our grass-level rendering generally, present in both DEMO attract and
GAME. Narrowed but NOT yet root-caused. Honest state:
- **The garbage = tiny static red/garish specks amid correct green grass**, e.g. VRAM pixel 0x9078 =
  (192,24,32) red at a fixed screen spot, with green grass (clut 816,224 = all greens, byte-identical
  to oracle) immediately around it. Clusters of these specks read as "scattered blocks". The dark
  faceting is the same thing at the dark end.
- **Not wrong vertex colors / not wrong terrain VRAM.** The terrain prims covering a red speck are
  gouraud-textured with NORMAL gray-blue vertex colors and a pure-green palette (no red anywhere); the
  terrain texpages + cluts are byte-identical to the oracle. So the red is NOT produced by the grass
  prim. (The all-red (240,0,0) prims I first found at DEMO (121,96) were **Tomba's own model**, not
  garbage — don't chase those.)
- **There IS a real VRAM divergence, but it's minor and the wrong color:** a ~355-sprite layer (16x16,
  op 0x7C, clut (1008,250-255), tp (896,0)) is drawn every frame; its palettes (1008,253/254/255)
  differ from the oracle by 6-15 entries — but only as **subtle cyan-shade shifts** (e.g.
  (200,240,248) vs (160,240,248)), all cyan/white, never red. So that divergence is real but does NOT
  explain the red specks.
- **Leading hypothesis (unconfirmed): the red specks are STALE framebuffer VRAM revealed through gaps
  in our terrain coverage.** A red speck sits where no opaque terrain triangle lands (bbox overlaps but
  the triangle misses), so an earlier draw shows through; the oracle either fully covers it or clears
  the buffer and we don't. NOT yet proven — the double-buffer (GAME prims alternate off=(0,0)/(0,256);
  the displayed front buffer was drawn the prior native frame) confounded every pixel→prim correlation
  this session, so each "red pixel → covering prim" lookup dead-ended on a non-red palette.
- **NEXT (do this fresh, systematically):** (1) settle the offset/buffer mapping first (dump the BACK
  buffer that frame N draws at off=(0,256) → VRAM y+256, and only correlate within that buffer). (2)
  Decide stale-vs-drawn with a sentinel: fill VRAM with a unique color at boot — if the specks take the
  sentinel they're uncovered/stale (fix = framebuffer clear / terrain coverage), if they stay they're
  actively drawn (find the prim, incl. lines op 0x40-0x5F and the 0x7C sprite layer). (3) Add a sprite
  hook to the oracle (wide60rt currently hooks only polys) for a true prim-by-prim our-vs-oracle diff.
- **Tool added:** PSXPORT_POLYDUMP/POLYAT now also covers the sprite/rect path (GP0 0x60-0x7F), logs
  the OT node addr, and logs all 4 vertex colors for polys.

## 2026-06-15 (later 41) — later-40 FALSIFIED: the clut-(880,507) quad is the DEMO intro subtitle, not the level bug
Picked up the handoff (kill the "spurious title-overlay quad"). Traced the clut-(880,507) op-2D
quad end to end and **falsified later-40's root cause.** The real on-grass garbage in the playable
level is a *different* artifact that is NOT yet root-caused. Do not re-chase the clut-880,507 quad.

- **The clut-(880,507) quad is the DEMO-stage intro NARRATION SUBTITLE, and it renders CORRECTLY.**
  Built by the resident sprite/text renderer `FUN_8007E1B8` from a glyph string (descriptor base
  0x80158000, glyph idx ~149-164) at screen (43,172). It is the on-screen story text
  "Tomba is living peacefully in the country when Zippo finds a mysterious…" overlaid on the attract
  demo (scratch/screenshots/redcheck/f02790.png — readable cream text, not red dots).
- **It is drawn ONLY during the DEMO stage, never during GAME (the decisive correlation).** Stage =
  task0 entry @0x801fe00c: 0x801062E4 = DEMO, 0x8010637C = GAME. With `PSXPORT_WWATCH=800BFEDC,800BFEE0`
  (logs the store of the uv0|clut word 0x7EF71100) every one of the 16 builds is `pc=8007E67C
  stage=801062E4` (DEMO); **zero** in GAME. redpkt confirms: at the GPU frames where the quad emits
  (≈f2690-2800) `stage=0x801062E4`. The oracle draws clut-(880,507) **nowhere** at the level
  (`PSXPORT_POLYWATCH=6900-7400 PSXPORT_POLYCLUT=880,507` → no hits). So the quad is correct title/intro
  content, present in our DEMO, absent (correctly) from GAME. later-40's "title overlay leaks into the
  level" is **wrong** — there is no clut-880,507 prim in the level at all.
- **Stage timeline (PSXPORT_STAGETL, per gpu frame):** gpu f0-2600 task0entry=0 (boot/FMV/intro),
  ≈f2700 DEMO (intro narration over attract gameplay), f2800+ GAME (playable level). The earlier
  native-frame↔gpu-frame guesses in the handoff were off; trust STAGETL.
- **THE ACTUAL BUG (still open): scattered garish red/pink/purple/blue blocks on the GAME grass.**
  Confirmed a real divergence — the oracle's same level (scratch/screenshots/redcheck/oracle_level.png,
  oracle f7290) renders **clean** grass; ours (f03000.png/f03255.png) has scattered garish rectangles
  on the grass (Tomba's area). This is what the user actually sees; it is NOT the subtitle quad.
- **It is NOT a texture-VRAM divergence.** Dumped our VRAM at the level (`PSXPORT_VRAMDUMP_AT=3000:path`)
  and diffed vs the oracle's (scratch/bin/oracle_vram.bin) across the level texpages
  (576,0)/(448,256)/(768,0)/(960,256) and clut rows — **~0% diff (byte-identical)**. One garish grass
  pixel (121,101 magenta 168,0,128) is covered by an op-3C clut-(816,236) tp-(768,0) gouraud prim
  whose palette has NO magenta (yellows/browns/cyans/black, all == oracle) — so that prim can't be the
  source. The magenta must come from another prim/path not yet pinned: the **sprite path** (GP0
  0x60-0x7F, not in polydump), an op-2D prim with a magenta clut, or a semi-transparent blend artifact.
- **NEXT (real fix):** pin the garish prim source — extend the dump to the GP0 0x60-0x7F sprite path,
  and do an **offset-aware** prim-by-prim compare of our level display list vs the oracle's
  (PSXPORT_POLYDUMP/POLYAT here vs PSXPORT_POLYWATCH on wide60rt). Mind the double buffer: GAME prims
  alternate off=(0,0)/(0,256); the *displayed* frame (disp @0,0) is the off=(0,0) set, drawn the prior
  native frame — so a prim dumped at frame N with off=(0,256) shows at N+1. Find the extra/wrong prim,
  then root-cause (UV/vertex divergence from game logic, or a sprite-path rasterizer subtlety).
- **Tools added this session (durable, env-gated):** `PSXPORT_WWATCH=lo,hi` (mem.c — logs interp PC +
  stage of any store landing in [lo,hi); finds who builds a RAM/OT struct; needs the new per-insn
  `g_interp_pc` tracking now also in the coro interpreter). `PSXPORT_POLYDUMP=frame` [+ `PSXPORT_POLYAT=x,y`]
  (gpu_native.c — dump every poly at a frame: op/clut/tp/col/uv/verts/off, optionally only prims whose
  screen bbox covers (x,y)). `PSXPORT_VRAMDUMP_AT="frame:path"` (gpu_native.c — our 1024x512x16 VRAM at
  a frame, to diff vs the oracle dump). `PSXPORT_STAGETL` (gpu_native.c — per-gpu-frame stage timeline).
  `PSXPORT_RAMDUMP_FRAME=N` (native_boot.c — mid-run RAM dump; the 0x8010/0x8011xxxx overlay at gameplay
  differs from end-of-run). `PSXPORT_TEXTDBG` (interp.c — calls to the 2D text/row drawer 0x8007E998).
  redpkt now logs `node=` (OT RAM addr) + `stage=`. Oracle `PSXPORT_POLYWATCH` now takes a `lo-hi`
  range and `PSXPORT_POLYCLUT=x,y` filter (main.cpp).

## 2026-06-15 (later 40) — GRASS RED-BLOCKS root cause: our port draws an EXTRA title-overlay quad
**[SUPERSEDED by later 41 — this diagnosis is WRONG. The clut-(880,507) quad is the DEMO intro
subtitle and renders correctly; it is never drawn in the GAME level. The real on-grass garbage is a
separate, still-open bug. Kept below for the trail, but do not act on its NEXT.]**
Continued the later-39 oracle compare and **falsified its "wrong CLUT contents" hypothesis**, then
root-caused for real. The red blocks are NOT a VRAM or rasterizer bug — our port emits a spurious
primitive the real game doesn't.
- **The offending prim (our port, via PSXPORT_REDDBG + CLUTWATCH):** a flat RAW-textured quad
  `op=0x2D` (textured+quad+raw, opaque, no gouraud), clut=(880,507), texpage=(832,256), screen
  (43,172)-(139,188), uv (0,17)-(96,33) — a fixed-screen 96x16 strip at the bottom-left. The
  texture is mostly index 1 (palette[1]=0x0000 = transparent) with sparse index 15
  (palette[15]=0x0408 = dark red) ⇒ "scattered red dots". Drawn EVERY level frame at the same
  screen position (a 2D overlay, not a GTE world object).
- **VRAM is byte-identical to the oracle (hypothesis killed).** Added `PSXPORT_VRAMDUMP="frame:path"`
  to wide60rt (dumps mednafen's native 1024x512x16 `GPU_get_vram()` at 1x). At the level
  (oracle frame 7290) the oracle's palette @(880,507) = `...2C32 0408` and texture @(833,273) =
  `FFF1 FFFF 1111...` — EXACTLY ours. So the texture/CLUT contents are correct; palette[15]=red is
  correct data. The "pink/no-green palette" in later-39 is real but is NOT a grass palette and NOT
  wrongly placed — it's just unused-at-the-level data sitting in a reused VRAM slot.
- **The oracle does NOT draw this quad at the level (the decisive test).** Added `PSXPORT_POLYWATCH=frame`
  to wide60rt (registers the psxport_on_gpu_poly hook; logs every textured GP0 poly at `frame` with
  decoded clut/texpage + first 3 screen verts). At the oracle's level (f7290, 924 textured polys),
  the geometry around (43,172) is **gouraud-textured grass terrain** (cc=34/3E/3C, clut 800,210 /
  352,496 / 496,488 — none is clut 880,507, none is a flat cc=2D quad). So the oracle's level does
  not emit our red quad. The oracle DOES emit exactly this prim (cc=2D, clut 880,507, screen 43,172)
  — but only at **frames 1286-1324 = the TITLE SCREEN** (scratch/frames/oracle2/e1290.png). So the
  red-dot quad is a **title-screen 2D overlay element**.
- **Conclusion (verified):** our port draws a TITLE/menu 2D overlay quad DURING the level; the real
  game only draws it on the title. Since our port HLE-boots and uses a custom stage state machine
  (native_boot.c / FUN_80052078 transitions), a title/menu overlay object is not torn down when
  transitioning to GAME, so its per-frame draw persists into the level. It's a **game-logic /
  object-state divergence**, NOT GPU (sample_tex/blend/texwindow and all VRAM contents are correct).
  NB our port does NOT draw this quad at OUR title (redpkt's first hits are all level frames) — it
  appears ONLY at the level, consistent with state that's mis-set during the HLE stage transition.
- **Tools added this session (durable, env-gated):** `PSXPORT_REDDBG` + `PSXPORT_CLUTWATCH=x,y`
  (gpu_native.c — REDDBG logs the prim params + palette + texrow on a dark-red pixel, and (with
  CLUTWATCH set) the full GP0 packet `[redpkt]` for polys using the watched clut; CLUTWATCH alone
  logs VRAM uploads covering the watched CLUT row). `PSXPORT_VRAMDUMP="frame:path"` and
  `PSXPORT_POLYWATCH=frame` (wide60rt/main.cpp — oracle VRAM dump + per-frame textured-poly list).
  Oracle-to-level recipe: see later-39.
- **NEXT (the fix):** find the title/menu 2D-overlay object that emits the clut-(880,507) quad and
  why it stays active at the level. Approaches: (a) in GAME.bin, find the draw code that builds this
  fixed-screen quad (search for the clut/uv constants or the (43,172) screen coords) and trace what
  object/flag gates it; (b) check the stage-transition teardown (FUN_80052078 / the title→GAME
  object-list reset) — our custom scheduler likely skips a clear that the real boot does. Then
  deactivate/clear that object on entering GAME. Compare our display list vs the oracle's
  (PSXPORT_POLYWATCH) to confirm the extra prim is gone after the fix.

## 2026-06-15 (later 39) — ORACLE COMPARE: grass "red blocks" = wrong CLUT contents at (880,507)
User reported graphical errors in the now-playable level. Ran the **oracle** (Beetle/mednafen
`runtime/wide60rt`, real GPU) on the same disc to the same first jungle level and compared against
our native renderer (gpu_native.c, our OWN rasterizer). Result: our grass has scattered **dark-red
blocks**; the oracle's grass is **clean green grass-blade fringe** (scratch/screenshots/compare_grass.png,
oracle frame scratch/frames/oracle2/frame_007290.ppm). So the red blocks are a real bug in OUR
rendering, not game content.
- **Oracle run recipe (reaches the level in ~7300 emulated frames):**
  `PSXPORT_NOWIDE=1 PSXPORT_INTERNAL_RES=1x runtime/wide60rt "$TOMBA2_CHD" -bios bios -frames 12000
  -inputscript scratch/oracle_input.txt -dumpdir scratch/frames/oracle2 -dumpinterval 30`, where
  oracle_input.txt pulses Start (`<f> <f+8> Start` every 40 frames) to skip logos + select the menu.
  OpenBIOS works (no -fastboot). Oracle renders at 350x240; ours 320x224 — fine for visual compare.
- **Root-caused with a new probe `PSXPORT_REDDBG`** (gpu_native.c — logs a textured prim's
  mode/clut/texpage/uv + the 16 CLUT entries + the sampled texture row whenever it writes a dark-red
  pixel). The red blocks are: textured 4bpp polys, **neutral modulation color (0x808080)**, blend off,
  clut=(880,507), texpage=(832,256). The fringe texture is fine (indices 1 and 15: palette[1]=0x0000
  = transparent → grass shows through; palette[15] = the fringe color). The bug is the **CLUT
  contents**: our VRAM palette at (880,507) = `0000×8, 03FF, 7380, 7BBF, 727F, 699F, 60BF, 2C32, 0408`
  — a pink/purple/yellow set with **zero green**; palette[15]=0x0408 is the dark red we draw. The
  CLUT pointer + texture indices come from the SAME game GP0 packet on both runtimes, yet the oracle
  renders green ⇒ the oracle's VRAM at (880,507) holds a GREEN palette and ours holds this pink one.
  So our VRAM CLUT at (880,507) is WRONG (stale/overwritten/mis-placed) — a texture/CLUT **VRAM
  upload or ordering** bug, NOT a rasterizer-logic error (sample_tex / blend / texwindow are correct).
- **Ruled out:** `shade = gouraud || !textured` (gpu_native.c:199) skips command-color modulation for
  flat-textured polys — a real faithfulness gap, but NOT the cause here (the modulation color is the
  neutral 0x808080, so it's a no-op for these prims). Worth fixing separately for correctness.
- **NEXT (this bug):** find why (880,507) holds the wrong palette. Likely a VRAM CPU→VRAM transfer
  (GP0 0xA0) or DMA placed the grass palette elsewhere / a later upload overwrote it, due to our
  synchronous-CD + custom-scheduler upload ORDER differing from hardware. To pin it, either (a) add a
  VRAM dump to wide60rt (libretro GPU_RAM) and diff VRAM at (880,507)/(832,256) vs ours, or (b) log
  every CPU→VRAM/DMA write that touches the (880,507) CLUT row in our runtime and check what lands
  there last. Then fix at the transfer/ordering level.

## 2026-06-15 (later 38) — REAL PLAYABLE GAMEPLAY: async area-reader fix → level loads, plays, MOVES
later 37 OVERCLAIMED. The level-intro renders once (gpu ~f2850, native frame ~224) and then the
screen goes **permanently black** — the GAME state machine freezes at sm[48=2 4a=1 4c=2 4e=8] and
the framebuffer (VRAM x<320) stays black while the texture pages (x≥320) stay loaded
(`nonblack=252298` constant). later-37 only ran 240 native frames (the level appears at the very
END of that window) and misread the brief 4e-leaf cycling (9,10,7,8) as a "live game loop". It is
not — it halts at 8. (The 19 "CdRead: retry" log lines are all BOOT-time and do NOT grow during the
black region — a red herring; ruled out.) **This session fixes the real freeze; the game now loads
the level, renders sustained animating gameplay, takes input, and Tomba MOVES.**
- **Root cause — the engine's ASYNC area-data reader never completes (no-IRQ), so the level-load
  flag is never set.** The 4e=8 leaf (GAME.bin `FUN_80106f68`) is a clean wait: `if
  (DAT_1f80019b == 0) return;` (yield, loop) else advance to 4a=1/4c=2/4e=6. `DAT_1f80019b` is the
  area-load done flag. It is set by `FUN_8001db38` (task1's body) **only after `FUN_8001d940`
  RETURNS** (task+0x6c is already 1). `FUN_8001d940` (engine streaming reader) issues a raw libcd
  ReadN and loops (yielding each frame) until the remaining WORD count `_DAT_1f8001f4` hits 0 — but
  that count is decremented only by the per-sector data-ready IRQ callback `FUN_8001d7c4` (a plain
  CdGetSector copy into `_DAT_1f8001f8`). Our no-IRQ runtime never fires it ⇒ count never 0 ⇒
  reader never returns ⇒ flag never set ⇒ the 4e=8 wait spins (cooperatively — yields each frame, so
  no hard spin; the native loop keeps running, screen black).
- **Why the existing CD overrides missed it.** cd_override.c already replaces the *synchronous*
  loaders `FUN_8001db8c` / `FUN_8001dc40` with native reads. But the level uses the *async* path:
  `FUN_80044cd4` (fire-and-forget) / `FUN_80044bd4` (spawn+yield-wait) set `_DAT_1f8001f0`(LBA) /
  `_DAT_1f8001f4`(words) / `_DAT_1f8001f8`(dest) and spawn task1 with entry `FUN_8001db38` →
  `FUN_8001d940` **directly** (`FUN_80051f14(1, FUN_8001db38)`), bypassing the overridden loaders.
- **Fix — `ov_cd_async_read` (cd_override.c, overrides `FUN_8001D940`).** Do the read natively &
  synchronously: copy `_DAT_1f8001f4 * 4` bytes from consecutive sectors at LBA `_DAT_1f8001f0`
  into `_DAT_1f8001f8` (word-granular, exactly as `FUN_8001d7c4` does: 0x200 words = 1 sector =
  2048 B; dest advances by words*4, no sector padding), then zero the count and set the position
  tracker `DAT_800be0e0 = last sector`. The reader returns immediately, `FUN_8001db38` sets
  `DAT_1f80019b = 1`, task1 ends, and the GAME advances. (`FUN_8001D940` is recompiled — index 14 —
  so the override fires even though task1 runs in the flat interpreter: interpreted `jal 0x8001d940`
  → `call_addr` → `is_recompiled` → `rec_dispatch` → `func_8001D940` → `g_override[14]`.)
- **RESULT (verified, headless, FORCE_BUTTONS=FFF7):** at native frame 224 the 4e=8 wait now
  advances (4e=8→6, 4c→1→2, then normal play). Over a 1500-native-frame run (4156 gpu frames),
  **0 near-black frames** in the gameplay region (≥f2900, 1256 frames); scene non-black count varies
  continuously (26k↔71k) = real animation. Frame dumps show a fully rendered jungle level (Tomba,
  trees, house, sky — scratch/screenshots/fix/s03200.png), not the old intro flash.
- **Interactive control + movement VERIFIED.** Pulsing Start in-level opens the in-game pause menu
  (Options / Load data / Quit game — scratch/screenshots/long2/late.png), which only appears during
  real gameplay. Holding Right (PSXPORT_FORCE_HOLD=FFDF FORCE_HOLD_AT=240) scrolls the level: the
  scene moves to a new area between f2950 and f3150 (scratch/screenshots/move/m02950.png vs
  m03150.png). So menu→load→level→play→move all work on input.
- **Tools added this session (durable, env-gated):** `PSXPORT_SCHEDDBG` (native_boot.c — per-slot
  task resume_pc/ra/sp each scheduler step; THE way to locate a cooperative yield-wait: a parked
  task resumes at 0x80051FA4, then read its caller RA off the task stack at sp+0x10 to find the
  real waiting loop). `PSXPORT_RAMDUMP=<path>` (native_boot.c — dumps 2MB main RAM at end of run;
  then `tools/disasm.py <dump> <start> <end>` to read live overlay code, e.g. the 0x8011xxxx level
  overlay absent from the GAME.bin.asm dump). gpu_native.c now `mkdir -p`s PSXPORT_GPU_DUMP (was a
  silent no-op when the dir didn't exist).
- **NEXT:** (1) Audio (tests run PSXPORT_NOAUDIO — verify SPU/XA in real play). (2) wide60: the
  paced 30fps → 60 interpolation (project headline; see [[wide60-scope-decision]]). (3) Play deeper
  — confirm no later async-read or event stalls in subsequent areas/levels.

## 2026-06-15 (later 37) — GAME RENDERS GAMEPLAY: CD-streaming spin fixed → level loads + draws
The GAME-black residual from later 36 is **fixed** — the in-game level now loads and renders
actual 3D gameplay (scratch/screenshots/game_fix/view_2850.png: a Tomba2 jungle level — foliage,
water, character). Root cause was a non-yielding busy-wait in the CD *streaming* reader, not a
render bug.
- **Root cause — `FUN_8001cfc8` (CD streaming task, slot 2) busy-spun forever, wedging the whole
  frame.** When GAME enters its load sub-state it spawns the area/XA streaming task
  `FUN_8001cfc8` (via `FUN_8001d364`→`FUN_8001d2a8`→`FUN_80051f14(2,…)`). That task SEEKS the
  drive to a target sector and then polls the drive head position — `FUN_8001ce90(0x10)` =
  **GetlocL** → result MSF → `FUN_8008a110` (MSF→LBA) — looping until the head reaches the target
  window [task2+0x54 .. +0x58]. We serve all CD data synchronously and model **no drive motion**,
  so our CD-command override left the GetlocL result MSF **zeroed**; `FUN_8008a110(00:00:00)` =
  −150 = 0xFFFFFF6A is never in range → the poll loop (which does NOT yield) spins forever, so
  `native_scheduler_step` never returns → the frame loop is stuck at native frame 71, no present,
  black screen. (The GAME logic in slot 0 had already advanced fine; it only *looked* like a
  render/state hang.)
- **Found with a new durable tool — `PSXPORT_SPINDBG`** (interp.c): rec_coro_run counts
  iterations; if it runs ~80M without yielding/returning it dumps the looping pc window + branch
  regs (+ the CD-stream contract). It pinned the spin to the FUN_8001cfc8 GetlocL poll
  (pc 0x8001CE04..0x8008A188, a0=0xFFFFFF6A). Keep this — it localizes any future non-yielding
  busy-wait instantly (gdb shows pc `<optimized out>`).
- **Fix — `ov_cd_cmd_stream` (cd_override.c, overrides `FUN_8001CE90`):** report the drive AT the
  requested sector immediately (we don't model seek latency). For GetlocL (cmd 0x10) fill the
  result buffer with the BCD MSF of the stream's target start LBA (task2+0x54), so the head is
  "in range"; FUN_8001cfc8 then proceeds into its normal per-frame **yielding** read loop instead
  of spinning. Other streaming commands report success (matches our synchronous-CD model). Only
  the FUN_8001ce90 wrapper is intercepted — FUN_8001d940's reader calls FUN_8001ce04 directly and
  is untouched. NB: this stream copies no data to RAM (dest/words = 0 — it's XA/auto-routed), so
  not modeling its payload is correct for level data; only the seek-position needed unblocking.
- **RESULT (verified, headless, FORCE_BUTTONS=FFF7):** past frame 71 the GAME state machine
  advances normally (0x50 1→4, then 0x4c/0x4a/0x4e cycling = live game loop, no spin), VRAMSCAN
  framebuffer bbox goes (0,0)-(1023,511) (was all x≥320 = textures only), and frame dumps show the
  title menu, a "Please wait" load, then a **rendered 3D level**.
- **Rendering is SMOOTH (no flicker).** Consecutive frame dumps show the scene-pixel count
  ramping continuously (f2840..f2855 ≈47k→63k, then back down) as the camera pans / level
  animates — NOT a per-frame scene↔black alternation. The occasional all-black dump (f2890) is a
  normal fade transition, not a dropped buffer. (Earlier "flicker" suspicion was sampling-bias —
  the sampled frames happened to land on fade endpoints. Corrected.)
- **Speed is a NON-ISSUE — the interp runs GAME at ~3000 fps headless** (measured: 3000 native
  frames incl. boot in ~1.0s; a 100000-frame run completes cleanly, no spin). The earlier
  "~13fps / recompile overlays for speed" guess was WRONG — it was confounded by PSXPORT_GPU_DUMP
  PPM disk I/O (one file write per present) and tool-call timing, not interp cost. Do NOT invest
  in recompiling GAME.BIN/SOP.BIN for speed; the flat interp is ~50× faster than needed for 60fps.
- **Frame pacing DONE — windowed now runs at the game's rate (~30fps), headless stays full speed.**
  `gpu_pace_frame()` (gpu_native.c), called once per native game-frame from `ov_frame_update`
  (games_tomba2.c, NOT from gpu_present — the boot stub drives many presents/frame and pacing
  those stalled the boot), SDL_Delay-throttles to the engine's vblank quota DAT_1f800235 (=2 =>
  30fps). Gated on PSXPORT_GPU_WINDOW != "0" (run.sh sets it to "0" headless — must check the VALUE,
  getenv presence is truthy for "0"); PSXPORT_NOPACE disables (fast-forward). Verified: windowed
  120 frames in ~5s (~30fps, reaches GAME, no crash); headless 3000 frames in ~1.0s (unpaced).
- **GAME progresses through real game flow.** Frame dumps show: title menu (Start Game/Options) →
  "Please wait" load → GAME stage → the **level intro cutscene with story text** ("Tomba is living
  peacefully in the country when Zippo finds a mysterious letter addressed to Tomba." —
  scratch/screenshots/move_test/mv_2847.png) → pulsed Start advances the dialogue → the level view
  (the clean jungle scene, view_2850). So menu/load/cutscene/level all work; input drives the flow.
- **Input test hooks (pad_input.c).** `PSXPORT_FORCE_BUTTONS=<hex>` pulses an active-low mask (edges,
  for menus/dialogue). New: `PSXPORT_FORCE_HOLD=<hex>` + `PSXPORT_FORCE_HOLD_AT=<frame>` HOLD a mask
  continuously from a native frame onward (movement input) — e.g. reach GAME via FFF7 then hold Right
  (FFDF) in-level. NB holding a non-confirm button in a cutscene freezes it (no Start edge to advance
  the text), so to reach interactive play, keep pulsing the confirm button until control begins.
- **NEXT — interactivity + audio:**
  1. Reach interactive control: pulse the confirm button past the intro cutscene(s) until Tomba is
     player-controlled, then verify movement (windowed with a controller is the natural check;
     headless, FORCE_HOLD a direction once control begins and watch the scene scroll).
  2. Audio (tests run PSXPORT_NOAUDIO).
  3. wide60: interpolate the paced 30fps to 60 (the project's headline feature).

## 2026-06-15 (later 36) — INPUT WORKS + OTC DMA fix → title→menu→GAME stage loads (no hang)
Two native-MAIN residuals from later-33 fixed; the boot now drives title → menu → **GAME stage**
on real pad input. Verified headless (state log + frame dumps).
- **Pad input was entirely dead in the native MAIN loop.** The game reads its slot-0 pad packet
  from the FIXED global `DAT_800BF4F8` (status/id/btnlo/btnhi) in `FUN_800524b4`; that buffer is
  filled per-VBlank by the SIO read `FUN_80003A4C`, hooked into the VBlank IRQ by StartPAD. Our
  no-IRQ runtime never fires it, AND `FUN_80003A4C`/`FUN_800040c4` (InitPAD) live in low text
  (0x80004xxx) BELOW MAIN.EXE so they're dead interp misses — the SIO pointer table at `0x0000AEC8`
  stays NULL (verified aec8==0 at boot), buffer status stays 0xFF (no-pad). FIX: new
  `pad_service_frame()` (pad_input.c), called once per frame in the native loop BEFORE the game
  reads input — polls host SDL input and writes the standard digital packet (status 0, id 0x41,
  buttons) straight to the fixed buffers `DAT_800BF4F8`/`51A` (the addrs `FUN_800520e0` passes to
  InitPAD via `FUN_80088b00`). Slot1 forced to 0xFF (no 2nd pad). Headless test hook
  `PSXPORT_FORCE_BUTTONS=<active-low-hex>` PULSES the mask (8 on / 24 off) so each press is a fresh
  EDGE the game's `cur & ~prev` logic (`FUN_800788ac`: `DAT_800e7e68 = ecf54 & ~ecf56`) sees.
- **THE big fix — OTC DMA (DMA channel 6) was unimplemented → malformed/cyclic OT → render hang.**
  `ClearOTagR` (FUN_80081458) builds the reverse-linked empty ordering table NOT in CPU code but by
  firing the **OTC DMA**: its libgpu-vtable worker `FUN_80082424` sets D6_MADR=&ot[n-1], D6_BCR=n,
  then writes `0x11000002` to D6_CHCR (0x1F8010E8). mem.c had no channel-6 handler, so the OT was
  never linked — entries kept stale values and the head pointed into the prim bump-buffer at
  0x800bfe68, where 4 menu prims formed a 4-node CYCLE (9C→88→7C→68→9C…). DrawOTag then walked the
  65536-node cap EVERY frame; harmless at the near-empty title but at the menu it drew huge prims
  65536× = effective hang (gdb caught it spinning in put_px_b under gpu_dma2_linked_list). FIX:
  implement OTC in mem.c io_write (0x1F8010E0/E4/E8): write n words descending from MADR, each →
  next-lower entry, lowest → 0x00FFFFFF terminator; clear CHCR busy so the lib's poll passes. After
  this the OT is a clean ~0x800-entry list ending at the 0xa5a60 sentinel — no cycle, no cap.
- **RESULT (verified, headless, PSXPORT_FORCE_BUTTONS=FFF7 = pulsed Start):** START→DEMO(title s2)
  → Start edge → s3 (menu) → Start → s5 (`FUN_80052078(2)` = load GAME) → **stage=GAME(0x8010637C)
  at frame 66**, GAME's own state machine ticking (demo_state 5→2). No OT warning, no hang,
  baseline (no input) still parks cleanly at the title. The interactive loop now runs until the
  window closes (PSXPORT_NATIVE_FRAMES caps headless; default 120 headless, unbounded windowed).
- **NEXT — GAME renders BLACK (residual).** GAME IS running (its sequencer ticks in the flat interp
  at pc≈0x80108a64, GAME.BIN) and IS drawing: per-frame GPU log shows a big VRAM upload (~15.4k
  gp0words = level textures) then ~29 prims/frame; `PSXPORT_ENVDBG` confirms the draw env is correct
  (clip (0,0)-(319,239) off (0,0) / clip (0,256)-(319,495) off (0,256), alternating double-buffer).
  BUT `PSXPORT_VRAMSCAN` shows all non-black VRAM is at x≥320 (the texture pages); the framebuffer
  region x=0..319 (both buffers) is ENTIRELY black. So the 29 prims don't rasterize into the
  framebuffer — GAME is most likely still at an early loading/fade state (or an asset it needs
  didn't load: the strNext attract stream times out, and there's an early "file not found" + UNIMPL
  B0:0x18). NB the flat interp makes GAME slow (~native-frame 130 in ~100s), so 120 native frames
  doesn't get far past the load. Tools added this session for the chase: `PSXPORT_VRAMSCAN` (whole-
  VRAM non-black bbox per present), `PSXPORT_ENVDBG` (GP0 E3/E4/E5 draw-clip/offset), `PSXPORT_OTDBG`
  (cyclic-OT chain dump). Next: drive GAME further (more native frames / recompile GAME.BIN fns for
  speed), find what it waits on to leave the load (CD/strNext/asset), and confirm whether prims are
  clipped-out vs sampling-black.

## 2026-06-15 (later 35) — AUTHENTIC BOOT WORKS: recompiled stub draws SCEA → LoadExec → MAIN title
Replaced the FAKE native_fmv intro with the AUTHENTIC boot: the disc's boot executable SCUS_944.54
is now **recompiled** and run as the real PSX entry, drawing toward SCEA, then handing to native
MAIN boot (later 33). One faithful path — no boot-mode env toggles (user directive).
- **Recompiler emits the stub as a SEPARATE module** (`emit.py --stub`, STUB_NAMES). The stub
  overlaps MAIN.EXE's address space (both load @0x80010000; stub text 0x10000–0x38800, entry
  0x80018B6C), so a shared `func_<addr>` namespace would collide. emit.py was refactored into
  `emit_module(exe, out_dir, Names, …)`: MAIN keeps `func/rec_dispatch/rec_set_override/g_override`;
  the stub gets `stub_func/stub_dispatch/stub_set_override/g_stub_override` + its own
  `stub_shard_*.c`/`stub_disp.c`/`stub_decls.h`. Both share `rec_dispatch_miss` (BIOS/interp) on a
  dispatch miss. 214 stub fns recompiled from the entry's jal graph; the rest run via the interp on
  the stub's RAM bytes (hybrid, same as MAIN). run.sh extracts SCUS_944.54 + passes `--stub`.
- **boot.c**: removed the `native_fmv_play("LOGO.STR"/"OP.STR")` fake intro. Single path →
  `native_stub_run(&c, MAIN.EXE path)` (runtime/recomp/native_stub.c): loads the stub over MAIN's
  low text, `stub_dispatch(c, 0x80018B6C)`; intercepts the stub's **LoadExec (BIOS A0:0x51)** via
  `g_loadexec_hook` → reloads MAIN.EXE (restoring the text the stub overwrote, as the real boot
  does) + longjmps out → `native_boot_run` takes over. (The stub loads MAIN by name through BIOS
  LoadExec — wrapper @0x80011340 calls the A0(0x51) trampoline with `cdrom:\MAIN.EXE;1`.)
- **Stub libcd/libetc overrides** (mirror cd_override.c/timing.c for the stub's own PSY-Q copies):
  CD_cw **0x8001A0C0** (NOTE: entry is 2 insns BEFORE the `addiu sp` prologue @0x8001A0C8 — the
  trace `… -> 8001A0C0` is ground truth, overriding 0x8001A0C8 silently no-ops) → success; CdSync
  0x80019B78 → 2; CdDataSync 0x8001A944 → done; VSync vblank-wait 0x80017FC4 → advance the native
  frame clock (counter DAT_800267B4) + deliver VBlank events + `gpu_present()`. With these, CD init
  passes cleanly (no more "CD timeout"/"Init failed") and ResetGraph runs.
- **Override plumbing for the stub**: `rec_set_override` is keyed by recompiled-function INDEX, so
  it can't target non-recompiled stub fns. Added (a) `stub_set_override` (the stub module's
  index-keyed table, for recompiled stub fns) and (b) `rec_set_interp_override` (a raw-address table
  in interp.c, consulted by `call_addr` AND by `rec_dispatch_miss` before it enters the interpreter)
  for interpreter-run stub fns. `native_stub.c::stub_override()` registers in both. Watchdog now also
  reports the interp PC and catches SIGSEGV/SIGABRT/SIGBUS with a backtrace.
- **SCEA NOW RENDERS → LoadExec → native MAIN boot (full authentic chain works).** The SCEA state
  machine (crt0 → 0x800111B4 → 0x80011A78, jump table @0x80010054, 20 states) drives the **CD
  controller at the REGISTER level** (0x800123B0 pokes 0x1F801800–0x1F801803 via pointers baked in
  stub .data @0x80025434/38/3C/40; polls the IRQ-flag reg low 3 bits = CD response 1=DataReady
  2=Complete 3=Ack 5=DiskError). Implemented a **native CD controller** (runtime/recomp/cdc_native.c,
  wired into mem.c io_read/io_write for 0x1F801800–3): index banking, param/response/data FIFOs, an
  interrupt queue (INT3-ack-then-INT2-complete), and the command set SCEA issues — GetTN(0x13),
  Init(0x0A), GetTD(0x14), ReadTOC(0x1E), **GetID(0x1A) → returns "SCEA" (licensed/America)**,
  Setloc(0x02), Setmode(0x0E); data reads served from disc.c. Commands complete SYNCHRONOUSLY (ready
  on the next poll) — correct for code that busy-polls without advancing time. The SCEA image is an
  embedded 4-bit-CLUT TIM @0x8001FB5C (no data load needed).
- **VSync: override the PUBLIC VSync(mode) 0x80017E4C** (not the low-level wait) — the stub's timed
  holds BUSY-POLL VSync(-1) expecting a preemptive VBlank IRQ to tick the count; we deliver none, so
  in our cooperative model EVERY VSync call (incl. query) ticks one frame + delivers VBlank events +
  `gpu_present()`. That makes the holds/fades progress and the screen visible. (mirrors timing.c.)
- **VERIFIED on-screen:** SCEA "Sony Computer Entertainment America" renders
  (scratch/screenshots/scea_logo.png), holds, fades; the stub then issues `LoadExec(cdrom:\MAIN.EXE;1)`
  which we intercept → reload MAIN + native MAIN boot → **TOMBA!2 title renders**
  (scratch/screenshots/post_handoff.png). Headless: `PSXPORT_NOWINDOW=1 PSXPORT_GPU_DUMP=dir
  PSXPORT_WATCHDOG=20 ./run.sh`. Remaining = the later-33 native-MAIN residuals (malformed OT, DEMO
  needs pad input, OP movie strNext streaming) — unchanged by this work.

## 2026-06-14 (later 34) — REAL BOOT PATH laid out (oracle call-trace): SCEA = the SCUS boot stub
**User correction:** SCEA is NOT an FMV and the port's `native_fmv_play` intro is a FAKE boot.
Used the oracle (Beetle wide60rt, real BIOS) with a new **streaming call trace**
(`PSXPORT_CALLTRACE="lo-hi[:path]"`, runtime/psxport_hooks.cpp + Beetle cpu.c; logs jal/jalr
targets in range with `# frame N` markers) to get the REAL boot call path. See memory
[[psxport-scea-boot-stub]] + [[psxport-use-oracle-trace]].
- **Finding (definitive):** across all 400 boot frames, 100% of calls execute in `0x80018xxx` =
  **SCUS_944.54 (the boot stub)** code; game-main `FUN_80050b08` is NEVER reached in that window.
  0x80018B6C decodes as valid code in SCUS, garbage in MAIN.EXE → it's the stub running. SCUS holds
  the PSX license string ("Sony Computer Entertainment Inc. for North America area") + `MAIN.EXE;1`.
- **Real boot path:** BIOS → **SCUS_944.54 stub draws the SCEA "…America Presents" screen** (high-
  res 700×480, TIM/font — not ASCII, not FMV, not BIOS) + loads MAIN.EXE → MAIN crt0 (0x800896E0)
  → game-main FUN_80050b08 → START → DEMO (Whoopee logo + OP movie FUN_80106f80) → menu.
- **Why the port has no SCEA:** `runtime/recomp/boot.c` loads MAIN.EXE DIRECTLY and enters game-
  main — it starts at the MAIN.EXE step, skipping the entire stub (SCEA). The native_fmv intro was
  a fake stand-in.
- **NEXT — replicate authentically:** run **SCUS_944.54 as the real entry** (like the BIOS does):
  load it to 0x80010000, run from its header entry (interpret via rec_interp/rec_coro_run — it's
  not recompiled; or add it as a 2nd recomp input). It draws SCEA itself, then loads MAIN.EXE and
  jumps to 0x800896E0 → the existing native MAIN boot (later 33) takes over for Whoopee→OP→menu.
  Blocker to expect: the stub's MAIN.EXE LOADER uses BIOS/its-own file I/O at stub addresses (NOT
  the MAIN.EXE cd_override addresses) — wire native CD/BIOS-file-read for the stub's loader. Trace
  the stub's load path with PSXPORT_CALLTRACE to find its file-I/O calls. REMOVE the fake
  native_fmv_play intro from boot.c.

## 2026-06-14 (later 33) — TITLE SCREEN RENDERS natively (full hybrid boot: init→sched→DEMO→draw)
The PC-PSX hybrid boot now reaches and RENDERS the Tomba!2 title screen
(scratch/screenshots/nb5_f20.png) with NO PSX scheduler/threads/ucontext — verified on-screen.
Chain: crt0 → native init prefix → native cooperative scheduler runs START (loads assets via
the task1/task2 loader handshake) → FUN_80052078(1) → DEMO stage → DEMO draws the title.
- **Native cooperative scheduler (no ucontext)** — `runtime/recomp/native_boot.c` +
  `rec_coro_run` (interp.c). Each task is a resumable coroutine: a yield captures the PSX
  register context and longjmps out; resume restores it and continues at the captured PC. The
  PSX stack lives per-task in g_ram (obj+8), so no native stack/ucontext is needed. `rec_coro_run`
  is a FLAT interpreter (PSX calls chain via the PSX stack, not the C stack), so it can resume
  mid-call-chain; native overrides + BIOS are still invoked as C. **ChangeThread (FUN_80080880,
  ov_switch) is THE switch primitive**: yield (FUN_80051f80,state1) / task-end (FUN_80051fb4,
  state0) / stage-transition (FUN_80052078,state3) all set state then ChangeThread → capture+
  longjmp. native_scheduler_step walks the 3 task slots like FUN_80051e60; FUN_800506d0 re-arms
  a yielded task 1→2 each frame. Per frame it delivers VBlank + sound-DMA(0xF0000009) events the
  game's TestEvent waits poll.
- **Recompiler now seeds from the overlays** (emit.py --overlays): functions reached only from
  the stage overlays (FUN_80044bd4 the task registrar, etc.) were Ghidra/jal-invisible → ran in
  the interpreter, un-overridable. emit.py scans START/DEMO/GAME.BIN for jal targets into resident
  text (109 fns; 1118→1220 recompiled). discdump `get` does nested paths (BIN/X.BIN).
- **Rendering wiring** — the native loop now does the draw/display-env handling it had omitted:
  per frame ClearOTagR the back buffer's OT + set PTR_DAT_800ed8c8 (env pair @ 0x800e80a8 +
  DAT_1f800135*0x2070 — Ghidra `+uVar1*0x81c` is WORD arithmetic = 0x2070 bytes; wrong stride
  was corrupting the odd buffer), and on DAT_1f80019c==0 submit PutDispEnv/PutDrawEnv/DrawOTag +
  flip. GPU hardening: sprite/rect blit clips its dx/dy to the drawing area up front (an
  off-screen sprite was burning w*h sample_tex calls and wedging the frame); OT traversal capped
  at 0x10000 nodes with a malformed/cyclic warning.
- **TOOL: frame-progress watchdog** (`runtime/recomp/watchdog.c`, PSXPORT_WATCHDOG=<sec>) — SIGALRM
  fires if no frame presents within N sec, dumps the stuck backtrace (backtrace_symbols_fd, link
  -rdynamic, build -g) and _exit. Found both wedges above precisely. Use it for any boot hang.
- **RESIDUALS (next):** (1) a MALFORMED/CYCLIC ordering table — DrawOTag from the OT head
  (madr=0x800ea0a4) chains a next-ptr into 0x800bfe68 and hits the 0x10000 cap EVERY frame →
  slow. Root-cause the OT the game builds vs the OT I ClearOTagR/DrawOTag (PTR_DAT_800ed8c8) —
  likely DEMO draws into a different OT, or AddPrim/primitive-buffer setup I'm missing. (2)
  non-fatal "time out in strNext()" CD-stream warning (attract-demo stream; CdReadyCallback not
  driven). (3) DEMO sits at title state (obj+0x48=2) — advancing to the menu/game needs pad input
  (obj+0x6b; ==7 → GAME) and/or the strNext stream. Run headless: PSXPORT_NATIVE_BOOT=1
  PSXPORT_SKIP_INTRO=1 PSXPORT_GPU_DUMP=dir PSXPORT_WATCHDOG=8 PSXPORT_NATIVE_FRAMES=N.

## 2026-06-14 (later 32) — NATIVE HYBRID DRIVER stood up: init prefix + 1st stage transition
Built `runtime/recomp/native_boot.c` (PSXPORT_NATIVE_BOOT=1, wired in boot.c). The PC engine
now drives game-main natively instead of running the infinite PSX scheduler:
- **Approach:** override game-main `FUN_80050b08` with `ov_game_main`. crt0 `func_800896E0`
  runs its BSS-zero/SP/gp/heap setup and calls main, which lands in our override. The override
  runs the ~25 init calls (transcribed 1:1 from FUN_80050b08:31275-31299) via `rec_dispatch`,
  NOT the scheduler loop. Helpers rc0/rc1/rc2/rc3 set a0..a2 then dispatch.
- **VERIFIED (RAM probes):** (1) init prefix runs clean — ResetGraph fires, no break during it
  (the `[break] code 1` after is crt0 post-main, expected). Scheduler state correct: task0
  state=2 runnable, entry=0x800499E8, name=A0F.BIN stack; tasks 1/2 free. (2) Running task 0's
  initial entry FUN_800499e8 natively (after `mem_w32(0x1f800138,0x801fe000)` to point the
  scheduler "current task" at task0) resolves \BIN\START.BIN and FUN_80052078(0) loads the
  overlay raw to 0x80106228 via the native CD override: count@0x80106228=6,
  entry-word@0x8010649c=0x27BDFE38 (exact), task0 state=3 entry=0x8010649C.
- **KEY mechanic:** with BIOS threads stubbed to no-ops, FUN_80051f80 (yield) / ChangeThread
  just return — so a NON-looping task fn (FUN_800499e8) called via rec_dispatch runs straight to
  completion. But the stage SEQUENCERS (FUN_801064f0 / DEMO / GAME entries) are infinite
  do/while(true) loops whose only exit is the scheduler's state==3 RESTART — with no-op
  ChangeThread they'd spin forever. So they MUST be reimplemented natively as per-frame state
  machines (one obj+0x48 iteration per frame == one original yield), called from a native frame
  loop that replaces the scheduler call FUN_80051e60 in LAB_80050c6c.
- **Game-main loop body (LAB_80050c6c), to reimplement natively:** per frame — DAT_800e809c=0;
  framebuffer ptr swap (DAT_800bf544/4f4 from DAT_1f800135); FUN_800788ac (tick, overridden by
  ov_frame_update); **FUN_80051e60 (scheduler -> replace with native stage step)**; FUN_80080f6c(0)
  (draw sync); busy-wait DAT_800e809c<DAT_1f800235 (vblank); FUN_800506d0 (present); buffer swap
  on DAT_1f80019c (0=swap+continue, 2=swap, 3=stay).
- **NEXT:** native frame loop + native step of stage 0 (START, FUN_801064f0): its loop loads
  asset manifests (OPN/A0*/TOMBA2.* into DAT_800be118/1e0/0f0), registers loader tasks
  FUN_80044f58/FUN_8004514c (run them directly — they complete synchronously w/ native CD), then
  transitions to DEMO. Then native DEMO step (the s0-s7 SM, leaf fns listed in "later 31") to
  reach the title/menu on-screen; then GAME. Drive native FMVs at the right point (START plays
  OPN, the menu->game uses the existing native_fmv path).

## 2026-06-14 (later 31) — STAGE MAP NAILED: START/DEMO/GAME overlays; corrects "later 30"
The 3 stage entries in `PTR_LAB_800a3ecc[0..2]` = `{0x8010649c, 0x801062e4, 0x8010637c}` are
entry points into THREE SEPARATE overlay files, all loaded RAW to the **same base 0x80106228**
(so only one is resident at a time — that's why stages 1/2 are NOT disassemblable from
`ram_f1000.bin`; at that snapshot 0x801062e4/0x8010637c hold START.BIN's *string/data* bytes,
not code). `FUN_80052078(stage)` = `FUN_800450bc` reads `(&DAT_800be1e0)[stage*2]` (lba,size)
to `0x80106228` then restarts task 0 at `PTR_LAB_800a3ecc[stage]`. The 3 files come from the
START.BIN manifest `\BIN\{START,DEMO,GAME}.BIN`:
- **Stage 0 = `\BIN\START.BIN`** (LBA 1904, 1648 B) → entry `0x8010649c` = intro/boot
  sequencer (`FUN_801064f0` is its tail). Loads asset manifests (OPN/CRD/SOP/A0*.BIN,
  TOMBA2.{IDX,IMG,DAT,SND}, SWDATA.BIN, VOICE/DEMO/BGM.XA), registers loader tasks, then
  state 3 → `FUN_80052078(1)` → DEMO.
- **Stage 1 = `\BIN\DEMO.BIN`** (LBA 1879, 5372 B) → entry `0x801062e4` = **title/menu +
  attract** sequencer. 8-state jump table @`0x8010622c`. State 4 (`0x80106580`) reads the menu
  result `obj+0x6b`: ==7 → set `DAT_1f800134=1`, go state 5 (`0x801065dc`) → `FUN_80052078(2)`
  = **start GAME**; ==1/2 → state 2.
- **Stage 2 = `\BIN\GAME.BIN`** (LBA 1882, 11636 B) → entry `0x8010637c` = **gameplay**
  sequencer. Sub-state from `DAT_1f800134`; per-frame loop (incr `obj+0x198`, `FUN_80051f80`
  yield) dispatching state handlers `FUN_801086e0/720/784` (all inside GAME.BIN). GAME→...
  `FUN_80052078` at `0x80108a48` (back to a stage).
- **CORRECTIONS to "later 30":** (a) the 0x80106xxx resident overlay is **START.BIN, not
  OPN.BIN** (OPN.BIN is just one asset START.BIN loads; verified: extracted START.BIN is
  byte-identical to ram_f1000@0x80106228, 0 diffs, entry word `27bdfe38`). (b) START.BIN is
  BOTH the manifest AND code (one overlay). (c) "disasm stages 1/2 from ram_f1000" is a dead
  end — extract the files instead.
- **Tooling:** `tools/disasm_overlay.py <bin> [start] [end] [--base=]` disassembles an overlay
  at base 0x80106228 (resyncs past data/jump-tables). Extract overlays with
  `scratch/bin/fmv_compare dumplba <lba> <size> <out>`. Reference dumps in
  `scratch/decomp/{START,DEMO,GAME}.bin.asm`; overlay bins in `scratch/bin/overlays/`.
- **DEMO state machine** (obj+0x48 = state; jump table @0x8010622c; common tail incr obj+0x198
  then `FUN_80051f80` yield then loop). Decoded from scratch/decomp/DEMO.bin.asm:
  - **s0** `0x801063c0`: `FUN_80045080(0x80108f9c,2)`; `FUN_80044bd4(FUN_80044f58,2,0,0)` (run
    loader); `FUN_8007982c`+`FUN_80075240`+`FUN_8001cf00(1)` (gfx/audio setup); -> s1.
  - **s1** `0x8010641c`: `FUN_80106f80(0)` (load/fade poll) -> if done s2; watches DAT_800e7e68.
  - **s2** `0x80106464`: `FUN_8010696c()` = title input/decision -> 1: s7; 2: branch on obj+0x68
    -> s3 or s4 (clears obj+0x50/0x6b, the menu result).
  - **s3** `0x801064e8`: `FUN_80106ac4()` -> 1: s7; 2: obj+0x68==1 -> s5 (DAT_1f800134=0) else s6
    (`FUN_800750d8(1)`); 3: back to s2.
  - **s4** `0x80106580`: `FUN_8007bf20(0,0,0)`; reads **menu result obj+0x6b**: ==1 -> s2/obj+0x68=1;
    ==2 -> s2/obj+0x68=0; **==7 -> s5 + DAT_1f800134=1** (start GAME).
  - **s5** `0x801065dc`: `FUN_80052078(2)` -> load GAME.BIN, restart task 0 at stage 2.
  - **s6** `0x801065ec`: `FUN_8007b45c`; if obj+0x50==3 `FUN_80106824(1)`+`FUN_80106690(1)`;
    `FUN_8001cf2c`/`FUN_80075a80`. **s7** `0x80106668` (tail, undisassembled here).
  Leaf fns the native DEMO driver must call: FUN_8010696c, 80106ac4, 8007bf20, 80106824,
  80106690, 80106f80, 80045080, 80044bd4(FUN_80044f58), 8007982c, 80075240, 8001cf00/8001cf2c,
  80075a80, 800750d8.
- **NEXT:** decode DEMO s7 + GAME's handlers (FUN_801086e0/720/784); then build the native PC
  driver (hybrid): run `FUN_80050b08` init calls, then a native frame loop = native re-impl of
  each stage's per-frame state machine calling the recomp leaf draw/update fns (CD/wait already
  native), no scheduler. Exploit "leaf tasks complete synchronously" — call them directly.

## 2026-06-14 (later 30) — DIRECTION: PC-PSX HYBRID (PC drives recomp logic); boot RE started
**User architecture decision** (see memory `psxport-hybrid-architecture`): psxport is a PC-PSX
HYBRID. PC-native engine owns boot/graphics/FMV/audio/input and is the DRIVING FORCE — it owns
the frame loop and **calls the game's RE'd per-frame entry points directly**, never blocked by
PSX threading/waits/IRQ ping-pong (all stripped). Game LOGIC (AI/quests/player/menu) stays
recomp. Execution model = "PC loop calls RE'd per-frame entry points": reimplement the control
flow (sequencing / per-frame state machine) natively, call the game's leaf logic functions
(draw, per-object update) which are normal recompiled functions that return. NOT running the
infinite-loop yielding tasks as-is (needs ucontext, ruled out). SCEA is game-drawn (not BIOS).
Target flow: PC boot -> SCEA -> WhoopeeCamp FMV -> OP FMV -> main menu. FMVs done.
- **Boot RE so far:** game main = `FUN_80050b08` (init calls: FUN_80089788/80085b20/800898a0/
  80080bf0(3)/.../80085900(3)/8001cc00/800520e0/80085900(1)/**80051e00**(sched init)/
  **80051f14(0,FUN_800499e8)**(register first task)/80085bb0; then the scheduler loop at
  LAB_80050c6c: per-frame FUN_800788ac(tick)+FUN_80051e60(scheduler)+vblank-wait+FUN_800506d0,
  branching on DAT_1f80019c). The PC engine should run the INIT calls then replace the loop.
- **First task** `FUN_800499e8` resolves `\BIN\START.BIN`, then `FUN_80052078(0)`.
- **START.BIN is a FILE MANIFEST**, not code (extracted via new harness `dumplba`): count=6 +
  paths `\CD\SWDATA.BIN \CD\TOMBA2.{SND,DAT,IMG,IDX}` then many `\BIN\A0*.BIN` overlay files.
- **Stage-entry table** `PTR_LAB_800a3ecc[0..2]` (MAIN.EXE data @0x800a3ecc) = `{0x8010649c,
  0x801062e4, 0x8010637c}` (the 0x80106xxx OVERLAY region = intro sequencer FUN_801064f0 etc.,
  loaded from disc, interpreted — partially decompiled in scratch/decomp/overlay.c).
  `PTR_FUN_800a3ed8` = `0x800499e8` (resident).
- **Stage entries are NOT in ram_f1000_all.c decomp** — disassemble from the RAM snapshot
  `scratch/bin/tomba2/ram_f1000.bin` with `python3 tools/disasm.py <dump> <a> <b>` (capstone).
  **Stage 0 = 0x8010649c = the PROLOGUE of the intro sequencer**; overlay.c's FUN_801064f0
  (0x801064f0) is just its TAIL (the do/while loop — Ghidra split the function). Stage 0
  sets up a task obj (stack@sp+0x190), calls FUN_80081218 + FUN_80080f6c, then resolves
  OPN.BIN/START.BIN/IDX/XA files and runs the state machine that chains stages via
  FUN_80052078(param) -> restart task at PTR_LAB_800a3ecc[param]. Stages 1,2 = 0x801062e4,
  0x8010637c (disasm next). The 0x80106xxx overlay (OPN.BIN) is resident in ram_f1000.bin.
- **NEXT (step by step):** (1) disasm stages 1/2 (0x801062e4, 0x8010637c) + find where SCEA
  is drawn and the SCEA->WhoopeeCamp->OP->menu transitions live; (2) build the PC-driven boot:
  native engine runs FUN_80050b08's init calls, then a native frame loop drives each stage
  (call leaf draw/update via rec_dispatch/interp, native FMV for the two movies), no scheduler.
  Harness `dumplba <lba> <nbytes> <out>` extracts any sector range.

## 2026-06-14 (later 29) — FMV FULLY WORKING (video+audio+speed); next: boot -> main menu
FMV is done — video, audio, and speed all correct (verified on-screen by user: WhoopeeCamp
logo + Tomba render cleanly, with sound, correct rate, clean to the corners).
- **Column-major macroblocks** (`911bf4a`'s predecessor `9ddb98f`): the game emits MDEC
  macroblocks COLUMN-major (emit index k -> row=k%mby, col=k/mby); we placed row-major ->
  sheared/spread frames. Found via the `framemap` ASCII oracle (synthetic row-major tiling
  tests all passed; only a real frame showed the shear, stride=mby). Don't re-chase.
- **XA-ADPCM audio** (`911bf4a`): STR interleaves CD-XA audio sectors (submode 0x04, Form2);
  player dropped them. `xa_decode_sector` transcribed from mednafen cdc.c (oracle), dedicated
  SDL device at 37800Hz. `disc_read_raw` exposes the raw 2352B sector (the XA subheader).
  Sound oracle `tools/fmv_compare xacmp` = bit-exact vs independent reference (0/250k diff).
- **Speed**: paced to the AUDIO/media clock (was a fixed-15fps guess = too slow).
- **Bottom-right black bits**: final <0x20-word MDEC remainder fell to a linear data-port
  read (no voffs scatter); `mdec_dma_out_rest` drains it scatter-aware (MDEC_DMARead isn't
  gated by the 0x20 burst, mdec.c:899). Drain now 100% scattered.
- FMV harness modes: `<lba> <size> <frame#>` (VLC diff), `idcttest` (table self-test +
  placement tests), `framemap` (ASCII luma map), `strscan` (CD-XA framing), `xacmp` (sound).

**NEXT — user's target flow:** PC boot -> SCEA -> FMV#1 (WhoopeeCamp/LOGO.STR) -> FMV#2
(Tomba/OP.STR, Start to skip) -> **main menu**. FMVs done; SCEA + the main menu are the gap.
The main menu is interactive GAME code built on the cooperative task scheduler (FUN_80051e60 +
OpenThread/ChangeThread, which we stubbed to no-ops). Reaching it needs that scheduler working
WITHOUT ucontext/threading (per user) — i.e. run cooperative tasks via the hybrid interpreter,
which holds explicit PSX CPU state (PC/regs/SP-in-g_ram) so a yield = save/restore a state
struct (no native stack, no ucontext). Investigate interp.c + scheduler next.

## 2026-06-14 (later 28) — FMV compare harness vs oracle: VLC proven correct; quant-order fix
Built `tools/fmv_compare` (commit `2d41a68`) per user request. Our FMV pipeline already runs
the decoded run/level stream through mednafen's MDEC (the oracle) for IDCT/YCbCr, so the only
non-oracle code is the STR-demux + BS/MPEG-1 VLC (`bs_decode_frame`). The harness reads a real
STR frame off the disc and diffs our VLC output against an INDEPENDENT reference decoder
(fresh Table B-14 transcription as bit strings, independent bit reader/escape/sign), word-for-
word, first divergence → (block,coeff). An `idcttest` mode links the REAL mednafen MDEC and
decodes synthetic blocks to verify the quant+IDCT table uploads.
- **VLC is CORRECT (verified):** LOGO frames 5/40 + OP frame 70 all MATCH the reference
  (3600/7001/12731 codes identical). The earlier "identical ramp across blocks" was real
  content, not a bug. Earlier fear of a DC-prediction/VLC bug = WRONG, ruled out.
- **Tables reconstruct correctly (idcttest):** DC-only block → flat; single AC coeff → clean
  cosine ramp. (Orientation looks "transposed" but that is mednafen's internal Coeff/ZigZag
  convention, self-consistent with how the game feeds it — NOT our bug. Don't re-chase it.)
- **FIX (quant order):** the quant matrix was uploaded RASTER, but mednafen stores it linearly
  (mdec.c:844) and dequantizes via `QMatrix[CoeffIndex]` (scan order, mdec.c:703) → it must be
  ZIGZAG/scan order: `qz[scan]=quant_raster[ZigZag[scan]]`. DC (idx0) unaffected (ZigZag[0]=0);
  AC got the wrong per-frequency weight. Now reordered in `mdec_upload_tables`.
- **Harness gotcha (fixed):** for the synthetic test, sample a macroblock well past FIFO
  warm-up and index `px[(mb*16)*WIDTH + ...]` (row stride!) — an early off-by-stride read hit a
  no-AC block and falsely showed "AC dropped".
- **STILL RESIDUAL:** real frames remain visibly blocky (recognizable but heavy). VLC + core
  tables are verified-correct, so the residual is DOWNSTREAM: suspect the MDEC FIFO feed/drain
  interleave (`mdec_decode_to_rgb555` burst loop) and/or the chroma (Cb/Cr 8x8) path. The
  `idcttest` showed an odd/even-row wrinkle on the single-AC block worth chasing there. Use the
  harness (extend `idcttest`) to nail it. `bs_decode_frame`/`mdec_decode_to_rgb555` are now
  non-static for harness use.

## 2026-06-14 (later 27) — DIRECTION: NO threading / NO ucontext; PC-native intro boot
**User (emphatic): "no threading", "you can't use ucontext", "boot the game PC native way."**
The intro wedge was diagnosed, then the whole emulated-thread approach was dropped.
- **Root cause of the intro wedge (the ucontext path):** the game runs a cooperative task
  scheduler (`FUN_80051e60`) over task objs at `0x801fe000` (stride 0x38; state word at
  obj+0: 0=free 1=sleeping 2=runnable 3=restart-at-new-entry 4=running). The intro sequencer
  (`FUN_801064f0`, idx0) is an infinite SM on its OWN obj+0x48; at inner-state 1 it calls
  `FUN_80044bd4(FUN_8004514c,1,1,0)` which (param_4==0) **busy-waits `while(DAT_1f80019b==0)
  FUN_80051f80(1)` for the loader to signal done**. The loader (`FUN_8004514c`, idx2/801fe070)
  yields at state=1 inside a sub-call and **never reaches `DAT_1f80019b=1; FUN_80051fb4()`**,
  so the flag stays 0 → registrar spins forever → sequencer frozen at inner-state 2 → intro
  never advances. Confirmed via the obj+0x48/state trace in threads.c (PSXPORT_THR_TRACE).
  The native CD/file loads all SUCCEED (12 reads incl START.BIN@1904 + 326KB@LBA9684); the
  wedge is the cooperative-task completion handshake, not I/O.
- **Decision:** the game's runtime IS a coroutine task system (infinite-loop tasks that yield/
  resume mid-function each frame) — running that recompiled code faithfully needs stack
  save/restore (ucontext/fibers). Per user we are NOT doing that. So we **do not run the
  game's intro scheduler at all** — drive the intro natively.
- **Done (commit `b60c3d4`):**
  - `threads.c`: ucontext coroutine layer **removed**; OpenThread/CloseThread/ChangeThread are
    now no-op stubs (scheduler no longer run).
  - `boot.c`: native intro — `native_fmv_play("MOVIE/LOGO.STR")` (SCEA + Woopee Camp) then
    `("MOVIE/OP.STR")` (Tomba!2 opening). `func_800896E0` (scheduler entry) NOT entered.
    `PSXPORT_SKIP_INTRO=1` bypasses.
  - `native_fmv.c`: per-frame Start-skip (rising-edge) + pacing (`PSXPORT_FMV_FPS`, default 15).
  - `run.sh`: **native_fmv.c was never in the build** (FMV player was dead code, 0 callers) —
    added to SRC; added `-lm` (IDCT cos/lround).
  - Disc map: `MOVIE/LOGO.STR` LBA 11491 (2.6MB), `MOVIE/OP.STR` LBA 152238 (20.7MB),
    `MOVIE/END.STR` LBA 162374.
- **Verified:** builds, boots, both FMVs demux→VLC→MDEC→present end-to-end (300 MBs/frame,
  320x240), zero threads/ucontext. Frames in `scratch/screenshots/intro2/`.
- **KNOWN BUG (next):** `bs_decode_frame` DC-coefficient prediction is wrong — early frames
  wash out (pale), later frames show the real scene with vertical banding (OP visibly a dark
  red scene; LOGO mostly white). Pre-existing decoder bug (from the e75f1fd FMV commit), only
  now exercised. Decode-quality pass is the next task.
- **OPEN (future milestone):** post-intro hand-off (title/gameplay) is the game's task system;
  running it without threading/ucontext needs a resumable-execution design (undecided).

## 2026-06-14 (later 9) — DIRECTION CHANGE: native PC port (static recomp); decoder S0 done
**User: "new direction — port to PC, no PSX emulation, no PSX BIOS."** wide60/emulator path
paused; full plan in `docs/recomp_port_plan.md`. Approach = instruction-level static
recompiler (MIPS R3000A → C), HLE BIOS, peripherals (GTE/GPU/SPU/MDEC/CD) **lifted from the
GPL-2 Beetle fork**, diffed bit-exact against Beetle as oracle. Faithful-first, then wide60.
- **S0 decoder DONE + validated:** `tools/recomp/{psexe.py,decode.py,test_decode.py}` (8/8,
  anchored to the real Tomba2 entry words). Full R3000A + COP0 + COP2/GTE coverage. Verified
  **0% unknown over 28480 words** of real game code.
- **CRITICAL input finding:** the recompiler input is **NOT the boot EXE `SCUS_944.54`**.
  Boot-EXE text `[0x80010000,0x80038800)` differs from frame-1000 RAM in **98.8%** of words
  (EXE `0xFFFFFFFF` vs RAM `27BDFFD8` real prologue at `0x8001FC50`). The boot EXE is a
  **loader stub**; the real game = a **resident core + overlays loaded from the CD** over
  `0x80010000+` (spans past boot text — `jal 0x8011534C`). The 1886-fn Ghidra decomp matches
  the RESIDENT image (0% unknown), not the boot EXE. NEXT: recursive ISO9660 lister (extend
  `tools/discdump`) to find the on-disc main executable + overlay files = clean static inputs.

## 2026-06-14 (later 10) — recompiler input found: MAIN.EXE (validated 99.9% vs RAM)
Added `discdump list` (recursive ISO9660 tree) + `discdump get <NAME>`. Disc tree shows the
real game executable: **`MAIN.EXE`** (root, LBA 23, 716800 B) — entry `0x800896E0`, load
`0x80010000`, text `0xAE800`, SP `0x801FFFF0`. Extracted to `scratch/bin/tomba2/MAIN.EXE`.
- **Validated:** MAIN.EXE text vs resident RAM_f1000 = **99.9% identical** (262/178688 diffs =
  runtime data writes); **all 1596 in-range Ghidra fns decode 0% unknown** from the clean
  file. So MAIN.EXE IS the recompiler input; `SCUS_944.54` is just the boot stub that loads it.
- Overlays load above MAIN's text end `0x800BE800` (`jal 0x8011534C`, intro SM `0x80106xxx`)
  from `BIN/*.BIN` — later concern. FMVs are `MOVIE/{LOGO,OP,END}.STR`. Full disc map in
  `docs/recomp_port_plan.md`.
- NEXT (S1): emitter — recursive-descent decode from `0x800896E0` (+1596 fn-entry seeds) →
  C per function, dispatch table, modeled R3000 state + memory accessors.

## 2026-06-14 (later 11) — S1 emitter done: full core compiles, leaf semantics verified
`tools/recomp/emit.py` translates MAIN.EXE → C: **all 1597 functions** → `generated/
tomba2_rec.c` (6.6 MB), **compiles clean** (3.5 MB .o). Runtime: `runtime/recomp/{r3000.h,
mem.c,stubs.c}` (R3000 state, flat 2 MB RAM+scratchpad, lwl/lwr/swl/swr, R3000 div sem).
- Emitter handles delay slots, intra-fn goto/labels (only for emitted addrs; data-region
  branch targets route to rec_dispatch → no undefined labels — this was the one compile bug,
  caused by data blobs in inter-fn gaps), direct-call vs rec_dispatch, generated dispatch.
- **Verified** on 3 hand-checked leaf fns incl. delay-slot effects (`test_leaf.c`, all pass):
  `0x80089A30`→v0=0x800ABFD4 (lui+DS addiu), `0x800535D4`→mem8(a0+374)+1, `0x800269EC`→v0=1
  +store. Reproduce: `tools/recomp/build.sh`.
- Faithful-first simplifications to verify via harness: no load-delay; add==addu; computed
  `jr`→rec_dispatch (switch-table recovery later); data blobs emitted as dead fns.
- NEXT (S2): load MAIN.EXE into g_ram, entry trampoline `func_800896E0`, HLE syscalls +
  A0/B0/C0 vectors; stand up S4 diff harness vs Beetle in parallel.

## 2026-06-14 (later 12) — S2 started: recompiled core RUNS from boot; HLE surface mapped
`runtime/recomp/boot.c` loads MAIN.EXE into g_ram, enters `func_800896E0`. Emitter now
discovers direct-`jal` targets (fixpoint, stops at first UNKNOWN so data doesn't inject
seeds) → caught a Ghidra-missed fn `0x80089860` (1597→1598). Dispatch misses route to
runtime `rec_dispatch_miss`. **The core executes real boot code.** Measured boot needs:
- BIOS (in order): `A0:0x39` InitHeap, `B0:0x19`, `B0:0x5B`, `C0:0x0A` ChangeClearRCnt,
  `A0:0x72`, `B0:0x35`. Then indirect fn `0x8009A8E8` (via `jalr` — direct-jal discovery
  can't see it; needs a fn-ptr/indirect seed or manual add).
- HW regs: I_MASK/I_STAT, DMA DPCR, Timer1, CDROM, and a **GPUSTAT `0x1F801814` ready-poll**
  that spins (mem.c returns 0). Minimal GPU/timer status needed to advance.
- NEXT: A0/B0/C0 HLE table for those ~6 calls + seed `0x8009A8E8` + minimal GPU/timer
  status; stand up S4 diff harness vs Beetle to verify bit-exact. Build: `tools/recomp/
  build.sh` (leaf tests); boot recon: compile boot.c instead of test_leaf.c, run under
  `timeout`.

## 2026-06-14 (later 13) — S2: recompiled core boots through BIOS into CD/event subsystems
`runtime/recomp/hle.c` = recomp-native HLE BIOS (transcribed faithfully from the proven
`hle_kernel.cpp`): heap A0:0x33-0x39, HookEntryInt, FileWrite→stderr, GetB0/C0Table,
ChangeClearPAD, GPU_cw, C0 installers, and `syscall` Enter/ExitCriticalSection via `$a0`.
`mem.c` reports GPUSTAT (`0x1F801814`) permanently ready (+toggling bit31) to clear the
boot ready-poll. Emitter EXTRA_SEEDS for jalr-reached fns `0x8009A8E8/ADC4/AA4C`.
- **Verified**: boot runs deep real game code — heap init → HookEntryInt → CD init (emits
  `CD_init`/`CD_cw`/`CD timeout` via FileWrite) → past GPU handshake → OpenEvent/EnableEvent/
  WaitEvent loop + CD-command retry loop. Reproduce: `tools/recomp/build.sh` (leaf tests +
  boot). Leaf tests still pass.
- **S5 boundary (honest stop):** the CD-retry + WaitEvent loops block on CD-complete / VBlank
  **IRQs that nothing generates yet**. Faking "event fired"/CD-done = bandaid (refused).
- NEXT (big phase): peripheral + IRQ/event delivery (lift CD/VBlank/GPU/SPU from Beetle; wire
  IRQ → invoke s_int_handler like wide60 hle_irq.cpp; implement events). Plus S4 diff harness
  vs Beetle, and an auto indirect-pointer (lui+addiu) seed scan to end CD-helper whack-a-mole.

## 2026-06-14 (later 14) — DIRECTION: no CD/HW emulation; native overrides infra DONE
User refined: "no CD code, no emulation, pure PC native." → don't emulate CD/IRQ; **override
the game's CD/streaming fns with native file I/O, synchronous completion**. This is the
recomp-overrides path. Override points already RE'd this session: `FUN_8008c1ec` (read
blocks@LBA), `FUN_8008bf50`/`FUN_8008b8f0` (CdSearchFile), read-SM `FUN_8008c294`/done flag
`0x800AC308`/completion `FUN_800899bc`, low-level `CD_cw` loop (`0x8009Axxx`).
- **Override infrastructure DONE + validated:** emitter emits `gen_func_X` (recomp body) +
  `func_X` wrapper checking a runtime override slot; `rec_set_override(addr,fn)`/
  `rec_func_index`. Body kept alive (A/B + diffable), overrides fire on direct+indirect
  calls, super-call = `gen_func_X`. `test_leaf.c` verifies replace/fire/super-call/toggle-off
  (all pass). Matches recomp-overrides skill (runtime table, not compile-time exclusion).
- NEXT (S3): native by-LBA disc backend (flat image or libchdr) + override the CD
  read/resolve/complete fns to use it synchronously; native VBlank/event source for
  WaitEvent. Then verify boot reaches title/FMV. Plan: docs/recomp_port_plan.md.

## 2026-06-14 (later 15) — CD override targets pinned; seed mistake corrected
Mapped the exact functions to override for native-file CD (no emulation), all recompiled:
- **`0x8008B2D8` CdInit** = boot blocker (emits CD_init then CD_cw/CD timeout polling CD I/O
  regs with no IRQ → spins). `0x8008AC34` CD_cw, **`0x8008A6EC`** low-level command+wait
  (CD-timeout chokepoint). `FUN_8008c1ec` read-N@LBA, `FUN_8008c294` read-SM/done
  `0x800AC308`, `CdSearchFile 0x8008b8f0`.
- **Corrected my mistake:** `0x8009A8E8/ADC4/AA4C` were NOT functions — they're mid-function
  jump-table labels inside the **printf/format-parser at `0x8009A76C`** (indirect-only,
  Ghidra-missed), surfaced as misses because computed `jr` → rec_dispatch (no jump-table
  recovery). Replaced those seeds with the real entry. Parser still needs jump-table recovery
  OR a native printf override (the PC-native fix). Not the boot blocker (just debug logging).
- NEXT (S3): native by-LBA disc backend (discdump image / libchdr) + override CdInit +
  command-wait + FUN_8008c1ec to complete synchronously from file; native VBlank/event for
  WaitEvent; verify boot → title/FMV. Override targets all in docs/recomp_port_plan.md.

## 2026-06-14 (later 16) — S3 CD DONE: native by-LBA reads, CdInit/timeouts gone; boot → VSync
Implemented the native CD backend + overrides. **Boot now runs CdInit and CD commands
natively (no controller, no IRQ handshake) and advances past CD into graphics/event init.**
- **Disc backend `runtime/recomp/disc.c`** (libchdr, prebuilt `build/.../libchdr-static.a`):
  `disc_read_sector(lba, out2048)` = hunk-cached CHD read, extracts the 2048-B user data
  (mode-aware offset), same as `tools/discdump.cpp`. Disc path via PSXPORT_TOMBA2_DISC /
  PSXPORT_DISC / `.env`. **Verified standalone:** LBA 16 = `CD001` (ISO PVD), LBA 23 = `PS-X`
  (MAIN.EXE header) — correct bytes by LBA.
- **CD overrides `runtime/recomp/cd_override.c`** (recomp-overrides; bodies kept alive):
  `0x8008B2D8` CdInit → v0=0 (drive ready, skip HW handshake; caller still installs libcd
  callbacks); `0x8008AC34` CdCommand → 0, `0x8008A6EC` CdSync → 2 (the spin-on-DAT_800ac298
  waiters, now moot — every data read is native); `0x8008C1EC` `FUN_8008c1ec(blocks,lba,buf)`
  → reads blocks×2048 from the disc straight into buf, returns 1. Registered by
  `cd_overrides_init()` from boot.c. build.sh links libchdr+lzma+miniz+zstd.
- **Result:** the `CD timeout` / `CdInit: Init failed` spin is GONE. Boot proceeds: ResetGraph
  → **`VSync: timeout`** (next blocker) + event/thread/pad/card BIOS calls now surfacing
  (B0:0x08 OpenEvent, 0x0A WaitEvent, 0x0C EnableEvent, 0x0E OpenThread, 0x4A/4B card,
  C0:0x02/03 SysEnqIntRP, A0:0x70 _bu_init). No `FUN_8008c1ec` read fires yet — the game
  stalls at VSync before it mounts the CD filesystem, so the override read is verified by the
  standalone backend test, not yet end-to-end in-game.
- **NEXT (S3 cont.): native VBlank/VSync + events.** `FUN_80085900` is libetc VSync; the
  vblank counter is **`DAT_800abde0`**; `FUN_80085a78(target)` spins until it reaches target
  → `VSync: timeout` because no IRQ increments it. Fix = a native frame source: override
  `FUN_80085900` to advance `DAT_800abde0` per VSync(0) and return it for VSync(-1); implement
  the event table (OpenEvent/EnableEvent/TestEvent/WaitEvent) in hle.c + deliver VBlank per
  frame tick (transcribe from the proven wide60 hle_kernel.cpp). Then the game should mount the
  CD FS (FUN_8008bbe8 → FUN_8008c1ec reads) and we verify the native read path end-to-end.

## 2026-06-14 (later 17) — events+VSync+threads HLE'd: boot REACHES the StrPlayer main loop
Implemented the rest of S3's "native VBlank/event" surface; **boot now runs into the resident
StrPlayer main loop `FUN_80050b08` (the per-frame game loop)** — verified deterministically
via gdb backtrace (the recomp uses the native C stack, so `bt` names the game fn:
`gen_func_80050B08 <- gen_func_800896E0 <- main`, identical across 3 runs). Leaf tests pass.
- **Events in `hle.c`** (transcribed from proven wide60 hle_kernel.cpp): B0:0x07 DeliverEvent,
  0x08 OpenEvent, 0x09 Close, 0x0A WaitEvent (can't block → reports ready+clears `fired`),
  0x0B TestEvent (read+clear), 0x0C Enable, 0x0D Disable. 16 EvCB slots, id base 0xF1000000.
  Plus B0:0x12-0x16 pad no-ops, 0x4A/0x4B card, C0:0x02/0x03 SysEnq/DeqIntRP→elem, A0:0x70
  _bu_init.
- **Native VSync `runtime/recomp/timing.c`**: overrides libetc VSync `FUN_80085900` — VSync(0)
  advances a native frame clock into `DAT_800abde0`, VSync(-1) queries it. Killed the
  `VSync: timeout` spin (`FUN_80085a78`).
- **BIOS threads (hle.c, STOPGAP):** OpenThread hands back a handle + records entry PC;
  **ChangeThread is a NO-OP** — the static-recomp core runs on the native C stack, so a real
  PC+reg context switch isn't possible by swapping a struct (unlike the wide60 interpreter).
  Fine while boot is straight-line; the StrPlayer FMV prebuffer thread + the 0x80080860
  green-thread coroutine primitives will need a real coroutine override (ucontext/sep stack).
- **CURRENT BLOCKER (next):** the main loop spins at the per-frame **vblank pace-dwell
  `0x80050CE4`** = `do {} while (DAT_800e809c < DAT_1f800235)`. `DAT_800e809c` (display-frame
  counter, 0x800E809C) is bumped by the game's **VBlank ISR callback**, registered at
  `0x80050C58` via `FUN_80085bb0` = libetc **VSyncCallback** (routes to the libapi interrupt
  vector `*(0x800abda0+0x14)(4, cb)` — UNMODELED, so nothing increments it). The callback is
  `&LAB_800506b4`, a **mid-function label** (not a recompiled entry → can't just rec_dispatch
  to it). FIX OPTIONS: (a) override `FUN_80085bb0` to capture the cb addr(es) + seed
  `0x800506b4` as a callable entry (emitter seed) and pump the cb once per frame between the
  counter reset (0x80050C?? sets DAT_800e809c=0) and the dwell — natural pump point is the
  pre-dwell call `FUN_80080f6c(0)`; (b) model the libapi interrupt vector + a frame tick that
  invokes registered class-4 (vblank) callbacks. (a) is the localized, PC-native route.
  Tooling note: **gdb attach + `bt` is the spin locator** for the recomp (C stack == game
  call stack); build `boot_dbg` with `-O1 -g`.

## 2026-06-14 (later 18) — "don't dwell": main loop runs per-frame work; next = BIOS threads
**User steer (saved to memory [[recomp-port-busywaits]]): port HW busy-waits to PC behavior
("make it not dwell"), don't simulate the VBlank IRQ to satisfy them.** Applied: dropped the
vblank-callback capture/seed/pump idea; instead `games_tomba2.c` overrides the per-frame state
update `FUN_800788ac` (sole caller = the main loop, runs once per iteration before the dwell)
to super-call its body then set the display counter `DAT_800e809c` to the quota `DAT_1f800235`
— so the pace-dwell `0x80050CE4` falls through on its first check. (Exactly the state the real
VBlank handler — cb `0x800506B4`, a pure counter increment — would have produced, computed
directly. Host present loop will pace frames later.) `timing.c` VSyncCallback override is now a
clean no-op. **Result: the StrPlayer main loop `FUN_80050b08` runs its per-frame work** —
gdb samples hit varied real fns each tick (libgpu `80083364`/`80081458`, StrPlayer dispatch
`8008179C`, scheduler `80051E60`, memcpy `8009A3E0`); StrPlayer state `DAT_1f80019c`=0.
- **NEXT BLOCKER pinned — BIOS thread context switch.** Still **zero CD reads**: the intro/
  loader **task** (`FUN_800499e8`, registered via `FUN_80051f14(0,…)`) never starts. The
  cooperative scheduler `FUN_80051e60` runs tasks via **BIOS threads**, not custom coroutines:
  disasm of the "context-switch primitives" shows they are plain libapi gate stubs —
  `FUN_80080860`=OpenThread(B0:0x0E), **`FUN_80080880`=ChangeThread(B0:0x10)**,
  `FUN_80080890`/`a0`=Enter/ExitCriticalSection(syscall a0=1/2). The scheduler does
  `state2→ChangeThread(handle)`, `state3→{EnterCS; handle=OpenThread(pc,sp,gp); ExitCS;
  ChangeThread}`. **Our ChangeThread is a NO-OP** (later-17 STOPGAP) → tasks never run.
  FIX = real BIOS threads: give each PSX thread its own **native stack (ucontext/makecontext)**;
  OpenThread creates a context that will enter `gen_func_<pc>`; ChangeThread `swapcontext`s;
  the boot/main thread is also a context. This is the static-recomp coroutine subsystem — the
  one genuinely hard piece. Tooling: gdb attach + `bt` locates the spin/return (C stack == game
  stack); `break gen_func_<addr>` checks whether a fn is reached.

## 2026-06-14 (later 19) — NATIVE BIOS THREADS (ucontext): loader task runs; CD reads in-game
**The hard piece — native BIOS thread context switch — is in (`runtime/recomp/threads.c`),
and the native CD read path is now verified END-TO-END inside the running game.** The
cooperative scheduler's tasks are BIOS threads; disasm confirmed the "coroutine primitives"
are libapi gate stubs: `FUN_80080860`=OpenThread, `FUN_80080870`=CloseThread,
`FUN_80080880`=ChangeThread; a task yields via `ChangeThread(0xFF000000)` (the main/scheduler
thread). `FUN_80051f14` creates each task with `OpenThread(entry, stack, gp)`.
- **threads.c:** each PSX thread gets its own **native stack via ucontext**; `ChangeThread`
  saves the running thread's R3000 regs, restores the target's, and `swapcontext`s to the
  target's native stack (main = slot 0; handles 0xFF0000NN). A fresh thread starts in a
  trampoline that `rec_dispatch`es its entry PC and, on return, switches back to main. The
  single shared R3000 is register-swapped across switches. Overrides the three gate stubs;
  hle.c B0:0x0E/0x0F/0x10 route to the same impl. Replaces the later-17 ChangeThread no-op.
- **VERIFIED:** boot now runs the loader task `FUN_800499e8`, which `CdSearchFile`s
  `\BIN\START.BIN` → our native `FUN_8008c1ec` → real disc reads **LBA 16 (ISO PVD), 18 (path
  table), 373 (dir sector)** — correct bytes, the override read path exercised by real game
  code at last. Cooperative scheduling is healthy (gdb: clean `swapcontext` from
  `FUN_80051f80`, threads round-robining, per-frame memcpy `8009A3E0`).
- **NEXT:** the loader task only *resolves* `\BIN\START.BIN` (stores its descriptor
  `DAT_800be1e0`=LBA) then yields; the actual file-content ReadN hasn't fired yet (still 3
  reads = dir only). Find what consumes the descriptor to load+run START.BIN (likely the next
  scheduler task / StrPlayer state advance), and confirm boot progresses toward the title/FMV.

## 2026-06-14 (later 20) — native file load (FUN_8001db8c); START.BIN loads; overlay exec is next
Boot now loads the first code overlay natively and reaches the **overlay-execution** boundary.
- **The engine's file loader is `FUN_8001db8c(dest, lba, size)`** — NOT FUN_8008c1ec. Its real
  body spawns a reader sub-task (`FUN_8001db38`→`FUN_8001d940`) that issues a raw libcd ReadN
  and copies sectors in a per-sector IRQ callback (`FUN_8001d7c4` = plain `CdGetSector` copy,
  no decompression). That async/IRQ path can't be fed by our no-IRQ overrides → the reader
  looped forever (remaining-count never hit 0). **Overrode `FUN_8001db8c`** (cd_override.c) to
  read `ceil(size/2048)` consecutive sectors from `lba` into `dest` natively, copying exactly
  `size` bytes — faithful (the callback was a plain copy). `FUN_8008a110` confirmed the
  descriptor LBA is absolute (`(min*60+sec)*75+frame-150`).
- **Thread bug fixed:** tasks `CloseThread(self)` then `ChangeThread` away (`FUN_80052078`);
  freeing the live native stack in thread_close → SIGSEGV in munmap. Now thread_close just
  frees the slot; the stack is reclaimed on slot reuse (thread_open), never while live.
- **VERIFIED:** `[cd] loadfile 1648 B @ LBA 1904 -> 0x80106228` — `\BIN\START.BIN` loads to the
  intro-overlay region. No crash.
- **NEXT SUBSYSTEM — overlay code execution.** START.BIN (1648 B) at `0x80106228` IS MIPS code
  (the intro sequencer `FUN_801064f0` lives inside it). The game jumps into it → **miss
  `0x8010649C`**: the `0x80106xxx` overlay region is ABOVE MAIN.EXE's text (`0x800BE800`) and
  was never recompiled. Options: (a) statically recompile the overlay files (START/OPN/GAME/…
  .BIN) with overlay-aware dispatch (they may share the `0x80106xxx` load address → only one
  resident at a time); (b) a hybrid in-RAM MIPS interpreter as the rec_dispatch-miss fallback
  (also clears the printf/SetVideoMode jump-table misses). Decide + implement next.

## 2026-06-14 (later 21) — HYBRID INTERPRETER: overlays run; game executes intro logic
**Overlay code execution solved with a hybrid fallback interpreter (`runtime/recomp/interp.c`).**
The static recomp covers MAIN.EXE's resident text; overlays load from disc at runtime above it
(0x80106xxx) and swap at shared addresses, so they can't be statically recompiled ahead of
time. `rec_interp(c, pc)` runs any non-recompiled RAM code directly from g_ram using the SAME
runtime + the SAME instruction semantics as the emitter (so interpreted == recompiled). Wired
as the `rec_dispatch_miss` fallback for code addresses in [0x10000,0x200000): a jal/jr/jalr
into non-recompiled RAM enters the interpreter; a call back into a recompiled fn routes to
rec_dispatch (`is_recompiled` check). Also clears the in-function jump-table misses (printf
0x8009A8E8, SetVideoMode 0x80091E18) by interpreting from the computed target.
- **VERIFIED:** the START.BIN overlay (incl. intro sequencer `FUN_801064f0`) now executes — no
  misses, no `[interp] bad opcode`. It runs `CdSearchFile` for the next playlist file (new read
  LBA 1905), and the game progresses through its **timer-paced task schedule** (task-0 state
  1→2 as its timer expires over ~8s). Leaf tests still pass. The recomp core stays 100%
  recompiled; only dynamically-loaded overlay code is interpreted (legit hybrid execution).
- **STATE: the game boots MAIN.EXE and runs its full software stack** — HLE BIOS, libcd (native
  file I/O), libetc VSync, events, the cooperative scheduler on real native threads, overlay
  load + execution, and the StrPlayer main loop drawing each frame. It advances the intro logic
  but invisibly: the next subsystems are the **output/IO peripherals — GPU (rasterizer +
  display), MDEC (FMV video), SPU (audio), pad input** — to be lifted from the Beetle GPL-2 fork
  per the plan. GPU first (so output is visible/verifiable). These are the large remaining tier.

## 2026-06-14 (later 22) — GTE (COP2) LIFTED from Beetle: real geometry coprocessor
First peripheral-tier lift: the **GTE is now Beetle's real implementation**, not a no-op stub.
All the game's geometry (RTPS/RTPT projection, NCLIP, matrix/color/depth) flows through COP2;
our stub silently zeroed it, so any 3D was inert.
- **`runtime/recomp/gte_beetle.c`** compiles `vendor/beetle-psx/mednafen/psx/gte.c` as-is and
  adapts it to our interface: `gte_op`→`GTE_Instruction`, `gte_read/write_data`→`GTE_ReadDR/
  WriteDR`, ctrl→`GTE_ReadCR/WriteCR` (1:1). Faithful-first shims for the externs gte.c needs
  (PGXP off `gMode=0`/no-op NCLIP, savestate stub, **widescreen GTE-scale hack OFF** — that's
  the wide60 tier later). `gte_init()` (GTE_Init+Power) called from boot. stubs.c keeps only
  COP0. build.sh adds the mednafen/libretro-common include paths.
- **VERIFIED:** standalone RTPS — identity rotation, vertex (64,0,256), H=256 → **SX=64**
  (= IR1·H/IR3 = 64·256/256), the projection is bit-correct. Builds clean, leaf tests pass, no
  boot regression. (No GTE ops fire during the 2D logo intro — expected; GTE is gameplay 3D.)
- **Lift pattern established** (compile the Beetle C module as-is + a thin adapter + faithful
  externs) for the remaining peripheral tier: **GPU** (the big one, needed for visible output),
  then MDEC, SPU, pad.
- **GPU lift SCOPED (next):** software path is viable — `rhi_intf.c` defaults `rhi_type =
  RHI_SOFTWARE` and the GL/Vulkan backends are `#ifdef HAVE_OPENGL/HAVE_VULKAN` (omit those
  defines → software only, headless). Files: `gpu.c` (command processor, 95KB) +
  `rhi_intf.c` (renderer dispatch) + `gpu_polygon.c`/`gpu_sprite.c`/`gpu_line.c`/
  `gpu_polygon_sub.c` (software rasterizer). `gpu.c` interface: `GPU_Write(ts,A,V)` (GP0/GP1),
  `GPU_Read(ts,A)` (GPUREAD/GPUSTAT), `GPU_WriteDMA/ReadDMA/DMACanWrite` (GPU DMA),
  `GPU_Update(ts)` (scanline timing), `GPU_StartFrame(espec)` (render to a surface),
  `GPU_Init/Power`. **Shim surface ~54 externs** (gpu.c) + rhi_intf.c's settings/libretro deps:
  IRQ_Assert, TIMER_* (dot/hretrace/vblank), PSX_SetEventNT/EventCycles, ReadMem (→ our
  mem_r32 for DMA), PGXP_* (off), psx_gpu_* config globals, rhi_lib_* (omit). **Wiring work:**
  route mem.c 0x1F801810/14 ↔ GPU_Write/Read; model **DMA channel 2** (GPU DMA, the
  ordering-table linked-list walker the game uses — `FUN_80082d04` submits OTs) feeding
  GPU_WriteDMA; provide the VRAM/scanout surface + a present/dump path; feed a synthetic
  timestamp (we pace via VSync, not cycles). Largest single lift; needs iterative verification.

## 2026-06-14 (later 23) — DIRECTION: NATIVE rendering, not PSX-GPU emulation (user)
**User: "make the game itself do PC native rendering instead of PSX emulated rendering."** So
we do NOT lift Beetle's PSX GPU (that's emulated rendering). Instead the game submits its draw
primitives as GP0 command packets (its output protocol) via **GPU DMA channel 2 walking
ordering-table linked lists**; we parse that stream and rasterize it with **our own native
renderer** to a window, at our chosen resolution. No PSX GPU hardware emulation. This is the
from-scratch native renderer the wide60 plan already chose, and it makes widescreen/60fps
natural. (The Beetle-GPU-lift scoping in "later 22" is therefore superseded for rendering — but
the lift pattern + GTE stay; GTE projects the geometry whose 2D primitives we then draw.)
- **Intercept point:** GPU DMA2 (`0x1F8010A0/A4/A8`) linked-list walker → GP0 packet parser;
  direct GP0/GP1 (`0x1F801810/14`) writes; GPUSTAT reads report DMA/cmd ready (game polls
  `&0x4000000`). Game submits OTs via libgpu (`FUN_80082d04`/`FUN_80082fb4` queue+DMA).
- **Native GPU module (building):** VRAM (1024×512×16b for textures + framebuffer) + GP0
  parser (draw-env/texpage/clut, fill, VRAM load/store/copy, flat/gouraud/textured tri+quad,
  sprites/rects, lines) + software rasterizer with VRAM texture+CLUT sampling + GP1 display +
  present (PPM dump headless / SDL window). Built ground-up so resolution/widescreen are ours.

## 2026-06-14 (later 24) — MDEC + SPU lifted (parallel subagents) + SDL window; all integrated
PM-mode session: two developer subagents lifted **MDEC** (`mdec_beetle.c`) and **SPU**
(`spu_beetle.c`) from Beetle in parallel, each following the `gte_beetle.c` template
(compile the Beetle .c as-is + thin adapter + faithful externs). Both verified standalone
(MDEC status correct post-reset; SPU produced exactly 735 stereo frames/NTSC). PM integrated:
- **MDEC** wired in mem.c: regs MDEC0 `0x1F801820` (data) / MDEC1 `0x1F801824` (ctrl/status);
  **DMA0** (MDEC-in, RAM→decoder) and **DMA1** (MDEC-out, decoder→RAM), block-mode. `mdec_init`
  from boot. (Note: `mdec_dma_out` drains linearly — ignores the per-word macroblock scatter
  offset; if FMV pixels come out mis-ordered, switch DMA1 to `MDEC_DMARead(&offs)` placement.)
- **SPU** wired into the build + `spu_init` (register/DMA4/audio-pull wiring + an SDL audio
  sink is the remaining step; module links & runs). STOPGAPs noted in spu_beetle.c: IRQ_Assert
  and CDC_GetCDAudioSample (CD-DA) need routing later.
- **SDL live window** (`gpu_native.c`, `PSXPORT_GPU_WINDOW=1`): the native framebuffer in a
  real window (3× scale), SDL always linked, opens on demand. Headless PPM dump still works.
- **Dedup:** the three adapters each defined `MDFNSS_StateAction` → multiple-definition link
  error; kept one copy (gte_beetle.c), removed the others. Builds clean, leaf tests pass, no
  boot regression (START.BIN still loads, scheduler runs).
- **WHY no FMV yet:** the logo plays through the StrPlayer's **async streaming** CD path
  (`FUN_8008c960` ReadN + per-sector IRQ callbacks → MDEC), which we have NOT overridden — only
  the synchronous reads (`FUN_8008c1ec`, `FUN_8001db8c`) are native. So MDEC is integrated and
  ready but never fed. **NEXT:** wire the StrPlayer streaming read natively (feed stream sectors
  → MDEC decode → VRAM upload), the intro-sequencing path the earlier "later 4-8" work mapped.

## 2026-06-14 (later 25) — MDEC placement + SPU audio integrated; one-shot run.sh (macOS+Linux)
PM sprint, two more developer subagents (both delivered + verified):
- **MDEC DMA-out macroblock placement** (mdec_beetle.c): `mdec_dma_out` now PLACES each word at
  `buf[i + offs]` matching Beetle's DMA1 `CH_MDEC_OUT` (verified: `i+offs` is an exact
  permutation of [0,total), stride 6=24bpp / 4=16bpp; value-for-value vs a reference scatter).
  PM wired DMA1 in mem.c to clear+drain+copy the full post-scatter region (the interleave
  reaches forward; copy the whole MADR transfer, not the return count).
- **SPU audio output** (spu_audio.c, SDL): `spu_audio_init` (44100/S16/stereo, lazy, gated by
  PSXPORT_NOAUDIO) + `spu_audio_frame` (advance 564480 sys-clocks = 735 frames, drain, queue,
  4-frame cap). PM wired the SPU register file `0x1F801C00-1FFF` + DMA4 in mem.c, `spu_init`/
  `spu_audio_init` at boot, `spu_audio_frame` once per frame (sole spu_update driver).
- Builds clean, leaf tests pass, no boot regression. (MDEC/SPU end-to-end output is untestable
  until the intro streams — see below — but both are wired + unit-verified.)
- **`run.sh` (repo root) — fully automated, macOS + Linux:** resolves the disc (arg / env /
  .env / *.chd drop-in), CMake-builds libchdr + discdump, extracts MAIN.EXE, recompiles the
  core + builds the native runtime, launches in an SDL window. macOS-aware: `_XOPEN_SOURCE=700`
  (ucontext), pkg-config sdl2, getconf/sysctl cores, no `timeout`/GNU-isms, brew hints.
  Verified end-to-end on Linux (builds `scratch/bin/tomba2_port`, runs, CD reads). Knobs:
  PSXPORT_NOAUDIO / PSXPORT_NOWINDOW / PSXPORT_GPU_DUMP / CC.
- **Critical-path status (visible output):** the StrPlayer **streaming** read (`FUN_8008c960`)
  is NEVER reached — the game inits MDEC (`FUN_8009C620`) but the interpreted intro overlay
  (START.BIN) stalls before chaining to load/play the logo, so no FMV. Only 5 CD reads ever.
  This is the deep intro-sequencing blocker ("later 2-8"); needs further RE of what the
  interpreted sequencer waits on (logic/state vs an interp-correctness gap). Single-owner next.

## 2026-06-14 (later 26) — DITCH GHIDRA (binary-only) + parallel shard build + run.sh; R/B fix
Reproducibility + build-speed sprint (user-driven). The build now needs **only the repo + the
ROM** — no Ghidra, no committed decomp-derived data.
- **Binary-only recompilation:** `emit.py` seeds purely from the binary now — `{entry} | EXTRA_
  SEEDS`, grown by `discover_funcs` (direct-jal fixpoint). 1154 functions recompiled; the ~445
  reached only via function pointers run through the hybrid interpreter (faithful). **Verified
  identical boot** to the Ghidra-seeded build (same CD reads, START.BIN@1904) — and the printf
  jump-table now prints clean strings (`ResetGraph:jtb=…`, `MDEC_in_sync timeout:`) since the
  interpreter handles it. `PSXPORT_USE_GHIDRA=1` (+ local scratch decomp) still available to
  recompile more for speed; default doesn't touch Ghidra. Repo audited clean: scratch/ (decomp
  dump) gitignored, the optional address list gitignored — only our own Ghidra *tooling* scripts
  remain (don't ship decomp output).
- **Parallel build:** `emit.py` splits output into `generated/rec_decls.h` + 8 `shard_<n>.c`
  (gen_func bodies, round-robin) + `shard_disp.c` (override table + wrappers + dispatch
  switches). `run.sh` compiles all TUs to .o with `xargs -P` then links (`-j16` observed); old
  monolith path stubbed. `PSXPORT_SHARDS` tunable.
- **`run.sh` (repo root): one command, repo + ROM only.** CMake-builds libchdr/discdump,
  extracts MAIN.EXE, binary-only recompiles, parallel-builds, launches the SDL window. macOS
  fixes from user testing: committed func-list dependency removed (was the Mac blocker),
  libchdr *header* path (source tree) vs *.a (build/), no pipefail+`ls`/`head` footgun. Verified
  end-to-end on Linux; built + ran the game loop.
- **Rasterizer R/B-swap fixed** (gpu_native.c `cmd_r`/`cmd_b`): GP0 color packs `0x00BBGGRR`
  (R=low byte). Found by the GPU-QA subagent (which otherwise proved the rasterizer's geometry/
  fill/gouraud all pixel-correct). build.sh leaf tests pass; no boot regression.

## 2026-06-14 (later 8) — CORRECTION to "later 7" RE map (overlay sequencer decompiled)
Read the overlay decomp (`scratch/decomp/overlay.c` = `FUN_801064f0`) + the worker/scheduler
chain from the full decomp. Three labels in "later 7" are **WRONG** — fixing them so the next
session doesn't chase a non-existent activator:
- **`FUN_8008BF50` is the CD directory-cache reader for `CdSearchFile`, NOT a "stream
  activator."** Given a path-component dir-record index it calls `FUN_8008c1ec(1, descriptor,
  buf)` to read **one** directory sector and parse `0x2c`-stride dir records out of the table
  at **`0x80102d44`** (= the **directory-record** table, `0x2c`=dir-record size — NOT a
  stream-descriptor table). `FUN_8008c1ec` is a generic "read N blocks from LBA into buf"
  (1 block = a dir sector; many blocks = a stream). So "later 7"'s `0x8008BF50(a0=N) → plays
  stream N` and "descriptor table `0x80102d44`" are both wrong. Do not drive `0x8008BF50` to
  trigger FMV#2 — it just reads a directory.
- **`FUN_80051E60`** = cooperative task **dispatcher** over `0x801fe000` (stride `0x38`);
  `FUN_80080860/80/90/a0` are **context-switch coroutine primitives** (task create/start/resume
  — green threads), NOT libgpu draw wrappers as labeled.
- **`FUN_801064F0`** = intro/opening **sequencer**: resolves `\BIN\OPN.BIN` (×25 entries),
  `\BIN\START.BIN` (×3), `\CD\TOMBA2.IDX` (×5), `VOICE/DEMO/BGM.XA` via `CdSearchFile`
  (`FUN_8008b8f0`); stores descriptors at `DAT_800be118`/`DAT_800be1e0`/`DAT_800be0f0`; then
  bootstraps loader coroutines (`FUN_80044f58`, `FUN_8004514c` via registrar `FUN_80044bd4`)
  on a state byte at `obj+0x48` (`_DAT_1f800138`). The `obj+0x48` transitions are
  **unconditional** (load bootstrap), so this SM is NOT the consumer-paced logo gate.
**Corrected model:** the logo→FMV#2 hand-off is a **coroutine task playing the logo MDEC
stream**; when it yields end-of-stream the sequencer advances to the next playlist entry. The
real skip target remains (per "later 6"): the **logo stream's frame-count / length field** so
the segment consumes in ~1 frame and the game advances through its OWN code. Candidate: the
OPN.BIN descriptor for the logo segment (`FUN_8008a110` yields LBA; size = field+4). Not yet
pinned to a writable counter.

## 2026-06-14 (later 7) — FULL DECOMPILATION + StrPlayer playback architecture mapped
**Did what the user asked: "decompile everything with tools."** Built headless-Ghidra
decompilation tooling (committed): `tools/decomp.sh` + `tools/ghidra_decomp.py` (all 1886
MAIN.EXE functions → `scratch/decomp/ram_f1000_all.c`) and `tools/ghidra_overlay.py`
(force-disassemble a fn-ptr-only overlay range). Also **ripped out the turbo** (committed):
`g_module_turbo` + `Tomba2_LogoHoldTurbo` gone; `-play` fast-forwards only on manual Tab.

**StrPlayer playback architecture (from the decompiled C):**
- **Main loop `FUN_80050b08`** (resident, 0x80050b08): infinite per-frame loop. Per frame:
  graphics + `FUN_800506d0()` (timer-array tick) then dispatches on **state `DAT_1f80019c`**
  (scratchpad 0x1F80019C): 0 = display current stream frame (`FUN_8008179c` = PutDispEnv etc.,
  these 0x80080xxx/0x80081xxx are **libgpu wrappers**, not the demux); 2 = display one more +
  set state 1; **3 = stream end → outer loop restarts = advance to next playlist entry**.
  The dwell is the `do {} while (DAT_800e809c < DAT_1f800235)` vblank wait at ~0x80050ce4.
- **Cooperative task scheduler:** task array at **`0x801fe000`** (state byte at offset 0:
  1=timed,2=ready,3/4=running; stride 0x38/0x70). `FUN_800506d0` decrements timers (1→2 on
  expiry); `FUN_80051e60` dispatches (2→4 start `FUN_80080880`, 3→step). `obj = *(0x1F800138)`
  is the current task iterator (journal "obj+0x48" = overlay SM state).
- **Playing a stream = `FUN_8008c1ec(blocks, startLBA, buffer)`**: BCD-converts LBA →
  `Setloc`(cmd2) → `FUN_8008c960`(start stream) → which calls **`FUN_8008c5d8`** (read setup:
  sets `DAT_800ac2e4`=blocks, `DAT_800ac2f8`=remaining, registers read-cb `FUN_8008c294`).
- **Read SM `FUN_8008c294`** (per CD-read-complete): decrements `DAT_800ac2f8` (remaining); on
  0 → sets **`DAT_800ac308`=1 (done)** and fires completion cb `PTR_FUN_800abf24`=`FUN_800899bc`.
- **Activator `0x8008BF50`(a0=N)** reads a 44-byte descriptor from the table at **`0x80102d44`**
  (record N) and calls `FUN_8008c1ec` → plays stream N. The playlist overlay SM `0x80106xxx`
  calls the walker `0x8008B8F0` → activator on advance.

**Why the advance isn't yet forceable (measured):** the logo's CD *read* finishes by ~f1060
(`DAT_800ac2f8`=0, `DAT_800ac304` freezes at 0x772) — the ~250f to f1310 is the logo **MDEC
video task playing out at VBLANK rate**, a separate task in the **overlay `0x80106xxx`**. That
task's completion is what calls the activator for FMV#2. `FUN_8008c294`/`FUN_8008c1ec`/
`FUN_8008c960`/`FUN_800899bc` are all **advance-only** (coverage) = they are FMV#2's read, not
the logo's. Forcing state `DAT_1f80019c`=3 is **inert** (re-confirmed, hammered 80f). The
logo-task completion trigger lives in the overlay SM, which **does not cleanly decompile** in
Ghidra's default MIPS mode (GTE/cop2 "bad instruction data").

**NEXT (well-scoped):** decompile the overlay with a **GTE/cop2-aware** Ghidra processor (PSX
variant) from a *hold-phase* dump (logo overlay resident, e.g. `hle_hold_f950.bin`), find the
logo MDEC-video task's end-of-stream → it calls the walker/activator(FMV#2). Then the native
override is: on Start during the silent hold, drive that task's completion (or directly call
the activator for the FMV#2 descriptor index, found from the 0x80102d44 table) — the game then
plays FMV#2 through its OWN code. Verify FMV#2 plays clean AND advances to title afterward.

## 2026-06-14 (later 6) — REALITY CHECK on HLE: intro-skip + turbo do NOTHING; logo hold is VBLANK-paced ~370f; all low-level forces fail
**User report (ground truth): "skipping FMV#1 still waits 5 seconds till FMV#2." Plus
directive: HLE BIOS, RE, PC-native overrides — NO emulator turbo.** Investigated entirely
on the **HLE BIOS path** (`PSXPORT_HLE_BIOS=1`, instant CD, `-repl`). Findings, all measured:

- **The whole intro-skip (incl. the "later 5" turbo) was tuned to the OpenBIOS ROM path and
  does NOTHING under HLE.** HLE no-input timeline: FMV#1 (boot EXE) ReadS f413 → Pause f931;
  StrPlayer logo hold f944 → **FMV#2 ReadS f1318** (~370f ≈ 6s gap). Held-Start-from-boot
  gives the IDENTICAL f413/f1318 — the SCEA/Whoopee/Dwell/LogoHold hooks don't move anything
  under HLE, and **FMV#1 itself is not Start-skippable under HLE** (tap at f500 → still Pause
  f931). (`g_module_turbo` only acts in the `-play` loop; in `-repl` the dwell-hook still runs.)
- **CAVEAT that invalidated earlier "held-Start" repl tests:** REPL button names are
  **case-sensitive** (`press Start`, not `START`). My first held tests used `START` → no-op.
- **Dwell-escape (0x80050CE4) caps at ~−105f under HLE.** New knob forcing the 0x80050CE4
  pace-loop exit *unconditionally* (every reached frame): FMV#2 f1318 → **f1213 only**. So the
  display pace dwell is NOT the gate; the consumer-paced ring fill is. This is the hard ceiling
  of the entire dwell-escape family — it can never collapse the ~370f hold.
- **Forcing the prebuffer-wait gate DESYNCS (FMV#2 never comes).** The gate is
  `0x8008A784 bnez v1,0x8008A7B8` (advance when ring pos > target, else wait ≤60f). Hooking it
  to force v1=1 (always "buffer ready") → **no cmd 1B at all** through f1400. Same failure
  family as faking disc EOF / poking 0x80102748. Do not retry forcing this gate.
- **The StrPlayer state byte `*(0x1F80019C)` is 0 for the ENTIRE hold** (→1→2 only at f1318).
  Dispatcher `0x80050D00`: state 0 → calls per-frame driver `0x8008179C` EVERY frame; the
  advance decision is INSIDE 0x8008179C's consume logic (callee chain incl. status flag
  *(0x800ABE20) and the FMV#2 load 0x8008A6EC/0x8008B4B8). That is exactly why poking the
  state byte is inert — 0 is the *active* driver state, not a "waiting" state.
- **Mechanism (re-confirmed via PCCOV coverage-diff wait-frame vs advance-frame):** the
  advance frame uniquely runs the playlist walker `0x8008B8F0`, activator `0x8008BF50` (called
  from walker @0x8008BA60), and the FMV#2 overlay init/SM `0x80106xxx` (0x801064F0 parses the
  '\'-playlists; walker called from 6 sites 0x80106514..0x801066F0). The overlay SM is dormant
  during the hold and wakes only when the logo segment is consumed → it then drives the advance.
- **Savestates are UNRELIABLE under HLE** (retro_serialize captures Beetle state but NOT the
  runtime-side HLE BIOS thread/callback state) → on load the StrPlayer desyncs (0 CD cmds).
  Fast-iteration must run from boot (instant-CD makes f0→f1318 a few seconds).

**Bottom line:** the ~370f logo hold is ~265 logo frames displayed one-per-real-VBLANK
(consumer-paced) — NOT removable by escaping any spin (that needs more VBLANKs = turbo) and
NOT forceable at any low-level gate (all desync). The ONLY clean native skip is to **cut the
logo SEGMENT short** (drive the overlay-SM advance, or shorten the logo stream's
length/frame-count so it consumes in ~1 frame and the game advances through its OWN code).
That is the documented next step and remains uncracked — needs the consume counter / logo
stream length field RE'd (scratchpad-aware watchpoint; PSXPORT_WATCHW is main-RAM only).
**Tooling proven useful this session: `PSXPORT_PCCOV="s-e:path;s-e:path"` coverage-diff** (set
difference of executed PCs between a waiting window and the advance window — pinpointed the
advance-only functions). Experiments reverted (dead ends): force-dwell, force-prebuffer-gate.

## 2026-06-14 (later 5) — inter-FMV logo hold residual COLLAPSED via scoped fast-forward
The dwell-skip (later-4) got the hold f719->f598 (-121f) but left a ~3.3s residual =
the StrPlayer's per-VBLANK MDEC decode of the (invisible, skipped) logo clip's ~210
data frames (profile under dwell-skip: spread decode work 0x800834A0/0x8008B6D0 MDEC
poll/0x80044E5x RLE/0x8009A3F0 copy; one frame per VBLANK; no pokeable flag — the clean
"drive the advance" attempts all re-seek-loop). User-directed override: re-enable the
existing `g_module_turbo` (8x emulated frames/present, pacing bypassed) SCOPED to the
verified-silent hold: `g_tomba2 && Tomba2_LogoHoldTurbo() && !psxport_cd_strsnd_on()`.
Tomba2_LogoHoldTurbo() = the dwell-skip hook (signature-gated to the StrPlayer overlay,
never gameplay) fired within 45 emulated frames (bridges read/decode frames between
pace-dwells); STRSND-off is the hard cutoff (FMV#2 turns CD-XA audio on -> turbo ends
the same step). play-loop batch rewritten to re-check turbo per step & break on drop.
**Safe, not general turbo:** runs the SAME emulated frames unpaced, so FMV#2 state at
f598 is identical -> plays bit-for-bit the same, just sooner. Verified: turbo ON
continuously f388->598, off f599; no-Start path unchanged (FMV#2 f1181); -play boots
clean. **Wall-clock (-play): hold ~3.5s -> ~1.1s** (floor = emu speed ~190fps for the
210-frame consume). Combined intro gap (FMV#1-skip + dwell-skip + turbo): **5.6s -> ~1.1s**.

## 2026-06-14 (later 4) — inter-FMV logo skip IMPLEMENTED + verified (dwell-escape, STRSND-gated)
**Shipped:** `LogoHoldSkip` in runtime/games/tomba2.cpp — a Start-gated native override
that collapses the silent logo hold. **Verified result: FMV#2 ReadS f719 -> f598 (-121f)
when Start is held during the hold**, FMV#2 renders cleanly (Tomba jungle/character scene,
meanRGB (40.4,18.9,1.1)->(81.4,40.0,22.3), matches known-good baseline), plays to completion
(jungle scene still streaming at f1300, no hang), and **no regression** (Start NOT held =
natural f719). Default ON (gated with the other intro skips via PSXPORT_T2_NOINTROSKIP).
- **Hook:** PC `0x80050CE4`, sig `0x9482809C` (the per-frame StrPlayer pace-dwell body),
  redirect to loop-exit `0x80050CF8` — the SAME lever as the loading-screen FmvDwellSkip.
  Fires only when `s_skip_held && !psxport_cd_strsnd_on()`.
- **Phase gate = STRSND (CD-XA audio) OFF.** The silent logo hold runs STRSND-off (Setmode
  0x80/0xA0); every real FMV/cutscene streams audio (STRSND-on, 0xC0). New generic primitive
  `psxport_cd_strsnd_on()` (cdc.c) exposes it. Verified: holding Start through FMV#2 does NOT
  fast-forward it (same f598 start, full duration) — the gate protects real movies.
- **CORRECTS "(later 3)"'s claim that forcing the dwell saved 0 frames.** That was tested via
  PSXPORT_REA_FORCEDWELL on a different build/probe. Measured directly on the f719 skip path,
  escaping the 0x80050CE4 dwell DOES accelerate the consume: f719->f598. (fmv-skip.md's
  "held-Start saves ~120f" was right.) The residual f324->f598 is genuine consumer pacing
  (still-logo decoded 1 frame/VBLANK) — the safe floor; forcing past it underruns.
- **Approach 1 (read genuine EOF early) RULED OUT, newly tested:** redirecting the consume
  Setloc to the real EOF LBA 11492 makes the StrPlayer re-seek 11492 forever without advancing
  — its advance is gated on its OWN consumed-sector bookkeeping, not on physically reading EOF.
  Same failure family as forging an XA EOF submode. Do not retry disc-position fakery.
- **Approach 3 (drive the SM) unnecessary:** RE confirmed the inner SM 0x80106F80 is DORMANT
  during the hold (first ticks f709), states 1/2/3 are pass-through increments (jumptable
  @0x801062C4 all -> 0x80107034), state 0 = CD poll 0x80089bac(a0=0xE), state 4 @0x80107054 =
  advance. The SM only wakes once the consume completes, so the consume rate IS the gate; the
  dwell-escape addresses it directly. Tooling: added `[setloc f%u] lba= pc=` log (cdc_log).

## 2026-06-14 (later 3) — inter-FMV logo hold: DATA-bound, NOT audio/time; mechanism mapped
**Question answered:** is the ~317f logo hold (skip FMV#1@f380 -> FMV#2 ReadS@f719)
DATA-bound or TIME/AUDIO-bound? **Answer: DATA-bound (stream-position-driven), NOT
audio- or display-clock-bound.** Evidence (all `wide60rt_reA`, instant-CD default):
- **NO audio of any kind plays during the hold.** Setmode during FMV#1=0xC0 (STRSND on,
  XA processed every ~3f); during the hold Mode=0x80/0xA0 (**STRSND OFF**, zero `[xa]`
  sectors); FMV#2 (f719) Mode back to 0xC0. The CD-DA `Play`@f311 was Paused@f322. So
  the "logo jingle" hypothesis is FALSIFIED — the reads are plain data ReadN.
- **Display-clock collapse saves 0 frames.** Forced the per-frame pace dwell 0x80050CE4
  to always elapse (PSXPORT_REA_FORCEDWELL probe): FMV#2 ReadS STILL at f719, identical.
  So the hold is NOT gated by the StrPlayer's display counter (0x800E809C vs threshold
  0x1F800235=2). (Re-confirms fmv-skip.md's instant-CD/dwell findings on THIS skip path.)
- **Fixed 401 sectors** DMA'd during the StrPlayer hold phase (reproducible to the word
  across runs). Setloc LBAs creep slowly through TWO interleaved logo streams (~LBA
  1879-1908 and ~6565-6717, advancing 1904->1905->1906->1908 over f388-f522 = consumer-
  throttled), then JUMP to LBA 152238 (OPS.STR = FMV#2) at f719. The hold ends when the
  logo streams reach their descriptor end-LBA -> advance to next playlist entry.
- **Read pacing** during active stretches is ~2-3 sectors/frame (consumer-paced, real-
  VBLANK-clocked); reads are instant when issued (instant-CD), the long idle gaps (e.g.
  f455-f526) are the CPU spinning 62% in the dwell + ~11% in an RLE/decode loop at
  0x80044E14 — i.e. decoding/displaying the still logo, not waiting on disk.

**Per-frame decision chain (execution mapped, not a single pokeable gate):**
- The StrPlayer playlist is a `'\'`(0x5C)-delimited name list; the walker at **0x8008B8F0**
  tokenizes it and calls **0x8008BF50(a0=N)** to activate the Nth stream (a0=2 f387,
  a0=3 f402, **a0=4 @f717** -> activates FMV#2 -> ReadS f719). 0x8008BF50 stores N to
  0x800AC2D4 (the 3->4 the lead saw; poking it is INERT, re-confirmed).
- The actual advance is driven by the FMV#2-overlay state machine at **0x80106388**
  (outer state `[obj+0x48]`, obj=`*(0x1F800138)`, jumptable @0x8010622C) and inner SM
  **0x80106F80** (state `[obj+0x4a]`, jumptable @0x801062C4; state 4 @0x80107054 does the
  playlist-advance). This dispatcher is DORMANT during the hold — first runs f654 (one
  tick) then f709+ — because the StrPlayer stream scheduler only ticks the next segment's
  SM when the current (logo) segment's data is consumed. The hold proper (f386-f707) is
  the logo-stream consume; the SM spin-up (f709-f719) is the tail.
- Inner state 0 (0x80106FF0) spins `0x80089bac(a0=0xE)` until nonzero = a CD-command-
  complete poll (0x8008AC34 reads per-channel state 0x800ABC00+ch*4, issues via 0x8008A6EC).
  So the terminal gate is CD-command/stream-position state, consistent with data-bound.

**Forceability:** no clean single-flag lever (stream-count 0x800AC2D4 poke INERT; scratch
state 0x1F80019C is a downstream effect, written by 0x80050DA8 each frame, ->2 only at
f724 AFTER ReadS). The advance is genuinely data-position-driven. The forceable approach
remains the lead's option (b): a native override that DRIVES the segment advance (set the
logo streams' end-reached / invoke 0x8008BF50(a0=4) + the 0x80106xxx outer-state advance)
on Start. FMV#2 reached early is verified glitch-free (fmv-skip.md), so risk is only WHICH
state to drive. NOT YET implemented — needs the logo-stream descriptor end-LBA field RE'd
to set "logo consumed" cleanly, OR drive the 0x80106388 outer-SM transition directly.
**Tooling added (kept):** PSXPORT_PCTRACE_EXCL="lo-hi" excludes a hot spin sub-range from
the pctrace ring (so the dwell 0x80050CE4 can't flood it). Probes used then reverted:
cdc.c [setmode]/[setloc]/[xa] logs (gated on PSXPORT_CDC_LOG), tomba2 PSXPORT_REA_FORCEDWELL.

## 2026-06-14 (later 2) — inter-FMV logo skip: deep RE, new tooling, NOT yet cracked
**User's actual goal:** pressing Start during FMV#1 skips it, but then there's a
**~5.6s gap to FMV#2** they want gone. Reproduced: skip FMV#1@f380 → FMV#2 ReadS@f719
(=339 frames), the SAME fixed-duration hold as the no-input case (f842→f1181).
- **Structure:** FMV#1 is played/skipped by the **boot EXE** (0x8001xxxx, ReadS
  lastpc=0x80017E58). The logo + FMV#2 (OPS.STR) are the **MAIN.EXE StrPlayer**
  (0x8008xxxx). After FMV#1 the StrPlayer holds the **Whoopee logo as a static load
  mask** (screenshot: bright logo f880-1000, black f1120) while streaming the logo
  clip (jingle) for its fixed duration, then advances to FMV#2.
- **NEW TOOLING (committed):** scoped PC-trace ring (`PSXPORT_PCTRACE="lo-hi"` +
  REPL `pctrace`), CPU-scratchpad access (REPL `sr`/`sw8`, accessors in cpu.c) —
  games keep hot state in the scratchpad (0x1F800xxx), invisible to main-RAM tools;
  DMA3-arm + CD-cmd-write issuing-PC logs; StateProbe (PSXPORT_T2_STATEPROBE).
- **RE via pctrace (diff advancing frame vs waiting frame):** the advance runs a
  code path absent on waiting frames → command-complete handler **0x80085Exx**
  (clears flag *(0x800AAD1A)) → caller **0x80050D00** (StrPlayer state machine:
  reads scratchpad state `*(s5+0x19c)=0x1F80019C`; s5=0x1F800000) → dispatch
  **0x8008179C** → advance work (0x8008A6EC, 0x8008B4B8 load FMV#2 group + ReadS).
- **The trigger is the logo clip's STREAM END** (last CD command completing), not a
  pokeable flag. Every candidate is an EFFECT, verified by poking (no early FMV#2):
  scratchpad state byte 0x1F80019C (0→2, but changes AFTER the ReadS), 0x800AC2D4
  (active-stream count 3→4), 0x800AC299, 0x800BF8A7, main-RAM 1/frame counter
  0x800A5ADC. Forcing the counter high BREAKS progression.
- **Read acceleration = CONFIRMED DEAD END** (3 tests + journal): the reads are
  clamped to the StrPlayer consumer-ack (one batch/vblank); forcing faster delivery
  wedges the CD pipe (0 sectors), and the logo hold is real-time/audio-clocked, not
  read-bound (XAFAST = 0 frame change).
- **OPEN:** no clean override found. The advance is data-driven (stream end). NEXT
  candidates: (a) find the StrPlayer command-list position / stream frame-count that
  the command-complete handler checks for "last command", and force it; (b) make a
  native override INVOKE the advance dispatch (0x8008179C path) directly on Start
  during the logo. FMV#2 reached early is verified glitch-free (fmv-skip.md), so the
  risk is only WHICH state to drive, not early playback.

## 2026-06-14 (later) — HLE FMV#2 stall FIXED (new threads seeded IEc=0); prebuffer is VBLANK-paced
- **FIX (committed d3fd1a2):** `open_thread()` seeded a fresh thread's SR by copying
  the creator's saved TCB SR. Tomba2's StrPlayer OpenThreads the FMV#2 prebuffer thread
  from inside a critical section (IEc=0), so it inherited interrupts-disabled and spun
  forever (the IEc=0 deadlock from the entry below). Fix: force 0x404 (IEp+IM-IP2 = the
  LeaveCriticalSection enable mask) into the seeded SR. **Verified under pure HLE:** full
  FMV#2 prebuffer (12 SeekL + ReadN cycles, ReadS@f1181), ~2445 sectors DMA'd, FMV#2
  renders cleanly (Tomba jungle scene). Pinned via new PSXPORT_CS_LOG (Enter/Leave SR +
  ChangeThread resumed-IEc) — the last op before the hang was `ChangeThread -> new TCB
  resume=0x800499E8 newsr IEc0`.
- **OPEN — the ~5s gap from Whoopee-skip to FMV#2 is a VBLANK-real-time-paced prebuffer**,
  NOT CD-load-slow and NOT the CPU dwell. Measured: FMV#1 Pause@f842 -> FMV#2 ReadS@f1181
  (~339f ≈ 5.6s). Reads are instant (instant-CD) but spaced 70->10f apart (ring-drain /
  consumer-paced, the still-logo MDEC decode at VBLANK rate). The dispatch counter
  0x800ABDE0 advances exactly 1/frame. **Forcing the 0x80050CE4 pace dwell (76.5% of CPU
  during prebuffer) saved 0 frames** — so the gate is real-time VBLANK pacing, not CPU
  spin (contradicts fmv-skip.md's "held-Start saves 120f"; that path also fired other
  overrides). To make FMV#2 instant on PC needs a native lever on the game's per-frame
  prebuffer pump rate or its completion gate — fmv-skip.md showed the gate vars resist
  poking. NEXT: RE the StrPlayer script-processor command list (0x8008AE00, 2-byte
  records) for a "hold N frames" opcode, or drive the VBLANK prebuffer callback faster.
- **Prebuffer RE — gate FOUND, but it's consumer-rate-locked (no clean frame-counter
  lever).** Tooling: added issuing-PC logging at the CD command-register write
  (`[cmd-write]`, beetle cdc.c). Findings:
  - All CD commands (prebuffer ReadN + FMV#2 ReadS) issue from **0x8008ADE4** (the
    command-register store in the per-command processor 0x8008AC34); the opcode is the
    caller's arg. The caller walks a **script VM**: interpreter loop at **0x8008C034**
    (walks variable-length stream descriptors to 0x80104B68; per-stream handler
    0x8008A00C is just an LBA→MSF converter), command issue inline.
  - **The ReadS trigger is `0x800AC2D4` (active-stream count) going 3→4** — written by
    0x8008C174 (s6 = #active streams). It sits at 3 the ENTIRE prebuffer (f864→1179)
    then →4 at f1179 → ReadS@f1181. So the FMV#2-start gate = the 4th (FMV#2) stream
    descriptor becoming active.
  - **RULED OUT the "counter>target" wait (0x8008AE60) definitively**: measured live,
    `target − counter ≡ 960` every frame (target rewritten to counter+0x3C0) → that
    branch never fires; it's a self-resetting watchdog, not the pacer. fmv-skip.md's
    "held-Start saves 120f" did NOT reproduce (forcing the 0x80050CE4 dwell saved 0f).
  - **Root nature:** the StrPlayer streams everything at real-time MDEC/display rate
    (~1.8 sectors/frame — FMV#1 too: 922 sec / 511f). The inter-FMV prebuffer fills
    FMV#2's ring at that consumer rate while the (now-skipped) logo "plays", for its
    designed ~6s. No single pokeable dwell; the clean fix is to advance the StrPlayer
    script past the logo stream (force the 4th-stream activation / logo-stream-done),
    which needs the stream-descriptor activation field RE'd — the next focused step.
    Forcing FMV#2 early is verified SAFE (fmv-skip.md: clean frames at f1111), so the
    risk is only in WHICH state to flip, not in early playback.

## 2026-06-14 — HLE FMV#2 stall ROOT-CAUSED: interrupts stuck disabled (IEc=0), NOT a CD-ring bug
- **Falsified the prior "FMV ring never fills / lossy CD-IRQ / per-sector DMA" framing**
  (docs/tomba2-hle-irq.md banner added). Frame-stamped probes prove FMV#1 streams fine:
  **1969 DMA3 arms / ~922 sectors** into the ring to f835. The stall is the FMV#2
  *prebuffer*, after FMV#1: Setloc@f848 (acks fine) then **zero further CD commands**.
- **Root cause:** the StrPlayer spins in the prebuffer wait `0x8008AE54` (gate =
  `*(0x800ABDE0) > 0x3C3`) with **interrupts disabled**: new `irq` REPL cmd shows
  `I_STAT=0005 I_MASK=000D pending=0005 SR.IEc=0 EPC=800808A4` (LeaveCriticalSection),
  identical f880→f1280. VBLANK+CDROM IRQs are pending+unmasked but IEc=0 so beetle never
  vectors them. The gate counter `0x800ABDE0` is bumped only by VBLANK callback
  `0x800909C0`, dispatched by the game's I_STAT-polling dispatcher `0x80085D8C`
  (`I_STAT & I_MASK & 0x0D`, bit0=VBLANK). Dispatcher runs **578× on ROM** (every frame),
  **11× on HLE** then stops → counter frozen at 3 → infinite spin → no FMV#2.
- EnterCS/LeaveCS trace: HLE goes net **+1 Enter** at the f845-848 transition then silent;
  the outermost Leave that restores IEc=1 is never reached. ROM recovers. DEAD ENDS
  (re-confirmed): instant-CD on/off identical; ring/DMA path is healthy; mode-0x2000
  callback wiring is irrelevant (0x800909C0 is poll-dispatched, not a BIOS event callback).
- **NEXT (fix):** instrument IEc across each Enter/Leave syscall + return_from_exception
  RFE-pop (hle_irq.cpp) around f845-848 vs ROM. Tooling added: `irq`/`gpr` REPL cmds;
  `[dma3]`/`[cd-reqdata]`/`[hretrace]`/`[timerN-mode]` probes (PSXPORT_CDC_LOG). Full
  RE chain in docs/tomba2-hle-irq.md (2026-06-14 section).

## 2026-06-13 (later) — native intro skip IMPLEMENTED + verified
- **Found the real intro driver:** `0x800111B4` is the logos sequencer (straight-line
  blocking code, NO data-driven stage var). It calls `0x80010D54` (SCEA license:
  fade-in/hold(180)/fade-out/done state machine on `$s5`) then a poll loop running
  `0x8001138C` (Whoopee anim player; loop exits when `*(0x800253EC)==1`).
- **Falsified the falsifications** (kept notes honest): `0x800111B4` IS the sequencer
  (prior "stale stack-scan" note was wrong — it tested an unrelated fn); `0x8001E0CC`
  DOES run; `0x800253EC` is the Whoopee done-flag (not a "scene step"), but is NOT a
  usable lever (only re-checked at anim-pass boundaries — poking it mid-pass is inert,
  verified).
- **Native skip (default ON, Start-gated; runtime/games/tomba2.cpp):** `SceaSkip`
  @0x80010ED0 forces `$s5=3` → SCEA jumps to done/return (skips hold AND fade
  directly). `WhoopeeSkip` @0x80011414 sets the loop terminator + redirects to the
  epilogue. **Verified:** Start held → post-Whoopee FMV stage at ~f505 vs ~f1181
  baseline (~676f / ~11s earlier); end-to-end reaches the opening FMV cleanly.
- **Retired** the rejected `Tomba2_WantTurbo` fast-forward + falsified `kScenePhase`.
- **OPEN (user directive):** skipping the logos unmasks the loads they hid — the
  opening FMV streams in (white/black gap) at native XA speed; inter-stage black gaps
  are partly the game's frame-counted dwells. NEXT: RE the FMV load + paced dwells and
  make them PC-instant. Timing/IRQ primitives mapped but LEFT EMULATED (owning them =
  faking hardware). See docs/tomba2-intro.md.

## 2026-06-13 — intro RE restart: prior orchestrator FALSIFIED; logos NOT input-skippable
- **User reframed the goal:** the SCEA + Whoopee Camp logos must be **natively
  skippable via RE + native port**, NOT emulator fast-forward (the shipped
  `Tomba2_WantTurbo` Start-hold is rejected). Methodology the user fixed:
  **tooling → RE → native port → patch the native port**. Full RE in
  `docs/tomba2-intro.md`.
- **Tooling:** added REPL `shot <path>` (dumps current framebuffer to PPM on demand
  while driving `-repl`); VideoCb caches the last frame in g_last_fb. Made the
  skippability tests below possible.
- **Verified intro timeline** (frames.py contact sheet, instant CD, no input): SCEA
  license ~f200–600 → black → Whoopee Camp logo ~f850–1700 → opening **MDEC FMV**
  ~f1800+ → title screen.
- **The opening FMV IS natively Start-skippable** (REPL: press Start at f1900 →
  title by f1990 vs still-FMV at f2160 no-input). **SCEA + Whoopee are NOT** (hold
  Start from boot still shows them) — they're load masks with display-hold timers,
  pad not polled. Render heartbeat 0x8003CCA4 dormant the whole intro (expected).
- **FALSIFIED `0x800675CC`** (the scene-orchestrator doc's "scene phase"): never
  written during SCEA/Whoopee (watchpoint logs only the f25 BIOS clear). Also
  **FALSIFIED the `0x80011B4`/`0x80018C10` "intro driver"**: it came from a stale
  `bt` stack-scan; tracing `0x8001e0cc` (in that chain) over f0–1900 = ZERO hits —
  that code is post-intro title/menu, not the logo driver. Banner added to
  tomba2-scene-orchestrator.md. (Lesson: the heuristic stack-scan reports stale
  return addresses; confirm a "driver" by TRACING it before building on it.)
- **Ground truth (PCCOV intersection SCEA∩Whoopee):** intro driver code at
  0x80017E4C (VSync/elapsed-frame timing; frame counter 0x800267B4, markers
  0x80025684/88), 0x800181E8-0x80018484 (stage machine — disasm NEXT), + smaller
  regions. RAM-diff stage-var candidates: 0x80025454(5→0), 0x80025458(3→0),
  0x8002667C(1→0), 0x80026620/28(0→8). NEXT: watchpoint these to find the dispatch.
- KEY gotcha: the FMV stream overwrites 0x80011xxx/0x80018xxx — dump RAM **during
  the logo phase** (f400/f1000) to disassemble the intro driver, not during the FMV.

## 2026-06-13 — display enhancements: widescreen + 4x internal res + sharp scaling
- User: "not widescreen, doesn't look higher resolution, no bilinear." All three
  addressed via stock Beetle core options + presentation fixes (no GL context):
  - **Higher resolution = `beetle_psx_internal_resolution`.** The SOFTWARE
    renderer honors it: libretro.c:4287-4290 sets psx_gpu_upscale_shift =
    upscale_shift_hw when there is NO hw renderer, and the framebuffer is emitted
    at the upscaled size (libretro.c:3581). Verified 1x->350x240, 2x->700x480,
    4x->1400x960; 4x runs ~174 emu-fps headless (~5.8x realtime) so it's fine for
    live play with the software renderer. Default 4x in -play.
  - **Widescreen = `beetle_psx_widescreen_hack` + `_aspect_ratio`.** Reports the
    chosen aspect via av_info (16:9->1.778, 21:9->2.370, off->1.333). Default
    16:9 in -play.
  - **No bilinear:** SDL_HINT_RENDER_SCALE_QUALITY = nearest. Present now scales
    to the core-reported aspect (g_aspect from av_info + SET_GEOMETRY/
    SET_SYSTEM_AV_INFO), not a hardcoded 4:3, so widescreen isn't squished.
- **KEY INTERACTION: widescreen_hack AND internal_resolution change the
  coordinates the wide60 harness captures.** With both default-on, rtps-reproject
  fell to 2% and wide60-verify to 8% — the reproject math + GP0<->GTE join assume
  NATIVE screen coordinates, which the upscale (vertices at 4x) and the widescreen
  X-scale both break. So these are **play-time enhancements only**: default ON for
  -play, OFF (native 1x/4:3) for headless/RE runs. Env overrides either way
  (PSXPORT_INTERNAL_RES, PSXPORT_WIDE/PSXPORT_NOWIDE, PSXPORT_WS_ASPECT). Battery
  back to 100% at native. TODO: when the wide60 present stage is built, its
  capture must account for the upscale shift + widescreen scale to coexist.
- **Tomba2 widescreen caveats (expect, per-game tier):** the hack widens
  GTE-projected geometry (characters/models) but Tomba2's terrain is CPU-projected
  and 2D HUD isn't projected at all — those won't widen consistently, so expect
  misalignment/stretch. Also the wider FOV reintroduces edge pop-in (the
  cull-cone-widening override that masked it is disabled — it broke gameplay,
  see below). Proper widescreen for Tomba2 needs the per-game cull/terrain work.

## 2026-06-13 — user-reported fixes: blinking objects, real logo skip, dynamic res
- **Blinking / walk-through objects = the cull-cone-widening override.** User
  reported game objects blinking in/out *and being walk-through* (logic, not just
  visual). Root cause: `CullSlti` (runtime/games/tomba2.cpp) is the only Tomba2
  hook that REDIRECTS + writes regs; it forced the engine to draw objects it had
  culled, but their collision/logic was never set up -> visible yet walk-through,
  flickering at the widened boundary. The six slti sites are NOT all confirmed to
  be the same cull test. **Fix: gated OFF by default** (PSXPORT_T2_CULLWIDEN=1 to
  re-enable for RE). Correctness-first: a widescreen enhancement must not corrupt
  base gameplay. User confirmed blinking gone.
- **Intro logo: a TRUE in-game skip (user rejected fast-forward; "we're on PC").**
  First disproved the "make the disc read instant" instinct empirically: the data
  reads (ReadN) are ALREADY ~64x; the logo is paced by the **jingle audio stream**
  (ReadS/STRSND, irq1 every frame f680-1181), not a slow read. Accelerating audio
  sector delivery does NOT help — gated by consumer-ack it's ~10% faster; ungated
  it reaches the post-logo milestone at f2451 vs f1352 baseline (SLOWER: the
  intro's sequencing is tied to the audio playing in real time). DEAD END: no
  CD-timing change shortens the logo. Reverted (cdc.c back to committed).
- **FALSIFIED: "`0x800253EC` is the intro STEP".** The earlier RE used byte-level
  MEMWATCH (the tool logged single bytes, not words). The byte at 0x253EC is 0x10
  and looked flag-like; the actual 32-bit word is **0x26A60010 — a constant
  pointer**. The auto-skip (LogoSkip) wrote 1 into it whenever the logo audio
  clock ticked, which in -play **corrupts that pointer mid-intro** = the user's
  "Whoopee Camp doesn't play" bug. Headless never tripped it (no audio sink, clock
  frozen at 0). Removed entirely. Lesson: MEMWATCH now logs aligned 32-bit words
  in hex.
- **Real intro state machine RE'd + decompiled — see
  `docs/tomba2-scene-orchestrator.md`.** Found with the new write-watchpoint
  (`PSXPORT_WATCHW`): the scene-phase word is **`0x800675CC`** (0=SCEA license,
  1/2=transition+load, 3/4=Whoopee Camp logo + opening cutscene loop), driven by
  `scene_update` (0x8002C97C) dispatching through jump table 0x8004CDC0. Each scene
  holds until an advance event (`0x80076AC0`) fires; the event is paced by the
  kernel ready-signal `0x80047174` (VSync/CD) via the advance pump (0x8003AAA4).
  **Forcing the event races the loaders** (reproduces the white screen), so the
  clean skip is to fast-forward the ready-signal-paced dwells. Implemented:
  **Start-held fast-forward** of the intro (Tomba2_WantTurbo, scoped to phases via
  0x800675CC). Whoopee Camp now plays; holding Start skips SCEA + Whoopee (and the
  shared-phase opening cutscene). test-wide60 still 100%.
- **Dynamic resolution (play window).** Window now sizes to ~85% of desktop height
  at 4:3 (was fixed 960x720); framebuffer rescaled to the window every present with
  correct PSX 4:3 aspect (letterbox), adapting live to resize. F11 = fullscreen,
  linear filtering. (Visual confirmation pending — headless can't verify SDL.)
- wide60 matcher (first half of the prior NEXT): nearest-TR object-identity match
  across flip-segments + in-between synthesis (lerp transform, reproject joined
  verts, unjoined left at frame-A = no flicker). Verified at t=1 against the game's
  own next-frame projections: 79/79 xforms matched, 97% of next-frame verts within
  2px. Present/rasterize stage still pending (needs the rasterizer-path decision).

## 2026-06-13 — object-based 60fps: scope reframed + Tomba2 object system RE'd
- **User reframed the 60fps work (supersedes the DuckStation primitive matcher).**
  Not screen-space prim matching (its flaw: not object-based). Instead: RE the
  game's own entity system, an override that tracks objects (map IDs), the
  interpolator reads object transforms from there, and a *custom* renderer draws
  interpolated state (not bound to beetle's/duck's render path).
- **Cull-cone widening is now a native override** (was PSXPORT_POKE): six slti
  sites hooked in runtime/games/tomba2.cpp (CullSlti), RAM untouched, region-
  preserving redirect, overlay-signature-gated. Boot stays deterministic
  (RAMHASH). patches/tomba2/cull-widen.md updated.
- **Runtime gained savestates + REPL input driving** (main.cpp): -loadstate/
  -savestate, REPL save/load + press/release/tap (g_repl_buttons), F5/F9 in
  -play. Unblocks reaching live scenes headlessly. (Journal's "savestate TODO"
  done.)
- **Tomba2 object system mapped** (patches/tomba2/objects.md; tools/disasm.py =
  new capstone MIPS disassembler for RAM dumps). The cull/LOD dispatcher
  **0x8007712C** is the universal per-object chokepoint: a0 = object* for every
  live drawable, once per logic frame. Hooking it (ObjectCull, PSXPORT_T2_OBJLOG)
  enumerates the whole live set. Verified at frame ~7037+: 68-90 objects in a
  contiguous pool (base ~0x800EF478, **stride 0xC4**), positions at obj+0x2e/
  0x32/0x36 (s16 X/Y/Z), type at +0x0c, visible flag at +0x01. Camera world pos
  in scratchpad 0x1F8000D2/D6/DA. Pointer is stable per-entity across frames =
  the object ID; pool-slot reuse on scene change = snap, not lerp.
- **Renderer chosen: from-scratch reprojection** (user pick over re-running the
  game's renderer). GTE tap extended to forward OFX/OFY/H (CR24-26); new RTP
  vertex tap (psxport_set_rtp_hook, RTPS/RTPT in gte.c) reports
  (local V, transform) -> game screen SXY. PSXPORT_T2_RTPDUMP dumps tuples.
- **Projection core PROVEN faithful:** tools/reproject.py reimplements GTE RTPS
  (DivTable/CalcRecip, dist/Z divide, IR + screen saturation) and reproduces the
  game's SX/SY on **6,348,755 / 6,348,755 = 100%** of captured vertices. We can
  reproject the same geometry at an interpolated (R,TR) bit-faithfully.
- **Renderer capture layer built (runtime/wide60.{h,cpp} + GPU poly tap in
  gpu.c).** Captures per frame: GTE transforms, RTP projected verts (SXY->local
  +transform), GP0 polygons (verts/uv/color/clut/tpage). Joins poly verts to
  their transform by SXY. New psxport_set_gpu_poly_hook; PSXPORT_WIDE60=1 enables
  the renderer module (owns the GTE/RTP/GPU hooks), PSXPORT_WIDE60_LOG logs
  coverage.
- **KEY FINDING: the game projects one logic frame BEFORE it draws.** Same-frame
  poly<->projection join = ~0-2%; previous-frame join = 40-78%. So frame
  boundaries must be the GPU display flip (GP1 0x05), draws joined to the prior
  segment's projections. The <100% remainder is 2D geometry (UI/text/2D bg) with
  no RTPS origin — those snap, not lerp. (This also explains why the cur-frame
  join looked broken — it was a timing offset, not a coordinate-space bug.)
- Flip boundaries done (GP1 0x05 tap); beetle now a committed in-submodule fork
  (psxport branch), patch file retired (user call). GP0 vertex coords extracted
  as 11-bit signed (Coord11) to match the GPU/GTE.
- **CPU-projected terrain confirmed definitively.** Flip-segmented join = ~56% of
  drawn verts. Diagnosis (per-segment): projections are ABUNDANT (rtp ~7000-8000
  vs ~4700 drawn verts), yet 44% of drawn verts have NO projection even within
  ±4px (near-match 9-31%). So the unjoined 44% is genuinely transformed by a
  non-RTPS path (terrain/background = CPU-projected), not a coord nudge or timing
  miss. **User direction: RE + tap the CPU projection path too** (full fidelity,
  not a screen-space fallback).
- MVMVA terrain hypothesis FALSIFIED (those ops are lighting, not projection) —
  terrain is pure-CPU-projected, no GTE op to tap (do-not-retry).
- **User scoped the renderer:** interpolate only camera + GTE 3D models (RTPS-
  tappable), leave CPU-projected terrain + 2D UI at 30fps, NO FLICKER top
  priority. (memory: wide60-scope-decision.)
- **Core renderer op verified in C++:** wide60 retains per-joined-vertex local
  coords + the producing transform (s_xforms_prev); ReprojectRTPS (R*V+TR then
  the verified divide/screen-map) reproduces the game's captured object SXY on
  **1755/1755 = 100%** of joined verts. So object screen positions can be
  regenerated from (local, transform) in-runtime -> interpolation = lerp the
  transform + reproject the same local verts.
- NEXT: match each object's transform across flip-segments (nearest-TR), build
  the in-between display list (frame-A polys; GTE-object verts reprojected at the
  lerped transform; non-GTE polys unchanged = no flicker), rasterize + present
  A / in-between / B at 60fps.

## 2026-06-13 — read pacing root-caused via driven debugging; full fast boot chain works
- **The "stuck on Whoopee logo" class is solved.** Chain of findings, all via the REPL/
  trace/CDC-log tooling (no screenshot-guessing until final calibration):
  (1) the "frozen vsync counter" was a misread — the game main loop RESETS its counter
  each frame; identical bt = normal idle. (2) pad input verified delivered end-to-end
  (kernel pad buffer 0x8010246F shows pressed bits). (3) the logo persisting was real:
  frame-stamped CDC logs showed bit-8 fast read pacing made the game's chunked ReadN
  loader retry forever (577 cmds vs 284 healthy; sectors outran per-vsync chunk
  accounting). (4) FIX: fast pacing is consumer-conditional — next sector in 7000cy
  only if the previous data IRQ is ACKed, else native gap. Also: pacing applies to
  ReadN only (new psxport_read_is_readn; games stream audio manually over ReadS where
  sector pacing IS the audio clock — accelerating it wedges stream-end detection).
- Result, fastboot BIOS + full instant CD (default): EXEC f~50, logo jingle f~679,
  in-engine cutscene stream (Setmode+ReadS) at **f1778** (stock+native: f2833; original
  chain: ~4000+). RAM-hash deterministic. Remaining logo time is jingle-audio-bound
  (a per-game override could skip the jingle itself — future).
- CDC log lines now frame-stamped (psxport_frame). Reliable progress markers: cutscene
  start = "Setmode+ReadS" pair; load activity = ReadN/Pause cycles. The stream clock at
  0x8011824C is NOT a valid logo-end detector (byte-wraps, resets — misled two rounds).
- DEAD END (again, harder): consumer-pacing via pulling PSRCounter on ack — desyncs the
  pipe. The working form is forward-only conditional scheduling at delivery time.
- Driving pattern that worked: wide60rt -repl under a FIFO + holder process; run in
  chunks; bt/state/r/trace between; restart cheap (boot ~1s). Sessions are the new
  default workflow for game RE.

## 2026-06-13 — instant CD + fastboot OpenBIOS: game EXEC at frame 50; runtime is driveable
- **Boot chain now: ~50 frames to game EXEC** (retail-style ~4000, stock OpenBIOS ~700).
  Pieces: (1) fastboot OpenBIOS (FASTBOOT=1 upstream no-shell mode, built from a
  pcsx-redux sparse clone with mips64-linux-gnu cross gcc, FORMAT=elf32-tradlittlemips;
  scripts/build-openbios.sh; binary committed at bios/openbios-fast.bin);
  (2) instant-CD in the imported cdc.c, bitmask psxport_cd_instant (env
  PSXPORT_CD_INSTANT, default 0xF): 1=instant seeks (~2000cy incl. spin-up/pause),
  2=instant Reset (no random 0-3.25Mcy reset-seek - also a determinism hazard - and
  10kcy completion), 4=1ms disc startup delay, 8=fast data-read pacing 7000cy/sector
  (~64x; audio-paced CDDA/XA modes untouched); (3) beetle cd_access_method=precache
  (whole disc to RAM) - REQUIRED: beetle's threaded CD reader cond-wait wedges under
  instant request rates (host backtrace: CDIF_ReadRawSector/pthread_cond_wait).
- **DEAD ENDS (do not revisit):** ack-accelerated reads (pull PSRCounter on IRQ ack)
  desync the CDC sector pipe and wedge the BIOS bootstrap - twice. Holding the sector
  clock while IRQ pending degrades PS_CDC_Update to tiny chunks (livelock). 1000cy/
  sector pacing collides INT1s (consumer cannot ack between sectors). Beetle skip_bios
  hangs OpenBIOS (intercepts the retail shell); superseded by our fastboot OpenBIOS.
  The shell wait ("Data is acceptable", ~650 frames) was masking DiscStartupDelay.
- **Runtime is now driveable + self-debugging:** -repl mode (stdin/FIFO: run/r/w/cd/
  cdclog/trace/bt/state) so experiments need no rebuilds; TTY capture via PC hooks on
  the kernel A0/B0 putchar dispatchers (OpenBIOS narrates its whole boot); CDC cmd/IRQ
  log (PSXPORT_CDC_LOG); watchdog (SIGALRM, 5s no-frame-progress) dumps host backtrace
  + emulated GPRs + heuristic MIPS stack scan (jal-preceded return addresses) then
  kills. RAMHASH/GAMELOG/RAMDUMP/MEMWATCH/PCCOV env probes. Verified deterministic
  (RAM hashes identical across runs, full instant config).
- Tomba 2: engine running by frame ~1200 (GTE/card kernel patches logged), no hangs to
  4000+. Heartbeat hook (0x8003CCA4) verified plumbing-wise; fires only in real
  gameplay, which the X-mash script does not reach in this runtime (savestate support
  is the proper fix, TODO).
- User direction embedded in workflow: aggressive change + find-and-override on
  breakage; debug probes over screenshots; runtime must be interactive (REPL) and
  self-diagnosing (watchdog+stack traces). BIOS code is part of the project now.

## 2026-06-12 — runtime: hook layer + RE tooling; intro segments are LOAD MASKS
- Hook/override layer live in the runtime: per-instruction hook point in beetle's
  interpreter (patches/beetle-psx/0001, -DPSXPORT_HOOKS), registry in
  runtime/psxport_hooks.* (PC + expected-instruction signature -> native fn; REDIRECT
  return skips original code, resumes at chosen PC = native override). RE aids ported:
  PSXPORT_RAMDUMP (frame:path snapshots), PSXPORT_MEMWATCH (per-frame byte CSV),
  PSXPORT_PCCOV (executed-PC bitmaps over frame ranges). Per-game modules in
  runtime/games/ get RAM + per-frame tick + scoped input injection via main.cpp.
- **Tomba 2 intro: the license text and Whoopee Camp logo are NOT skippable — they are
  load masks.** Verified: scripted X presses change nothing (A/B identical timeline);
  PC-coverage diff shows the segment-end code is absent from RAM until the loader
  finishes (main overlay lands ~frame 1989 mid-logo; logo then runs out its jingle,
  stream clock at 0x8011824C freezes at 7776 at segment end ~2400). Poking the clock
  does nothing (it is an output, not the gate). DEAD END: do not look for an input
  check or a timer compare to patch.
- Implemented instead: scoped auto-turbo (8x, no presents/audio on skipped substeps)
  while main overlay absent (word @0x8005082C == 0) OR logo stream clock ticking,
  frame-capped. Ends exactly at the cutscene. No input injection (user correction:
  the X-mash "skip" was never wanted as automash).
- Beetle's skip_bios HANGS with OpenBIOS (intercepts the retail shell; game stalls at
  the logo) — -fastboot is opt-in, default off. 14x CD loading is safe and default.
- Tab = manual 8x fast-forward in play mode.

## 2026-06-12 — scope change #3: PC port via interpreter + overrides (Beetle/mednafen base)
- User direction: build the actual PC port — NOT static recomp; an interpreter+overrides
  design (native function overrides hooked by PC over an interpreted base), because the
  generic+matching tiers still flicker and the real fix is rendering new frames from
  interpolated state, which needs first-class control of the render path.
- Base: **Beetle PSX (mednafen) sources imported into our build** — vendor/beetle-psx
  submodule, runtime/Makefile reuses upstream Makefile.common source lists but compiles
  everything ourselves (no .so, no prebuilt core; user explicitly wants source import).
  Interpreter-only: HAVE_LIGHTREC=0. GPL-2: distributable with source, unlike the
  CC-BY-NC-ND DuckStation fork (which stays as lab/oracle).
- runtime/main.cpp: our host (libretro callbacks for now, to be replaced by native glue):
  headless, -frames/-dumpdir/-dumpinterval (PPM), -inputscript (same format as regtest),
  -bios dir. **VERIFIED: Tomba 2 boots and renders in-engine intro at frame ~4000 with
  OpenBIOS** (copied as scph5501.bin; SHA warning is benign). Built deps-free in ~1 min.
- PCSX-Redux was tried first and dropped: GPL-2 (fine) but heavy deps (luajit/luv/uv/
  ffmpeg) vs beetle's zero-dep build; user picked beetle.
- Next: port the hook layer (PC trace, pokes, GTE taps) into the imported cpu.cpp/gte.cpp;
  override dispatch table (PC -> native fn, signature-checked for overlays); then the
  first real override: Tomba 2 render entry at 60Hz with interpolated state.

## 2026-06-12 — per-game object-identity interpolation LIVE (both games)
- MatchFrames now has the per-game pass: objects matched across frames by mutual-nearest
  GTE translation (TRANSFORM_MATCH_RANGE 4096 L1), each matched object's prims paired in
  capture order with token equality, gate 160px (vs 48 generic), overriding the heuristic
  pairing. Identity-says-same but geometry-jumped pairs snap (match=-1), they don't fall
  back to the heuristic.
- Coverage: Crash Bash gameplay ~38% of draws tagged (the GTE-projected 3D models — the
  moving things that flicker), 57 objects, 100% TR continuity, ~95% of tagged matched.
  Tomba 2 gameplay ~10% tagged (NPC models; terrain is CPU-projected — confirmed: 152/500
  prim verts have NO recorded SXY within ±8px, so the engine projects terrain without
  per-vertex GTE) — but those 10% are exactly the characters. ~94% of tagged matched.
- **GTE tagging requires CPU = Interpreter** (swc2/mfc2 hooks are interpreter-only; the
  recompiler inlines them). Without it the generic tier still works, tagging is just 0.
  GUI users: Settings -> CPU -> Execution Mode -> Interpreter.
- Next for coverage: hook the game's CPU-side projection output path per game (Tomba 2
  terrain), rotation-matrix tracking for camera-motion-aware gates, per-game config files.

## 2026-06-12 — per-game tier started: GTE tagging + Tomba 2 cull-cone patch (user repro fixed)
- **Tomba 2 fisherman culling RE'd and patched.** Engine culls objects against a view
  cone narrower than the screen (six `slti $v0,$v1,0x350..0x370` threshold sites in the
  overlay enqueue function ~0x80077100-0x800776E0; v1 = cos-scaled dot/distance, below
  threshold = culled). Pokes in patches/tomba2/cull-widen.md scale thresholds by ~0.72:
  13 objects drawn vs 5 at the repro savestate, walk pop-ins 12 -> 5. Applied via new
  PSXPORT_POKE env (every-vblank RAM pokes, conditional "old:new" form for overlay
  safety) — the prototype of the per-game patch layer.
- RE toolchain built into the fork (all env-gated): -loadstate / -widescreen regtest
  flags, PSXPORT_TRACE_PC/-OUT (interpreter PC-hit logger: a0/a1/ra per hit + vblank),
  PSXPORT_WIDE60_RAMDUMP (2MB RAM snapshot for capstone disassembly offline),
  PSXPORT_WIDE60_TRDUMP (per-frame GTE transforms with writer pc/ra). Workflow that
  found the cull: trace draw-object entry (0x8003CCA4) per frame -> diff drawn-object
  sets across a scripted walk -> found live enqueue path (0x8007763C variant; NB the
  0x8007703C sibling never runs in this scene) -> disassemble backwards to the cone test.
- **GTE transform tagging tier (in progress):** TRX/TRY/TRZ ctc2 writes hooked (gte.cpp)
  = object world transforms: Tomba 2 has 110-125/frame, 100% frame-to-frame continuity
  (nearest-TR) — object identity is real. Linking transforms to prims: swc2/mfc2 SXY
  hooks keyed by VALUE (Tomba 2 memcpys prims from scratch buffers, so addresses don't
  survive; the packed coord word does). Status: only ~12% of draws tagged even with a
  +-2px probe (game nudges coords post-projection; demo tunnel mesh appears to be
  CPU-computed, not per-vertex GTE). Needs more work before it can drive matching.
- Matcher: added motion-coherence filter (matches whose displacement deviates >12px
  from the local median of +-3 arena-order neighbours snap instead of lerping) — kills
  ~8 wrong pairs/frame on Crash Bash, match rate stays 97%. Aimed at the user-reported
  vertex flicker; user verification pending.
- Tomba 2 in-engine (demo, real gameplay): generic matcher ~96% (1132/1172 typical).
  User-verified 60fps in the Qt GUI. Note: regtest headless to frame <12000 is all FMV
  for Tomba (draws=0) — gameplay tests need -loadstate or frames >20500.
- User direction: per-game quality (RE) is the priority, not generic-only.

## 2026-06-12 — REAL-TIME 60fps working in-emulator (patch 0002); user-verified on Tomba 2
- `gpu_wide60.{h,cpp}` in the fork: captures each logic frame's backend command list
  (tee at `GPUBackend::PushCommand`, draws carry DMA src addr + E5 offset from the GP0
  dispatch hook), segments by GP1(0x05) flips, matches adjacent frames (src-addr sort +
  difflib-style alignment on type/texcoord fingerprints, C++ port), and at the vblank
  after a flip replays a vertex-lerped copy of the frame's list (its own clear included),
  then the original one vblank later. Presented: A, lerp(A,B), B, lerp(B,C)... 60 fps,
  zero added latency, timing-invisible to the game (no FIFO/tick changes). Replays skip
  UpdateVRAM/CopyVRAM (stale uploads would clobber streamed textures) and re-apply live
  drawing-area/CLUT after. Enable: regtest `-wide60` or `PSXPORT_WIDE60=1` (Qt GUI too).
- **CRITICAL finding — E5 draw offsets DO alternate per double-buffer, bundled
  mid-packet.** All our offline tools (`interp_dump.py`, the E5 scans) only parse
  `words[0]` of each GP0 packet, so they never saw the E5s and compared raw
  (= buffer-relative) coords — the offline matcher worked BY ACCIDENT. At the backend,
  vertices are absolute (offset baked in) and alternate by e.g. y+256 per frame: gate in
  **buffer-relative space** (abs − that draw's E5 offset), rebase prev verts to the
  current offset before lerping. Before this fix: 0% matched (uniform disp ≈256);
  after: **97% matched** on Crash Bash gameplay (1637/1688 draws/frame), beats offline.
- Flip boundary: GP1 writes bypass the GP0 FIFO, so flip-at-GP1-time can cut the capture
  mid-frame; the boundary now fires once all words pushed before the GP1 are consumed
  (pushed-words counter). (In practice CB's FIFO was empty at GP1, but keep the guard.)
- Verified: consecutive presented frames all distinct in gameplay (vs identical pairs at
  30fps); interpolated frames visually clean; ~186 emu-fps headless SW renderer.
  **User-verified 60fps in Qt GUI on Tomba! 2** (built with `-DBUILD_QT_FRONTEND=ON`).
- Diagnostic tooling kept in the fork: `PSXPORT_WIDE60_DUMP=<csv>` dumps one frame-pair's
  sorted (addr, token, type, x, y) sequences for offline analysis; distance-2 arena-slot
  ground truth was used in-emulator to prove capture sanity (95% same-slot, disp ≤8).
- **Open issues:** (1) vertex flicker during play — generic-tier mismatches (wrong pairs
  within degenerate token runs lerping, borderline pairs alternating lerp/snap); the
  per-game GTE-transform-tagging tier is the designed fix, or matcher hysteresis.
  (2) Widescreen ineffective on Tomba 2 despite WidescreenHack=true + 16:9 (no gamedb
  override; likely the game bypasses standard GTE projection — needs the per-game tier).
  (3) Headless Tomba run showed draws=0 pre-frame-11900: that's the FMV region (demo
  starts ~20500), not a capture failure — GUI run interpolated fine.

## 2026-06-12 — object-identity matching via arena slots; 60fps PoC verified visually
- User direction: don't rely on screen-space nearest matching — use object identity.
  PSX-specific insight: matrices never reach the GPU (GTE is CPU-side), but the DMA
  linked list gives every GP0 packet's **RAM address**, and engines bump-allocate
  per-object prims from a **double-buffered arena** (measured: 0% address overlap
  between adjacent frames, 88.5% two frames apart → same slot = same prim at N↔N+2 =
  free ground truth).
- Fork change (patch 0001): GPU::DMAWrite records `SRCADDR <hex>` Comment packets in
  GPU dumps (the dump player ignores Comment packets, so dumps stay replayable).
- Matcher (in tools/interp_dump.py): sort draws by arena address (allocation order =
  entity iteration order), sequence-align (difflib) on position-independent mesh
  fingerprints = cmd byte + **texcoord low halves only**. KEY FINDING: clut/texpage
  high halves alternate with frame parity (the game double-buffers CLUTs) — including
  them drops adjacent-frame token overlap from 92.6% to 4%. Validated against the
  N↔N+2 address ground truth: **89.1% aligned, 99.2% correct** (Crash Bash gameplay).
  Naive rank pairing: 1%. Diff-align on (cmd,len) only: 18.7%. UV fingerprints with
  clut included: 3.2% coverage. (Dead ends — don't revisit.)
- tools/interp_dump.py converts a 30fps dump → 60fps dump (lerped in-between frames,
  abs-coordinate lerp across E5 offsets, 100px displacement gate snaps residual
  mismatches). Output replays cleanly through the real renderer (regtest replay mode);
  earlier screen-space-key version produced giant stretched-polygon artifacts, v2 has
  none visible. Comparison video: scratch/screenshots/crashbash-30v60.mp4.
- Tomba 2 with SRCADDR: arenas also double-buffered (d1 overlap 0%) but slots NOT
  stable at distance 2 (22.3% — allocator churns with streaming tunnel geometry), so
  slot-based ground truth is Crash-Bash-specific, not universal. The 51% "consistency"
  measured for Tomba 2 is against an invalid truth — ignore it. Fingerprint alignment
  covers 90.5%; visual check of interpolated frames is clean (no stretched polys).
  Videos: scratch/screenshots/{crashbash,tomba2}-30v60.mp4.
- Next: real-time implementation of this matcher in the fork (synthesize+present the
  interpolated frame at the vblank between game flips); widescreen tier; per-game
  validation tooling that doesn't depend on slot stability.

## 2026-06-12 — both games measured: 30 fps presented framerate (gameplay)
- Added `-inputscript` to regtest (in patch 0001): scripted pad-1 digital input,
  `<start> <end> <Button>` per line. Deterministic across runs — menu navigation
  scripted blind via screenshots worked reliably.
- **Detection method correction:** bucketing draw commands by vsync is misleading
  (submission spans vblanks → alternating big/small buckets). Ground truth is
  **GP1(0x05) display-start changes** (buffer flips) per vsync — added to
  gpudump_stats.py.
- **Crash Bash gameplay (Battle/Jungle Bash, 4 players active): 30 fps** — 64/64 flips
  at 2-vsync gaps. Menu also 30 fps. The "Crash Bash is 60fps" belief is false, at
  least for menu + this minigame. ~1200 draw cmds per logic frame in gameplay.
- **Tomba! 2 (attract DEMO, minecart, in-engine): 30 fps** — 73/75 flips at 2-vsync
  gaps (outliers = loading hiccup). ~1400 draw cmds max.
- Input scripts for reaching gameplay: `scratch/inputs-crashbash-gameplay.txt`
  (menu → Battle → 1P → Jungle Bash, gameplay from ~frame 11700);
  Tomba 2: title at ~3500, attract DEMO (in-engine minecart) ~frames 20500-24500 —
  no menu navigation needed for engine captures (`scratch/inputs-t2d.txt`).
- Tomba 2 cutscene-skip via Start did NOT work in scripted runs (FMV runs to ~18000
  then Loading → demo); reaching actual player-controlled gameplay still TODO.
- Dumps: `scratch/raw/crashbash-{menu,gameplay}.psxgpu`, `scratch/raw/tomba2-demo.psxgpu`.

## 2026-06-12 — oracle running; Crash Bash menu measured at 30 fps
- DuckStation regtest builds (prebuilt deps release-20260526 in `dep/prebuilt/`; system
  `extra-cmake-modules` required). Three fixes needed, kept as
  `patches/duckstation/0001-*.patch` (submodule gitlink stays clean upstream — apply
  patches after `submodule update`, see patches/duckstation/README.md).
- **No retail BIOS on this machine.** Using **OpenBIOS** (PCSX-Redux's open-source BIOS):
  downloaded from pcsx-redux GitHub Actions artifact "OpenBIOS" (gh api, needs auth),
  installed to `~/.local/share/duckstation/bios/openbios.bin`. Crash Bash boots fine with
  it (no fast-boot; menu reached ~frame 3500-4000). Keep a copy in `scratch/bios/`.
- GPU dump pipeline verified end-to-end: `duckstation-regtest -gpudump <path>
  -gpudumpstart N -gpudumpframes M -- <chd>` → `.psxgpu.zst` → `zstd -d` →
  `tools/gpudump_stats.py` (counts draw cmds/frame, hashes display lists, infers logic
  rate from identical-frame runs).
- **Measured: Crash Bash main menu renders every other vblank → 30 fps menu logic**,
  ~430 draw cmds per logic frame, all frames distinct (sequence: 0,429,0,430,...).
  Gameplay rate unknown — needs controller input to reach a minigame (regtest has no
  input mechanism yet; next fork feature: scripted input or memcard/savestate boot).
- Log level names are case-sensitive (`-log Info`, not `info`).
- regtest run perf: ~3000 emulated FPS headless software renderer — 4000 frames ≈ 1.4 s.

## 2026-06-12 — scope change #2: generic "wide60" layer (see CLAUDE.md)
- Refined goal: a *reusable* widescreen+60fps system for PSX games, logic untouched,
  per-game RE minimized. Architecture = generic tier (primitive-level display-list
  interpolation + DuckStation widescreen hack) + per-game tier (GTE transform tagging
  config + culling/HUD patches). Full rationale in CLAUDE.md.
- Framerates NOT verified for either game yet — the interpolator's logic-rate detector
  will measure them. Do not assume Crash Bash is 60 fps (earlier note retracted;
  user is unsure too).

## 2026-06-12 — DuckStation hook-point survey (code reading, pre-build)
- All primitives flow through a single backend command stream: `GPU::HandleRenderPolygonCommand`
  (`src/core/gpu.cpp:3063`) → `GPUBackend::NewDrawPolygonCommand` (`gpu.cpp:3229`) →
  video-thread queue of `GPUBackendDrawPolygonCommand` (+Line/Rectangle variants).
  This *is* the per-frame display list for the interpolator: capture per frame at this
  layer, match prims across frames, re-emit lerped copies on synthesized frames.
- Frame boundary: vblank handling in `gpu.cpp` CRTC state (~line 1637).
- `src/core/gpu_dump.cpp`: existing GP0 stream recorder → use it to dump real frames
  and prototype the primitive matcher OFFLINE before modifying the render loop.
- GTE register writes for the per-game tagging tier live in `src/core/gte.cpp`.

## 2026-06-12 — scope change #1: no recomp, patches + modified emulator
- DuckStation license is CC-BY-NC-ND-4.0 → modified fork must stay private; patches
  themselves are publishable.
- DuckStation regtest build: prebuilt deps downloaded (release-20260526, sha verified)
  into `dep/prebuilt/`. Configure blocked on missing system `extra-cmake-modules`
  (ECM, for Wayland) — needs `sudo dnf install extra-cmake-modules`.
- The recompiler/harness scaffolding below is superseded; discdump, disc provisioning,
  and the submodule remain in use.

## 2026-06-12 — project init
- Repo scaffolded per recomp-init: `recompiler/ runtime/ overrides/ harness/ tools/
  generated/(gitignored) scratch/(gitignored)`.
- DuckStation vendored as shallow submodule, pinned at `3a98566`.
- Disc: `Crash Bash (USA).chd` via `PSXPORT_DISC` in `.env` (gitignored).
- No chdman on this machine; CHD is read directly with DuckStation's vendored
  `dep/libchdr` — `tools/discdump` extracts SYSTEM.CNF + the boot executable to
  `scratch/bin/`.
- `tools/discdump` built and verified against the real disc:
  - Boot executable: `SCUS_945.70` (432128 bytes on disc, LBA 23).
  - PS-X EXE header: entry PC `0x8002E7B0`, load addr `0x80010000`, text size
    `0x69000` (430080 = file size − 2048 header), initial SP `0x801FFFF0`.
  - SYSTEM.CNF: `TCB = 4`, `EVENT = 16`, `STACK = 801FFF00`.
- Next: harness scaffolding — drive DuckStation headless as the oracle (savestate
  freeze/restore, fixed timebase, pinned input) BEFORE any recompilation. Then the
  R3000A recompiler skeleton targeting the extracted EXE.
- Phase-2 feature targets (recorded now, implemented only after faithful base):
  widescreen (projection-level) + interpolated 60 fps via per-object transform
  interpolation (n64recomp style).

## 2026-06-17 (later-89) — HW renderer M3: textured GPU rasterization (CLUT-in-shader) renders Tomba2; draw-area clip fix.
Textured triangle pipeline (tritex.vert/frag): samples a VRAM snapshot (avoids render/sample feedback
loop) with 4/8/16bpp + CLUT lookup + texture-window, affine (noperspective) UV, per-pixel modulation,
transparent-texel discard, exactly matching SW sample_tex's addressing. Per-prim state (texpage, CLUT,
window, draw-area) carried as flat vertex attributes -> one draw call. Tee'd from gp0_exec (textured
opaque polys). Diff harness (PSXPORT_VK_DIFF) draws untextured + textured over the uploaded SW VRAM.
- **Renders correctly:** title screen + the in-engine demo scene (structure/lava/character/HUD) all
  render via the GPU textured path, visually matching SW.
- **DRAW-AREA CLIP was the key fix:** SW clips polys to s_da_*; VK didn't, so polys overdrew the atlas/
  top region (big spurious block). Per-prim draw-area discard in the shader: f3000 demo 19.0% -> 14.4%
  mismatch. Texture window added (no measurable change on this scene; correct to have).
- **Residual ~14% on the busy demo** = sub-pixel edge-coverage + UV/color rounding differences on ~944
  small tris (GPU pixel-center vs SW integer-coord rasterization) — visually invisible (scene matches),
  same class as the SW-vs-Beetle residual. Reducing it = matching SW's exact fill/rounding rules (later).
- OPEN: semi-transparency (4 blend modes; skipped in the tee), sprites/rects/lines/fills (M4), then
  switch present to VK VRAM (M5). PSXPORT_VK=1 windowed; SW path + default untouched.

## 2026-06-17 (later-90) — HW renderer M3/M4 VISUALLY VALIDATED (user can't tell VK from SW).
User compared the VK render vs SW render of the demo scene front buffer (vk_out/sw_out): "can't even
tell them apart." So the GPU textured pipeline (M3 polys + M4 sprites, CLUT-in-shader) renders Tomba2
correctly; the ~14% pixel "mismatch" was invisible off-by-1 (ordered dither + UV/color rounding, not
implemented to match SW exactly — and not worth chasing per the user). Validation method going forward:
SEND renders for the user to eyeball, don't naive-pixel-diff. M4 sprites tee'd (rects as 2 tris).
- REMAINING for VK to BE the renderer (M5): VK owns VRAM (CPU uploads, VRAM->VRAM copies, fills, lines),
  semi-transparency (4 blend modes — needs VK-owned VRAM to matter), then present from VK VRAM + retire
  SW (default-on). Until then VK renders the tee'd prims over the uploaded SW VRAM (validated identical).

## 2026-06-17 (later-91) — HW renderer COMPLETE: Vulkan is the DEFAULT renderer; SW retired to oracle/fallback.
M5 finished. VK owns VRAM and renders every PSX primitive type (polys, sprites, lines-as-quads, fills/
copies/uploads via dirty-region mirroring, semi-transparency 4 blend modes). gpu_gpu_enabled() now
DEFAULTS ON for windowed runs; PSXPORT_SW_GPU=1 (or PSXPORT_VK=0) forces the SW rasterizer (the proven
oracle). Headless always stays SW (no window -> no VK). Validated: 5000-frame windowed run across boot/
title/ship-demo/field-demo/tutorial = ZERO validation errors (RADV); frames render indistinguishably
from SW (user-confirmed "looks great"). PSXPORT_VK_SHOT=frame dumps the live VK frame.
- Architecture per frame: mirror SW-written dirty regions -> snapshot textures -> OPAQUE pass ->
  snapshot post-opaque framebuffer -> SEMI pass (samples snapshot as texture + blend dest) -> present.
- OPEN (refinements, not blockers): strict per-op draw order (currently opaque-batch then semi-batch =
  standard separation, fine for Tomba2); residual reduction (off-by-1 dither/rounding); perf numbers.
- This is the foundation the user wanted before widescreen + object interpolation.

## 2026-06-17 (later-92) — Widescreen WORKS on Tomba2 (falsifies the old "ineffective" note) + extended culling.
With the native VK renderer owning display, the GTE widescreen hack works on Tomba2:
- **PSXPORT_WIDE=1**: gte_init() sets widescreen_hack=1 + aspect 16:9 (squish projected X around centre);
  gpu_gpu present fits 16:9 instead of 4:3 -> wider horizontal FOV. (Old later-era note "ineffective on
  Tomba2" was on the oracle; FALSIFIED here.) 2D/sprites bypass the GTE (HUD-stretch is the next per-game item).
- **PSXPORT_CULL=1 (extended culling, user-requested):** the game's FUN_8007712c culls each object by
  distance AND a FOV cone (depth/dist < ~0x370 ≈ ±77°) — over-culls (pop-in; widescreen edges dropped).
  ov_object_cull (game_tomba2.c) now, after the game's cull, RE-INCLUDES objects it dropped that are
  within an extended distance (PSXPORT_CULL_FAR, def 0x6000 ≈ 3.4x the 0x1c00 max) + wider cone
  (PSXPORT_CULL_FOV, def 0x80 vs 0x370): mark visible@+1, return 1. Near/behind culling kept intact.
  Verified: more right-edge structure/objects render in widescreen. Tunable via the two envs.
- OPEN: HUD stretch under 16:9 (sprites need un-stretch/reposition); the re-included far objects rely on
  the +1 visible flag driving the draw (works in test); tune thresholds with the user.

## 2026-06-17 (later-93) — PC-native widescreen: TRUE wider FOV, NO squish/stretch (replaces the rejected hack).
User REJECTED the old PSXPORT_WIDE (Beetle GTE squish-X 0.75 + display-stretch 4:3->16:9): "we are making
a PC game, don't squish anything." Replaced with a genuinely native renderer-side widescreen:
- **Key insight (no squish):** keep the GTE's NATIVE projection scale; to show a wider FOV, just
  RE-CENTER the framebuffer-local view into a WIDER target. Math: widescreen_x = native_x + (FBW-320)/2
  (a constant shift, NOT a 0.75 scale) — same per-unit scale (full horizontal resolution preserved),
  +54px of world on each side. The GTE already projects geometry past the 4:3 edges (measured earlier:
  ~11% in the right band); it was only being clipped. So widescreen = native projection + widened clip
  + wider target. widescreen_hack now hard-OFF in gte_beetle.c.
- **Where the wide image lives (VRAM is fully packed — can't widen in place):** the VK R16_UINT image is
  grown VRAM_H(512) -> IMG_H(992); rows [512, 992) are a VK-only scratch framebuffer (FB_Y0=512, up to
  856x480 = 16:9 @ 2x). gpu_gpu.c relocates the tee'd geometry into the FB via a VERTEX push-constant
  transform (tritex.vert): local = i_pos - i_da.xy (da.xy = active framebuffer origin), fb =
  ((local.x+WIDE_OFF)*ss + fb_x0, FB_Y0 + local.y*ss); clip overridden to the FB rect so wide geometry
  isn't dropped. Textures still sampled from VRAM rows <512 (unchanged). Present samples the 16:9 FB 1:1
  (no stretch). Reuses the whole existing pipeline (image/renderpass/snapshot/semi); non-wide path is
  byte-identical (verified: f1500 nowide == baseline). PSXPORT_SS (1..2) supersamples (FB 856x480).
- **Cull coupled to widescreen (the user's point: "I thought we ported the culling and adjusted it"):**
  the static terrain/water TILES also go through the per-object cull, so the wider FOV needs a wider
  re-include cone + farther distance or the new edges/corners stay black. ov_object_cull now AUTO-widens
  when PSXPORT_WIDE is on (PSXPORT_WIDE implies cull, defaults fov 0x00 / far 0x8000 vs 0x80 / 0x6000 for
  plain CULL). Fills the deep-water horizon + side bands; only the water tile-grid's TRUE outer edge
  leaves tiny corner wedges (would need bg-plane extension to kill — deferred).
- Validation-layer clean (RADV). User-shown f1500/f3000/f700: correct proportions, more world each side,
  HUD at native scale + centered (NOT stretched). OPEN/next (user: "all of these"): SSAA downsample
  present shader, HUD edge-anchoring (2D sprite path), shaders/lighting. WATCH: aggressive cull can cause
  entity walk-through ghosts (journal later-52) — needs playtesting now that the user is engaged.
- **later-93b (same session):** PSXPORT_SS now defaults to 2 (FB 856x480 = fills the image exactly;
  sharper than 428x240 upscaled to the window). **HUD edge-anchoring done** (gpu_gpu_sprite_anchor_dx,
  wired in the gp0 0x60-0x7F sprite tee in gpu_native.c): 2D sprites bypass the GTE, so instead of the
  renderer centering them, each sprite shifts by (Xc-160)*(FBW/ss-320)/320 native px before the ss scale
  — Xc=160 stays centered, Xc=0 pins to the new left edge, Xc=320 to the right. Native size preserved.
  Verified f3000: score/items move to the corner (cmp_hud_left.png). Inert when not wide.
- **STILL OPEN (user: "all of these"):** (1) true hi-res beyond the 1024-wide image cap — needs a
  DEDICATED FB image (the cram-into-VRAM-rows trick caps FBW at 1024; the semi-blend frag samples one
  combined sampler for textures<512 AND the FB blend-dest, so a separate FB needs a 2nd sampler binding
  + frag change). (2) shaders/lighting — wants an RGBA8 FB + post passes (same dedicated-FB refactor).

## 2026-06-17 (later-94) — MEMORY CARD now works (was hung on "Checking MEMORY CARD..."). PC-native, zero delay.
User: "memory card doesn't work." Symptom (reproduced via tools/drive.py: title -> Load Game -> slot):
the Load screen hung forever on "Checking MEMORY CARD...".
- **Root cause 1 — card I/O primitives were UNIMPL in the HLE.** memcard.c set rec_set_override on BIOS
  addresses 0x8009xxxx, but those addresses NEVER execute in this pure-HLE-BIOS build — the game's
  statically-linked libcard/libmcrd calls the BIOS via the `li t0,0xB0; jr t0` trampoline, which funnels
  to rec_dispatch_miss -> recomp_hle (hle.c). recomp_hle only handled B0:0x4A/0x4B; _card_read(B0:0x4E),
  _card_write(B0:0x4F), _card_status(B0:0x5C), _card_info(B0:0x4C), _card_chan(B0:0x50), and the A0-table
  _card_info(A0:0xAB)/_card_load(A0:0xAC) all fell through to "UNIMPL". Fix: `card_hle_a0`/`card_hle_b0`
  in memcard.c, dispatched from the A0/B0 default cases in hle.c; they do SYNCHRONOUS host-file I/O
  (card_read_frame/card_write_frame) = PC-native, zero delay. Dead rec_set_override calls removed.
- **Root cause 2 — completion delivered to the WRONG event class.** libcard completion is event-based;
  this HLE has no SIO IRQ, so the override must DeliverEvent the completion itself. Captured (PSXPORT_EV_LOG)
  the game's "checking" loop: it TestEvents the **SwCARD class 0xF4000001** (NOT HwCARD 0xF0000011) every
  frame for spec EvSpIOE(0x0004). `card_deliver_complete` now fires SwCARD+HwCARD EvSpIOE (NOT NEW/0x2000,
  which would mean unformatted -> format prompt). hle_deliver_event only fires open+ENABLED matching slots.
- **VERIFIED (drive.py):** Load -> "Select slot" (slot 1/2 detected) -> select slot 1 -> reads dir frame 63
  -> **"No data for Tomba!2"** (correct for an empty card; was a hang). scratch/screenshots/card_final.png.
  Card file = scratch/saves/tomba2.mcr (128 KB). OPEN: WRITE/save round-trip not yet tested end-to-end
  (needs a save-point playthrough) — but it's the same now-proven completion path (B0:0x4F + SwCARD IOE).

## 2026-06-17 (later-95) — Vertex smoothing (PGXP subpixel) — kills the PS1 vertex wobble, PC-native.
PSX projected 3D vertices to INTEGER screen coords, so geometry jitters as the camera/object moves
(the classic PS1 "wobble"). Beetle's GTE ALREADY computes the subpixel-precise projected coords
(gte.c TransformXY -> precise_x/y/z) and hands them to PGXP_pushSXYZ2f — which was a dead no-op stub
in gte_beetle.c. Implemented it for real instead of leaving 3D snapped-to-integer:
- **gte_beetle.c:** PGXP_pushSXYZ2f now caches (precise_x,y,z) in a 32K-entry hash, keyed by the
  packed integer SXY (`v` = XY_FIFO[3]) the game copies verbatim into its GP0 vertex packets. Exposes
  `pgxp_lookup(sx,sy,&px,&py,&pz)` (1=hit) and `pgxp_frame_reset()`. **Value-keyed PGXP-lite:** on a
  key collision lookup just misses -> integer fallback, so a wrong match can only cost smoothing, never
  correctness. **Reset every presented frame** (gpu_native gpu_present_ex) so a stale precise value from
  a prior frame can't be re-applied to a freshly integer-placed vertex (the cross-frame-wobble trap).
- **gpu_gpu.c:** added float-position draw entries `gpu_gpu_draw_tritri_f`/`gpu_gpu_draw_semi_f` (TexVtx.x
  was already float; the vertex shader already divides floats). The old int entries are now thin
  wrappers that widen to float -> one impl. (+ stubs in the no-VK build.)
- **gpu_native.c:** ONLY the polygon tee (op 0x20-0x3F = GTE-projected 3D) looks up the precise coords
  by each vertex's integer (v[i].x,v[i].y) and passes FLOAT subpixel positions (+s_off). Sprites
  (0x60-0x7F, bypass the GTE / 2D HUD) and lines stay integer — correct, they have no GTE-precise coord.
  Gated PSXPORT_PGXP (default ON; =0 = exact old integer behavior for A/B vs the oracle).
- **VALIDATED (f1500 swing/water, f3000 lava):** geometry fully intact, nothing warped/missing in either
  scene. On-vs-off diff (scratch/screenshots/pgxp_diff_1500.png): differences are concentrated on the
  POLYGON EDGES of every 3D primitive = subpixel vertex repositioning (proves broad cache-hit coverage),
  exactly the expected signature. widescreen_hack is OFF so precise_x is native (no squish baked in).
  NOTE: wobble reduction is a TEMPORAL effect — stills can't show it; user judges in motion. OPEN: if
  motion reveals mis-snap (value-key collisions), add PGXP-proper RAM-address tracking (handoff note).

## 2026-06-17 (later-96) — Lighting/shading model REVERSE-ENGINEERED (groundwork for a native lighting engine).
User goal: RE the game's rendering/lighting so we can intercept + replace it PC-native (e.g. a real
lighting engine instead of the PSX one). Built `PSXPORT_GTEPROBE=<frame>` (gte_beetle.c): dumps the GTE
ops that ACTUALLY execute + a lighting/fog control-register snapshot; corroborated by a static histogram
of every `gte_op(...)` immediate in `generated/shard_*.c`. **Full model now in docs/engine_re.md.** Result:
- **NO dynamic GTE lighting at all** — `NCDS/NCDT/NCCS/NCCT/NCS/NCT/CC/CDP` = 0 executions, 0 call-sites.
  No light sources, no normal·light-matrix shading.
- **Vertex colors are BAKED** in model data; **`GPF`** (very high count) scales them by a scalar IR0
  (per-object brightness / fade). **`DPCS`/`DPCT` depth-cue = the atmosphere "lighting":** color lerped
  toward **FarColor (CR21-23)** by `IR0=DQB+DQA·H/Sz`. FarColor is **scene-tinted** — f1500 water=(0,0,0)
  fade-to-black, f3000 lava=(1280,0,0) red; DQA=6/DQB=0 both. (RTPS/RTPT/MVMVA/NCLIP/AVSZ run as expected.)
- **Interception point:** final per-vertex RGB → GP0 gouraud polys → gpu_native gp0_exec tee (rs/gs/bs).
- **Unlock for native lighting:** PGXP (later-95) already caches per-vertex screen x/y + precise_z, so we
  can unproject to view-space position and derive per-FACE normals (edge cross-product) IN THE RENDERER —
  enabling native directional/point lighting, normal shading, SSAO, and a replacement per-pixel fog (tint
  from CR21-23). No need to fight any existing dynamic lighting (there is none). NEXT: pick the lighting
  style + build the normal-reconstruction + shader path (scope question to user).

## 2026-06-17 (later-97) — NATIVE LIGHTING ENGINE: normal+depth reconstruction + directional light + fog.
User scope (AskUserQuestion): "Full pipeline (normals first)" — build the shared normal/depth
reconstruction, then layer directional light + fog + AO. Built the foundation + first two layers:
- **View-space position capture (gte_beetle.c):** PGXP cache now also stores the GTE's view-space vertex
  (IR1/IR2/IR3 = rotation·V + translation, read via GTE_ReadDR(9/10/11) at PGXP_pushSXYZ2f time, where
  TransformXY hasn't touched IR yet). New `pgxp_lookup_view(sx,sy,...)`. This is the ONLY normal source —
  the game has no GTE lighting (later-96).
- **Per-face normal (gpu_gpu.c tex_emit):** for each triangle with 3 view-space verts (looked up in the
  polygon tee, gpu_native.c), normal = normalize(cross(e1,e2)), oriented toward the camera at the origin
  (flip if dot(N, faceCenter) > 0). depth = view Z. Passed as new TexVtx fields nx/ny/nz/depth (vertex
  attrs loc 7/8). 2D sprites/lines/HUD pass NULL view -> zero normal -> shader leaves them untouched (they
  bypass the GTE so they'd never hit the cache anyway). If ANY vert misses the cache, the whole tri is unlit.
- **Shaders (tritex.vert/frag):** frag gets a fragment push-constant LPC (offset 48): l0=(dir.xyz view
  space, mode 0=off/1=directional/2=normal-viz), l1=(ambient,diffuse,fogNear,fogFar), l2=(fogTint.rgb,
  fogEnable). Lighting modulates the SOURCE color (unpacked from 555 -> float -> relight -> repack) BEFORE
  the semi-blend. Output is still R16_UINT 555 (5-bit) — quality ceiling until the dedicated-RGBA8-FB
  refactor (handoff). Native fog = mix(color, tint, depth ramp).
- **Config (gpu_native.c, env one-shot):** PSXPORT_LIGHT=0/1/2, PSXPORT_LIGHT_DIR="x,y,z",
  PSXPORT_LIGHT_AMB / PSXPORT_LIGHT_DIFF, PSXPORT_FOG=1 + FOG_NEAR/FAR/RGB. Default OFF (byte-identical to
  later-96 unless enabled). `gpu_gpu_set_light()` pushes the LPC each tritex batch.
- **VALIDATED:** normal-viz (PSXPORT_LIGHT=2, scratch/screenshots/normviz_1500.png) shows the 3D world
  cleanly colored by face orientation — vertical tower uniformly green, ground red, catapult/barrel faceted
  = reconstruction is CORRECT. Directional light (=1) shades the world (tower left-dark/right-bright, grass
  gradient), 2D HUD + water untouched; RMSE vs unlit 0.04. Validation-layer clean. By-eye (no oracle — this
  is a new PC-native feature Beetle lacks). OPEN/NEXT: SSAO (needs depth attachment + dedicated FB), world-
  space light (transform dir by the GTE rotation matrix CR0-4 so the sun is camera-stable), RGBA8 FB to lift
  the 5-bit banding, smooth (per-vertex) normals if flat shading looks too faceted.

## 2026-06-17 (later-98) — DIRECTION RESET: no GP0-stream tricks; RE the engine, port it native.
User rejected the renderer-side tricks I'd been adding (supersampling, PGXP value-keyed vertex smoothing,
lighting read out of the emulated GTE): "I don't want super sampling or other tricks, I want you to reverse
engineer and port the game engine to PC native so we can tweak those natively." Then: build tooling that
runs BOTH cores side-by-side synced on a GAME STATE (attract/demo start), not frame numbers — but RE the
game more FIRST. Memories: [[pc-native-not-emulator-hacks]] (updated), [[dual-core-state-synced-diff]] (new).
- **De-tricked the defaults:** PSXPORT_SS=1 (native res), PSXPORT_PGXP=0 (the value-keyed smoothing was the
  likely water-grid breaker). Trick code kept only as opt-in env flags pending the native port. (uncommitted)
- **RE win — projection setup found (docs/engine_re.md):** `gen_func_800509B4` (0x800509B4) does InitGeom +
  `SetGeomOffset(160,120)` + `SetGeomScreen(350)` → screen center (160,120), focal length H=350, 320×240.
  This is the **native widescreen lever**: override to OFX=214 + widen draw-env/clip to 428, keep OFY/H →
  the GTE projects a genuinely wider FOV, no squish, no renderer re-center. (Found by histogramming
  gte_write_ctrl reg targets in generated/: OFX/OFY/H written at exactly 2 sites.)
- **Water:** established (from the lighting normal-viz) that water is NOT GTE-projected — it's a separate
  screen-space layer (terrain gets normals, water doesn't). The user reports it rendering wrong (green
  smear). NOT yet root-caused. NEXT RE: provenance (`PSXPORT_PROVAT`) on a water pixel → owning node/handler
  → read in decomp; compare vs oracle at a synced game state (dual-core harness).
- **Dual-core harness BUILT (`tools/dualcore.py`):** runs port + oracle together, each on its own FIFO,
  gates both to a guest-RAM **state latch** (not frame number). Found the right latch: **0x800BE258==2**
  = scene/field active (port in-demo=2, oracle title=0); the stage word 0x801fe00c is too coarse
  (0x801062E4 = attract = title AND demo both; 0x8010649C = START/logo). `sync`/`step`/`shot` (side-by-side
  + diff heatmap). **Limitation found:** the latch catches the scene-LOAD edge (oracle shot can be black)
  and the two cores' attract demos are NOT frame-locked — equal `step` drifts (port demo cycles back to
  title while oracle still mid-scene). NEXT: a `loadram` port REPL cmd to load an identical 2MB guest-RAM
  water-scene snapshot into BOTH cores (oracle `-loadstate`), so both rebuild the same frame from RAM →
  drift-free render compare. THEN root-cause the water. (docs/diff-driver.md updated.)

## 2026-06-17 (later-99) — Graphics OWNERSHIP: scene classifier + first engine fns ported native (0-diff).
Direction: stop black-boxing the graphics; reimplement the engine's draw path in native C. Done + verified:
- **Scene classifier** (PSXPORT_SCENEDUMP, gpu_native.c): native read-only OT walk classifying every prim
  (poly/rect/fill/VRAM-copy/env). The port now ACCOUNTS for each draw. Finding: 2 DrawOTag/frame (clear +
  main), water = textured GEOMETRY (no reflection copy) → broken water was the PGXP trick (now off).
- **GTE projection ported native** (game_tomba2.c ov_set_geom_offset/ov_set_geom_screen, 0x800846D0/F0):
  writes CR24/25/26 directly; OFX=160/OFY=120/H=350. **0-pixel-diff** vs recomp (PSXPORT_GEOM_RECOMP=1).
- **DrawOTag ported native** (ov_draw_otag, 0x80081560 → our gpu_dma2_linked_list): the per-frame draw
  submission routes through our native OT walk. **0-pixel-diff** vs recomp (PSXPORT_OT_RECOMP=1) at f1500.
- **libgpu fn labels corrected** from debug strings: FUN_80080f6c=DrawSync (NOT DrawOTag), 80081560=DrawOTag,
  800815d0=PutDrawEnv, 8008179c=PutDispEnv. engine_re.md fixed.
- **Fade flash root-caused**: the engine's prologue fade-in is a SMOOTH modulation-color ramp (frame-stepped,
  no overlay) — so the flash is RENDERER-side (VK present / FMV handoff), not the engine.
- **Widescreen (native) attempt — REVERTED, blocked:** moved the horizontal shift into the engine projection
  (OFX 160→214, WIDE_OFF→0) — math-equivalent to the proven later-93 re-center, and OFX is correct
  (CR24=0x00D60000, 428x240). BUT the VK wide-FB shows **regular vertical black bars**, and the PROVEN
  later-93 config (WIDE_OFF=54, OFX=160) shows them TOO → a **pre-existing VK wide-FB regression** (capture
  or render), NOT my OFX change. Readback buffer is correctly sized (VRAM_W*IMG_H), so not a shot-size bug;
  cause not yet found (suspect a change between later-93 and now). Reverted the wide experiment to keep the
  tree clean; widescreen is off by default (./run.sh = 4:3, unaffected). NEXT: isolate the wide-FB bars
  (bisect gpu_gpu since later-93 / compare live-window vs VK_SHOT), then re-land native widescreen on OFX.
- **later-99b — wide bars ROOT-CAUSED + tricks removed:** bisected — wide is CLEAN at SS=2, BARS at SS=1.
  The vertical bars are a wide-FB rasterization gap at native 1x (the 320→428 +54-shifted geometry leaves
  periodic 1px column gaps that SS=2 covers); NOT my OFX/DrawOTag change (bars persist with both reverted).
  My SS default 2→1 exposed it. Action: **reset the renderer (gpu_gpu.c/gpu_native.c/shaders) to the clean
  pre-session state (e6de790), removing the user-rejected PGXP + lighting tricks entirely**; re-applied only
  SS=1 default + the scene classifier (RE tool). Native projection + DrawOTag overrides (game_tomba2) kept,
  re-verified **0-pixel-diff** vs recomp on the default path. Default ./run.sh = 4:3 native, clean, no tricks.
  OPEN: widescreen still needs the 1x wide-FB gap fixed (gap-free without SS) before re-enabling — the gap
  mechanism (1px columns at 1:1 raster of the +54-shifted geometry) is the next renderer target.
  (gte_beetle still defines PGXP_pushSXYZ2f — required by Beetle's gte.c — but its cache is now unused/dead.)

## later-101 — DECISION: consolidate to ONE execution substrate (interpreter-only runtime)
**Context:** chasing task #6 (full field native depth) I instrumented the interpreter's GTE-op path
(prologue back-scan attribution of *interpreted* RTPT/RTPS — reliable, unlike the prior session's
"windowed jal-decode" RTPCALLER histogram, which was a red herring). Ground truth: the field's
missing-depth world polys come from **interpreted OVERLAY submitters**, dominantly:
- `0x8013FB88` GT3 (RTPT=187838 over 600f, ~313/f) — SAME record layout + algorithm as the resident
  GT3 library `submit_poly_gt3`, only the color mask differs (0x00F0F0F0 vs 0xFFF0F0F0 — padding byte,
  VRAM-identical).
- `0x8013FE58` GT4 (RTPT=258542 RTPS=164519, ~431/f) — the GT4 sibling.
- `0x801464C0`/`0x8013DD34`/`0x80109C80`… smaller.

**The bug that motivated the pivot:** the scan-on-load DOES register native overrides for `0x8013FB88`
/`0x8013FE58` (`[submit] own overlay GT3/GT4 @ …`), but they **never fire** — the body runs
interpreted anyway (proven: the GTE probe counts them). Root cause = the recomp/interp SPLIT: two
override tables (`g_override[]` by recomp-fn-INDEX for resident MAIN, `g_iov[]` by raw ADDRESS for
overlays) and two interpreters (`rec_interp` flat + `rec_coro_run` coroutine). The override only fires
on call paths that consult the right table; the overlay submitter is entered via a path that doesn't.
Perverse allocation: the SLOW interpreter runs the HOTTEST code (the render submit firehose, which lives
in overlays), while the recompiler runs resident bookkeeping.

**Decision (user):** consolidate to a SINGLE substrate = **interpreter-only runtime**. Drop the
recompiler from the runtime. Rationale: overlays are first-class in Tomba2 (the render path itself is
overlay code) and the interpreter handles them for free; recompiler-only would mean *building*
N64Recomp-style overlay relocation/dispatch. Interpreter-only deletes the most complexity (emit.py from
the build, generated/, shard build, rec_dispatch, the index-vs-address override duality — the very seam
that caused this bug). The real oracle stays Beetle (`wide60rt`). **Keep emit.py + generated C as a
SEPARATE OFFLINE analysis tool** (Ghidra-like pseudo-C; it's exactly how the byte-packed variants were
RE'd this session) — just not in the runtime.

**Speed de-risked (the one real risk):** measured ~140.5M interpreted instructions / 600 field frames
(~234K interp-inst/f, the overlay render firehose) at ~100–150M inst/sec. A worst-case FULLY-interpreted
frame (~400–560K PSX inst total) ≈ 3–5ms — well inside the 16.7ms/60fps budget, and the firehose that
dominates today's interp load is precisely what becomes native C. MAIN.EXE's raw bytes are ALREADY in
g_ram (`boot.c load_exe` memcpy's text), so the interpreter can run MAIN directly — feasible.

**Bonus:** interpreter-only is the proper root-cause fix for task #6 — with one address-keyed override
table the scan-registered overlay GT3/GT4 overrides will actually fire and record depth, so the
252+136/f misses should largely vanish for free.

**Verified milestone kept (substrate-independent):** native byte-packed POLY_GT4 submit (`0x80027768`,
`engine_submit.c ov_submit_poly_gt4_bp`), **0 u16 VRAM diff** vs `PSXPORT_SUBMIT_RECOMP=1` at field f560.
ABI/record fully RE'd (a1=CLUT-Y<<22, a2=OT-Z bias s16, a3=U offset; no count, loop while ctl>0; OT base
*0x800ED8C8; DPCT/DPCS depth-cue colors). Note: barely used in the field (~0.8 prim/f) — the field GT4s
are the overlay variant above.

**Migration scope (counted):** 23 `gen_func_` super-calls in runtime+engine → an interp super-call;
14 `func_8…` direct refs; 49 override registrations (already address-based). Plan: unify override tables
(one address-keyed), hand-written `rec_dispatch` = override-or-interp-from-RAM, `rec_super_call(c,addr)` =
interpret original bytes, stop linking generated/shard_*.c, route MAIN entry through interp, then unify
the two interpreters. Verify boot→field→runs + frame rate + depth coverage jump.

## later-102 — DONE: interpreter-only runtime LANDED + full field depth coverage (task #6)
Executed the later-101 decision. The runtime no longer links the recompiler: MAIN.EXE and the boot
stub both run from g_ram via the interpreter. New `runtime/recomp/dispatch.c` replaces the generated
dispatch infra (rec_dispatch / rec_set_override / rec_func_index / stub_dispatch / stub_set_override +
`rec_super_call` = interpret the original body for A/B oracle). One address-keyed override table
(rec_set_override now routes to the same g_iov as rec_set_interp_override). build_port.sh + run.sh drop
generated/shard_*.c + stub_shard_*.c; emit.py is analysis-only (run.sh gates it behind PSXPORT_RECOMP=1).
~9 `gen_func_XXXX(c)` super-calls → `rec_super_call(c,0xADDR)`; MAIN entry `func_800896E0(c)` →
`rec_dispatch(c,0x800896E0)`.

**Verified:**
- Boots stub→MAIN→field, runs the native frame loop, clean exit. ZERO bad opcodes.
- **0 u16 VRAM diff** at field f560: interp-only MAIN == the old recompiled build (bit-identical), and
  native overlay-owned == fully-interpreted (PSXPORT_SUBMIT_RECOMP=1).
- **Speed: ~1.45s / 565 headless frames (~390 fps)** — same as the recompiled build; the speed risk is
  dead. (The earlier 5-min "spin" was the BIOS bug below, not slowness.)

**Three real bugs fixed on the way (all latent seam bugs of the old split):**
1. `rec_dispatch` must route BIOS vectors (`li $t2,0xA0; jr $t2`) to `rec_dispatch_miss` (HLE), NOT
   interpret code at 0xA0. (The old generated rec_dispatch did this via its `default:`.) This was the
   boot crash into string data.
2. `rec_interp` only honored overrides on `jal`/`jalr`, NOT on plain `j` / computed `jr` TAIL-CALLS —
   so a tail-called native override (submitters are tail-called) was bypassed and the original body
   interpreted. Now both interpreters check `coro_native_call` on every transfer.
3. `rec_overlay_loaded` flushed ALL auto (scan) overrides on EVERY overlay load — so a later data/asset
   overlay silently wiped the GAME code-overlay's submitter overrides, leaving the field's dominant
   submitters un-owned. THIS is why the prior session saw "owning overlays changed depth by ZERO."
   Fixed: `iov_flush_auto_range(base,size)` drops only overrides INSIDE the just-loaded region.

**Result — task #6 SOLVED:** with the overlay submitters (`0x8013FB88` GT3, `0x8013FE58` GT4, …) now
actually firing as native with depth, field depth coverage jumped **records made 634→1807, misses
428→34** (~30% → ~98% real per-vertex depth; the residual 34 are genuine 2D UI). Faithful 0-diff holds.

NEXT: (a) optional — unify the two interpreter loops (rec_interp recursive + rec_coro_run flat) into
one (they share exec_simple; this is the remaining "two things"); (b) build the SBS depth pic for the
user to judge visuals; (c) overlay-banner depth semantics (design call). Recompiler now lives only as
the offline Ghidra-like analysis aid (generated/, PSXPORT_RECOMP=1 to regen).

## later-103 — DONE: ONE interpreter loop (flat) + run.sh fix
Executed handoff task #1 (the user's core remaining ask: "one interpreter"). `interp.c` had TWO
control-flow loops sharing `exec_simple`: `rec_interp` (RECURSIVE — mirrors PSX calls on the C stack,
`jr ra` = C return) for synchronous nested calls / `rec_super_call`, and `rec_coro_run` (FLAT/resumable
— keeps the PSX stack in g_ram, exits at CORO_SENTINEL) for cooperative tasks. Collapsed them into a
single flat core `interp_flat(c, pc, stop_ra)`; the two public entry points are now thin wrappers that
differ only in the return sentinel:
- `rec_coro_run(c,pc)` = `interp_flat(c, pc, CORO_SENTINEL)` (task; scheduler enters its top fn with
  ra=CORO_SENTINEL).
- `rec_interp(c,pc)` (synchronous): save ra, set ra=CORO_SENTINEL, `interp_flat(c,pc,CORO_SENTINEL)`,
  restore ra. The target's own prologue/epilogue saves+restores whatever ra holds, so the net effect
  matches the old recursive rec_interp exactly; nesting is safe (each invocation's PSX frames sit above
  the caller's, so only the target's own `jr ra` hits the sentinel). Removed `call_addr`, `is_recompiled`,
  `override_for` (the index-vs-address override duality is fully gone — coro_native_call does ONE
  address-keyed lookup). `trace_call` (PSXPORT_INTERP_TRACE) re-wired into interp_flat's jal/jalr sites.

**Two real bugs found+fixed turning the recursive loop flat (both latent gaps the C-stack model hid):**
1. **crt0 terminal halt.** crt0 (0x800896E0) is `…; jal main(0x80050B08); break 0x1`. On real PSX main
   never returns; our native main (ov_game_main) returns after N headless frames, so control reaches the
   `break` at 0x80089784, falls into FUN_80089788 which saves/restores ra=0x80089784 and `jr ra` LOOPS
   back to the break → 27M-line `[break] code 1` spin. The old recursive rec_interp escaped this by chance
   (returned to C on the next `jr ra`). Root-cause fix (NOT an address special-case): a MIPS `break` is a
   program trap and we HLE the BIOS — there is no handler to resume into, so `break` ENDS the run. Safe:
   the field run executes exactly ONE break (this terminal), never on a hot path.
2. **tail-call into an override at the sentinel level** (a top-level body that `j`/computed-`jr`s into an
   override sets `pc = c->r[31]` = sentinel): added a top-of-loop `if (pc == stop_ra) return;` so reaching
   the sentinel by ANY path ends the run (sentinel 0xDEAD0000 is poison, never real code → no-op for
   normal flow).

**Verified (unified build):** boots stub→MAIN→field, clean exit, ZERO bad opcodes, ONE break (terminal),
clean shutdown ("returned from crt0" → "native_stub_run returned"). Depth coverage IDENTICAL 1807/34.
**0 u16 VRAM diff** at field f540 on THREE axes: (a) unified-flat == committed-recursive build (proves
the refactor changed nothing), (b) native-submit == PSXPORT_SUBMIT_RECOMP=1 fully-interpreted (faithful
gate holds on the unified interp). Same ~390 headless fps.

**Also fixed run.sh (was outdated/broken):** `[ -n "$PSXPORT_RECOMP" ]` under `set -eu` aborted EVERY
normal run with "unbound variable" (the var is unset unless you ask for the analysis recompile) → now
`${PSXPORT_RECOMP:-}`. Dropped the dead `-Igenerated` include (no linked TU includes generated/* since
the interpreter-only pivot) from run.sh + build_port.sh, and refreshed the stale "recompiler input" /
"compiles the recompiled core" comments. run.sh now builds+launches+exits cleanly end-to-end.

NEXT (handoff remainder): #2 SBS visual verify (PSXPORT_SBS shotseq not writing — debug gpu_gpu.c
gpu_gpu_shotseq), #3 overlay-banner depth semantics (ASK user), #4 optional generated/ include trim
(done for the build scripts; rec_decls.h is no longer included by any linked TU).

## later-104 — DONE: native-depth occlusion fixed (3-band depth model) + SBS visual verify
Handoff #2 (visual verify) + #3 (depth semantics, user steer: "more ownership"). The SBS A/B dump
(now working — fixed a silent `fopen` failure in gpu_gpu.c gpu_gpu_dump: it never `mkdir`'d the
PSXPORT_VK_SHOTSEQ dir) revealed that **task #6's "98% native-depth coverage" did NOT yield correct
occlusion**: with PSXPORT_NATIVE_DEPTH=1 the **entire foreground (terrain/hut/trees/Tomba) was occluded
by the water+sky background** — only the GTE-projected 3D objects (fruit, bird) survived. Reproduced in
the real single-channel NATIVE_DEPTH (not just an SBS artifact).

**Root cause:** the native-depth D32 buffer had only TWO bands — 3D world [0, 0.9375] (real per-vertex
depth) and a 2D OVERLAY band (0.9375, 1] for HUD. But Tomba2's **water and sky are screen-space 2D
layers with NO GTE projection** (engine_re.md "Water/reflection"), so every backdrop prim was `is3d=0`
and got dumped into the NEAR overlay band (nearest, wins) → it covered the whole 3D world. The overlay
band is right for HUD (composite OVER the world) but wrong for backdrops (must sit BEHIND it).

**Fix — 3-band depth model** (gpu_gpu.c + gpu_native.c tee): split the non-3D prims into
- **2D BACKGROUND band [0, NATIVE_3D_MIN=0.0625)** — backdrops (water/sky), FAR, behind the world,
- **3D WORLD band [0.0625, 0.9375]** — real per-vertex depth (proj_pz_to_ord remapped via `ord3d()`),
- **2D OVERLAY band (0.9375, 1]** — HUD/UI/banners, NEAR, over the world (unchanged).

Background vs HUD is split by **OT submission order**: a 2D prim drawn BEFORE any 3D prim this frame is
a backdrop (`gpu_gpu_set_order_2d_bg{,_n}` → far band); AFTER, it's HUD (overlay band). Tracked by a
per-frame `s_seen3d` flag (reset with s_prim_order). This matches the painter/OT semantics (backdrops
are submitted first, HUD last) and is what "owning more" of the depth means — the backdrop now gets a
deliberate native far depth instead of an accidental near one.

**Verified:** PSXPORT_NATIVE_DEPTH=1 now renders the full field correctly (water behind terrain, HUD
banner "Go to the Burning House!" on top); SBS A/B panels match across field frames 500/550/600. The
only intended difference is true-3D depth on objects (floating fruit now correctly occludes/shows vs
foliage by real Z, not OT order). **Faithful gate intact: 0 u16 VRAM diff** at f540 (the depth bands
only touch the VK render path, never the software rasterizer / s_vram).

Residual / next: the OT-order backdrop-vs-HUD split is a heuristic (a backdrop drawn AFTER a 3D prim, or
a HUD drawn before one, would be misbanded) — fine for the static field; revisit if a scene interleaves
them. Foreground objects vs tree-canopy foliage now occlude by true Z (a look change from the OT
original) — the banner-style "OT-on-top intent" question (handoff #3) is now concrete and may want a
per-layer call.

## later-105 — 60fps + widescreen SEPARATED (rename wide60 → fps60) + feature readiness
User direction: finish 60fps, but first SEPARATE the widescreen and 60fps code (they were conflated
under the "wide60" name even though widescreen lives in gpu_gpu.c PSXPORT_WIDE/IRES and 60fps lives in
engine/wide60.c). The two were already decoupled in code (neither references the other); the conflation
was purely the name. Renamed the PORT's 60fps feature wide60 → fps60: `engine/wide60.c`→`engine/fps60.c`,
`wide60_*`→`fps60_*`, `gpu_w60_*`→`gpu_fps60_*`, `g_wide60_on`→`g_fps60_on`, `PSXPORT_WIDE60[_GATE/_SYNTH/
_SDBG]`→`PSXPORT_FPS60_*`, debug channel `wide60`→`fps60`. LEFT ALONE: the Beetle ORACLE's own
`runtime/wide60.{cpp,h}` + the `wide60rt` binary (separate reference emulator), and provenance comments
in hle.c/native_boot.c that cite the oracle's HLE. Verified pure-rename: full build OK, **0 u16 VRAM
diff** at f540 (faithful gate), 60fps measures identically under PSXPORT_FPS60.

Feature readiness audit (asked "ready for hi-res/widescreen/60fps/lighting/AO mods?"):
- **Higher internal resolution** (PSXPORT_IRES=N, gpu_gpu.c) — WORKS (verified: genuine denser
  rasterization, crisp 3D edges; up to 3x 4:3 / 2x 16:9, VRAM_W-capped).
- **Widescreen** (PSXPORT_WIDE) — WORKS (verified: true wider FOV, more world on the sides, no stretch;
  HUD edge-anchoring). Native depth makes both render correctly now.
- **60fps** (PSXPORT_FPS60) — full system built (capture/match/synth/frame-behind present) but **0%
  interpolating**: measured live `rtp_with_obj=0 tagged=0`, all 927 field prims SNAP → presents the real
  frame = 30fps. Blocker: the field's render-time RTPS has no object context. Unblock paths: (a) own the
  field per-object render dispatch to tag draws, or (b) switch the synth to the already-built GTE-
  transform SXY remap (xobj/wide60_build_remap, bypasses the tag) — note build_remap/xobj_match are NOT
  currently called in fps60_frame_commit (dormant). Needs a MOTION scene to validate (idle field has
  nothing to interpolate).
- **Better lighting** — was built (9d81ff8) then REMOVED (8e959e9 "remove rejected renderer tricks");
  current tritex.frag has no lighting/normals/fog. Needs fresh approach.
- **Ambient occlusion** — not started; UNBLOCKED by the new 3-band depth (D32 s_depth attachment exists);
  needs a new SSAO pass + normals (reconstructable from depth) + composite into the 1555 uint VRAM.

## later-106 — 60fps foundation VALIDATED: GTE-transform object matching is 100% on the field
Activated the dormant GTE-transform object matcher in fps60_frame_commit (xobj_match + xobj_report +
xobj_commit — were defined but never called, so s_xA stayed empty). This is the interpolation path that
does NOT need the per-poly object tag (the ocen/Prim.obj path is blocked at rtp_with_obj=0). Each run of
RTPS/RTPT sharing one GTE transform (CR0-7 = model-view, camera baked in) = one object; cross-frame
identity = its local-vertex fingerprint.

**Measured (idle field, PSXPORT_FPS60=1 PSXPORT_DEBUG=fps60):** objects=126/frame, **matched=126
(100.0%)** every frame, TRdelta avg=369 max=3220 (GTE fixed units) — i.e. there IS real per-object motion
even "idle" (ambient/water/idle-anim) for interpolation to smooth. So the object+camera identity
foundation the user asked for ("interpolate game objects and camera between frames") is SOLID and proven
on real gameplay. Cheap (126×126 ≈ 16k cmp/frame; only when PSXPORT_FPS60=1, default off → faithful path
untouched).

**Remaining to finish 60fps = the SYNTHESIS.** Two prior synth attempts: (a) ocen per-object 2D centroid
translation — blocked (Prim.obj=0, all snap); (b) build_remap SXY reprojection re-rasterized into the
separate s_interp buffer — looked "terrible/smeared" (later-86) because re-rasterizing the captured 2D
GP0 SUBSET is lossy (missing occluders/fills → hidden geo reappears, no depth). The RIGHT approach now
that we own the native renderer + 3-band native depth: for the in-between frame, interpolate each matched
object's transform and RE-SUBMIT the display list through the NATIVE VK renderer (gpu_gpu_draw_* with the
D32 depth), so occlusion is correct — no lossy s_interp re-rasterize. Present frame-behind: A, lerp(A,B),
B… NEXT: wire build_remap's per-vertex SXY remap into a native-renderer re-submit + a motion-scene visual
check (idle field has motion but driving Tomba/camera pan shows it best).

## later-107 — 60fps: restored the SEPARATE-LAYER synth (user architecture: layer on top, NO resubmit)
User correction: "interp60 should be a separate layer that lives on top. no resubmit." (Matches
[[wide60-60fps-architecture]] + the later-83 design — I had wrongly proposed re-submitting through the
native renderer.) The current code had REGRESSED from the proven later-83 build_remap path to the
ocen per-object centroid path, which is blocked (Prim.obj=0 → all snap → 30fps). Restored the correct
design in fps60_synthesize:
- `fps60_build_remap()` now runs in frame_commit (after xobj_match, before xobj_commit): interpolate
  each 100%-matched object's GTE transform to the midpoint, reproject its verts through the REAL Beetle
  GTE → an old-SXY → interp-SXY table.
- synthesize() ALL-OR-NOTHING per prim (later-83): a poly remaps only if EVERY vertex resolves in the
  table (whole prim = one interpolated object); sprites/lines + partial/2D prims SNAP. Re-rasterizes into
  the SEPARATE s_interp buffer ON TOP (VRAM untouched) — not a resubmit. Removed the dead ocen machinery.

**Result:** interpolation ACTIVATES — field synth f600 = 927 prims, **528 remapped / 399 snapped** (was
0/927). Object match 100%, gated by PSXPORT_FPS60 (faithful path 0 u16 VRAM diff at f540, verified).

**Two open problems found (the real remaining work):**
1. **Re-raster fidelity.** On a STATIC field frame (A==B, mean|A−B|=0) the synthesized in-between still
   differs from the real frame by **mean ~8.24/255 (~3%)** — the separate-layer re-rasterizer does NOT
   faithfully reproduce the frame (missing non-captured GP0 ops: background fills, VRAM-copy/water
   reflection, semi-transparency order). So with motion OR not, the in-between shimmers vs the real
   frames. THIS is the blocker for the separate-layer approach: the re-raster must reproduce the full
   frame (capture+replay ALL draw ops, correct blend/occlusion), or the present must show the in-between
   only on real motion. NEXT.
2. **Headless motion validation is hard.** The idle field is fully static (A==B) and the new
   `PSXPORT_AUTO_WALK=l/r/u/d` (native_boot.c — holds a D-pad dir after field-reached, a deterministic
   motion scene) did NOT visibly move the character (TRdelta a constant 369/frame = ambient anim only;
   A==B persisted). Need to confirm the held input reaches the game / the char can walk here, or pick a
   scene with real camera pan, to validate interpolation visually. Live eyeball (`PSXPORT_FPS60=1
   ./run.sh`) remains the documented check.

## later-108 — STATUS: 60fps PARKED (waiting for user live-test); other mods handed to a fresh session
60fps (PSXPORT_FPS60) is implemented (later-106/107) — separate layer, camera+3D-object interpolation,
100% object match, 528/927 field prims reproject. **Parked waiting for the user to live-test**
(`PSXPORT_FPS60=1 ./run.sh`); headless validation is a dead end (synth dumptest reads byte-identical VRAM
buffers in the field — see later-107). Do NOT keep iterating fps60 until the user reports.
Fresh session continues the OTHER mods (handoff: scratch/handoff.md): (1) ambient occlusion / SSAO —
unblocked by the 3-band native depth (D32 s_depth exists); (2) better lighting — rebuild fresh (the
9d81ff8 lighting was removed in 8e959e9; read that first). Hi-res + widescreen already DONE (later-105).

## later-109 — DONE: PC-native SSAO (PSXPORT_SSAO), curvature model, between opaque & semi
Task #1 from the handoff. Built a screen-space ambient-occlusion post pass over the VK renderer, gated
`PSXPORT_SSAO` (implies NATIVE_DEPTH; OR'd into the native-depth gates in gpu_native.c + gte_beetle
attach_enabled; disabled under SBS). New: `shaders_vk/ssao.frag` (+present.vert reused as its vertex
stage), gpu_gpu.c `create_ssao`/`ssao_pass` (own R16_UINT target s_ssao_img, color-only rpass ending in
TRANSFER_SRC, 2-binding descriptor color+depth, pipeline), s_depth gets SAMPLED_BIT, gte_beetle
`proj_near_pz()` getter. AO'd color → s_ssao_img → copied back into s_tex (present/dump pick it up free).
Depth linearize: stored depth = ord3d(proj_pz_to_ord(pz)), affine in 1/pz → undo band remap + affine
to view-Z; only the 3D band [MIN,MAX] is touched (sky/backdrop/HUD pass through).

**Two real findings (not tuning), each fixed at the root:**
1. **Naive "is the neighbour closer" AO washed the whole TILTED ground** (21% of pixels darkened, a
   uniform smear on the grass). Root cause: a tilted plane always has a "closer" downhill neighbour →
   false occlusion. Fix = **curvature AO via opposite-neighbour pairs**: a flat/tilted plane has
   center == average(opposite neighbours) → 0 AO; only genuine concavities darken. Dropped to 2.7%,
   concentrated on creases/foliage/contacts.
2. **AO darkened the 2D menu UI.** Root cause: the menu's blue fill is SEMI-transparent → writes no
   depth → the depth buffer under it is the 3D terrain, so a post-everything AO pass saw "3D" there.
   Fix = run SSAO **between the opaque and semi passes** (AO belongs to opaque geometry; translucent
   UI/water composites OVER it). The menu now correctly shows the AO'd terrain *through* its glass and
   is not itself darkened.

**Verification (single deterministic run — cross-run pixel A/B is unreliable; AUTO_GAMEPLAY scene
state drifts between processes, the same headless dead-end as fps60).** Added `PSXPORT_SSAO_VIZ=1`
(AO factor as grayscale on 3D pixels, original color on 2D/sky) → confirmed on the field: UI menu +
sky EXCLUDED (shown in color), 3D world AO-eligible, flat ground NOT washed, AO lands on hut/foliage/
object creases + contacts. Vulkan validation layers: ZERO errors from the new rpass/barriers/depth-
sample/copy (only the pre-existing headless present-rpass PRESENT_SRC_KHR VUID remains). Faithful gate:
SSAO only touches the VK path, never s_vram; default off → SW/oracle path byte-identical.
Tunables (env, logged once): SSAO_STRENGTH=1.0, _RADIUS=5px×IRES, _BIAS=0.01, _RANGE=0.15. Next: user
live-tune the look; then task #2 (better lighting, rebuild fresh).

## later-110 — DONE: PC-native directional lighting (PSXPORT_LIGHT), deferred normals from depth
Task #2. User steer on the prior (rejected) lighting: "I don't remember but if it was rejected either
the agent did it or it wasn't PC native like it was hacky." So: do it the PROPER PC-native way, not the
old PGXP per-face-normal hack (value-keyed cache, entangled in the forward tritex pass). Built a
DEFERRED directional light that **reconstructs real geometric normals from the depth buffer** —
shares the SSAO deferred pass (ssao.frag + gpu_gpu.c ssao_pass, now AO+light), gated PSXPORT_LIGHT
(implies NATIVE_DEPTH via the same gates; disabled under SBS). No GTE/PGXP coupling.

How: per depth pixel reconstruct view pos P = ((sx-cx)*pz/H, (sy-cy)*pz/H, pz) — cx,cy,H from new
gte_beetle getters proj_screen_center()/proj_plane_h() (H = CR26, set each frame by engine_submit
proj_set_H; center = OFX/OFY, standard 160/120). VRAM→screen map handles faithful AND wide/hi-res-FB
(inverse of tritex.vert relocation: origin/inv_scale/wide_off). Normal = normalize(cross(dPdx,dPdy))
with a closer-neighbour pick (don't bleed a normal across a silhouette), oriented to face the camera
(view -Z). Shade = ambient + diffuse·max(0,N·L), applied to the baked color as albedo. Light dir =
to-light vector in view space.

Verified (single deterministic run; cross-run pixel A/B unreliable as for SSAO): PSXPORT_SSAO_VIZ=2
shows the reconstructed normals = COHERENT per-surface (flat ground uniform, hut faces distinct, sky/UI
excluded) — proves H/cx/cy correct (a wrong H would collapse all normals to camera-facing). VIZ=3 = lit
factor. Lit screenshots: terrain/foliage gain real directional FORM, conservative by default (amb .65 /
diff .5 → subtle, doesn't crush baked art); composes with SSAO; UI/sky untouched. Vulkan validation
ZERO new VUIDs (only the pre-existing headless present-rpass PRESENT_SRC). Faithful gate: only VK path,
default off. Tunables: PSXPORT_LIGHT_DIR="x,y,z"/_AMBIENT/_DIFFUSE. Both PC-native mods (SSAO+LIGHT)
now done; user to live-tune the look. Hi-res/widescreen/SSAO/lighting all landed; 60fps still parked.

## later-111 — DONE: Dear ImGui mod-toggle overlay (PSXPORT_UI) + live mod state
User: "add imgui and a toggle for everything (wide/60/lighting/ao) then we'll RE the game's own options
menu and move them there." Built the interim overlay; the game-options-menu RE is the NEXT step (move the
toggles there later — this overlay is the stopgap UI).

- **Vendored Dear ImGui** stable v1.91.9b into `vendor/imgui/` (core + SDL2 + Vulkan backends; MIT).
- **Live mod state** `runtime/recomp/mods.{h,c}` — `g_mods` (ui/wide/ires/ssao/light + ssao & light
  params), seeded once from cfg by mods_init(), then mutated LIVE by the overlay. gpu_gpu.c now reads
  g_mods EVERY frame (s_wide/s_ires became accessor macros over g_mods; ssao_on/light_on read g_mods;
  ssao_pass params read g_mods live) so a toggle/slider takes effect immediately. 60fps = the existing
  extern int g_fps60_on, flipped directly by the overlay.
- **Overlay** `runtime/recomp/imgui_overlay.{h,cpp}` (C++; a C bridge header). Inits ImGui on the port's
  EXISTING VK device + present render pass (no second device), draws into the swapchain inside the present
  render pass (after the present quad, before EndRenderPass). Toggle visibility with ` or F1. Checkboxes:
  Widescreen, 60fps, SSAO, Directional light; sliders: Internal res (1-3, capped 2 in wide), and the SSAO
  / light params (shown when their toggle is on).
- **Runtime-toggleable SSAO/LIGHT:** PSXPORT_UI forces the native-depth path + deferred-resource creation
  ON (OR'd into the native-depth gates in gpu_native.c/gte_beetle.c, and create_ssao via ui_infra()), so
  SSAO/LIGHT can be flipped on at runtime even if they started off. Disabled under SBS.
- **Build:** the port is otherwise pure C; added C++ support to BOTH run.sh and tools/build_port.sh
  (compile .cpp with $CXX, link with $CXX for libstdc++; -Ivendor/imgui[/backends]). New TUs: mods.c +
  imgui_overlay.cpp + the 6 vendored imgui .cpp.

Verified: builds+links clean; windowed `PSXPORT_UI=1 PSXPORT_GPU_WINDOW=1` brings the overlay up
("[imgui] overlay up"), runs 30s+ with NO Vulkan validation errors / crash, all toggles render (cropped
screenshot confirms). Game render path with UI on is fine (headless UI=1 field frames bright). Faithful:
default off. NOTE the default ImGui font has no em-dash glyph (use ASCII in labels). Run windowed:
`PSXPORT_UI=1 PSXPORT_GPU_WINDOW=1 ./run.sh` (or add PSXPORT_WINDOWED=1).

## later-112 — DONE: replace the game's in-game Options menu with our PC-native (ImGui) menu
User: "The game has an options menu but it doesn't have any options worth keeping. We can replace it
with a much richer menu." → RE'd the in-game pause/Options menu and hooked it to show our overlay.

- **RE (full state machine in `docs/engine_re.md` "In-game pause / Options menu"):** the pause menu is a
  **task in the GAME overlay**; body/dispatcher `0x8010810C` indexes a 12-entry table at `0x801062EC` by
  the page byte `task+0x6B` (task = `*(u32)0x1F800138`). Page 1 = main menu "Options / Load data / Quit
  game" (`FUN_8007eae4`); Cross over "Options" sets page→3. Page 3 (`0x801082C0`) calls **`FUN_8007b45c`**
  = the Options submenu (Messages / Sound / Screen adjust / Controls = `FUN_8007f104` — the options the
  user discarded). Disassembled `FUN_8007b45c` via `tools/recomp/decode.py` (it's outside the decomp
  dump): Triangle→page 2 (close), Circle→page 1 (back), SFX `FUN_80074590`.
- **Hook (`engine/game_tomba2.c` `ov_options_menu`, gated `PSXPORT_UI`):** `rec_set_override(0x8007B45C,…)`
  — while page 3 runs, force our overlay visible (options-mode) instead of drawing the game's options, and
  own the same back-nav: **Circle** → `task+0x6B=1` + cursor reset + SFX `(0x14,0xFFF7)`; **Triangle** →
  `task+0x6B=2` + SFX `(0x11,0)`. **Faithful fallback:** if the overlay isn't inited (headless/window-less)
  it super-calls the real `FUN_8007b45c` so nothing is lost. Added `imgui_overlay_set_visible/_options_mode`
  to the overlay (suppresses `~`/F1 toggle in options-mode; shows "Circle: back  Triangle: close" hint).
- **Verified:** hook is reached — headless `PSXPORT_AUTO_GAMEPLAY=1 PSXPORT_UI=1 PSXPORT_DEBUG=ui` with a
  forced Cross at the auto-appearing pause menu (~f720) logs `[ui] FUN_8007b45c reached`. (This also
  CORRECTS docs/driving-the-game §5: the auto-appearing menu DOES respond to forced Cross — it selects
  "Options".) **Windowed verified:** `PSXPORT_UI=1 PSXPORT_GPU_WINDOW=1 PSXPORT_AUTO_GAMEPLAY=1` +
  forced Cross → the overlay path runs (`[ui] in-game Options -> PC-native overlay`) and the cropped
  screenshot (`scratch/screenshots/options_overlay_crop.png`) shows OUR menu (title + "Circle: back
  Triangle: close" hint + the mod toggles) standing in for the game's options screen. SDL window opens
  top-left (0,0), 960x720 — NOT centered (earlier centered-crop assumption was wrong; the overlay is at
  ~(20,20)).

## later-113 — overlay ON BY DEFAULT (windowed) + aspect modes {4:3,16:9,21:9,Auto} + auto internal-res
User (frustrated): "you keep gating everything behind flags; I type ./run.sh and press F1 but no imgui
shows." Root cause: the overlay init + native-depth/deferred infra were gated on `PSXPORT_UI`, which
`./run.sh` never sets. Fix: **the overlay + its live-toggle infra are now ON BY DEFAULT for any WINDOWED
VK run** — plain `./run.sh` brings the menu up and F1 toggles it; no flag. `PSXPORT_UI=0` opts out.
- Decision lives in `mods_init()` (mods.c): `g_mods.ui = 1` when windowed (`PSXPORT_GPU_WINDOW` && !headless
  && !SW && VK!=0), else 0; `PSXPORT_UI=1/0` forces. The native-depth gates (gpu_native.c ×3 lazy +
  gte_beetle `attach_enabled`) now read **`g_mods.ui`** (not `cfg_on("PSXPORT_UI")`) and call the idempotent
  `mods_init()` first so the value is set before the first GP0/GTE caches it (init-order safe). **Headless
  stays faithful** (ui=0 → no native-depth) so the VK render-diff tooling is unchanged. Options-menu
  override (0x8007B45C) is now always registered (its super-call fallback handles the no-overlay case).
- **Aspect selector** replacing the wide checkbox: `g_mods.aspect` ∈ {4:3,16:9,21:9,Auto}. 4:3=320,
  16:9=428, 21:9=560 native FB width; **Auto = the live window aspect** (`240*win_w/win_h`, even, ≤VRAM_W).
  Present letterboxes to the selected aspect; Auto fills the window. `wide_native_w()`/`WIDE_OFF()` are now
  dynamic (16:9 stays byte-identical: 428, off 54 — no regression to the known-good path).
- **Auto internal-res** `g_mods.ires_auto`: ires ≈ round(window_h/240), clamped so `native_w*ires ≤ 1024`
  (VRAM_W) and ≤3. Overlay shows the computed `(Nx)` + a "Render WxH | window WxH" status line.
- Env: `PSXPORT_ASPECT=4:3|16:9|21:9|auto` (legacy `PSXPORT_WIDE=1`→16:9); `PSXPORT_IRES=N|auto`.
- Verified: plain windowed run (only `PSXPORT_GPU_WINDOW=1`, NO `PSXPORT_UI`) → `[imgui] overlay up` +
  screenshot `scratch/screenshots/default_ui.png` (Aspect 4:3, Render 320x240 | window 960x720). 21:9 +
  auto-ires verified (Render 560x240, ires auto-capped 1x; `scratch/screenshots/aspect_menu.png`).
  KNOWN: pushing the FOV to 21:9 on the seaside scene shows a garbled band where the game submits no
  geometry for the extra-wide sides (a content/FOV limit of ultra-wide, NOT the 16:9 path) — needs a look.

## later-114 — REGRESSION fix (native-depth default) + engine-ownership audit (widescreen is a hack)
User: "the game becomes completely corrupted [widescreen] — most of the engine is still a black box; port
the render/camera layer." Two things:
- **Regression I caused, now fixed (`796740b`):** later-113 routed the native-depth/deferred path through
  `g_mods.ui` and defaulted it ON for windowed → every plain `./run.sh` ran the incomplete native-depth
  model over the whole game = corruption. **A menu being available must never change rendering correctness.**
  Decoupled: overlay stays default-on; native-depth/deferred (gpu_native gates, gte_beetle attach_enabled,
  ui_infra) revert to opt-in via `PSXPORT_UI` env (off on plain `./run.sh`). SSAO/light greyed in the overlay
  unless `PSXPORT_UI=1`. Verified plain windowed run renders the field scene FAITHFULLY (HUD+char+env intact,
  `scratch/screenshots/faithful_scene.png`).
- **Honest correction:** my "21:9 = content limit" was an eyeballed guess (the project bans that). Real root
  cause: **widescreen is a renderer-side HACK** — we render the native 4:3 projection into a wider FB and
  spread verts in a shader, while the engine still CLIPS (PutDrawEnv `0x800815D0`) and CULLS
  (`0x8007712C`) to 4:3 and draws water/sky/HUD as screen-space 2D. So wide → empty/garbled sides. Fix = own
  the view at the SOURCE (OFX via `gen_func_800509B4`, the clip, the frustum, the 2D layers), which our
  already-owned `proj_native_vertex` (0-diff) + per-vertex depth then render correctly.
- **Deliverable (user picked "audit first"): `docs/engine-ownership-audit.md`** — full map of what's owned
  (DrawOTag, projection math+depth, resident submit, asset upload) vs the black box (PutDrawEnv/PutDispEnv,
  projection-config, frustum cull, 2D water/sky/HUD, overlay emitters, VRAM copies, camera basis), why
  widescreen corrupts, and a prioritized, oracle-gated port plan (diagnose → PutDrawEnv/Env → OFX config →
  frustum cull → 2D layers → retire the FB hack). Scope confirmed: gameplay logic STAYS interpreted (the
  Beetle emulator is the oracle). **Terminology correction (user caught it):** the runtime is
  INTERPRETER-ONLY (later-103) — un-owned code runs on the flat interpreter, NOT recompiled; the recompiler
  is an offline analysis aid. Fixed the audit + engine_re.md wording ("recomp MIPS" → "interpreter").

## later-115 — DIRECTIVE: full ownership of the engine layer, no faking (respawn-driven)
User: "the next step is full ownership of the game engine, no faking anything, respawn when you need to."
= execute the WHOLE `docs/engine-ownership-audit.md` plan to completion: own the entire render/camera/submit/
loop layer natively, RETIRE every renderer-side fake (the FB-widescreen hack `push_wide`/`WIDE_OFF`/aspect
scratch FB, the sprite-anchor FB hack), so widescreen/effects come from a genuinely wider engine frame.
Gameplay logic stays interpreted (oracle = Beetle). Handed off to a fresh session via `cci respawn` with
`scratch/handoff.md` to run the audit plan with full context budget. First task there: diagnose the wide
corruption with SCENEDUMP/PROVAT vs the oracle, then own PutDrawEnv/PutDispEnv (0-diff @4:3) → OFX config.

## later-116 — DIAGNOSIS (audit step 1): wide "corruption" is a pure present-time effect, engine output is aspect-invariant
Fresh respawn session (handoff.md). Executed audit step 1 — characterize the wide corruption with the
render tooling, NOT by eye (the prior "21:9 = content limit" eyeball was wrong). Method: drive headless to
the field (`PSXPORT_AUTO_GAMEPLAY=1`, field reached ~native-frame 328) and classify the submitted OT with
`PSXPORT_SCENEDUMP` at the SAME game-state across aspects. (Note: `s_frame` is the *present* counter and
runs ~7× the native-loop frame — at native-frame 420 s_frame≈3006; dumped at s_frame=2900 = field.)
- **Result — byte-identical OT at every aspect.** 4:3, 16:9, 21:9 all submit the same field display list:
  `poly=531 rect=355 line=0 fill=0 vramcopy=1 env=9` (plus the env/clear OT `fill=1 env=6`). The engine
  submits the EXACT same geometry regardless of `PSXPORT_ASPECT`.
- **Conclusion (confirms audit B1/B3/B4, empirically):** the engine produces NO wider content. OFX stays
  160, the clip stays 320, the frustum cull stays 4:3, and water/sky/HUD stay screen-space 320 — nothing in
  the aspect path touches the engine's submission. Today's "widescreen" is entirely a **present-time shader
  effect**: `gpu_gpu.c` `push_wide` takes the fixed 320-wide projection and (per `tritex.vert` wide branch)
  CENTERS it into a wider scratch FB (`local.x + WIDE_OFF`, WIDE_OFF=(428-320)/2=54), then the present
  stretches that FB to the window. So 16:9 = 4:3 content stretched, not a wider FOV. That IS the "fake".
- **Levers verified for the genuine fix:** (1) Beetle's GTE does the real RTPS/RTPT projection using CR24
  (OFX) — `gte_op` runs `GTE_Instruction`; `proj_native_vertex` is only a PROJPROBE verifier, NOT the live
  path. So setting CR24 wider (via the already-owned `ov_set_geom_offset`) genuinely widens the 3D FOV.
  (2) the VK tee passes `s_da_*` (the clip) as `i_da` per-prim, and `gpu_gp0(0xE4…)` sets `s_da` — so the
  clip can be widened engine-side and it propagates to the VK shader. (3) the VK scratch FB (`use_fb`) is
  independent of PSX VRAM's 320-wide buffer layout, so genuine-wide content (X∈[0,428]) can be rasterized
  there 1:1 without VRAM clobber — genuine-wide is inherently a VK feature (SW VRAM stays 4:3 = the oracle).
- **Next (audit step 2/3):** own the projection config (OFX lever) + the clip (PutDrawEnv) engine-side,
  default-OFF behind `PSXPORT_WIDE_ENGINE` so 4:3 stays byte-identical (0-diff gate), then the shader places
  wide content 1:1 (drop WIDE_OFF centering). 2D water/sky/HUD (B4) + frustum cull (B3) remain after.

## later-117 — GENUINE engine-level wide FOV (audit step 3): OFX lever, default-OFF, 4:3 byte-identical
Implemented the first "no faking" widescreen lever: the ENGINE projects a genuinely wider horizontal FOV
(via Beetle's GTE) instead of the present-time FB spread. Gated `PSXPORT_WIDE_ENGINE` (default OFF) so the
faithful 4:3 path is untouched (the 0-diff gate).
- **OFX widen** in the already-owned `ov_set_geom_offset` (engine/game_tomba2.c): the gameplay projection
  center is 160 (=320/2); when `gpu_gpu_wide_engine()` is on, substitute the aspect center (214 @16:9 =
  428/2) so CR24 drives Beetle's RTPS/RTPT to project across the wider screen. Only the gameplay config
  (ofx==160) is widened; InitGeom's reset (ofx==0) is left alone. Verified: headless logs
  `[geom] WIDE_ENGINE OFX 160 -> 214` + `CR24=00D60000` (wide), and the default run keeps `OFX=160`
  `CR24=00A00000` with NO WIDE_ENGINE line — 4:3 path provably untouched.
- **1:1 placement** in `gpu_gpu.c push_wide`: WIDE_OFF (the 320-in-428 re-center) is the SPREAD; in
  wide-engine the projection is already wide, so pass wide_off=0 → content placed 1:1 in the scratch FB.
  For the non-wide-engine path the expression is byte-identical (`s_wide ? WIDE_OFF() : 0`), so the
  existing FB-hack path is unchanged (no regression).
- **Accessors** in gpu_gpu.c: `gpu_gpu_wide_engine()` (PSXPORT_WIDE_ENGINE && aspect!=4:3),
  `gpu_gpu_wide_engine_ofx()` (wide_native_w/2), `gpu_gpu_wide_engine_w()` (wide clip width).
- **Verified (headless VK shot at the field, s_frame=2900, `scratch/screenshots/field_{43,wide169,fbhack169}.png`):**
  - 4:3 (320×224): faithful scene, correct.
  - genuine-wide 16:9 (428×240): the 3D world is genuinely WIDER — terrain+structure on the far left and
    more ocean on the right that are OFF-SCREEN in 4:3 are now visible, with correct (un-stretched)
    proportions. ASYMMETRIC because the screen-space 2D (the "Go to the Burning House!" banner / HUD) is
    still anchored at 320 → it's cut off on the right. That asymmetry is the proof it's a real FOV widen,
    not a stretch.
  - FB-hack 16:9 (428×240, the old default): SYMMETRIC, banner fully centered = the whole 4:3 frame
    uniformly stretched (the "fake").
- **Known remaining artifacts (the next audit steps, NOT regressions):** (1) vertical sky/backdrop stripes
  appear in BOTH 16:9 paths (pre-existing — the non-wide-engine code path is byte-identical to before my
  change, and it has them too) = the screen-space 2D sky/water layer (**B4**) not yet widened; (2) the HUD
  banner pinned/cut at 320 (**B4**); (3) side geometry that the 4:3 frustum culled is still absent on the
  extreme sides (**B3**). None block the 3D-FOV foundation.
- **NEXT:** B4 — own the 2D water/sky/HUD submit and widen/anchor it to the aspect (kill the stripes + the
  cut HUD); then B3 frustum cull; then own PutDrawEnv/PutDispEnv to widen the clip engine-side (step 2,
  currently the VK shader clips to the FB rect so it's not blocking); then retire the FB spread (step 6).
- **Corrects memory [[tomba2-mods-status]]:** "Widescreen DONE/works (true wider FOV, no stretch)" was
  FALSE — that was the FB-hack stretch. Genuine wide is in progress behind PSXPORT_WIDE_ENGINE.

## later-118 — PC-NATIVE per-pixel depth is now the DEFAULT (always-on), not opt-in (user directive)
User (emphatic): "PC game native depth should ALWAYS be active. Genuine wide can't be genuine without
native depth. Everything needs to be PC GAME." Acted on it.
- **Root-cause of the opt-in:** the prior session (796740b) reverted native-depth to opt-in
  (`PSXPORT_UI`/`NATIVE_DEPTH`) because defaulting it on "could corrupt not-yet-owned submit paths." That
  was a PRECAUTIONARY revert (a bandaid), not a pinned bug — exactly the "gate it off instead of fixing the
  cause" trap. Re-checked empirically: native depth renders the field, the boot/prologue cutscene, AND
  genuine-wide 16:9 CORRECTLY (`scratch/screenshots/nd_{def43,boot,wide169}.png`). Field ndepth stats:
  **depth records=1807, lookups hit=1807, miss=34** — the 34 misses are exactly the 2D sprites/HUD that
  bypass the GTE (correctly classified 2D); 3D-vertex attach coverage is effectively 100%. No corruption.
- **Change:** new `native_depth_on()` (gte_beetle.c) = the single gate, **default ON**. Opt OUT only for
  oracle A/B diffing: `PSXPORT_FAITHFUL_DEPTH=1` (or legacy `PSXPORT_NATIVE_DEPTH=0`). Replaced the
  scattered `cfg_on("NATIVE_DEPTH")||SSAO||LIGHT||UI` gates (gpu_native.c ×3, gte_beetle `attach_enabled`)
  with it. The deferred SHADING pass (SSAO/light, `deferred_on()`/`ui_infra()`) stays OPT-IN — only the
  per-pixel DEPTH model is now default. `s_seen3d` (backdrop-vs-HUD 2D classification) is therefore now
  maintained every run, which the genuine-wide 2D work needs.
- **Why it matters for wide:** the OT-order painter's algorithm assumes the authored 4:3 viewpoint; widen
  the FOV and that order mis-occludes. Real per-pixel depth (D32, per-vertex view-Z) is correct at any FOV.

## later-118b — genuine-wide 2D backdrop fills the frame (kills the vertical stripes)
The genuine-wide 16:9 (later-117) showed vertical sky/backdrop STRIPES. Root cause (confirmed, not
eyeballed): the screen-space 2D sprites (sky/water = 355 rects in the field) were spread apart by the
per-sprite `gpu_gpu_sprite_anchor_dx` FB-hack — adjacent backdrop tiles get DIFFERENT anchor shifts →
gaps. Fix (gpu_native.c sprite path, gated `gpu_gpu_wide_engine()`): in genuine-wide, scale the whole 2D
plane uniformly to the wide width about the framebuffer origin (`XL/XR = o + (X-o)*wide_w/320`) so a tiled
backdrop fills the frame contiguously (no gaps). Verified: `scratch/screenshots/field_wide169b.png` — sky
gradient + ocean now fill the 16:9 frame cleanly, stripes gone. STEPPING STONE: this also scales HUD size;
the proper split (backdrop scales to fill, HUD anchors at native size) uses the now-always-on `s_seen3d`
bg/HUD flag — next. The HUD banner cut at 320 also remains (a clip item, B4/step2).

## later-118c — genuine-wide: 2D HUD/overlay POLYS also widen (banner spreads across the frame)
later-118b widened 2D SPRITES; the HUD banner is drawn as screen-space 2D POLYS (not sprites), so it
stayed left-anchored/cut at 320. Now that native depth is always-on, the poly path has the `is3d`
classification every run → a poly with is3d==0 is a screen-space 2D element. Fix (gpu_native.c poly path,
gated `s_ndepth && !is3d && gpu_gpu_wide_engine()`): scale its x to the wide width about the framebuffer
origin, same transform as the 2D sprites. Verified `scratch/screenshots/field_wide169c.png`: the banner
planks now spread across the 16:9 frame (vs left-clustered before). RESIDUAL: the last ~2 planks ("se!")
still drop at the far-right edge (a clip/source-position item, B4 polish — not chased from a still).
Together later-117/118/118b/118c: genuine-wide 16:9 now = real wider FOV + native per-pixel depth +
2D backdrop/HUD scaled to fill, no stripes. Remaining: HUD right-edge polish, frustum cull side-gaps (B3),
own PutDrawEnv clip (step2), then make genuine-wide the DEFAULT wide path + retire the FB spread (step6).

## later-119 — genuine-wide: the missing RIGHT-SIDE TERRAIN (own the submit frustum cull) + 2D-screen pillarbox
User: "there is terrain on the right in the real game — do more RE, port more of the engine." RE'd it.
- **Root cause (engine-level, in code we OWN):** the geometry submit (`engine_submit.c` GT3 `0x8007FDB0`
  / GT4 `0x8008007C`) frustum-culls a prim if ALL its verts have `SX >= 320` (off the 4:3 right edge).
  In genuine-wide the screen extends to 428, so geometry projected into the [320,428) right band is
  ON-screen but the engine's own submit DROPPED it -> ocean where terrain should be. Fix: `submit_xmax()`
  = 428 (wide width) when `gpu_gpu_wide_engine()`, else 320 (faithful). Applied to BOTH GT3 + GT4 culls.
  (The byte-packed GT4 variant `submit_poly_gt4_bp` `0x80027768` has NO SX cull, so it was already wide;
  the overlay-scanned submitters share the GT3/GT4 native impl, so they widen too.) Verified
  `scratch/screenshots/v2_field.png`: the hut/structure + terrain on the right now appear (was ocean).
  OPEN: un-owned recompiled submit variants (`0x8003B320`/`0x8003C8F4` etc., still interpreted) keep their
  own 320 cull — if a scene's side geometry comes from those, port them too (engine_re.md OPEN list).
- **2D-only screens pillarbox (PC-game behavior):** genuine-wide is a GAMEPLAY feature; fullscreen-2D
  screens (SCEA/FMV/title/menu) are authored for 4:3 and the uniform 2D-scale mangled them (later: SCEA
  text scaled+cut, boot cutscene black). Fix: track `s_prev_had3d` (did last frame draw any 3D = a
  gameplay frame; `gpu_had3d_last_frame()`). The wide 2D-scale (sprite + poly paths) now only applies on
  gameplay frames; and the present PILLARBOXES 2D-only frames (sample the 4:3 FB region + letterbox 4:3)
  instead of stretching 320 across the wide frame. Verified: SCEA text back to native size
  (`scratch/screenshots/v2_title.png`); field still genuine-wide. 1-frame transition lag is invisible.

## later-120 — overlay = real PC-game settings: SSAO/light live (no env gate) + persistence
User: "AO and Dynamic light need UI at launch? stop with stupid ENV gating. IMGUI settings don't persist.
Make this a PC game." Fixed:
- **SSAO/light toggle LIVE, no launch flag.** The PSXPORT_UI gate on the deferred infra only existed
  because native depth used to be opt-in — it's ALWAYS ON now (later-118), so the deferred infra is always
  created (`create_ssao` unconditional, skip only under SBS) and the overlay no longer greys out SSAO/light.
- **Settings persist** (`mods_save`/`mods_load`, mods.c) to `psxport_settings.ini` (gitignored; path via
  PSXPORT_SETTINGS). Every overlay change saves; `mods_init` restores on a windowed run AFTER env seeding
  (saved choice wins). Headless/tooling (ui==0) stays env-driven + deterministic so the render-diff harness
  is unaffected. Persists aspect, internal-res(+auto), SSAO/light(+params), 60fps.
- OPEN: live-toggling aspect/internal-res "completely breaks the game" (user) — investigating next with a
  headless aspect-switch diagnostic (don't guess).

## later-121 — genuine-wide is the DEFAULT wide path (FB-hack retired); cull live; "toggle breaks" diagnosed
User: "NO FB HACK. PC NATIVE FB. toggling resolution/widescreen breaks the game." Diagnosis + change:
- **"Toggling breaks" = the FB-hack widescreen is inherently broken, NOT a transition bug.** Proved with
  PSXPORT_ASPECT_SWITCH (new diag: switch aspect/ires at a present-frame headless): a FRESH FB-hack 16:9 at
  f2860 is IDENTICAL to one toggled at f2850 — both show the FB-hack's broken output. So the user toggles
  widescreen -> gets the broken FB-hack. Fix = make the GOOD path the default.
- **Genuine-wide is now the DEFAULT wide path** (`gpu_gpu_wide_engine()` default ON; legacy FB-spread is
  opt-out `PSXPORT_WIDE_FBHACK=1` for A/B). So selecting 16:9/21:9 in the overlay (or PSXPORT_ASPECT) gives
  the real wider FOV + native FB, no spread. Verified `scratch/screenshots/def2_field.png`: field renders
  genuinely wide (FOV, right terrain, native depth, 2D filled) with NO flag.
- **Frustum cull re-evaluated LIVE** (ov_object_cull): the wide decision is per-call (not cached once) and
  the override is ALWAYS registered, so a live aspect toggle in the overlay takes effect immediately.
- **DEAD END / my mistake:** tried to make PSXPORT_VK_SHOT dump the PRESENTED (pillarbox-cropped) region via
  s_last_* — produced BLACK dumps headless (s_last_* not valid in the headless present path). REVERTED; the
  shot dumps the full scratch FB (428) again. CONSEQUENCE: for PILLARBOXED 2D-only frames the shot shows the
  full FB incl. off-screen margins that hold stale content — so the "right-side garbage" seen on the
  pause-menu shots may be CROPPED-OUT margin the user never sees. Verify menu/2D-screens with a WINDOWED
  shot, not the headless FB dump.
- **OPEN:** (1) FB-hack code (WIDE_OFF centering, gpu_gpu_sprite_anchor_dx, the push_wide wide_off branch) is
  now DEAD (wide_engine default-on) — DELETE it (user: "NO FB HACK"); remove the PSXPORT_WIDE_FBHACK opt-out
  too. (2) Live-OFX-toggle: OFX is widened in ov_set_geom_offset when the game calls SetGeomOffset (scene
  setup) — a live aspect toggle mid-scene won't reproject the 3D until the next SetGeomOffset; re-apply OFX
  per-frame from the current aspect. (3) pause-menu / 2D-overlay-over-3D in wide — verify windowed first.

## later-122 — DELETE the FB-hack widescreen code (user: "NO FB HACK. PC NATIVE FB")
Genuine-wide became the default wide path in later-121; the legacy present-time FB-spread was already
DEAD code behind the `PSXPORT_WIDE_FBHACK` opt-out. Removed it entirely (audit step 6 "retire the FB
spread"), so the ONLY widescreen path is the genuine wider-FOV engine frame.
- **Removed:** `WIDE_OFF()` (the 320-in-428 re-center) and every use of it; the `push_wide` wide_off
  push-constant value (slot kept as reserved 0 to preserve the 32-byte VPC layout); `tritex.vert`'s
  `+ w.wb.x` re-center term (now `fx = local.x*ss + fb_x0`); the deferred screen-map `off_x = s_wide ?
  WIDE_OFF : 0` term; `gpu_gpu_sprite_anchor_dx()` + its 2D-sprite FB-hack caller branch in gpu_native.c
  (and the VK-disabled stub + the gpu_differ stub); the `PSXPORT_WIDE_FBHACK` opt-out itself.
  `gpu_gpu_wide_engine()` is now simply `g_mods.aspect != ASPECT_4_3`.
- **Verified — 4:3 byte-identical (the 0-diff gate):** every removed term was provably already 0 on the
  4:3 path (s_wide=0 ⇒ WIDE_OFF branch=0, sprite_anchor_dx returns 0 when !s_wide, off_x=0). Empirically
  the 4:3 headless field shot logs NO WIDE_ENGINE line and depth records=1807 / miss=34 — EXACTLY the
  later-118 4:3 stats, i.e. depth-attach coverage unchanged (`scratch/screenshots/postdelete_43.png`,
  faithful full-banner field).
- **Verified — genuine-wide unchanged:** 16:9 headless field logs `WIDE_ENGINE OFX 160 -> 214`, depth
  records=2370 / miss=30, renders the real wider FOV + native depth + filled 2D backdrop/HUD
  (`scratch/screenshots/postdelete_wide.png`, matches the later-121 def2_field).
- **Note:** the gpu_differ replay tool (`tools/gpu_differ/build.sh`) has PRE-EXISTING link rot (missing
  cfg_str/cfg_dbg/proj_probe_dump/rtpcaller_*/gpu_scene_dump stubs that gpu_native.c grew) — unrelated to
  this change (no sprite_anchor_dx undefined-ref). Left as-is.
- **NEXT (handoff B/C):** live per-frame OFX re-apply so an overlay aspect change reprojects instantly
  (currently only at the next SetGeomOffset); then windowed verify of the pause-menu / 2D-over-3D in wide.

## later-122 — DIRECTION RESET (user, emphatic): make it a PC GAME — port the engine to PC; guest = AI + level data ONLY
User, across several messages this session: "Make this a PC game. Port MORE of the game code to PC. Guest
memory is only used for enemy AI and level data. You are still writing to guest memory. Reverse Engineer
and port the game." Hard steer away from: FB-hack widescreen (deleted earlier this session), poking shared
guest GTE state (CR24/OFX) to fake wide, save/restore "isolation" tricks, and oracle-diff/PROJPROBE as a
working style. The bar is OWN the engine code in native C.

**Committed this session (clean baseline):**
- `7452f0c`..`338c21d`: deleted the FB-hack widescreen (genuine path was already default).
- `<this>`: **keep guest GTE faithful** — reverted the later-117 global OFX (CR24) widen. ROOT CAUSE of
  the "our changes corrupt the game": OFX is SHARED GTE state the GAME's OWN logic reads back (gameplay
  RTPS → on-screen tests / placement / cull consume the projected SXY). Widening it globally shifted those
  read-backs → corrupted gameplay. So widescreen must NEVER touch guest GTE state.

**RE finding — WHY render data is in guest RAM (the thing to port out):** it is NOT laziness. The ordering
table (OT) interleaves PRIMITIVES with PERSISTENT GP0 STATE (draw-offset GP0 0xE5, draw-area/clip 0xE3/E4,
texture-window 0xE2, plus env/tpage packets — SCENEDUMP: main field OT ≈ 514 poly + 389 rect + 11 env) and
`DrawOTag` processes them IN SUBMISSION ORDER at draw time. A primitive's effective state depends on the
state-packets that precede it in the OT. Consequences:
- You CANNOT snapshot GP0 state at SUBMIT time and pull prims out of the OT — state changes mid-OT (the 11
  env packets), so submit-time state ≠ draw-time state. The native display list must be built/ordered the
  way DrawOTag walks, carrying each prim's draw-time state.
- The address-keyed depth bridge (`projprim_set_pz(pkt+8,…)` in engine_submit → renderer
  `projprim_lookup_pz(vaddr)` keyed by the guest packet word address) is a SYMPTOM of packets living in
  guest RAM; a native prim carries its float view-Z directly and the bridge disappears.
- Prims enter the OT via INLINE AddPrim (`prim->tag=ot[idx]; ot[idx]=prim`) inside the game's per-entity
  render handlers — there is no AddPrim function to override. So fully removing guest-packet writes means
  owning the whole primitive-submission path (all libgpu AddPrim builders + the env packets), i.e. the
  classified native display list of engine_re.md "Native ownership plan" step 1.

**NEXT (the port, not pokes):** own DrawOTag as a NATIVE classified display list — reimplement the libgpu
primitive builders so prims+env flow into PC-native structs (carrying draw-time state + native view-Z),
render that list directly via gpu_gpu_draw_*; keep the recomp body as A/B oracle. Then widescreen = native
projection (proj_native_vertex with a render-OFX) on the native list, guest GTE untouched. Open snag for
the native-projection step: proj_native_vertex is 0-diff at the GTE-op hook (PROJPROBE) but diverged in
BOTH X and Y when called from the submit call-site (e.g. native 012F00B2 vs gte 015F00B7) — a register/
call-context bug to nail before relying on native projection in the submit.

## later-123 — PORT: own the geometry submit as a NATIVE display list (render data OUT of guest RAM)
The directive (later-122): make it a PC game, stop writing render data to guest memory, port the engine.
The three native-owned geometry submit fns (engine_submit.c — POLY_GT3 `0x8007FDB0`, POLY_GT4
`0x8008007C`, byte-packed GT4 field emitter `0x80027768` + the overlay auto-detected copies) projected via
GTE but still **wrote the 40/52-byte GPU packet (verts/colour/uv) into guest RAM** and linked it into the
guest OT. That packet IS the "render data in guest memory" — it is engine OUTPUT, not gameplay state.
**Done — native classified display list (engine_re.md ownership plan step 1):**
- New `engine/native_dl.{h,c}`: `NativePrim` (the would-be packet words + each vertex's real view-space Z,
  which the integer guest packet always threw away) + a per-frame arena with a node-keyed open-addressing
  lookup. Lazy reset on the first alloc after a DrawOTag consume — robust to the engine's setup-OT +
  main-OT double draw (no ClearOTagR hook needed).
- `engine_submit.c`: a `PkTgt` packet-target abstraction (`pk_w32/w16/w8/r16/r8`) lets the SAME submit
  body write either a guest packet (A/B) or a native word buffer. Default path: build into the native
  buffer, link a **ZERO-LENGTH** ordering node into the guest OT (ordering only — the game's inline
  AddPrim builds the same OT, so the 1-word link is unavoidable and is NOT render data), push a
  `NativePrim` carrying the packet words + per-vertex SZ. The value-keyed `projprim_set_pz` address bridge
  is gone on this path.
- `gpu_native.c`: the OT walk (`gpu_dma2_linked_list`) calls `ndl_render_node(addr)` per node — an owned
  node (zero guest payload) loads its native packet words into the FIFO and runs the normal `gp0_exec`
  path (so semi grouping / fade / fps60 capture / widescreen-2D are all identical) but with `s_ndl_cur`
  set so the depth path takes view-Z straight from `np->pz` (parse order), no address bridge. `ndl_mark_
  consumed()` after the walk.
- A/B: `PSXPORT_DL_GUESTPKT=1` reverts to writing the full guest packet; `PSXPORT_SUBMIT_RECOMP=1` keeps
  the recomp body.
**VERIFIED byte-identical (the gate):** headless field shot (s_frame 2900, AUTO_GAMEPLAY) at BOTH 4:3
(320x224) AND 16:9 (428x240) is `cmp`-identical native-DL vs DL_GUESTPKT — the render data moved out of
guest RAM with ZERO pixel difference, and the carried native view-Z reproduces the depth ordering exactly.
Field renders correctly (Tomba/grass/trees/"Burning House" banner; wide = genuine wider FOV). No arena
overflow / malformed-OT warnings. `scratch/screenshots/cmp_native.png`, `cmp_wide_native.png`.
**NEXT:** extend ownership to the remaining primitive builders so ALL render-data writes leave guest RAM:
the sprite/tile/flat/line builders + the env packets (the field's 389 rects + 11 env still write guest
packets via libgpu / inline AddPrim). Then PC-native projection on the native list (the proj_native_vertex
submit-call-site register bug — later-122 open snag — must be nailed first).

## later-124 — DIRECTION (user): the problem is WRITE-COLLISION, not "guest writes are forbidden"; benefactor is the reference architecture
User correction to later-122's "no guest memory" framing: **"I might have been wrong to say no guest
memory; but OUR writes always cause corruption — sometimes visual, sometimes worse. So we need to make
the game able to use a different memory layout. To solve this we need much more game ownership and maybe a
full port."** And: a sibling repo `~/repo/benefactor` (Amiga→PC) is **fully ported, runs fine, built from
the ground up** — the model to follow.

**Reframed root cause (the real one):** the bug is not "we touch guest RAM" — it is that our writes
*collide with the game's own data*. Two confirmed/likely mechanisms:
1. **Shared state the game reads back** — CONFIRMED (later-117/122): widening the GTE OFX (CR24) corrupted
   gameplay because the game's own logic RTPS-projects and reads the SXY back. Reverted.
2. **Fixed-size engine buffers we overflow** — HYPOTHESIS (the widescreen corruption): the packet pool
   (`0x800BF544`) and the OT (2048 entries) are sized for the 4:3 frustum. Widescreen adds geometry (the
   extended frustum cull `ov_object_cull` re-includes culled objects via `mem_w8(o+1,1)`, so the game's
   own submit emits MORE prims), which can overflow the pool/OT into adjacent game-owned RAM → corruption.
   The cull re-include ALSO forces the game to PROCESS objects it meant to skip → possible logic side
   effects ("sometimes worse"). NOT yet measured — measure pool/OT high-water 4:3 vs wide to confirm.

**benefactor lesson (instructions/widescreen-plan.md) — SAME problem class, decided answer:** the Amiga
engine keeps only a screen-width wrap buffer in chip RAM; off-screen terrain is absent. Verdict: *"approach
B (native tilemap render) is mandatory; no read-wider-from-page shortcut."* It renders the wider world
with a **native renderer reading the game's DATA structures** (tilemap + tile-gfx pointer table + camera
clamp `$107a/$107c`), into its OWN surface — the recompiled engine's chip-RAM updates stay as the game did
them; the WIDE part is native. Architecture: native game loop (`game_loop.c`) + native renderer
(`native_renderer.c`) reading game data + recompiled body behind overrides (`port/overrides/`).

**CONSEQUENCE for Tomba2Engine (the plan, sharpened):** widescreen/effects without corruption require the
PC engine to OWN the render of the extra/wider content from the game's DATA, not push the game's fixed
pipeline harder. The native display list (later-123) is the right foundation — it is a PC-native render
target for primitives. NEXT layers, in order:
- **Engine-owned render memory:** owned prims should stop consuming the guest packet pool / guest OT
  entirely (today the native DL still bumps the guest pool faithfully + links a guest OT node). Give the
  native display list its OWN ordering (per-bucket native lists keyed by the OT idx the submit already
  computes) so wide geometry can never overflow guest buffers. Watch: semi-transparent owned prims need
  painter order (the native D32 depth handles opaque 3D ordering, NOT blend order).
- **Native object→prim rendering (benefactor approach B):** the active entities are a doubly-linked pool
  list (stride 0xD0, engine_re.md §32; `ov_object_cull` already receives each object*). Walk it natively
  and emit each object's prims into the native DL directly — reading the object's model/transform from
  guest RAM but NOT routing through the game's submit/pool. Then the wide margins are OURS, the game's
  buffers are untouched, and the extended-cull `mem_w8` shared write can be DROPPED.
- **Map the memory layout** (the user's "different memory layout"): RE the pool buffer base+extent and the
  OT base/size so we know what is free and what the game reads back — the prerequisite to relocating/
  expanding engine-owned buffers safely. (benefactor did exactly this: pages `$2B3EC` row-stride/extent,
  camera clamp vars, level-data structs.)
Reference repo: `~/repo/benefactor` (AGENTS.md, docs/codebase-layout.md, instructions/widescreen-plan.md).

### later-124 measurement — widescreen pool/OT pressure CONFIRMED (PSXPORT_DEBUG=pool)
Same field scene, headless, main OT (gpu_native.c `[pool]` probe):
- **4:3:**  main OT = **2984** prims, packet-pool high-water = **0x0DB9B0**.
- **16:9:** main OT = **3244** prims (**+260**, the `ov_object_cull` re-include), pool high-water =
  **0x0DEAEC** (**+~12.6 KB**).
So genuine-wide DOES make the GAME's own submit emit ~260 extra prims and push the shared packet pool
~12.6 KB higher (extra load on buffers sized for 4:3) AND forces the game to process objects it culled
(logic side-effect risk = the "sometimes worse"). NOT proof of overflow at THIS scene (the pool buffer
still has headroom here — pool ends ~0xDEAEC, parity stride 0x14000), but it confirms the pressure
direction; the pool/OT buffer EXTENT + a denser scene's high-water still need RE to find the overflow
threshold. Probe kept (`cfg_dbg("pool")`, zero cost off). This is the empirical basis for moving the wide
geometry OFF the game's buffers into engine-owned render memory (benefactor approach B).

## later-125 — PORT: map the render-buffer memory layout + cut owned-prim pool footprint to its 1-word link tag
Handoff step 1 (RE the memory map) + step 2a (engine-owned render memory, first increment). Directive
later-124: move the wide/extra render off the game's FIXED buffers so our writes can't overflow into
game-owned RAM (benefactor approach B). Prerequisite = knowing the buffer extents / overflow threshold.

**Memory map RE'd (docs/engine_re.md "Render-buffer memory map"), confirmed empirically (PSXPORT_DEBUG=pool
madr = the DrawOTag OT roots):** double-buffered by parity=DAT_1f800135.
- packet pool: parity0 `[0x800BFE68,0x800D3E68)`, parity1 `[0x800D3E68,0x800E7E68)` — each 0x14000 B, a
  per-frame BUMP allocator (write ptr DAT_800bf544) shared by the geometry submit AND inline AddPrim 2D.
- ctx (OT + DISP/DRAW env): parity0 `0x800E80A8`, parity1 `0x800EA118` (stride 0x2070). OT = first 0x2000
  (2048 entries, head/root @ctx+0x1ffc → 0x800EA0A4 / 0x800EC114, matching the probe's madr exactly).
- **Overflow threshold:** parity1 pool past `0x800E7E68` → (0x240 gap) → ctx parity0's OT at `0x800E80A8`
  — a pool overflow corrupts the *alternate* frame's ordering table (double-buffer cross-corruption), not
  just its own packets. THIS is the mechanism behind later-124's "fixed-buffer overflow" hypothesis.

**Root cause of the pool pressure (found while doing this):** the native display list (later-123) moved the
render DATA out of guest RAM but STILL advanced the guest pool by the FULL packet size (40 B GT3 / 52 B GT4)
per owned prim — even though an owned node's only guest footprint is its 1-word OT link tag (the walk reads
0 payload words: `n=hdr>>24==0`, render comes from the native arena). So owned prims were burning ~10-13×
the pool they actually use. NOT a bandaid — it is the correct footprint: an owned node IS one word.

**Fix (engine_submit.c, all 3 owned submitters GT3/GT4/gt4_bp):** in the native-DL path advance the pool by
4 B (the link tag), not 40/52. Guest-packet A/B path (PSXPORT_DL_GUESTPKT=1) unchanged. Draw ORDER unchanged
(guest OT linked list stays the master order → the merge with inline 2D prims is identical).

**VERIFIED (the gate) — headless field shot s_frame 2900, AUTO_GAMEPLAY:**
- 4:3  `cmp` default vs DL_GUESTPKT=1 → **BYTE-IDENTICAL**; pool hi 0x000DB9B0 → **0x000D649C** (~31.5 KB →
  ~9.8 KB used, **−69%**). Field renders correctly (Tomba/grass/trees/"Burning House" banner).
- 16:9 `cmp` default vs DL_GUESTPKT=1 → **BYTE-IDENTICAL**; pool hi 0x000DEAEC → **0x000D68E4** (~44 KB →
  ~10.9 KB, **−75%**). The +260 wide prims (ov_object_cull re-include) now add ~1 KB, not +12.7 KB.
The widescreen pool-overflow pressure (later-124 mechanism #2) is effectively eliminated for owned geometry.

**NEXT (handoff steps 2b/3, still open):** owned prims still link a 1-word node into the GUEST OT (ordering
master) + use 1 word of the guest pool. To fully own render memory (benefactor approach B): give the native
DL its OWN per-bucket ordering keyed by the OT idx the submit already computes, and stop touching the guest
OT/pool for owned prims (WATCH: semi-transparent owned prims need painter order preserved when merging the
native buckets with the guest OT's inline 2D prims — measure bucket sharing first). Then walk the active-
entity pool list natively (stride 0xD0, §32) and emit each object's prims straight into the native DL,
dropping the extended-cull `mem_w8(o+1,1)` shared write (collision mechanism #1) at the source.

## later-126 — MEASURE owned-vs-guest OT bucket sharing → reprioritize the "native render memory" plan
Handoff NEXT step "measure bucket sharing first" (the precondition for native per-bucket ordering, step 2b).
Added `PSXPORT_DEBUG=bucket` (gpu_native.c, read-only re-walk of the OT, zero cost off): a maximal run of
consecutive POOL-region nodes between two OT-array bucket-anchor nodes = exactly one bucket's prim chain;
tally owned (native-DL 3D) vs guest (inline-2D) prims per run, count buckets that hold BOTH.

**Field scene result:**
- **4:3:**  owned=508 guest=428 | owned-only buckets=276, guest-only=9, **MIXED=0**.
- **16:9:** owned=766 guest=430 | owned-only=446, guest-only=7, **MIXED=2**.
So at 4:3 owned 3D and guest 2D never share a bucket, but in 16:9 the +258 wide-frustum-re-included owned
prims push **2 buckets to hold both** (those prims were culled at 4:3, leaving the bucket guest-only).

**Consequence — the plan is reprioritized (honest reassessment, NOT the original step order):**
1. **Overflow (later-124 mechanism #2) is already neutralized by later-125.** Owned prims now cost 4 B
   each, so even a pathological dense wide scene (~5000 owned ×4 B + ~430 guest ×52 B ≈ 42 KB) stays well
   under the 0x14000 (81920 B) pool buffer. The reason step 2b existed (prevent overflow) is largely moot.
2. **Naive native per-bucket ordering (step 2b) would now be INCORRECT in widescreen** — the 2 mixed
   buckets need owned/guest insertion order preserved, which only the guest OT currently encodes. Standalone
   2b buys merge complexity, not safety. Deprioritized; it falls out cleanly only AFTER step 3 removes guest
   3D prims.
3. **The geometry submit is ALREADY native** (engine_submit → native DL, later-123): a re-included wide
   object's prims already flow into the native display list. The wide re-include's only guest touch is
   `mem_w8(o+1,1)` (game_tomba2.c ov_object_cull) — and the entity walk `FUN_8007a904` calls every node's
   handler EVERY frame regardless; +1 is the per-frame RENDER flag (cleared each frame), gating whether the
   handler's submit path proceeds — NOT persistent gameplay state the logic reads back (unlike the CONFIRMED
   GTE-OFX collision, already reverted). There is no native shortcut to submit a re-included object without
   its handler (the handler builds the primitive-record list the submit consumes), so the +1 poke IS the
   minimal mechanism.

**Therefore the next real question is EMPIRICAL, not "implement step 3 blind":** does forcing submission of
wide-margin culled objects (the +1 re-include) actually perturb gameplay LOGIC ("sometimes worse"), or only
add the intended extra render prims? That must be MEASURED (drive wide vs 4:3 to a dense scene, diff game
state vs the oracle) before committing to a large native-object-render rewrite. If it does NOT diverge, the
collision concern is fully addressed by later-125 + the reverted OFX, and approach-B's value is purely the
long-term native-port goal, not corruption avoidance. Probe `PSXPORT_DEBUG=bucket` kept for that work.

## later-127 — PROVEN: the widescreen re-include perturbs gameplay LOGIC (not just rendering)
Answers later-126's open empirical question. Method: the port is deterministic, AUTO_GAMEPLAY gives
identical scripted input, so a 4:3 vs 16:9 self-diff of guest RAM (PSXPORT_RAMDUMP_FRAME) isolates exactly
the wide path. Tool: `tools/ram_region_diff.py` (buckets differing bytes by the later-125 render-buffer map
vs everything else). Entity walk compares each node's gameplay fields (pos +0x2e/32/36, state +0x28, type
+0xc, handler +0x1c).

**Determinism confirmed:** two identical 4:3 runs are byte-identical (cmp) — so any 4:3-vs-16:9 difference is
real, not run-to-run noise.

**Result (field, native frame 438):**
- 4:3 vs 16:9: entity LIST heads + node count/order/type/state/handler all identical, BUT **14 objects'
  POSITIONS diverge** (4 in list1 head 0x800FB168, 10 in list2 head 0x800F2624). list2 = ten type-0x06
  objects (z≈-32748 parked, x jitters ±70 = being animated/ticked); list1 includes a type-0x05 that moved
  far AND **two type-0x02 objects that go (0,0,0)→real position** = the re-include ACTIVATES dormant objects.
- **Isolation (PSXPORT_CULL_FAR=0 disables the re-include, keeps wide projection): 4:3 vs 16:9 position-diff
  drops to 0 in BOTH lists.** => the re-include `mem_w8(o+1,1)` in ov_object_cull (game_tomba2.c) is THE
  cause. Wide native projection (OFX/clip) is logic-clean (later-122), as expected.

**Conclusion — later-124 mechanism #1 is PROVEN, not hypothetical:** forcing culled objects render-visible
makes the game TICK/ACTIVATE objects it meant to skip (many games only advance an object's
animation/physics/spawn when it passes the cull). That is genuine gameplay-state perturbation — the
"sometimes worse." Genuine-wide via the re-include is NOT faithful.

**Plan, sharpened (supersedes the generic step-3 framing):** widescreen must render the wider *static* world
content (terrain/water tiles, which also pass this per-object cull) NATIVELY from its data, WITHOUT poking
dynamic objects to tick — benefactor approach B exactly ("keep the engine's own updates as the game did
them; the WIDE part is native"). A naive type-filter on the re-include is rejected (magic-constant bandaid):
the proper path is the native object→prim render of the static margin geometry, leaving dynamic entities
culled exactly as 4:3 does. Prereq RE: classify which entity types (+0xc) are static-world vs dynamic, and
whether the static ones can be projected+emitted natively from their model/transform without their handler.
Probes kept: `tools/ram_region_diff.py`, `PSXPORT_RAMDUMP_FRAME`, `PSXPORT_CULL_FAR=0` (re-include A/B).

### later-127 addendum — entity-type taxonomy (field, first pass)
Type (+0xc) histogram of the field's two object lists (4:3 dump):
- list1 (head 0x800FB168, 106 nodes): type 0x02=34, **0x04=58 (the bulk)**, 0x05=10, 0x09=4.
- list2 (head 0x800F2624, 47 nodes):  type 0x03=24, 0x06=23.
The position-divergence (re-include perturbation) hit types **0x02, 0x05, 0x06** (dynamic — ticked/activated
by being forced visible). The dominant **type 0x04 did NOT diverge** → static-world candidate (the terrain/
ground tiles we WANT rendered wider). So the re-include conflates two effects: (a) re-including STATIC
geometry = logic-safe + desired; (b) ticking DYNAMIC objects = the perturbation. Inference from ONE frame is
not proof a type is static (a static object never diverges, but so could an identically-ticked dynamic one)
— the taxonomy must be confirmed by examining each type's handler (+0x1c) before relying on it. This is the
prerequisite RE for the native static-margin render (approach B).
> **FALSIFIED by later-128** — the one-frame position inference above is WRONG. Per-type re-include
> isolation + a re-include counter proved: **type 0x06 is never re-included (vacuous), type 0x04 DOES
> perturb (422 B), and NO actually-re-included type is 0-diff.** Read later-128, not this addendum.

## later-128 — CORRECTED entity-type taxonomy (proper per-type isolation, supersedes later-127 addendum)
Did handoff step 1 properly: don't trust the one-frame position diff — per-type re-include + RAM self-diff,
plus a re-include COUNTER (to catch vacuous 0-diffs) + handler disasm. New tools: `tools/entity_walk.py`
(walks both entity lists, type→handler histogram), `PSXPORT_CULL_ONLY_TYPE`/`PSXPORT_CULL_SKIP_TYPE`
(type-gate the wide re-include, measurement), `PSXPORT_DEBUG=cullinc` (per-frame per-type re-include tally).
Method: deterministic AUTO_GAMEPLAY field, dump f438, 4:3 baseline = `ram43.bin`; ASPECT=16:9 with
`CULL_ONLY_TYPE=t`; `tools/ram_region_diff.py` reports OTHER (gameplay) bytes. (`tools/build_port.sh`.)

**Type → handler is MANY-to-one, not 1:1.** `entity_walk.py` on the 4:3 field dump: type 0x04 (58 nodes)
spans **20 distinct handlers** (mostly overlay 0x8012/0x8013xxxx); 0x02→7 handlers, 0x06→4, etc. So `+0xc`
is the **cull-switch discriminant** (FUN_8007712C branches on it only to pick per-type distance/FOV bands —
0x04: cull<512, keep-no-FOV<7169, else FOV depth/dist<856; 0x02/05/09 have their own), NOT a behavior label.
Static-vs-dynamic is a per-HANDLER property, decided by whether the handler's state-advance is gated on the
cull/visible result.

**Per-type re-include gameplay perturbation (OTHER bytes, 4:3 vs 16:9-only-type t):**
| type | OTHER B | objs re-included/frame | verdict |
|------|--------:|------------------------:|---------|
| 0x02 | 3405 | 25 | dynamic |
| 0x03 | 1276 | 10 | dynamic |
| 0x04 |  422 | 18 | mostly-safe but NOT 0-diff (~23 B/obj) |
| 0x05 |  461 | 23 | ~20 B/obj, NOT 0-diff |
| 0x06 |    4 |  **0** | **VACUOUS — never re-included; 0-diff is meaningless** |
| 0x09 | 1696 |  2 | very dynamic (~848 B/obj) |
(The constant 4-byte floor = the packet-pool write ptr 0x800BF4F4/0x800BF544 — render state, not gameplay.)

**Two later-127 conclusions FALSIFIED:** (a) 0x06 is NOT static/dynamic-relevant — it is simply **never in
the wide re-include set** at this scene (its objects sit at z≈-32748, beyond cull_far regardless of FOV), so
its "0-diff" is vacuous; the list2 0x06 position jitter later-127 saw under FULL re-include was a CROSS-OBJECT
effect from other re-included types, not 0x06 ticking. (b) 0x04 is NOT clean-static — re-including it perturbs
422 B (18 obj). **No entity type at the field scene is both non-vacuous AND 0-diff** → a type-gated re-include
CANNOT losslessly fill the wide margin. This is positive proof that approach B (native render of the margin
from object DATA, WITHOUT the +1 poke) is required, not a shortcut.

**Mechanism (handler disasm, type-0x06 handler 0x8013c538 via tools/disasm_overlay.py on ram43.bin):** the
handler runs its state machine (reads substate +0x4, writes +0x4e/+0x50.. arrays via jal 0x80032a44 loop,
advances +0x4) UNCONDITIONALLY at the top — BEFORE any cull/submit call. So for 0x06 the cull/visible flag
gates only the RENDER submit, not the logic; that is exactly why forcing it visible would be logic-safe — but
moot, since 0x06 is never re-included here. The dynamic types instead gate their state-advance DOWNSTREAM of
the cull result, so the +1 poke ticks them. Net rule: **re-include perturbs iff the handler's state-advance is
gated on the cull/visible result.**

**NEXT (approach B, native static-margin render):** the margin objects we must render natively are the
actually-re-included set (0x02/03/04/05/09, ~78 obj/frame). To render them WITHOUT the +1 poke we read each
object's model-data ptr (+0x38) + transform/pos (+0x2e/32/36) from guest RAM and emit prims straight into the
native DL (engine_submit owned path) — leaving the handler's visibility-gated logic untouched. OPEN RE: the
model-data format at +0x38 and the per-object transform load (the 96/54 ctc2 sites, engine_re.md §Camera) so
the native path can project+submit identically to the handler's submit. Per user direction (2026-06-18): full
RE + PC-native port (no guest pokes); new PC-native code (incl. the margin renderer) is written in C++/OOP.

## later-129 — approach-B-as-written INVALIDATED: margin geometry is handler-procedural, not data-driven
Step 2 RE (the native object→prim render). New probe `PSXPORT_DEBUG=cullobj` (game_tomba2.c) logs each
re-included margin object: addr, type, model id (+0xe), model-data ptr (+0x38), pos. entity_walk.py extended
to print +0xe/+0x38/+0x28 per node.

**Finding — the margin objects have NO readable model data:**
- The wide re-include touches ~78 obj/frame (0x02/03/04/05/09). The dominant **type 0x04 (18/frame) all have
  model=0x0000, mdata=0x00000000** — AND so do the VISIBLE, on-screen type-0x04 objects in the 4:3 dump
  (state=0x0001, centre positions). So mdata=0 is NOT a "culled → not yet lazily loaded" artifact: type-0x04
  geometry genuinely does not flow through the +0x38 model-data pointer. Only 2 of 58 type-0x04 nodes carry
  any mdata. **This falsifies the handoff/journal approach-B step 2** ("read the object's model-data ptr
  (+0x38) … and emit its prims") for the dominant margin type.
- The 18 re-included type-0x04 objects use **9 distinct handlers** (mostly overlay 0x8013xxxx:
  0x80138fc8 x5, 0x8013259c x4, …, + resident 0x80073cd8/0x800739ac/0x800741dc). Geometry is built
  procedurally INSIDE each handler's visible branch (the submit 0x800803DC consumes a geomblk the handler
  builds from model state + the per-object GTE transform), not from a generic model-data struct.

**Submit-chain RE (how a handler renders, for completeness):** handler → submit wrapper (e.g. FUN_8007778C:
computes pos−camera, clears mode, calls cull 0x8007712C which sets the +1 visible flag) → returns to handler
→ handler reads +1 and, ONLY if visible, builds the geomblk + calls the submit caller 0x800803DC → native
GT3/GT4 (8007FDB0/8008007C). The geomblk is built in the visible branch (NOT when culled), so there is no
pre-built per-object prim list to grab for a culled object. For dynamic handlers the gameplay state-advance
lives in that SAME visible branch → render and logic are entangled there.

**Consequence — the fork (planned approach is dead, alternatives differ):**
- (A) Full native per-object render: RE+port each margin handler's procedural geometry. Dozens of bespoke
  handlers, much of it per-scene overlay → very large, low-leverage. Not viable as the general path.
- (B) **Transactional region-scoped re-include (recommended):** re-include (set +1) so the game's OWN handler
  renders the margin object into the native DL, but journal all guest writes during the re-included visible
  branch and UNDO those landing OUTSIDE the render-buffer regions (the later-125 memory map: pool
  0x800BFE68+, ctx/OT 0x800E80A8+). Keeps the added prims, discards the gameplay perturbation — GENERIC
  (handler/scene-agnostic), faithful (gameplay state provably identical), and not a magic-byte bandaid (it
  undoes the WHOLE gameplay region, not cherry-picked bytes). Margin objects render at their "culled/frozen"
  animation state = consistent with how 4:3 leaves them un-ticked. Needs interpreter write-journaling around
  the entity walk — real but bounded.
- (C) accept genuine-wide as-is (cosmetic edge perturbation); (D) faithful narrow-cull = CULL_FAR=0 (0-diff,
  but empty edge wedges). Both already available.
Probes kept: PSXPORT_DEBUG=cullobj/cullinc, PSXPORT_CULL_ONLY_TYPE/_SKIP_TYPE.

## later-130 — approach-A step-1 PREMISE FALSIFIED: geometry submission is a DEFERRED FLUSH (not per-handler)
Attempted handoff step 1 (the per-object geomblk capture ORACLE). Built `PSXPORT_DEBUG=geomblk`
(engine_submit.c) + `PSXPORT_GEOMBLK_FRAME=<s_frame>` gate: dumps the raw primitive records of every geomblk
through the three natively-owned submitters (GT3 0x8007FDB0, GT4 0x8008007C, GT4bp 0x80027768). The probe
WORKS — at the field (s_frame 2900, 16:9) it captured 256 geomblks (256 GT3 + 256 GT4 calls via caller
0x800803DC, + 1 GT4bp), 2008 records. But the handoff's **keying premise is false**, which kills step 1
as specified and reframes approach A:

**Geometry SUBMISSION is a DEFERRED FLUSH PHASE, decoupled from the per-object entity walk.** Two independent
object taps both FAIL to name a submitted geomblk's source object:
- cull-tap (`g_render_object`, set in ov_object_cull, NOT restored): all 256 geomblks key to ONE object
  (800f0e80) — the last object that went through cull before the flush, not the source of each geomblk.
- walk-tap (`g_current_object`, set per-node by the active native walk ov_objwalk, restored after each
  handler): all 256 key to **0x00000000** — i.e. submission runs OUTSIDE any handler's node_call context.
- Ordering proof: every per-object cull (`cullobj` re-includes, 78 obj/frame) completes BEFORE the first
  owned submit (0 cullobj lines after the first geomblk line). So handlers do NOT "cull then immediately
  submit" (later-129's chain description is wrong as a temporal claim) — handlers ENQUEUE geometry during
  the walk; a separate FLUSH phase (g_current_object==0, after the whole walk) drains it via GT3/GT4.

**Object 800f0e80 = the field's world/map renderer** (type 0x03, model=0/mdata=0, pos≈camera). At the field
the entire owned-submit path is ITS geometry. The **78 margin objects produce ZERO owned-path geometry** —
they enqueue via UN-owned submitter variants (the dominant overlay margin handler 0x80138fc8 calls the cull
wrapper 0x8007778c + a tree of shared helpers 0x80073328/0x80077ebc/0x8007703c, never the owned caller
0x800803DC), submitted through interpreted variants (engine_re OPEN list: 0x8003B320/0x8003C8F4 GT3,
0x8013CDD4/0x8013DD34 overlay) that the probe can't see.

**Consequence for approach A (user's chosen direction):** it is heavier than later-129 already framed.
To build even the step-1 ORACLE for a margin handler you must FIRST (a) tap the geomblk ENQUEUE site to
recover object→geomblk attribution (deferred-flush, so neither cull/walk tap suffices), AND (b) own/RE the
un-owned submitter variant(s) the margin handlers use. THEN per-handler RE the procedural geometry through a
deep shared-helper call tree. Per-handler, per-scene, ~9+ handlers — exactly the "very large, low-leverage,
not viable as the general path" later-129 warned of, now with two added prerequisites. This **reinforces
later-129's recommendation of approach B** (transactional region-scoped re-include): generic, handler/scene-
agnostic, needs none of the per-handler RE — it lets the game's own deferred-flush render the margin and just
undoes the gameplay-region writes. DECISION SURFACED TO USER (2026-06-18).
Probe kept: `PSXPORT_DEBUG=geomblk` (+`PSXPORT_GEOMBLK_FRAME`); g_render_object exported from game_tomba2.c.

## later-131 — USER chose approach A (full native port). rcmd ORACLE built; margin = +24 render commands
User (2026-06-18) rejected approach B (a journaling hack) and directed the **full PC-native port** ("the
game is fully deterministic … find a PC-native solution … port the entire game if you have to"). Correct —
the deferred render pipeline (later-130) makes A tractable, not the per-handler slog later-129 feared.

**The real oracle — `PSXPORT_DEBUG=rcmd`** (engine_submit.c `ov_render_cmd_probe`, override on the mode
dispatcher 0x8003F698, registered only when the channel is on): dumps every queued RENDER COMMAND as a
self-contained unit — `mode` (*0x800BF870), `geomblk` (a0), `ot` (a1=*0x800ED8C8), `flag` (a2), and the
**per-object GTE transform** the flush just loaded into GTE control regs (CR0-7: CR0-4 rotation matrix,
CR5-7 translation). This is the COMPLETE input a native render-half must reproduce, for ALL modes (supersedes
the GT3/GT4-only geomblk probe). Shares the `PSXPORT_GEOMBLK_FRAME` gate.

**Field-frame render-command analysis (s_frame 2900, AUTO_GAMEPLAY, deterministic):**
- 4:3 = 100 commands; 16:9+re-include = 124; 16:9 no-re-include (CULL_FAR=0) = 100. All mode=00 here
  (renderer 0x80146478, which itself calls the owned GT3/GT4 → the 256 owned submits the geomblk probe saw
  = these commands expanded). The map/world is ~100 transformed sub-model commands, NOT one blob.
- **Re-include's effect (isolated: 16:9 with vs without, aspect held fixed): +24 NEW margin commands AND 24
  EXISTING commands perturbed** (their transforms change — the later-128 gameplay perturbation, now seen at
  the render-command level). No geomblk ever disappears. So the +1 poke is NOT cleanly separable: it adds the
  margin AND ticks gameplay. **Native plan = enqueue ONLY the 24 margin commands (from the culled objects'
  frozen state), never poke +1** → margin renders, the other 24 stay un-perturbed. This is precisely why A
  (not B) is right.
- **Margin objects DO have geometry — later-129's "no model data" is a RED HERRING.** The margin commands
  carry real geomblks (0x801e41dc, 801e474c×3, 801e682c…801e6e88, 801e976c…801ea5d0, 801f734c/73b4, +
  8015ca04×22 instanced). The static node field +0x38 (mdata) is 0, but the actual prim-list (geomblk) is
  resolved at ENQUEUE, not read from +0x38. The data-driven render path is ALIVE.

**ENQUEUE — partly found.** `PSXPORT_DEBUG=enq` taps `gen_func_80077EBC`: it pushes its `a0` onto a cap-40
scratchpad list (write-ptr 0x1F800148, count 0x1F800150). At f2900 only **2** pushes, and `a0 == the object
node itself` (a0+0x40 / a0+0x18 are object fields = 0, NOT geomblk/transform). So 0x80077EBC is an OBJECT-list
push (one of several lists; sibling 0x80077EFC → 0x1F800154/0x15c cap 28), **not** the render-command enqueue.
The render-COMMAND struct (transform@+0x18, geomblk@+0x40) that the flush `gen_func_8003F174` reads from
`list+0xc0` is filled on a DIFFERENT path — still open.

**NEXT (the one open RE) — the render-COMMAND enqueue.** Find who allocates+fills the command struct
(snapshot the GTE matrix → cmd+0x18, store geomblk → cmd+0x40) and links it into the flush list (a0 to
gen_func_8003F174; commands at `list+0xc0`, count `list+8`; layer table 0x800EC188 40×0x40; flush callers
0x80039f70/0x8003c0c8). Approach: tap the flush 0x8003F174 to print the cmd addresses (list+0xc0[i]) + fields,
then trace back the writer of those cmd structs. That gives (a) object→command attribution and (b) HOW
transform+geomblk derive from object state — so native C can build a margin object's command from its frozen
state and enqueue it directly (no +1, no perturbation), each validated 0-diff vs the rcmd capture. Probes:
`PSXPORT_DEBUG=rcmd`/`geomblk`/`enq` (+`PSXPORT_GEOMBLK_FRAME`).

## later-132 — ENQUEUE + geomblk-table FOUND; render commands are PERSISTENT (built once)
Found the render-command enqueue with `PSXPORT_WWATCH` (word-store PC tap, runtime/recomp/mem.c). Watching
the command's geomblk word (cmd+0x40 at 0x800F9CA4 for the f2900 flush list 0x800fb218) caught the writer at
**pc=0x80051B2C** — the leaf `gen_func_80051B04`, called from the enqueue `gen_func_80051B70` (which the
margin handlers 0x80073cd8 / 0x80138fc8 both call). Decoded (docs/engine_re.md §Deferred render pipeline):
- **Enqueue `gen_func_80051B70`**(a0=object, a1=group, a2=sub): allocates the cmd (`gen_func_8007AAE8`),
  stores cmd ptr at **node+0xc0**, sets scale cmd+0x38/3a/3c=0x1000, header cmd+6=-1 / +0/2/4/8/a/c=0.
- **Geomblk = data-driven table lookup** (leaf 0x80051B04): `geomblk = T + *(T+sub*4+4)`, `T =
  *(0x800ECF58 + group*4)` — a two-level model table at 0x800ECF58. Deterministic from (group,sub).
- **Transform** (`gen_func_80051C8C`): node+0x98 = rotation matrix (from node+0x54/56/58 via
  0x80084D10/EB0/5050), node+0xac/b0/b4 = translation (from pos node+0x2e/32/36). Flush loads it to cmd+0x18.
- **PERSISTENCE (the key correction):** cmd+0x40 was written **exactly ONCE in 2905 frames** → render
  commands are NOT rebuilt per frame. They are built at spawn/scene-setup and kept at node+0xc0; per frame
  only the transform updates + the flush renders. At steady-state f2900 the enqueue (0x80051B70 / leaf
  0x80051B04) is NOT called (cmdenq probe = 0 hits) — consistent. The list 0x800fb218 (the 8015ca04×24 static
  decor layer) is built once by `gen_func_8003AE28` (calls the leaf), not per-frame.
- **Therefore the NATIVE margin plan simplifies:** for a culled margin object, read its PERSISTENT cmd at
  node+0xc0 and add it to the appropriate flush list (or build a native equivalent from group/sub + the
  table + the transform), WITHOUT poking +1. No per-frame geometry rebuild needed.

**NEXT — the per-frame visibility→flush-list selection.** Find where a visible object's node+0xc0 cmd gets
appended to a flush list each frame (the path the +1 flag gates; the list-count is a BYTE store so WWATCH
won't catch it — watch the cmd-ptr array slot `list+0xc0+i*4`, a word store, as done here → populator
pc=0x8003AEA8 for the static layer). Then: build `engine/margin_render.{hpp,cpp}` (C++) to add culled margin
objects' cmds to that list. Validate 0-diff vs the rcmd capture (mode+transform+geomblk byte-identical when
the object is genuinely visible). New probe: `PSXPORT_DEBUG=cmdenq` (taps leaf 0x80051B04; use at the SPAWN
frame, not steady-state). Probes: rcmd/geomblk/flush/enq/cmdenq + PSXPORT_WWATCH.

## later-133 — the OBJECT NODE *IS* its render-command list; per-object flush = `gen_func_8003CDD8` (THE mechanism)
later-132's "node+0xc0 = the single persistent command, just re-append it to a flush list" is **INCOMPLETE/
misleading** — corrected here by tapping the MAJOR flush. Findings (probes `flush2`, `rcmd ra=`, disasm):
- **The minor flush 0x8003F174 is NOT the world/margin path.** It drains only ONE list (0x800fb218, the
  8015ca04x24 static-decor layer) - count=24 in 4:3 / 16:9-on / 16:9-off ALIKE (it never carries the margin).
  The rcmd dispatcher-caller histogram at f2900: **ra=8003d07c -> 100 cmds (world+margin)**, ra=8003f230 -> 24
  (static). The world/margin flush is the function at **ra 0x8003d07c = `gen_func_8003CDD8`**.
- **`gen_func_8003CDD8(a0=list, a1=flag)` is called ONCE PER VISIBLE OBJECT, and the `list` arg == the object
  NODE address.** Each object node embeds its own render-command list: **count at node+8, cmd-ptr ARRAY at
  node+0xc0[i]** (loop 0x8003ce40: `cmd = node[0xc0 + i*4]`, `geomblk = cmd+0x40`, skip if 0). So node+0xc0 is
  the *base of an array*, not a single ptr (for count=1 objects it looks like a single ptr -> later-132's view).
- **The flush COMPOSES the transform itself:** camera rotation (`0x1f8000f8`) x object-local matrix
  (`cmd+0x18`) via a GTE matmul (cop2 0x4a49e012), and object translation from `cmd+0x2c/0x30/0x34` -> GTE
  CR5-7. So `cmd+0x18` is the OBJECT-LOCAL matrix (often near-identity) - NOT the camera-composed GTE matrix
  the rcmd oracle prints. That's why node+0xc0's `cmd+0x18` did NOT match the rcmd `M=`. The dispatcher gets
  a0=geomblk, a1=OTbase(global `*0x800ED8C8`), a2=flag(=a1 of the flush). Margin objects: flag=0.
- **PROOF margin = the +24 via per-object flushes:** 16:9-on has 22 `gen_func_8003CDD8` calls at f2900, 16:9-off
  has 12; the **10 ON-only lists ARE the re-included objects** (list base == cullobj object addr) and their
  command geomblks are EXACTLY the +24 margin set (800fc6c8 count=12 = the 801e682c..801e6e88 instanced series;
  800fe5a8 count=4; + 8 count-1 lists = 24). No node+0xc0 reconstruction needed.
- **WHY poke-+1 perturbs (quantified, `tools/ram_region_diff.py`):** 4:3-vs-16:9-no-reinclude = **4 gameplay
  bytes** (render pointers 0x800BF4F4/0x800BF544 only) -> widening the projection alone is gameplay 0-diff.
  4:3-vs-16:9-WITH-reinclude = **5638 gameplay bytes** (object structs @0x800EDxxx/0x800EExxx) -> poking +1 runs
  the handlers' VISIBLE branch (animation/state), perturbing gameplay. Confirms later-128; rules out poke-+1.
- **NATIVE PLAN (clean, no reconstruction):** in `ov_object_cull`, when an object is re-include-eligible (wide
  frustum), DON'T poke +1 - COLLECT the node. After the entity walk, call `gen_func_8003CDD8(node, 0)` per
  collected node. Renders the persistent per-node command list (camera x object composed by the flush) into
  the OT, touching only render scratch (OT/packet pool) -> gameplay 0-diff AND margin renders.
- **NEXT:** implement `engine/margin_render.{hpp,cpp}` (collect-in-cull + flush-after-walk), validate via rcmd
  that the +24 appear byte-identical AND `ram_region_diff` 4:3-vs-16:9 = ~0 gameplay bytes. Open risk to check
  empirically: whether a CULLED object's `cmd+0x2c` translation is stale (visible branch may update it) - rcmd
  byte-match is the test; if stale, refresh from node pos / call the transform-build first.
  Probe added: `PSXPORT_DEBUG=flush2` (taps `gen_func_8003CDD8`); rcmd now also logs `ra=` (dispatcher caller).

## later-134 — native widescreen margin IMPLEMENTED (engine/margin_render.cpp) + RE REPL tooling
Built the native margin renderer on the later-133 mechanism. Result: the +24 margin commands render with
the base world byte-identical and gameplay perturbation collapsed from 5638 B (poke) to 597 B (all render-
cache). Plus an RE REPL (debug-server commands) so future RE doesn't need a recompile-a-probe loop.

**`engine/margin_render.cpp` (C++/OOP `MarginRenderer`):**
- `ov_object_cull` (game_tomba2.c): when the wide frustum re-includes an object, instead of poking +1 it
  calls `margin_collect(node)` (default; `PSXPORT_MARGIN_POKE=1` = old +1 fallback). Filter: entity type
  `node+0xc == 0x03` (world-geometry) — later-133 proved ONLY type-03 re-included nodes actually render,
  and they reproduce EXACTLY the +24. Collecting all 58 re-include-eligible nodes over-rendered (+185).
- After the entity walk (`ov_objwalk`, engine_tomba2.c) → `margin_render_flush(c)`: per collected node,
  `gen_func_80051C8C(node)` (build the CURRENT transform — culled objects never had it built → otherwise a
  degenerate zero-rotation matrix) then `gen_func_8003CCA4(node)` (the per-object render dispatch). Touches
  only render scratch (node+0x98 matrix cache, cmd+0x18 transform, OT/packet pool) — no gameplay logic.
- **Verified (rcmd oracle, f2900, 16:9):** native = 124 cmds = 100 base + 24 margin. The base 100 are
  **byte-identical** to 16:9-no-reinclude (`comm` diff = 0 → zero perturbation). The +24 margin geomblks
  match the real re-include exactly; **translation byte-identical**; rotation matches for 16/24. The other
  8 are ANIMATED objects where the +1 poke ORACLE is itself perturbed (it ticks the animation), so native
  (frozen current state) is the correct value, not the poke — rcmd-vs-poke is NOT a valid oracle for those.
- **Gameplay gate (`tools/ram_region_diff.py`, 4:3 vs 16:9-native @f438): 597 "OTHER" bytes, ALL render-
  derived** — two clusters: node render-matrix cache (node+0x98..0xb5, e.g. 0x800f0524 = node 800f048c+0x98)
  and the command-struct transforms (cmd+0x18..0x3c, the 0x800f6exx..0x800f75xx command pool). NONE touch
  gameplay fields (position +0x2e/32/36, state, AI, timers). vs 5638 B for poke. The matrix is derived from
  (unchanged) position/rotation — building it earlier (while in the wide margin) has no gameplay feedback.
- **OPEN:** (a) classify node+0x98..0xc4 + the command pool as RENDER in ram_region_diff so the gate reads a
  clean 0 (they are render state living in the node/cmd pools). (b) The widescreen PRESENT still crops to the
  4:3 display region (engine_re "widen draw-env + clip rect to 428" TODO) — so a headless screenshot won't
  SHOW the margin yet even though it's in the OT; that's a separate present-pipeline task, not the margin RE.
  (c) confirm the 8 animated objects render at the correct (frozen) pose once present width is fixed.

**RE REPL (runtime/recomp/dbg_server.c, PSXPORT_DEBUG_SERVER=1, client tools/dbgclient.py):** added live
commands so RE no longer needs a recompiled one-shot probe — `w8/w16/w32 A V` (poke), `call A [a0..a3]`
(run a guest fn on the live CPU ctx via rec_dispatch, reports v0/v1 — e.g. test 0x80051C8C/0x80051B04
live), `ents` (walk both entity lists: addr type pos handler rflag cmds geomblk), `node A` (decode one
node), `geomblk G S` (model-table lookup). `dbg_server_service` now takes the frame `R3000* c` for `call`.
`runtime/recomp/r3000.h` is now `extern "C"`-guarded so engine/*.cpp link the C ABI symbols.

## later-134b — gameplay 0-diff PROVEN (node-aware diff) + headless FMV fix (probes 77s→1.4s)
- **`tools/node_diff.py`** (new): walks the live entity list (heads 0x800fb168/0x800f2624, stride 0xD0)
  in a RAM dump and splits each node's diff into GAMEPLAY fields (node+0..0x98) vs RENDER cache
  (node+0x98..0xd0). Verdict on 4:3 vs 16:9-native @f438: **GAMEPLAY 0-DIFF** across all 153 nodes (0
  bytes); the only node diffs are 127 B of render-matrix cache in 7 nodes; everything else (20144 B) is
  outside nodes = render pools + command-struct transforms + the 2 render-pool write pointers. So the
  native margin renderer perturbs ZERO gameplay state — resolves later-134 open item (a). This is the
  authoritative "gameplay 0-diff" gate (supersedes eyeballing ram_region_diff addresses).
- **Headless FMV fix:** the intro FMVs were played back in REAL TIME even headless → a field probe took
  ~77s. Now headless skips the intro FMVs (native_boot.c) and auto-uncaps any in-game FMV (native_fmv.c);
  a plain headless field probe is ~1.4s, deterministic (field @present-frame 328, rcmd=124), no flag.
  Standard recipe in docs/driving-the-game.md §0.

## later-135 — NATIVE per-object render FLUSH (`gen_func_8003CDD8`) — world render submission is now PC-native, 0-diff
The major world/margin render flush — the per-object transform composition + dispatch that turns each
visible object's persistent render-command list into projected geometry — is reimplemented in native C
(`engine/engine_submit.c` `submit_perobj_flush` / `ov_perobj_flush`, registered on `0x8003CDD8` in
games_tomba2_init). This is the core of "make it a PC game": for the dominant world path NO guest render
code runs — not `gen_func_8003CDD8`, not the dispatcher `gen_func_8003F698`, not `gen_func_800803DC`.
- **What it does (decoded byte-for-byte from the recomp body, later-133):** per render command in the
  node's persistent list (count `node+8`/`node+9`, cmd-ptr array `node+0xc0[i]`, geomblk `cmd+0x40`):
  compose camera-rotation (scratch `0x1F8000F8`→CR0-4) × object-local matrix (`cmd+0x18`, 3 cols at
  +0x18/+0x1a/+0x1c, halfwords @+0/+6/+0xc) via one MVMVA `0x4A49E012` per column; transform the object
  translation (`cmd+0x2c/0x30/0x34`) by the camera (MVMVA `0x4A486012`) + add the camera translation
  offset (`0x1F80010C/110/114`); load composed rot→CR0-4, trans→CR5-7; dispatch geomblk with OT base
  `*0x800ED8C8` (+`cmd[0x3f]*4` when `node[0xd]&0xf==4`) and the flush flag. The MVMVA math stays a
  platform primitive (gte_op → Beetle GTE) so the composed CR0-7 are bit-identical.
- **`native_dispatch` (replaces `gen_func_8003F698`):** the generic GT3/GT4 path (forced-flag / force-byte
  `*0x1F800234` / mode≥22 / a mode-table `0x80015268[mode]` entry == `gen_func_800803DC`) is owned
  natively via `native_gt3gt4` (replaces `gen_func_800803DC`: split geomblk+0 tri/quad counts, run the
  already-native `ov_submit_poly_gt3/gt4`). Per-scene OVERLAY submitter variants (other mode-table
  entries — the `0x8013xxxx` ones) are NOT owned yet → those modes still run their original per-mode
  renderer via rec_dispatch. That + the byte-packed `0x80027768` are the documented next RE targets
  (engine_re "OPEN — full field depth coverage").
- **0-DIFF GATE (the real verification, NOT a screenshot):** headless field, 16:9, `PSXPORT_VRAMDUMP`
  → native flush vs `PSXPORT_PEROBJ_RECOMP=1` (recomp body) → **VRAM byte-identical**. A/B flag:
  `PSXPORT_PEROBJ_RECOMP=1`.
- **CORRECTION on the gate frame (don't trust f328):** `PSXPORT_DEBUG=pdisp` (dispatch-coverage probe,
  engine_submit.c) revealed `gen_func_8003CDD8` / native_dispatch is NOT exercised until **s_frame≈393**
  — at the standard f328 the playable field is mid-LOAD and the world is drawn by the direct submitters
  (the byte-packed `0x80027768` etc.), not the per-object deferred flush. So the f328/f345 VRAM matches
  were VACUOUS (toggling PEROBJ_RECOMP changed nothing there). The VALID gate is a frame where the flush
  fires: **f410, native=17 dispatches/frame → VRAM byte-identical** native-vs-recomp. (Lesson: gate a lift
  at a frame where the probe confirms the lifted code actually runs, not just "the field is on screen".)
- **pdisp finding — the per-object world flush is FULLY native at the field:** every present frame f393+
  shows `native=17 fallback=0` — all 17 per-object flush dispatches take the native generic GT3/GT4 path
  (`native_gt3gt4`); ZERO fall to an unowned overlay-variant per-mode renderer. So no guest per-mode
  renderer runs in the field's per-object flush. (The bulk terrain comes via the separately-owned
  byte-packed `0x80027768`, a different submit path, not via `8003CDD8`.) Probe: `PSXPORT_DEBUG=pdisp`.
- **Relation to the margin (later-134):** the margin's `gen_func_8003CCA4(node)` call now routes its
  per-object submission through this native flush; the remaining guest calls (`8003CCA4` transform
  dispatch, `80051C8C` transform build) are gameplay-0-diff (node_diff) and are the next lift so the
  margin needs no guest render at all.
- **`gen_func_8003CCA4` (per-object render DISPATCH) now also native** (`submit_perobj_render`): stash
  current render object (scratch `0x1F80028C`), flush flag = `node[0xb]==0xf`, case idx = `node[0xd]&0xb`
  (≥9 = not rendered); the flush-only case (jump-table `0x80014ec8[idx]` target `0x8003CD00`) runs the
  native flush — NO guest render. The secondary-effect-pass cases (`8003D584`/`8003F344`/`8003F3F4`/
  `8003F4C4`/`8003F594`) super-call the recomp body (not owned yet). `PSXPORT_DEBUG=ccase`: at the field
  8003CCA4 fires 1×/frame, idx0 (flush-only) → owning it is complete for the field. VRAM byte-identical
  native (8003CDD8+8003CCA4) vs recomp @f410.
- **`PSXPORT_DEBUG=subcnt` finding:** the un-owned GT3/GT4 variants `0x8003B320`/`0x8003C8F4` fire ZERO
  times at the field (they belong to other scenes — the f560 image). So the field's render is fully owned:
  native per-object dispatch+flush + the byte-packed `0x80027768` (bulk terrain) + the GT3/GT4 library.
- **Phase-2 render driver MAPPED (`PSXPORT_DEBUG=rwalk`) — `gen_func_8003C048`.** It is the ONLY phase-2
  render-walk firing at the field (1×/frame → `8003CCA4`). Structure (shard_0): a linked-list walk —
  head `*0x800F2624`, per node: skip if `node+1==0`, else dispatch by `node+0xb` (<33) through a 33-entry
  jump table `@0x80014DB8`; advance `node = node+36`. Cases call `8003CCA4` (per-object render, OWNED),
  `8003F174` (minor static-decor flush), `8003EF9C`/`80039F4C`/`8003C2D4`/`8003C464`/`8003C5F8`/`8003C788`
  (resident render fns), several overlay renderers via rec_dispatch (`8012A43C`/`801295B4`/`80129114`/
  `8013DD58`/`8011BE5C`), a big inline case that builds a matrix from `node+96..118` + calls `80084660/
  80084690` then the GT3 variant `8003B320`, and a default `rec_dispatch(node+24)`. Owning it = replicate
  the walk + the 33-case dispatch (cases have non-uniform/inline arg setup) — a multi-step lift, NOT a
  clean single step; the per-object SUBMISSION (the meaty projection/packet work) is already native.
- **`gen_func_8003C048` (the master phase-2 render-list WALK) now OWNED** (`submit_render_walk`): native
  linked-list iteration (head `*0x800F2624`, next `node+36`), skip non-live (`node+1==0`), dispatch live
  nodes by `node+0xb` (<33) via the `0x80014DB8` table. Own-when-fully-handleable: pre-scan the live
  nodes; if every one resolves to an owned case run the native walk, else super-call the recomp body
  (unfamiliar scenes always correct, never a fragile partial). Field cases (`PSXPORT_DEBUG=rlist`): two
  live node-types — `t0→0x8003C0B4` (per-object render → native submit_perobj_render) and `t32→0x8003C29C`
  (default → `rec_dispatch(node, *(node+24))`, the node's own render fn). **VRAM byte-identical** native
  walk+dispatch+flush vs full recomp @f410. So the ENTIRE field phase-2 render path — list walk → per-
  object dispatch → camera×object transform → geometry submission — is now native C (the "entity-list
  iteration → render submission" engine layer from the project goal); the owned-leaf boundary is the
  node's render fn (`node+24`, terrain via the owned byte-packed `0x80027768`) + the per-object submitters.
- **`gen_func_8002AB5C` (field TERRAIN/map renderer) now OWNED** (`submit_terrain`) — the render fn
  (`node+24`) of the t32 render-list node, the bulk map geometry. Was interpreted-only (reached via
  fn-ptr); seeded into the RE set, decoded, ported. It is the per-object flush specialised for the
  terrain strip: set FarColor=0 + IR0 depth-cue factor `(128-node[78])<<5` @0x1F800090, compute two sway
  angle bytes @0x800A2014/2016, build the object matrix (euler `80085480` + secondary sway `80084520`,
  kept as primitives), compose camera×object via the same 3 MVMVA columns + translation as 8003CDD8, then
  submit the terrain geomblk `0x800A1AE8` via the already-owned byte-packed `0x80027768`. **VRAM
  byte-identical** native vs full recomp @f410 AND @f420; deterministic (2 runs identical). With this the
  ENTIRE field render — list walk → per-object dispatch → flush → terrain → submit — is native C.
- **`gen_func_80051C8C` (per-object TRANSFORM BUILD) now OWNED** (`build_xform`): init node+0x98 identity
  (0x1000 diagonal), 3 euler rotations (`80084D10/EB0/85050`, kept as matrix primitives), translation
  node+0xac/b0/b4 from position node+0x2e/32/36, propagate to the command struct (`80051464`).
  Interpreted-only → seeded + decoded + ported. VRAM byte-identical native vs recomp @f450 (where it
  fires), deterministic. This is the last guest call the margin makes.
- **CRITICAL BUG fixed (native_dispatch) — found by live gdb on the hung process.** A single-frame VRAM
  diff (f410) does NOT prove the game keeps RUNNING: full-native HUNG at ~f434 while recomp reached f600.
  gdb backtrace: `margin_render_flush → 8003CCA4(native) → submit_perobj_flush → native_dispatch →
  rec_interp(pc=0x1F49E010 garbage)`. Root cause: the mode-table `0x80015268[mode]` entries are the
  dispatcher `gen_func_8003F698`'s OWN internal case-label addresses (`0x8003F6xx`), NOT renderer fn
  pointers; my fallback `rec_dispatch(tgt)` jumped into the MIDDLE of 8003F698 → ran garbage. Dormant at
  the field (always generic, `fallback=0`); the margin (mode=0, flag=0) triggered it. Also my "generic"
  sentinel was wrong (`0x800803DC` vs the real label `0x8003F788`). Fix: for unowned modes run the REAL
  dispatcher `gen_func_8003F698` (it owns its internal jump table); native fast-path only when provably
  generic (`tgt==0x8003F788` / force / flag&1 / mode≥22). After the fix full-native reaches f600, VRAM
  0-diff at f410 + f450. **LESSON: verify the game PROGRESSES (runs to f600), not just one frame's pixels.**
- The full FIELD render path is now native C end-to-end (walk → dispatch → flush+native_dispatch →
  terrain → transform-build → owned submitters), 0-diff vs recomp, no hang, deterministic. Per-override
  A/B gates added: `PSXPORT_NO_{FLUSH,DISP,WALK,TERRAIN,XFORM}=1`.
- **Widescreen-visibility finding (don't re-chase):** at the standard headless field frames, 16:9 s_vram
  is BYTE-IDENTICAL to 4:3. That is EXPECTED, not a bug: `ov_set_geom_offset` deliberately keeps OFX=160
  (widening the shared GTE CR24 corrupts the game's own RTPS read-backs), so widescreen is done by the
  WIDE CULL in engine_submit (`submit_xmax`→428, keep geometry projecting into [320,428]) — NOT a wider
  FOV. So 16:9 differs from 4:3 ONLY when the scene actually has geometry in the [320,428] margin band;
  the auto-gameplay field frames tested have none. The render-submission ports are verified faithful
  regardless (0-diff vs recomp in BOTH aspects). True wider-FOV widescreen would need the native-projection
  path (proj_native_vertex, memory note) wired into the owned submitters — a separate design step.
- **Remaining render work is NOT clean recomp-sourced ports anymore (assessed this session):** the
  per-mode renderers the dispatcher delegates to (`0x80146478` mode0, `0x80132DC0`, `0x8013xxxx`…) are
  OVERLAY/interpreted (not in MAIN.EXE recomp) → need the overlay-scan mechanism or RAM RE; the camera
  matrix builder `gen_func_800939A0` is a 308-line subtree with 8 callees and is camera-CONTROL coupled
  (gameplay-scope — keep recomp). So the next steps need a direction decision, not just another lift.
## later-136 — per-mode overlay renderers: their geometry was ALREADY native; own the generic-caller wrapper too
Investigated the dispatcher's per-mode renderers (the modes native_dispatch delegates to the guest
dispatcher). `PSXPORT_DEBUG=pdisp` past f434: the ONLY fallback mode that fires is **mode 0** (renderer
`0x80146478`), heavily — up to **90 dispatches/frame** at f497-553 (a render layer on top of the field).
- **The mode-0 renderer `0x80146478` is structurally IDENTICAL to `gen_func_800803DC`** (disassembled from
  a f500 RAM dump via tools/recomp/decode.py): a generic GT3/GT4 caller — `lw counts,0(a0); a0+=16;
  jal <gt3>; jal <gt4>`. Its two submitters `0x801465EC`/`0x801467BC` are **ALREADY OWNED** by the
  overlay-scan (`PSXPORT_DEBUG=submit`: "own overlay GT3/GT4 @ 0x801465EC/0x801467BC"). So mode-0's 90
  prims/frame were **already natively submitted with native depth** — the meaningful part was done.
- **Owned the thin generic-caller wrapper too** (engine_submit.c): extended `engine_scan_overlay` to detect
  the caller signature (`addiu a0,a0,16` + `andi a2,s0,0xffff` + `srl a2,s0,16`) and register
  `ov_gt3gt4_caller` (→ `native_gt3gt4`). Owns `0x80146478` (and any overlay's generic caller) on load.
  **VRAM byte-identical @f500 caller-native vs interpreted** (A/B `PSXPORT_NO_CALLER_OWN=1`); runs to f600.
- So the per-mode renderer ownership is complete for what fires: mode-0 (the only one) is fully native
  (caller + submitters). Tooling for RE'ing overlay fns: dump RAM (`PSXPORT_RAMDUMP_FRAME`) + decode.py.

## later-137 — native-depth COVERAGE measured: faithful render fully owned; enhancement gap is non-field scenes
Measured the native-depth coverage across the whole auto-gameplay (`PSXPORT_NATIVE_DEPTH=1
PSXPORT_DEBUG=ndepth`, made/hit/miss per present frame):
- **484 frames have miss=0** — the field is FULLY native-depth (owned submitters → NativePrim.pz, no
  projprim miss). `made=0` everywhere is EXPECTED: with the native display list (ndl_active, default) the
  owned submitters store depth in `NativePrim.pz`, not via `projprim_set_pz` (which counts g_pp_set) — so
  the meaningful metric is g_pp_miss (prims with NO native depth → fall to the 2D band).
- **~half the frames (the non-field scenes, e.g. f497-553) have a 20-32 prim/frame depth GAP** — guest-
  packet-writing submitters that aren't owned. `0x8003C8F4` is one contributor (`PSXPORT_DEBUG=subcnt`:
  3-7/frame at f440-490) but NOT the bulk; other interpreted guest-packet submitters supply the rest.
- **`0x8003C8F4` assessed, deprioritised:** it is a 196-line per-mesh QUAD renderer (per-vertex unpack
  `8003B220`, packet color/uv build `8003B054`, a 33-entry type jump table @0x80014E40, writes full guest
  packets to the pool + OT). Its cull is HARDCODED 320 (4:3 — not wide-aware). A faithful port is ~terrain
  complexity but adds NO native depth (the packet is built by `8003B054`); native depth needs reimplementing
  that too. Low ROI (3-7 prims/frame). Recompiled (shard_6) so it's 0-diff portable when prioritised.
- **Conclusion:** the FAITHFUL render (default) is comprehensively owned + 0-diff across all traversed
  scenes; the native-depth ENHANCEMENT (off by default) has a residual gap only in non-field scenes,
  closable by porting the remaining guest-packet submitters with the ndl_active path — a multi-function
  project, not a single lift. Next session: identify the BULK miss contributor (tap the un-owned submitters
  feeding the 2D band in f500) before porting.

## later-135 NEXT (options, need a pick):
- (a) native wider-FOV widescreen via proj_native_vertex in the owned
  submitters (the real visible payoff); (b) own the per-mode overlay renderers via overlay-scan; (c) drive
  to scenes using `0x8003B320`/`0x8003C8F4`/
  overlay `0x8013xxxx` and port those submit variants. (3) own `gen_func_80051C8C` (transform build,
  interpreted-only — RE from RAM). (4) native widescreen margin (replace the guest-flush margin_render.cpp).
- Probes added this session: `PSXPORT_DEBUG=pdisp` (dispatch coverage), `subcnt` (submitter call counts),
  `ccase` (8003CCA4 case histogram), `rwalk` (phase-2 walk caller). All gated, zero cost off.

## later-138 — HOST-MEMORY render conversion (the core directive: read guest, NEVER write guest)
User directive (ground truth, observed live via `./run.sh` 4:3+VK+native-depth): the native render
**corrupts both gameplay and visuals** (character models explode into stretched tris; gameplay diverges)
because it WRITES guest memory that the still-recompiled guest logic reads. The PC engine must live in
HOST memory — read guest, never modify it. GTE control/data regs are HARDWARE (not guest RAM) → writing
CR0-7/data for projection is fine. "Faithful-first byte-identical guest writes" is the WRONG methodology
here. (Diagnosis confirmed the divergence is config-induced — VK_HEADLESS is deterministic per-config but
changing the render config changes the gameplay scene reached → the render alters guest state. The native
submitters (native_dl) write the guest OT/pool differently than recomp; that is the gameplay-diverging
write. Memory: [[engine-host-memory-never-write-guest]].)

**Converted to host this session (committed):**
- `submit_perobj_flush` COMPOSE: camera×object transform now in host C locals (`uint16 hm[9]` + trx/try/trz)
  instead of the PSX scratchpad 0x1F800000. Renders identically.
- `submit_terrain` COMPOSE: same host conversion.
- `submit_terrain` sway-angle temps: host (drop the 0x800A2014 main-RAM writes). Runs to f800.

**STILL writing guest (the remaining "port more to host" work, in impact order):**
1. **THE main corruption — the submitters' guest OT/pool** (native_dl): each owned prim writes a 1-word OT
   node + advances the pool ptr 0x800BF544 (guest). This is what diverges gameplay. FIX = native ORDERING:
   submitters append the NativePrim to a HOST per-bucket list; `ov_draw_otag` renders the host 3D list +
   walks the guest OT for the game's inline 2D prims. OPEN risk: the 3D/2D OT interleave order (the game
   builds 2D in the same OT by bucket) — needs care so 2D HUD/shadows stay ordered vs 3D. This is the
   documented [[tomba2-native-display-list]] "native per-bucket ordering" next step.
2. terrain: IR0 stage 0x1F800090 (read by the 0x80027768 submitter) + the SCR matrix-build args; the
   matrix-build sub-fns 80085480/80084520 write SCR — porting them host (~euler→matrix, ~350 lines incl.
   80084D10/EB0/85050) removes those.
3. `build_xform` (80051C8C): writes node+0x98 matrix (read by the flush via cmd+0x18/80051464) — needs the
   matrix to flow host (build into a host map keyed by node; flush reads host).
4. `submit_perobj_render`: 0x1F80028C "current render object" — read by un-ported guest per-mode renderers;
   remove when those are host.

## later-139 — NATIVE ORDERING: owned submitters write ZERO guest OT/pool (the per-bucket host display list)
Implemented the handoff's item #1. The owned GT3/GT4/GT4bp submitters (engine_submit.c, ndl_active path)
no longer write the guest OT (the 1-word link node) or advance the guest packet-pool ptr 0x800BF544.
Instead each owned prim is appended to a HOST per-bucket display list keyed by its OT bucket-ANCHOR
address (`otaddr = ot_base + idx*4`), and the DrawOTag walk renders a bucket's host prims when it visits
that anchor node. RULE #0 advanced: the dominant render path (190 prims/frame) now writes no guest memory.

**Design (native_dl.{h,c} + gpu_native.c `ndl_render_node` + the 3 submitters):**
- `NativePrim` re-keyed: `.node` is now the bucket-anchor phys addr (was the per-prim pool node addr);
  added `.bnext` (intra-bucket LIFO chain). `ndl_alloc(otaddr)` prepends to that bucket's chain;
  `ndl_lookup(addr)` returns the bucket head; `ndl_next` walks the chain.
- Ordering is byte-identical to the old guest-OT merge by three facts: (1) ACROSS buckets the walk order
  (back→front) is unchanged; (2) WITHIN a bucket the guest AddPrim prepends (LIFO, most-recent first), so
  we prepend + render head-first; (3) **MIXED=0** (`PSXPORT_DEBUG=bucket`, measured: 77 owned-only + 6
  guest-only buckets, 0 mixed) — a bucket holds EITHER host-3D OR guest-2D, never both, so host prims at
  the anchor + guest 2D prims at their own pool nodes need no intra-bucket merge.
- Submitters: dropped `mem_w32(pkt,…)`/`mem_w32(otaddr,pkt)`/`pkt+=4`; the pool-ptr writeback is now
  `if (!dl) mem_w32(PKT_POOL_PTR, pkt)`. A0=`ndl_alloc(otaddr & 0x1FFFFC)` keyed to match the walk's
  masked addr.

**VERIFIED (not a vibe):**
- Render byte-identical: VRAM dump @f410 native-ordering == `PSXPORT_DL_GUESTPKT=1` (full-guest-packet),
  `cmp` = IDENTICAL.
- Gameplay 0-diff vs FULL recomp (SUBMIT/PEROBJ_RECOMP + NO_FLUSH/DISP/WALK/TERRAIN/XFORM + OT_RECOMP):
  `tools/node_diff.py` @f420/f440/f500/f560 = GAMEPLAY 0-DIFF every frame (only render-pool/cmd/global
  bytes differ). Native f470 screenshot is pixel-for-pixel the same dark/underwater scene as recomp f470
  (the "spiky tris" I first suspected were corruption are the real scene, present identically in recomp).
- Runs to f800 (`PSXPORT_DEBUG=stage`), no hang.

**HONEST CORRECTION to the handoff premise:** the handoff (+ memory note) claimed the submitter OT/pool
writes were "THE main corruption." In the headless AUTO_GAMEPLAY path that is NOT demonstrable: node_diff
shows `PSXPORT_DL_GUESTPKT=1` (submitters writing FULL guest packets) is ALSO gameplay-0-diff vs recomp at
f500. So the OT/pool writes do not measurably diverge gameplay in the traversed scenes — removing them is
correct per RULE #0 and regression-free, but it is NOT proven to be the fix for the user's interactive
corruption (which the deterministic headless path doesn't reproduce in any config). Whether it resolves
the on-screen exploding-tris needs a user `./run.sh` observation. Remaining host conversions (#2 terrain
scratchpad 0x1F800090 + SCR matrix-build, #3 build_xform node+0x98, #4 submit_perobj_render 0x1F80028C)
are the next writes to move host. A/B: `PSXPORT_DL_GUESTPKT=1` reverts to guest packets.

## later-140 — OWN the present/compose pipeline: 2D screens present from VRAM, not the empty scratch FB
User report (real ./run.sh, ground truth): EVERYTHING on screen is broken — SCEA, FMVs, menu, gameplay.
My headless shots looked perfect. Root cause found by reproducing the user's EXACT config: their persisted
`psxport_settings.ini` has `ires_auto=1` → internal-res 2-3 → `use_fb()` (the hi-res scratch-FB path). My
headless shots ran ui=0 (no settings load) → ires=1 → the simple path. DIFFERENT RENDER PATH. (The "good
shots while screen is broken" was the key clue — the user spotted it.)

THE BUG (gpu_gpu.c present/compose): the scaled scratch FB (rows ≥ FB_Y0) holds ONLY tee'd geometry (the
vertex shader relocates it there when use_fb). Pure-2D screens (SCEA/FMV/title/menu) are VRAM-RESIDENT —
an uploaded image displayed via the display area, NO tee'd geometry — so the scratch FB is EMPTY for them,
yet the present sampled the scratch FB whenever use_fb() → black/garbled 2D screens. The 4:3-pillarbox path
that should have caught this was gated on `gpu_gpu_wide_engine()` (wide only), so 4:3 + hi-res fell through.

THE FIX (own the FB-vs-VRAM decision with ONE consistent predicate): `frame_via_fb() = use_fb() &&
gpu_seen3d_this_frame()` — a frame's content is in the scratch FB only when hi-res/wide is configured AND
the frame actually drew 3D. Added `gpu_seen3d_this_frame()` (gpu_native.c) = THIS frame's s_seen3d (the OT
walk tees before present, so it's final — no 1-frame lag, unlike the old s_prev_had3d pillarbox gate).
Replaced use_fb() with frame_via_fb() at every render+present decision site: the scratch-FB clear +
push_wide relocation (panel_render), the present sample region + aspect (2D frame → native VRAM region at
4:3; 3D frame → scratch FB at the selected aspect), the headless readback, the SSAO screen map, and
gpu_gpu_dump. So relocation and present always agree.

VERIFIED (headless, real-disc boot path): SCEA @ires=2 — was text shoved bottom-left + mostly black; now
centered + complete. 3D field @ires=2 — renders hi-res 640×480 crisp from the scratch FB (unchanged).
ires=1 path BYTE-UNCHANGED (frame_via_fb==false at 4:3/ires1, same as old use_fb==false) → headless 4:3
tooling unaffected. Runs to f800 @ires=2, no hang.

IMPORTANT framing correction (later-138/139 were chasing the wrong thing): this breakage is 100% host-side
C++ in gpu_gpu.c — it never touches guest memory. So "writing guest memory corrupts the game; move
everything to host to fix it" (the later-138 handoff premise) is FALSIFIED for the on-screen corruption.
The corruption is in the native render/present layer itself. SEPARATE STILL-OPEN issue: the ocean
exploded-geometry (spiky stretched tris) reproduces even at ires=1 with ALL native overrides off (pure
interpreter) — so it's a 3D render/geometry bug (rasterizer/GTE) or a real scene, to be settled by
gpu_differ + the Beetle oracle (the mandated render-diff-first path). NOT guest-memory, NOT interp-vs-recomp
(verified 0-diff, later-103). Do not "switch back to the recompiler" — it would change nothing here.

## later-141 — OOP regression SOLVED: cooperative task context must save REGISTERS only, not the whole Core
**Symptom:** after the OOP refactor (c344d16), the headless AUTO build stalled at stage START
(task0 perpetually st_in=3, sm48 stuck at 0) and never advanced to DEMO/GAME/field. The parent
(05441fb) reaches the field at f328; the OOP build never left START. Previously flagged "unresolved"
([[oop-regression-hunt]]); the earlier "frame-1 RAM divergence / native-call I/O identical for 20
frames" reads were measured in the REPL+faithful config, which doesn't exercise this path.

**Diagnosis (this session, deterministic — no sleep-based dumps):**
1. Stage trace: parent task0 START advances sm48 0→1→2→3 and YIELDS (st_in=2) each frame; OOP task0
   restarts START fresh every frame (st_in=3, sm48=0), never yields. Frame-0 divergence, NOT 20 frames in.
2. Faithful mode (overrides OFF) also stalls → pure OOP-CORE regression, not an override.
3. `PSXPORT_NCALL_TRACE` (plain AUTO headless, NO REPL) on both builds: calls 0..3725 byte-identical;
   first divergence at call 3726 — `FUN_8001dc40` (intro file loader) gets **a1=LBA 0 (OOP) vs 0x19A5
   (parent)**, same dest. An INPUT divergence ⇒ a memory side-effect diverged earlier.
4. Frame-0 RAM diff located it: the file-directory table of (LBA,size) pairs at **0x800BE0F0** is fully
   populated on parent, ALL ZEROS on OOP at the loader-call moment.
5. `PSXPORT_WWATCH` + cd-verbose: table IS filled at pc 0x80106630 (task0 START) on BOTH, in the same
   order, yet the loader (FUN_80044F58, ra 0x80044FD8) still reads 0x800BE0F0 as 0 on OOP. The loader is
   a SEPARATE task — so task0's write was invisible to the loader task.

**Root cause:** `class Core : public R3000` embeds guest memory BY VALUE (`ram[0x200000]`, `scratch[0x400]`,
`s_dma_buf[0x10000]`, DMA regs). The cooperative-scheduler context in native_boot.cpp saved/restored a whole
`Core` (`static Core g_task_ctx[3]`; `Core loop=*c`; `g_task_ctx[i]=*c`; `*c=g_task_ctx[i]`). Pre-OOP that
struct was register-only (R3000), so the save/restore touched only CPU regs and guest RAM stayed shared.
After OOP it snapshots ALL 2MB of RAM per task — so **each task runs on its own stale RAM snapshot**: task0
(START) filled the file table, but the loader task restored a pre-fill snapshot and read LBA 0 → loaded
garbage → stage machine's file-table-driven progression never advanced → stall before the field.

**Fix:** the task context is the CPU register file ONLY (guest memory is shared, single). Changed
`g_task_ctx` to `R3000` and slice to the R3000 base on save/restore (`static_cast<R3000&>(*c)`), leaving
`c->ram` untouched. native_boot.cpp only.

**Verified:** OOP build now START→DEMO→GAME→field at **f328 (identical to parent)**; field renders
(scratch/oop/shots/oop_fixed_field_f470.png); two identical runs byte-identical at RAM@f350 (deterministic).
This UNBLOCKS the top-down engine rewrite (the handoff's mission) — the field is reachable on the OOP Core
substrate again, no revert of c344d16 needed.

## later-142 — hi-res "override corruption" SOLVED: it was a SHADER bug (semi-blend dest read), NOT the override system
**The handoff's central premise was FALSIFIED.** The handoff claimed the hi-res garble (ires≥2, overrides
ON = black-left + texture-noise; scratch/oop/shots/fix_on_ires2.png) was caused by the override system —
the interpreter jumping UP into native overrides mid-frame, render side-effects interleaving with guest
execution — and that the cure was a big "top-down engine" rewrite (replace overrides, engine drives the
frame, drains a render-command list natively). That diagnosis was wrong. The corruption is a localized
**Vulkan fragment-shader bug** in the hi-res scratch-FB present path, with nothing to do with override
timing or reentrancy.

**How it was found (render-diff-first, per gfx-debug):**
1. Reproduced ON vs OFF @ires=2. KEY REALIZATION: the "overrides-OFF = correct" baseline is **320×224**,
   ON is **640×480** — they are NOT the same render path. OFF (PSXPORT_FAITHFUL_DEPTH) → `native_depth_on()`
   false → `s_seen3d` never set → `frame_via_fb()` false → present plain 4:3 from VRAM. The hi-res scratch-FB
   path is ONLY exercised by overrides-ON; "OFF is fine" just means the 4:3 VRAM path is fine. The comparison
   in the handoff was misleading.
2. **Parent worktree (05421fb, pre-OOP) produced BYTE-IDENTICAL garbage** → not an OOP regression, not
   override-timing reentrancy (deterministic, same on both builds). (later-140's "ires=2 crisp" must have
   been a non-ocean frame; the ocean/underwater field scene was always broken at hi-res.)
3. **overrides-ON @ires=1 (no scratch FB) renders the ocean CORRECTLY** (scratch/oop/shots/on_ires1.png) →
   native geometry submission is right; ONLY the hi-res relocation/scratch-FB render is broken. The bug is
   100% in `push_wide` relocation + the scratch-FB present, NOT in what we submit.
4. Added `PSXPORT_VK_FULLSHOT=frame` (dumps the ENTIRE panel image, VRAM rows 0..511 + scratch FB rows
   512..IMG_H) — confirmed the SW framebuffers (0,0)/(0,256) are black at ires=2 ON (native 3D tees ONLY to
   VK, not SW VRAM) and the scratch FB (rows 512+) held the garble that gets presented.

**Root cause (tritex.frag):** `vram_at(x,y)` masks `y & 511` — correct for TEXTURE sampling (PSX textures
live in VRAM rows 0..511) but the SAME helper was used for the **semi-transparency blend DESTINATION read**
(`uint d = vram_at(px,py)` at `gl_FragCoord`). When `push_wide` relocates 3D geometry into the scratch FB
(rows ≥512; the tee'd geometry's `gl_FragCoord.y` is 512..991), `py & 511` wrapped the destination read back
to rows 0..479 — i.e. semi-transparent prims (the ocean water blends) read the **texture atlas** as their
blend destination → texture-looking garbage. Opaque prims were fine (they don't read the dest); that's why
some of the scene survived and the rest was scrambled.

**Fix:** split the framebuffer destination read from texture sampling — new `fb_at(x,y)` = `texelFetch(u_vram,
ivec2(x&1023, y), 0)` (NO y-mask; `s_vram_tex` is `IMG_H`=1232 tall, so rows 512+ are real framebuffer).
`vram_at` keeps the 0..511 wrap for textures. tritex.frag only.

**Verified (headless, real disc, field f470):** ires=2 ON ocean now renders crisp 640×480 in the scratch FB
(scratch/oop/shots/final_on_ires2.png) — bright water left, dark cliff + Tomba right, matching the ires=1
reference scaled 2×. Widescreen 856×480 @ires=2 also correct (fix1_wide_ires2.png). **ires=1 present is
BYTE-IDENTICAL to pre-fix** (`fb_at`==`vram_at` when py<512, so the 4:3/non-FB path is untouched) — no
regression. gpu_differ/replay_ours unaffected (SW rasterizer, doesn't use these shaders).

**Implication for the mission:** the user's stated end-goal (top-down native engine) still stands as
architecture, but the *bug it was supposedly needed to fix* is fixed without it. The "override system
corrupts at hi-res" justification is gone — the rewrite, if pursued, is for the architectural reasons
(engine owns the frame), not because overrides cause this corruption. Don't cite this corruption as the
driver anymore.

---

## later-150: boot→cutscene grind — LANDSCAPE CORRECTION (the boot2cut.funcs head is NOISE)

Continuing the hand-written-native-C++ boot→cutscene port (handoff: scratch/handoff.md). Before grinding
I characterized the 585-entry burn-down list (`scratch/trace/tripwire.funcs`, PSXPORT_INTERP_FUNCS) and
found the literal "port boot2cut.funcs top-down from #1" plan is **based on a polluted list head**:

- **The first ~211 entries have NO real recompiled body**, and the top ~40 (the `0x80018xxx` cluster) are
  **data, not functions**. Static MAIN.EXE bytes there == live RAM (no overlay/self-modify), and they decode
  as a const/dispatch table (pairs `{0x20020000, smallint}`, embedded ptrs to 0x80018CCC, stray
  `syscall`/`break` words). The recompiler (`tools/recomp/tomba2_funcs.txt`) finds **zero functions in
  0x80010000–0x8001CB00** — that whole prefix is the .rodata/const region. Real functions start at 0x8001CB00.
- **Decisive proof they don't execute:** a full boot→prologue run hits **exactly ONE `break`** (`[break]
  code 1`, the terminal crt0 break), not the 40× `break code 3` that executing the `0x80018xxx` table would
  produce. So those addresses are *recorded* as indirect-call targets by the tripwire (`ifn_record`) but the
  interpreter never actually runs code there. They are false positives that inflate the metric by ~80–150.
- **No-body distribution** (356 of 585): ~150 in 0x80011–0x8001B (the const-region noise), **~42 in
  0x80106–0x80109 (the REAL overlay DEMO-menu/GAME-prologue stage code** — loaded at runtime so absent from
  static MAIN.EXE; RE these from a live overlay RAM dump, e.g. `scratch/bin/tomba2/ram_menu.bin`), rest scattered.

**Corrected top-down order = `ov_game_main`'s init-prefix call list** (`runtime/recomp/native_boot.cpp`
~276–302: rc0/rc1/.. of 0x80089788, 0x80085b20, 0x800898a0, 0x80080bf0 …), NOT the tripwire first-seen order.
That list is the real boot call-tree root; walk it (and callees) to port in genuine top-down order.

**Function shapes** (229 of 585 have real gen_func bodies): 15 BIOS-vector thunks (`li t1,N; j 0xA0/B0/C0`
→ route to `recomp_hle`), **69 clean leaves** (no jal/jalr/BIOS — the easy clean burn-down), and ~199 real-code
funcs. Many of the real ones are **indirect-dispatch (`rec_dispatch(c, c->r[2])` through a guest object's
vtable** at e.g. +12) — porting those to "clean native C++" needs object-model RE, not transcription. The
clean leaves' native form is necessarily near-identical to the recompiled body (a memset is a memset).

**Done this session (later-150):** ported the first clean-leaf batch into `engine/native_path.cpp` (12 fns:
3× word-zero-fill 0x800861BC/86320/865C8, memset 0x80083AF8, EnterCritical 0x80080890, the 0x800ABE20
get/set pair 0x80086604/865F0, struct-inits 0x80083BF0/0x80051794, global stores 0x8009A480/0x80096370/
0x800963A0). Burn-down **585 → 573**; still `[autonewgame] reached GAME` at the identical frame 39 (no
divergence). Per-fn RE = its `gen_func_<addr>` body (recompiler decode is the reference, per user "don't
overrely on capstone"); trailing post-`jr ra` writes in those bodies are recompiler over-run, ignored.

**Tooling note:** `scratch/bin/tomba2/main_ram.bin` is a 2MB RAM-layout view of MAIN.EXE (body at file
offset 0x10000) so `tools/disasm.py <main_ram.bin> <kseg0_addr> <end>` resolves 0x800xxxxx directly. But
prefer the gen_func bodies over raw disasm.

## later-151: subagent leaf batch (32 fns) + A/B RAM-equivalence GATE established

Parallelized the clean-leaf grind across 3 subagents (engine/native_path_a1/a2/a3.cpp, each a
games_native_path_aN_init wired into games_native_path_init; files added to build_port.sh + run.sh).
32 leaves drafted. Then VERIFIED each against the interpreter with an A/B RAM diff — the real gate, since
"reaches frame 39" is too weak for audio/math leaves not exercised by boot.

**A/B harness (now the standard correctness gate for ported fns):**
- Determinism first: two identical headless runs (`PSXPORT_AUTO_NEWGAME=1 PSXPORT_REPL=1`, `run 50; dumpram`)
  → byte-identical 2 MB RAM. Confirmed deterministic.
- Isolate a batch's effect: build WITH the ports vs WITHOUT (comment its init call), dump RAM at the same
  point, `cmp -l`. A ported fn that's behaviorally equivalent to the interp body ⇒ **0 byte diff**.
- Per-fn classification: enable one registration at a time (zsh: use an ARRAY `A=(...)`, not `$VAR` — zsh
  doesn't word-split unquoted vars, which silently no-ops the loop), diff vs the all-interp baseline.

**Result: 25 of 32 verified 0-diff (kept, enabled). 7 had real transcription bugs → DISABLED (left
interpreted; correct, just not-yet-native) with `// TODO(verify):` notes naming the diff size + cause:**
  a1: 800752B4 (167B, stride-12 band table @0x800BE238), 80097540 (2B, return edge case)
  a2: 80090160 (80B, varint accumulate), 80082240 / 800822D8 (9B each, GP0 0xE3/0xE4 clip-word arg)
  a3: 80094C10 (15B, fixed-point mixer/pan), 80077FB0 (4B, 16-bit sqrt → cascades to 0x800E806x)
The bugs were all in the functions the subagents THEMSELVES flagged as highest-risk (good self-reporting),
and were ~all address-computation / edge-case errors. 80075E04 kept: its only diff (2B @0x801FE980) is its
OWN stack frame slot (sp-=8; save r10; read back; freed on return) — provably dead, correct return value.

**Lesson:** subagents are viable for this mechanical transcription IF every output passes the A/B gate —
do NOT trust "compiles + reaches prologue". The 7 buggy ones are the fix-queue (re-derive from gen_func,
re-A/B to 0-diff, then re-enable). Burn-down 543 → 505. Reaches prologue at frame 39, A/B clean (1 benign).

## later-152: non-leaf phase begins (batch b1, 15 fns) + clip fixes + non-leaf A/B note

Clean-leaf supply exhausted (only the 7 quarantined buggy leaves remained). Started the NON-LEAF phase:
functions whose static callees are ALL already native (portable leaf-up; no indirect dispatch). 34 such
fns identified. Ported the first 15 (engine/native_path_b1.cpp): memset/strncmp wrappers (call 0x8009A420/
0x8009A640 via rec_dispatch), 0x80097E40 id-pair callers, a GTE matrix×vector op (0x80084470), and a few
global-clear+sub-call init fns. A native override invokes a sub-fn via `rec_dispatch(c, addr)` (same as
native_boot's rc0/rc1) — routes to the callee's override or interp. Burn-down 505→487, prologue at frame 39.

**Non-leaf A/B gate note (important, recurs for EVERY non-leaf port):** a non-leaf native override is
frame-less, but the gen body has a prologue (`sp-=N; *(sp+16)=r31; …`). So the A/B RAM diff shows benign
diffs in the STACK region (0x801FExxx/0x801FFxxx): the gen saved r31 (a 0x800xxxxx return addr) to its own
frame slot, the override leaves stale stack — dead after return (confirmed by hex: interp=0x8008BAF8 etc.,
native=stale). **For non-leaf ports, A/B-accept stack-region (>=~0x801F0000) diffs; only flag diffs in
globals / pool (0x800Bxxxx) / scratchpad (0x1F80xxxx).** b1's 15 fns: only stack diffs ⇒ verified clean.

Also fixed+re-enabled the two clip-word builders (later-151 quarantine): bug was 32778<<16=0x800A0000 not
0x800F0000 (limits read from wrong base). 7-fn fix-queue → 5 left: 800752B4(167), 80097540(2-byte stack
cascade), 80090160(80, wrong stream cursor → cascade), 80077FB0(4), 80094C10(15). Burn-down now 487.

## later-153: non-leaf phase continued (b1+b2 = 23 fns) — burn-down 487→474; STACK-ARG gotcha

Ported up the ov_game_main init-prefix call tree: descriptor/viewport setup (80050738 = 4× 80083B30/
80083BF0 builders), GTE matrix×vector ops (80084470/80084250), strlen, table-search (8008BEAC via
strcmp), fixed-point lookups, large field/array inits. Sub-calls via rec_dispatch(c, addr). All A/B-
verified (non-leaf stack-frame-slot diffs accepted; globals/pool 0-diff).

**STACK-ARG gotcha (recurs for any non-leaf port that passes a 5th+ arg):** the callee reads arg5 at
sp+16. A frame-less override that writes c->r[29]+16 corrupts the CALLER's frame → hangs the interpreted
caller (80050738 hung the init prefix this way). FIX: replicate the gen's frame alloc — `c->r[29] -= N`
at entry, write the stack arg to the decremented sp+16, `c->r[29] += N` before returning. (Register args
a0-a3 need no frame; only stack args do.) Caught by the gate as a hang (timeout, never reaches GAME).

## later-154: non-leaf batch b3 (10 fns) — burn-down 474→464; portability-scan false-positive caught

Continued the non-leaf phase. Rebuilt the portability scanner (scratch/portscan.py: a fn is portable
when every static `func_<addr>` callee is an ENABLED override and the body has no `rec_dispatch(c, c->r[N])`
indirect dispatch). **Gotcha caught: the first scan over-counted because the override set was grepped from
all `rec_set_override(0x…)` source lines including the COMMENTED-OUT quarantined ones** (a1/a2/a3's 5
disabled fns). Filtering commented lines dropped two bogus candidates (80099310→80097540, 8006D02C→80077FB0
depend on still-interpreted quarantined leaves). Net: 11 clean on-path candidates.

Ported 10 into engine/native_path_b3.cpp (deferred 800977C0, a complex split/coalesce block allocator —
too error-prone to transcribe blind, do later with care):
- 800753AC, 80045080: thin wrappers around 0x8001DC40 (global-offset / stride-8 table lookup)
- 800834A0: call 0x80085900(-1), store ret+240
- 80051F80: descriptor poke + 0x80080880
- 8009C9D0: GPU packet finalize (call 0x8009CAEC, poke 5 reg cells via the 0x800AD0xx ptr table)
- 80089BAC / 80089E1C: 4-attempt retry wrappers around 0x8008AC34 (E1C also tails 0x8008A6EC)
- 800796DC: 104B control-block init + ~30 scratchpad clears + 0x800782F0/508A8/5082C
- 8007E9C8: GPU-prim build into the 0x800BF544 pool + OT link; **STACK-ARG** to 0x80083DE0 at sp+16 →
  replicated the gen 40-byte frame (c->r[29]-=40; write sp+16=0; …; +=40). No hang ⇒ correct.
- 800798F8: seed 5 contiguous object arrays (memset+link+tag per entry, last next=0), record base/count
  globals, wire 3 scratchpad list-head pairs.

**A/B gate (scratch/abrun.sh + scratch/abdiff.py, now committed helpers):** AUTO_NEWGAME=1 + REPL, run 50,
dumpram, cmp. Aggregate WITH-vs-WITHOUT diff = **20 bytes, ALL in stack region (>=0x801F0000)**, 0 in
globals/pool/scratchpad/low-RAM ⇒ all 10 equivalent. Determinism reconfirmed (final re-run byte-identical).
Both builds reach GAME prologue at frame 39. (NOTE: AUTO_NEWGAME=**2** + REPL deadlocks the run-budget loop
— the auto-pause blocks frame advance; the A/B harness must use =1.)

**DO NEXT:** re-run scratch/portscan.py (more fns become portable as b3's are now enabled), port the next
non-leaf batch leaf-up; tackle 800977C0 (block allocator) carefully on its own; then the indirect-dispatch
(`rec_dispatch(c, c->r[2])` vtable) fns + the 0x80106xxx overlay stage code (needs object-model RE).

## later-155: non-leaf batch b4 (8007B18C) — burn-down 464→463

After b3 enabled 800798F8 et al., the rescan surfaced 8007B18C (11 native callees) as portable. Ported it
(engine/native_path_b4.cpp): top-level object-pool init — calls 0x8004FB20 + 0x800798F8, zeroes 520×68B
slots @0x800F2740, builds a downward free-list @0x800E7E74 (head 0x800ED8C0, payloads 0x800FB11C step -68,
last→first), records free count 520 @0x800ED098, runs 8 further sub-inits. No stack args. A/B = **perfect
0-diff** (0 stack, 0 other), prologue at frame 39. Remaining on-path non-leaf: 800977C0 (block allocator,
still deferred — needs careful transcription).

## later-156: non-leaf b5 (0x800977C0 block allocator) — burn-down 463→462

Ported the deferred allocator (engine/native_path_b5.cpp). Block table @G_base(0x800AC66C), 8B blocks
(word0 = flags bit30 used / bit31 free | addr<28b>, word1 = size). Rounds size up to G_mask then aligns
to 1<<G_shift; scans for a used block or big-enough free block; used→splits new used block at idx+1,
free→carves front + pushes remainder block at G_nextfree; returns addr or -1. 0x80097A90 (lock/compact)
called around mutations. Transcribed faithfully with the gen's branch structure (delay-slot semantics:
the `r3=blk+size` in the nf<count delay slot is dead when the split is skipped). A/B: 11 stack bytes
(its own frame slots ⇒ it WAS exercised on boot), **0 globals/pool/scratchpad** ⇒ equivalent. Prologue
frame 39. No more on-path non-leaf candidates: next is the indirect-dispatch (vtable) fns + 0x80106xxx
overlay stage code (object-model RE).

## later-157: submit_terrain — sway-byte GAMEPLAY fix (DONE) + terrain RENDER bug ROOT-CHARACTERIZED (open)

**Method:** sequential A/B RAM diff (PSXPORT_RAMHASH=1, native terrain vs PSXPORT_NO_TERRAIN=1 recomp;
both deterministic so two runs are comparable). First gameplay-RAM divergence at **frame 86**, exactly
two bytes: **0x800A2014 + 0x800A2016** (everything else below the render pool 0-diff through f540).

**GAMEPLAY fix (committed 4e5823b, PUSHED):** native submit_terrain (engine_submit.cpp, 0x8002AB5C)
withheld the two `mem_w8` guest writes the recomp body does — sway0→0x800A2014, sway2→0x800A2016 — to
honor the (now USER-DISCARDED) no-guest-write rule. Restored them. Native gameplay RAM is now byte-
identical to the recomp config at f86/f300/f540 (entity list 0x800f2624, entity structs 0x800FD000 all
match). The fix changed EXACTLY those 2 bytes. **The no-guest-write rule is discarded** (user directive
2026-06-19): the engine MAY write guest state where that is the function's faithful behavior.

**SEPARATE terrain RENDER bug (OPEN — not fixed):** screenshots at the post-cutscene field/menu frame
(scratch/screenshots/native_f540.png vs recomp_f540.png) show native terrain renders **water/garbage**
while recomp renders the correct green village (matches docs/reference/ORACLE_village_f520.png). The
gameplay-RAM diff is BLIND to this: submit_terrain works almost entirely in **scratchpad (0x1F800000)
+ GTE registers**, which are NOT in the 2MB main-RAM dump. So gameplay-RAM-clean ≠ render-correct here.

  Diagnostics (PSXPORT_DEBUG=terrgte channel, added this session — engine_submit.cpp): logs the composed
  GTE CR0-7 at the terrain submit ([terrgte]), the compose inputs camera@SCR+0xF8 + object-matrix@SCR
  ([terrin]), and ov_terrain node dispatch ([ov_terrain]). Findings:
  - The compose MATH matches the recomp exactly (MVMVA_ROTCOL=0x4A49E012, MVMVA_TRANS=0x4A486012; the
    CR-packing of the 9 result halfwords matches the recomp's scratchpad layout line-for-line). Camera
    matrix read at SCR+0xF8 is correctly ~4096-scaled. Object matrix from 80085480/80084520 is a tiny
    diagonal (~36/64/52) — possibly legitimate terrain scale, NOT yet proven wrong.
  - **The real divergence is the WALK, not the compose.** ov_terrain dispatch counts over a 342-frame
    AUTO_GAMEPLAY run (NOAUDIO, field reached ~f328):
      * node **0x800ED8D8**: 42x in BOTH native and the recomp super-call — SYMMETRIC.
        **FALSIFIED EARLIER GUESS:** the "recomp gt4bp terrain submitter = 0" reading was an ARTIFACT,
        NOT a cull. gen_func_8002AB5C is fully LINEAR (no branch, always calls 0x80027768; verified the
        whole body has zero goto/if/return except the tail). rec_super_call => rec_interp interprets the
        body; a DIRECT JAL to 0x80027768 inside an interpreted function does NOT route through the
        override-based [terrgte]/gt4bp count (only rec_dispatch/JALR does — that is why the 200 OTHER
        gt4bp calls counted but the interpreted terrain submit did not). So the recomp terrain DOES
        submit, via the interpreted submitter, invisible to those counters. Do NOT chase a non-existent
        native "missing cull". (Whether the interpreter checks the override on a direct JAL needs a
        quick confirm in interp.c — if it SHOULD and doesn't, that itself is a bug worth a look.)
      * node **0x800EDB80**: **native 1x vs recomp 160x** — main resident-MAIN terrain node (dispatched
        from 0x8003FA0C). This is the strongest remaining lead, BUT first RULE OUT scene-timing skew:
        these were whole-run counts; the NOAUDIO field-reach gate (xa_stream_is_looping) could put the
        two runs in different scene phases. Confirm both runs are frame-synced (they were RAM-0-diff at
        f300/f540 in the ORIGINAL non-debug runs) before trusting 1-vs-160.
  - Render-walk is fully native-overridden in BOTH configs (only NO_TERRAIN differs): ov_render_walk
    0x8003C048, ov_perobj_render 0x8003CCA4, ov_perobj_flush 0x8003CDD8, ov_build_xform 0x80051C8C.

  **DO NEXT (render bug) — REVISED:** the gameplay-RAM diff is BLIND here (submit_terrain works in
  scratchpad+GTE). The right tool is a RENDER-level diff, not RAM. (1) Frame-stamp the [ov_terrain] /
  [terrgte] logs and compare native vs the NO_TERRAIN super-call oracle at a SINGLE synced field frame,
  to isolate the 0x800EDB80 1-vs-160 from cutscene transients / timing skew. (2) Dump scratchpad
  0x1F800000 (camera matrix @+0xF8, composed matrix, object matrix) AT the terrain call in BOTH paths
  (NOT in the main-RAM dump) and diff — that is where the compose input divergence (if any) lives.
  (3) Confirm whether interp.c checks the override on a direct JAL; if not, the recomp terrain submit is
  rendering via the un-instrumented interpreted submitter and the two submitters (native ov_submit vs
  interpreted gen_func_80027768) may themselves differ — compare their VRAM output. NB: NO_TERRAIN
  super-call is a valid recomp oracle for the walk since the walk overrides stay active.

## later-158: native terrain — ROOT CAUSE (it was GAMEPLAY corruption, NOT a render bug) + FIX

**later-157's framing was WRONG and is hereby corrected.** The terrain "render bug" (water/garbage at
the field) was NOT a render-pipeline problem. The water is the camera looking into the VOID because
**Tomba fell through the terrain** — the native terrain corrupts the gameplay/collision state. (User
ground-truth, 2026-06-19: "Tomba falls down the terrain, it's the gameplay that is broken… because
native terrain submits wrong data to guest memory.") Chasing it as a render bug — and building a
PC-native float terrain renderer — was a dead end: that float renderer came out **pixel-identical
(magick compare AE=0)** to the faithful GTE transcription, proving the projection/compose was never the
differentiator. The divergence vs the recomp oracle was two PORTING ERRORS in the native terrain body
(engine_submit.cpp submit_terrain / terrain_prep_object_matrix, the 0x8002AB5C port), both found by
diffing against the recompiled body gen_func_8002AB5C (generated/shard_4.c):

1. **WRONG geometry buffer.** The recomp loads `lui 0x800A; addiu r4, -1304` = **0x8009FAE8** as the a0
   to the submitter 0x80027768. The native code hardcoded **0x800A1AE8** (= 0x8009FAE8 + 0x2000). That
   address is a FABRICATION — it is referenced by NO function in the whole recomp as a geomblk (all three
   real callers of 0x80027768 — 0x8002AB5C/0x8002AE0C/0x8002B278 — pass -1304). So the native terrain
   read the wrong buffer → wrong/garbage strip geometry. (Also why the PSXPORT_DEBUG=terrgte probe,
   keyed on rec==0x800A1AE8, never fired for the recomp terrain — wrong key, not the "direct-JAL bypasses
   override" theory of later-157. Direct JAL DOES route through coro_native_call→override, interp.cpp:446.)

2. **GUEST WRITE the recomp never makes (the gameplay corruption).** The secondary sway-rotation needs 3
   angle words staged for 0x80084520. The recomp body stages them on its OWN STACK FRAME (`r29 -= 56`;
   words at r29+16/20/24) and passes that stack pointer. The native code wrote them to **scratchpad
   0x1F8001C0/1C4/1C8** instead — a guest write the recomp NEVER makes — clobbering whatever live engine
   state occupied 0x1F8001C0 → corrupted terrain collision → Tomba falls through. **This is why later-157's
   A/B RAM gate said "0-diff": that gate diffs only the 2 MB MAIN RAM; the scratchpad (0x1F800000) is not
   in it.** later-157 even flagged this blindness and still mis-framed the bug as render. Lesson: a
   scratchpad guest write can corrupt gameplay invisibly to the main-RAM diff.

**FIX (this session):** native terrain now matches the recomp body exactly — read geomblk 0x8009FAE8;
stage the sway angles on a guest stack frame (r29-=56, write +16/20/24, restore) instead of scratchpad.
run.sh stopgap dropped (NO_TERRAIN default 1→0; native terrain back on; set =1 to fall back to recomp).

**PROCESS CORRECTIONS (user directives, 2026-06-19):** (a) Do NOT visually verify — the agent builds,
the USER verifies via ./run.sh. Visual self-verification led this session to mis-assume "render bug" and
build a useless PC-replication float renderer. (b) The goal is to RECREATE the game on PC, not replicate
PSX on PC; a native function that byte-matches the recomp's PSX behavior but is still "poor PSX
replication" is not the end state — but FIRST it must be CORRECT (match the recomp oracle's guest
effects), which is what this fix does. The PC-native float terrain scaffold (engine/native_terrain.cpp,
PSXPORT_TERRAIN_PC, default off) is left gated off; it is byte-identical to the transcription and adds
nothing until the geometry is rebuilt from real PC-side data.

## later-159: top-down PC-native engine port STARTED from the entry point + durable disassembler tool

**Direction (user, 2026-06-19):** port the game ENGINE to PC (menu, level loading, asset loading,
terrain, object placement, scene mgmt, render, main loop) — keep the CONTENT/LOGIC (enemy AI, character
behavior, game physics/collision, quests) as recompiled PSX. CLAUDE.md rewritten to state this
unambiguously ("THE BOUNDARY"). "Start from the game's main entry point and go from there."

**Spine:** crt0 0x800896E0 → main FUN_80050b08 (=ov_game_main, native_boot.cpp) → init prefix → register
task0 = stage sequencer FUN_800499e8 → native frame loop. The init prefix was a 1:1 rec_dispatch
transcription (PSX-sim). Classified every init call platform-vs-engine (table in docs/engine_re.md).

**Reimplemented PC-native this session (engine/engine_init.cpp):** FUN_80050a0c (engine frame-state:
vblank ctr / buffer parity / frame divisor / swap-mode + DAT_80105ee8=0x45) and FUN_80050a80 (camera:
identity matrix → scratch 0x1F8000F8 = the camera-rot the renderer reads, + cam fields). ov_game_main now
calls eng_init_framestate/eng_init_camera instead of rc0(0x80050a0c)/rc0(0x80050a80). FUN_800509b4
(display + GTE projection + PSX draw/disp double-buffer env) stays dispatched — next target. Started
migrating engine logic OUT of the platform file native_boot.cpp INTO engine/.

**Exact store WIDTHS matter** (a wrong width corrupts interface state the PSX content reads, later-158;
Ghidra DAT_* hides them; 0x80050a0c isn't even in the recompiled set). Built a durable tool:
**`tools/disas.py <addr> [--mem]`** — MIPS-I disassembler for MAIN.EXE that resolves lui+addiu/ori address
builds and annotates each load/store with absolute target + width (sb/sh/sw). Verified it reproduces the
hand-decoded widths. USE IT before reimplementing any engine fn.

**Next:** finish init (800509b4 GTE part; 800520e0; font 80075130), then the named engine systems —
level LOADING (FUN_800499e8 → FUN_80052078), object placement, main menu (DEMO state machine). UNVERIFIED
by me — user verifies via ./run.sh (do NOT visually self-verify, later-158).

## later-160: removed the game's own PSX double-buffering (single-buffered for the PC renderer)

User: "remove the game's own double buffering too, it causes problems." The PSX engine flips a back/front
parity (DAT_1f800135) every frame, selecting one of two VRAM pages: OT region 0x800e80a8 + parity*0x2070
and packet pool 0x800bfe68 + parity*0x14000, with the display/draw env following parity (native_step_frame,
native_boot.cpp). On PC that page-flip is pointless — the VK renderer composites a COMPLETE frame and the
present provides display buffering — and it caused aliasing: the native display-list buckets are keyed by
OT ADDRESS (ndl_alloc on otaddr & 0x1FFFFC), so they alternated between the two pages frame-to-frame, and
the present could sample the page being drawn.

Fix (native_step_frame): pin parity = 0 — one OT region, one packet pool, the same env every frame, and
removed the `DAT_1f800135 = 1 - DAT_1f800135` flip. 0x1f800135 stays 0 (set by eng_init_framestate), so any
guest reader sees a stable single buffer. Verified the parity/OT-ptr/pool-ptr are read ONLY via the
pointers (*0x800ED8C8, *0x800BF544) that native_step_frame sets, and 0x1f800135 itself is referenced
nowhere else in engine/ or runtime/ — so pinning is complete. UNVERIFIED by me — user verifies via ./run.sh.

## later-161: FUN_800509b4 display init PC-native + single-buffer display fix (resolves later-160 hurt)

Continued the top-down port. **eng_init_display** (engine/engine_init.cpp) reimplements FUN_800509b4
PC-native: the GTE projection control regs (InitGeom = ZSF3/ZSF4/H/DQA/DQB + SetGeomOffset(160,120) +
SetGeomScreen(350); the libgte cop2-exception-handler install FUN_80085810 is moot for our always-on
native GTE) + DAT_801003f8=350. FUN_80050738 (PSX draw/disp env structs) still dispatched. ov_game_main
calls eng_init_display instead of rc0(0x800509b4).

**Fixed the later-160 single-buffer rendering hurt** (root-caused with tools/disas.py): PutDispEnv issues
GP1(0x05) → present samples VRAM at (s_disp_x,s_disp_y). The PSX env pair draws region P but its disp env
scans the OTHER region (so a flip shows the just-finished page). Pinned to one page, displaying disp-env
`parity` scanned the region we NEVER draw (stale → the hurt). Fix (native_step_frame): draw with buffer
`parity`'s draw env but DISPLAY with the opposite buffer's disp env (disp_envp = (1-parity)), which scans
the very region we draw — one consistent, always-fully-drawn VRAM region. Next display step: a real
PC-native single display env (native PutDispEnv/SetDefDispEnv), dropping the PSX env-struct dance entirely.

## later-162: level/stage LOADER core PC-native (FUN_800450bc)

Continued the top-down port into the engine system the user named ("level loading PC game"). Entry chain
from main: task0 = stage sequencer FUN_800499e8 (resolve \BIN\START.BIN → disc LBA/size into the stage
table DAT_800be1e0/e4) → FUN_80052078(stage) (restart task) → **FUN_800450bc(task, stage) = the overlay
LOADER**, now reimplemented PC-native in **engine/engine_level.cpp** (`eng_load_stage`, override
ov_load_stage @ 0x800450bc; A/B PSXPORT_LOADSTAGE_RECOMP=1; registered in games_tomba2_init).

Loader logic: stage!=3 → load the overlay from the per-stage (LBA,size) pair at 0x800be1e0/0x800be1e4
(stride 8) to 0x80106228 via the CD loader 0x8001db8c, then set the task entry from the stage-entry table
0x800a3ecc; stage==3 → resident default entry *0x800a3ed8. Then *task=entry, task[1]=FUN_80080930().

KEY: the PSX yields a frame after the load to wait for the async CD (FUN_80051f80(1)). Our overlay loader
0x8001db8c is OVERRIDDEN by ov_cd_loadfile = SYNCHRONOUS (data present on return), so the yield is DROPPED
— which also removes the only coroutine yield in the function, making the native reimpl safe (no longjmp
out of the C frame). Verified via cd_override.cpp + tools/disas.py.

NOT done (scheduler-coupled — left dispatched, they call the native loader): FUN_80052078 ends with
ChangeTh(0xff000000) = ov_switch (captures context + LONGJMPs to the cooperative scheduler), and
FUN_800499e8's file resolve is CD-platform. Reimplementing those natively needs the cooperative-scheduler
longjmp handshake worked out first. Next engine targets the user named: object placement, main menu (the
DEMO stage state machine @0x801062E4).

## later-163: PC-native single display — dropped the PSX disp-env dance (resolves later-161 "still hurt")

User (2026-06-20): the single-buffer change "hurt the rendering — many things don't render anymore", and
asked to consolidate the PSX-ish render path with PC-native. later-161's fix (draw page `parity`, but
DISPLAY via the opposite buffer's PSX disp-env struct, `disp_envp = (1-parity)`) was a band-aid: it kept
the PSX env-pairing alive and only worked while the two structs lined up. It is now REPLACED by a fully
PC-native display path in native_step_frame:
- We own the present. It scans VRAM at (s_disp_x, s_disp_y). So set that origin DIRECTLY with the new
  `gpu_set_disp_origin(c, 0, 0)` (gpu_native.cpp) — what GP1(0x05) would set — and drop `PutDispEnv`
  entirely. No disp-env struct, no (1-parity) trick.
- Still keep `PutDrawEnv(envp+0x2014)` (env0 = draw area/offset/clip at VRAM (0,0)) and `DrawOTag`; those
  set the GP0(E3/E4/E5) draw state, not display.
RE confirms env0 draws at VRAM (0,0) (SetDefDrawEnv x=0,y=0,w=320,h=240 @0x80050748), so displaying (0,0)
shows the page we draw. The display W/H stay from the boot mode env (GP1 07/08). FUN_80050738 (the PSX
env structs) is now only read for the draw env; the disp envs are dead. UNVERIFIED by me — user verifies
via ./run.sh.

## later-164: engine-owned render queue — the draw-ORDER authority (M1, plan noble-purring-pelican)

User directive (2026-06-20): REBUILD the engine as a complete PC game engine that OWNS render ordering;
do NOT inherit draw order from the guest OT (the sea/sky backdrop rendering ON TOP of the world is exactly
PSX-order inheritance + a fragile screen-coverage band heuristic). FULL OWNERSHIP is always the answer.

Before: every frame was drawn by walking the guest ordering table (gpu_dma2_linked_list) and rasterizing
each node inline; the owned world geometry (terrain + GT3/GT4 + byte-packed GT4, all via
gpu_draw_world_quad) drew inline too. Order = whatever the PSX OT said; 2D depth = bg_2d() coverage band.

M1 (engine/render_queue.{h,cpp}, on Game; gated PSXPORT_RQ, default OFF until visually verified):
- RenderQueue = per-frame host array of resolved RqItems (projected float verts + decoded material + real
  per-vertex depth + an explicit engine LAYER: BACKGROUND<WORLD<OVERLAY<HUD + an order-mode: real depth vs
  2D far/near band). flush() stable-sorts by (layer, submission seq) and emits via gpu_emit_rq_item.
- ALL three submit paths funnel through one helper rq_emit_or_queue (gpu_native.cpp): gpu_draw_world_quad
  (RQ_WORLD/depth), the guest poly path (is3d?WORLD:bg?BACKGROUND:HUD), the guest sprite path
  (bg?BACKGROUND:HUD). Under RQ the inline draw + inline set_order_2d*/set_vd are skipped; the item carries
  the layer+order-mode and gpu_emit_rq_item replays the exact set_order/2d-band/set_vd/semi_group/draw.
- ov_draw_otag: walk the guest OT (still the prim SOURCE this milestone — its ORDER is discarded), then
  rq_flush. PSXPORT_SBS keeps its dual-channel debug compare on the inline path (not queued).
- Consequence (expected): the sea/sky backdrop sorts into RQ_BACKGROUND -> drawn first -> behind the world.

KNOWN HAZARD (M3 fix): deferring poly/sprite to a post-walk flush moves them after any VRAM fill/copy that
happens DURING the walk. Framebuffer-read effects (water reflection / fb snapshot) may interleave wrong
until M3 captures prims at submit time and retires the OT read. That's why RQ is opt-in pending the user's
visual check. Builds + boots headless clean both ways. UNVERIFIED visually by me — user verifies PSXPORT_RQ=1.

## later-165: build-hygiene fix + M1/M2 status reconciliation (engine-owned render)

Two things this session.

**(1) Committed main did not build from a clean clone (fixed, ea4b777).** The later-158/159 top-down
engine foundation was wired into run.sh / build_port.sh but never committed: committed `engine_submit.cpp`
calls `proj_native_xform`, which only existed in an uncommitted `gte_beetle.cpp` diff; `native_terrain.cpp`
/ `engine_init.cpp` / `engine_level.cpp` were referenced by the build yet untracked. The later-164
render-queue commits were stacked on top of this missing base. Committed the foundation (gte_beetle
proj_native_xform, native_terrain, engine_init, engine_level, tools/disas.py, engine_re.md/project-map.md).
Left out: the imgui DPI tweak (pending the user's visual check) and an unrelated vendor/beetle-psx input.c.

**(2) The plan's M2 was ALREADY DONE — the handoff was stale.** The plan/handoff list M2 as "next work:
reimplement FUN_8007a904 natively in a new engine/engine_object.cpp." That code already exists and is the
default: `engine/engine_tomba2.cpp` `ov_objwalk` (registered on 0x8007A904 unless PSXPORT_RECOMP_OBJWALK)
walks both entity lists, calls each handler via rec_dispatch (gameplay stays PSX), then margin_render_flush.
Cull (ov_object_cull 0x8007712C), per-object render dispatch (ov_perobj_render 0x8003CCA4), the major flush
(ov_perobj_flush 0x8003CDD8), and the transform builder (ov_build_xform 0x80051C8C) are all native overrides
(game_tomba2.cpp), and the owned submit (engine_submit.cpp ov_submit_poly_gt3/4/gt4_bp) composes camera×
object and projects in FLOAT via proj_native_xform → gpu_draw_world_quad → the render queue (RQ_WORLD, real
depth). So the engine already owns the object walk + projection + world ordering (later-133/135 predate the
plan). In-code verification note: VRAM bit-identical at frames 4000/4720 of real gameplay. M2 = DONE; do NOT
create engine/engine_object.cpp.

**Actual remaining work = M3 then M4.** M3: capture the guest 2D/HUD/background submits at their SOURCE
(AddPrim / 2D draw) so they enter the queue without walking the guest OT, then stop driving the frame from
gpu_dma2_linked_list / ov_draw_otag's OT read. M4: own the still-recomp submit variants (overlay modes
0x8013xxxx, resident byte-packed 0x80027768) so 100% of world geometry carries real depth; delete the
OT-walk driver + bg_2d coverage heuristic. Open question for the user (M3 priority): under the new
render-queue default, did any framebuffer-read effect (water reflection / fb snapshot) glitch? Such copies
are usually issued OUTSIDE the OT, so the deferral hazard may not manifest — the answer sets M3 urgency.

## later-185 — DEMO substate s0 owned; jr-ra override-bypass dead-end (s4/s5 final-guest)
Owned DEMO front-end substate **s0 0x801063C0** PC-native (engine/engine_demo.cpp ov_demo_s0): genuine
pre-yield engine state (sm[0x68]=0, sm[0x48]++, sm[0x4a]=0) owned native, then coro-redirect into the first
loader jal 0x801063E4 (yields) so the loaders + fall-through into s1 run in-context. A/B (run 150 steady
menu, override-on vs -off) main-RAM + scratchpad **0-diff, NO saved-ra artifact** (the guest jal sets its own
ra). Also owned font init FUN_80075130 (ov_font_init, engine_font.cpp; boot run-2 0-diff vs initref.bin) and
the #4 auxiliary render walks 8003BCF4/8003BF00/8003EEC0 (per-node world-depth tag; field ndepth spans 3→5).

**DEAD-END (verified, retracts a prior plan):** I planned to own s4/s7's transition logic via a "post-yield
override" at the instruction after their deep yielder returns (s4: 0x8010658C). This is IMPOSSIBLE: the
interp's address-keyed override table is consulted ONLY on jal/j/jalr/computed-jr CALL/JUMP targets
(interp_flat lines 453-483); a `jr ra` RETURN (line 477) does NOT consult it. The post-yield address is
reached by `jr ra` from the yielder, so an override there never fires. Therefore **s4 0x80106580 and s5
0x801065DC stay GUEST (final)** — s4's only logic is the sm[0x6b] branch reached post-jr-ra; s5 is one
stage-transition call. **s7 0x80106668 IS still ownable** — its `jal 0x80106C24` is an override-checked jal
target and 0x80106C24's phase-selection prologue (sm[0x4a]) is pre-yield + phase2 teardown all-SYNC; own
selection + phase2, redirect yielding phase0/phase1. That is the remaining DEMO frontier step (needs reaching
s7 = confirm a menu option to A/B-verify).

## later-187 — GTE RotMatrix FUN_80085480 owned PC-native (−9.4%, 2nd of the GTE cluster)
Owned the hottest remaining hot-list fn, `FUN_80085480` = libgte **RotMatrix** (Euler SVECTOR a0 → 3x3
MATRIX a1, also returned), as `ov_rotmat` (engine/engine_math.cpp). NOT MVMVA like the matmul leaf — it uses
the GTE **GPF** op (cmd 0x3d, sf=1: MAC_i=(IR0·IR_i)>>12, IR_i=clamp16(MAC_i), + an RGB-FIFO push) as a
clamped scalar×vector multiplier, interleaved with 2 native 16-bit mults (cy·cx, cy·sx, >>12 then truncated,
NOT clamped). SIN/COS LUT @0x800a6490 (word = sin low half / cos high half; the angle's sign is re-applied to
sin only — sin odd, cos even — via the (x<<16)+sign^sign>>16 trick). Reimplemented the whole composition in
plain C (NO gte_op/Beetle); decoded the cop2 stream with an inline python decoder (4 GPFs + the move/MFC2
register choreography). Replicated ALL GTE leftover regs the body leaves: IR0=sz, IR1-3=clamp16(R4 MACs),
MAC1-3=raw R4 products, AND the RGB FIFO (regs 20-22 = R2/R3/R4 colors via Lm_C(MAC>>4)|RGB_CD<<24) — so a
still-PSX gte_op reader stays consistent (GPF actively writes the FIFO, unlike MVMVA, so I replicated rather
than excluded it). GTE-EXACT verified by ov_rotmat_verify (per-call A/B vs rec_interp(0x80085480)): 5 output
words + all 32 GTE data regs (XY-FIFO 12-15 / LZCR 31 excluded — untouched + can't round-trip a FIFO restore)
**0-diff over 55000+ live field calls**. Field run (newgame+skip 600, press r, 300 frames) **35.21M→31.90M
interp insns (−9.4%; cumulative −25.7% from the 42.93M pre-profiler baseline)**. The fn is gone from the
profiler hot-list. NEXT cluster siblings: FUN_80085050 (9.3%), FUN_80084EB0 (8.25%), FUN_80084D10 (7.5%),
FUN_80084220 (2.3%). New #1 hot fn = FUN_80115598 (28.6%, overlay 2D tilemap renderer) + FUN_8013F0DC (10.5%,
overlay, newly visible) — the second (overlay-override) arc.

## later-187b — GTE transform cluster FINISHED (80085050/84EB0/84D10/84220), −37% cumulative
Owned the remaining 4 GTE-transform-cluster fns native (engine/engine_math.cpp), finishing the cluster
(after ov_mat_mul 84110 + ov_rotmat 85480). Used 3 parallel RE subagents (USER request) to spec
FUN_80084EB0/80084D10/80084220 from the disas, then verified every spec myself against the disassembly
before implementing (later-170 false-confidence discipline — and the subagents DID need cross-checking:
the trig sign convention differs per fn).
- **FUN_80085050 / FUN_80084EB0 / FUN_80084D10** = `ov_rot_z`/`ov_rot_y`/`ov_rot_x`: ONE generic kernel
  `rotpair(rowA,rowB,posSin)` — compose an axis rotation onto a 3x3 matrix: rowA'=(cos·rowA−sin·rowB)>>12,
  rowB'=(sin·rowA+cos·rowB)>>12 element-wise over 3 cols. PURE (no GTE): SIN/COS LUT @0x800a6490 + native
  `multu` (only MFLO read = the signed product for 16-bit operands → plain signed C math bit-exact).
  Differ ONLY in (rowA,rowB) byte offsets + the sin sign for positive angles: 80085050 rows +0/+6 posSin+1
  (Z); 80084EB0 +0/+12 posSin−1 (Y — the asm NEGATES sin on the positive branch, opposite the others);
  80084D10 +6/+12 posSin+1 (X). cos = LUT word high half (sign-independent); sin = low half, negated by
  (angle<0)^(posSin<0).
- **FUN_80084220** = `ov_apply_matlv`: GTE MVMVA (sf=1, mx=ROT, v=V0, cv=Null, lm=0) of the rotation matrix
  ALREADY in GTE CR0-4 (loaded by a prior CTC2) × vector a0 → IR1-3 sign-extended to a1+0/4/8. Reused the
  ov_mat_mul MVMVA core (sign44 44-bit accum, >>12, clamp16) but reads the matrix from gte_read_ctrl(0..4).
  Replicated GTE leftover: VXY0/VZ0 (input), IR1-3, MAC1-3.
- **VERIFIED** by per-call comparators vs rec_interp: rotX 55k+ calls 0-diff, ov_apply_matlv 75k+ (output
  + all GTE data regs, FIFO/LZCR excluded), rotZ 35k+ — all 0-diff. **rotY only 1 live call** (the Y-axis
  matrix rotation barely fires in the 2.5D seaside field even with motion/jumps), matched; confidence rests
  on the IDENTICAL kernel verified at 55k+ on its siblings + offsets/sign read directly from the disas.
- Field run **31.90M→27.06M interp insns (−15.2% this batch; cumulative −37% from the 42.93M baseline)**.
  The entire GTE transform cluster is now OWNED and GONE from the profiler hot-list. NEW #1 hot = the
  OVERLAY render arc: FUN_80115598 (34.7%, 2D tilemap renderer, needs overlay-override), FUN_8013F0DC
  (12.7%, overlay), and FUN_8007712C (11.2%, cull — ov_object_cull registered but body still hot, investigate).

## later-188 — per-object CULL body FUN_8007712c owned PC-native (−7.5%, cumulative −41.7%)
Resolved the later-187 open question ("ov_object_cull registered but the body still runs hot"): the override
only WRAPPED the recomp body via `rec_super_call(0x8007712C)` and added the wide-margin re-include, so the
MIPS cull body ran in full every call (~11.2% of sampled interp time). Reimplemented the body PC-native in
`engine/game_tomba2.cpp` (`cull_native_body` + the pure `cull_decide`), replacing the super_call.
- **RE** (tools/disas.py 0x8007712c → jump table @0x80016cc0, 5 state handlers + a state-0 typed
  sub-dispatch on obj+0xc). Decision: dist=isqrt16(dx²+dy²+dz²) (FUN_80077FB0=eng_isqrt16, bit-exact leaf,
  exposed from engine_math.cpp); fwd vec @0x1f8000e8/ea/ec (s16, scratchpad). If mode byte @0x800bf870==4
  then state @0x1f800084:=2; state≥5 ⇒ always KEEP; else per-state {near,far,fovthr} cone test: KEEP iff
  near≤dist<far AND q=(fwd·d)/(dist*4) ≥ fovthr. q is MIPS signed `div` (truncates toward zero — C `/`
  matches); fwd·d and dx²+dy²+dz² use addu-wrap (computed via uint32 then cast). Thresholds: s0 t4
  {512,7169,856} t2/9 {512,5121,880} t5 {512,6657,872} other {512,6145,872}; s1 {512,7169,856} s2
  {768,4097,880} s3 {512,4097,848} s4 {1024,6657,872}.
- **Side effects (content/render interface)**: clears visible flag @obj+1=0 at entry; on KEEP sets it 1
  and (when @0x1f800080==0) pushes the obj ptr onto one of 3 type-keyed downward-growing render queues:
  t2/9→A(ptr 0x1f80013c,cnt 0x1f800144,cap24) t4→B(0x1f800148/0x1f800150,cap40) t5→C(0x1f800154/0x1f80015c,
  cap28). Returns v0=visible flag. These are SCRATCHPAD (main-RAM diff is blind).
- **VERIFIED** via the `cullverify` REPL channel (NOT env — `debug cullverify`): predict native pure, run
  the recomp body for the real writes, then compare observed effects (obj+1, r[2], state-word, queue count
  delta = exactly +1, ptr advanced −4, pushed ptr == obj) against the prediction. **0 mismatches over
  60000+ live calls** incl. directional motion (tap right/left/up) — both culled (kept=0) and kept-with-queue
  (q=2) paths exercised. Margin re-include (margin_collect) still fires (it reads obj+1 after the body).
- Field run **27.06M→25.02M interp insns (−7.5% this batch; cumulative −41.7% from the 42.93M baseline)**.
  FUN_8007712C gone from the hot-list.

### later-188b — overlay profiler resolution + pure tile-lookup leaf 0x8013fae0 owned
The "FUN_8013F0DC 13.9%" hot bucket was a MIS-ATTRIBUTION: tomba2_funcs.txt lists only resident + a sparse
set of overlay fns, so several distinct overlay functions merged under one label. **Fixed prof_report.py**:
merge the FREQUENCY section's call-target addresses (genuine jal/jalr entries logged by the interpreter)
into the TIME boundary set, splitting the merged buckets at real function starts (overlay starts not in
funcs.txt are labeled `ov_<addr>(>enclosing)` — call-target-resolved, verify w/ disas before porting).
The bucket split into ov_8013FAE0 4.25%, FUN_8013F0DC 4.07% (the real 256-insn state machine, 0x8013f0dc..
0x8013f4d8, 21-way jump table @0x8010a088 collapsing to 4 handlers), ov_8013F988 3.87%, ov_8013FA80 1.66%.
- **Owned ov_8013FAE0 native** (`ov_tile_lookup`, engine_submit.cpp) — the single hottest overlay piece
  (4.25%, 39.9k calls). Pure 2D table lookup: `tab[52*a1+a0] & (mask16<<4)`, tab=*0x8014c804 mask=*0x8014c800
  (overlay data). Registered by SIGNATURE in engine_scan_overlay (anchor on the unique pair lw 0x8014c804 /
  lhu 0x8014c800 0x10 apart, backtrack past the prev `jr ra` to the entry) — fires for the gameplay overlay
  (loads at 0x80108F9C+0x459A8, covering 0x8011-0x8014). VERIFIED 60000+ calls 0-diff via the `tileverify`
  predict-vs-recomp gate. Field **25.02M→24.46M insns (−2.2%; cumulative −43% from baseline)**.
- **NEXT (the real lever): FUN_80115598 39.4%** = the OVERLAY 2D scrolling-tilemap RENDERER (biggest single
  fn). Render-boundary → reimplement as a PC-native 2D layer feeding the RenderQueue (NOT a packet/OT
  transcription), register via the same signature scan. Then resident FUN_800931C0 6.5%, FUN_80084A80 4.4%.

### later-198 — 60fps tier REBUILT on the ACTOR-TRANSFORM layer (reproject, not screen-match)
User spec (2026-06-21): "60fps should only compute camera and moving/animating objects… use the game
object's OWN transform layer, NOT GTE/screen-space prim matching." The old live path (`build_lerp`'s
fingerprint matcher) paired CURRENT render-queue prims to PREVIOUS ones by a position-independent
material/UV hash + nearest-centroid and lerped their SCREEN verts — it collided on real meshes (many tris
share UV/colour/texpage) → vertex explosion (masked by a 48px gate → snap/judder), and it "computed
everything like static objects". RETIRED.
- **New mechanism — capture + midpoint REPROJECTION (no guest re-run, no guest writes):** at the native GTE
  projection chokepoint every GTE-composed world quad records, into its host `RqItem` (`fps60_stamp_world`,
  engine/fps60.cpp): its 4 MODEL verts, the composed transform CR0-7 it was projected with (+CR24/25/26 =
  OFX/OFY/H, which `proj_native_xform` also reads), the draw offset, and its ACTOR KEY = the per-object
  render command `cmd` (set by `submit_perobj_flush` around `native_dispatch`, engine_submit.cpp — this is
  the one tap point that actually reaches the drawn geometry, unlike the cull-time `current_object`). The
  composed transform already bakes in the camera, so lerping it per actor across frames reproduces BOTH the
  camera pan (a static actor's transform differs frame-to-frame only by the camera) AND object motion in one
  mechanism — exactly the user's "static objects move via the interpolated camera only; movers also get
  object-motion interp", with the per-actor transform delta as the static/mover signal (no separate flag).
- **build_lerp**: map actor key→prev composed transform; for each current world prim, lerp its actor's
  transform to the A/B midpoint (rotation = packed-int16 average, translation/OFX/OFY/H = integer average)
  and reproject its model verts in pure float via `proj_native_xform_cr` (gte_beetle.cpp; saves/restores
  CR0-7+CR24-26, no guest state). 2D/HUD, terrain (separate float projection), unkeyed/new-this-frame
  actors, and any prim whose reprojection jumps > the 48px gate (cut/teleport) SNAP.
- **WHY (b) not the handoff's recommended (a):** re-running the native render WALK with lerped guest
  transforms would re-execute the field's INTERPRETED per-mode renderers (byte-packed GT4 is reached via
  `rec_dispatch(0x8003F698)`), which mutate guest packet RAM — unsafe to run twice/frame. The reproject
  path touches zero guest RAM.
- **MECHANICAL GATE (`debug fps60chk`):** reprojecting every world prim at t=1.0 (its OWN captured
  transform, no averaging) must reproduce its real screen verts. **max=0 avg=0.000 px EXACT, every field
  frame** — the capture/recompose/round path is bit-faithful to the engine's own projection. At the field:
  ~500 of ~1450 prims reproject across ~98 matched actors/frame; 949 snap (2D/HUD/terrain). No crash; field
  renders intact (scratch/screenshots/fps60_field.png).
- **Bugs found+fixed building it:** (1) `RqItem.fps_*` left uninitialised in `rq_emit_or_queue` → garbage
  `fps_world`/`fps_key` on 2D items → bogus map keys + huge reproject error (fixed: clear `fps_world` at
  construction, gpu_native.cpp); (2) captured only CR0-7, but `proj_native_xform` also reads CR24/25/26
  (OFX/OFY/H) live at present time → wrong projection scale (fixed: capture+restore all 11); (3) `crM[8]`
  stack buffer while `fps60_compose_mid` writes 11 → stack smash corrupting the adjacent unordered_map →
  SIGSEGV (fixed: `crM[11]`). Also dropped the dead per-frame `fps60_build_remap()` (drove the Beetle GTE
  per vertex every frame for output nothing consumes).
- **OPEN (next):** terrain uses its OWN float projection (native_terrain.cpp Rview/Tview), not the GTE
  CR0-7 path, so it currently SNAPS under camera pan — add a camera-keyed reprojection (capture terrain
  model verts + the camera Rview/Tview) so the ground interpolates too. SMOOTHNESS itself is USER-EYEBALL
  ONLY (windowed, F1 overlay 60fps toggle) — the mechanical gate only proves the math, not the feel.

### later-199 — render-issue batch: #7/#11 FIXED + verified; #8/#9 RE'd; #4/#5/#6/#12 need live user diagnosis
Worked the 8 open GitHub render bugs (specs in docs/reference/issues/).
- **#7 + #11 (FMV-skip stale overlay) — FIXED + HEADLESS-VERIFIED (commit 29bd85f).** Added
  `gpu_clear_display(core)` (gpu_native.cpp) and called it at the single return of `native_fmv_play_lba`
  (every exit incl. Start-skip) + once in `native_boot_run` after the intro-FMV block. Repro (NO_FMV=0
  SCEA_SKIP=1 FMV_MAXFRAMES=3): post-FMV frame is now solid BLACK (was near-white SCEA fill), title
  renders clean (TOMBA!2 logo + menu, no SCEA text / no garbled glyphs). USER eyeballs windowed skip feel.
- **#8 + #9 (dust/impact striped bars) — RE'd into issue8_9_re.md.** Root cause = effect prims sample
  beyond their texpage cell because the GP0-0xE2 texture-WINDOW confinement isn't in force at draw time;
  a prior "fix" (2a11b4f) was a bandaid (dropped `u&=255` so SW==VK but both march off-cell). Real fix =
  apply the real texture window in draw order so the existing tritex.frag wrap confines U — NOT a clamp
  constant. Needs a LIVE `provat`/texture-window read on a bar pixel to pick the branch; dust is
  animation-gated (not reliably headless-reachable).
- **#4 / #5 / #6 / #12 — NOT acted on (correctly): each needs live USER diagnosis the agent cannot do
  headless.** #4 flame-hut + #5 barrel are past free-roam (driving §5 blocker); their fix branches on
  whether the prim is OT-walked-shared-obj_depth vs owned-GT4 (must confirm live with `provat`/`objz`).
  #6 HUD gray box is LATENT until the menu page handler is owned native (doesn't reproduce now). #12
  cutscene black bar doesn't reproduce headless and the fix is delicate present-region math (gpu_gpu.cpp
  :1221) that would touch ALL frames — unsafe to change without a repro to verify. Applying any of these
  blind = bandaid/regression risk (forbidden). Specs carry the exact live repro recipe for the user.

### later-200 — FUN_800498C8 `ov_grid_resolve_498c8` owned (collision-grid resolve loop, top of the grid family)
Owned the collision-grid RESOLVE LOOP `FUN_800498C8` PC-native in `engine/game_tomba2.cpp`. It is the head
of the grid family whose two leaves were owned earlier (later-194 `FUN_80049968` setup, later-195
`FUN_80047CBC` query); this fn drives them in a descent loop. a0 = probe object.
- **RE** (`tools/disas.py 0x800498C8`): a loop —
  `jal 0x8004798C(obj)` (per-step grid-origin/index setup; non-trivial, calls 0x80048ecc/0x80048fc4 → kept
  DISPATCHED), `jal 0x80049968(u8@0x1F8001FE)` (owned row-ptr setup), `v0 = jal 0x80047CBC()` (owned cell
  query/walk). If v0==0 → return 0. Else v1 = w[0x1F8001E0] (the record ptr the query latched); if
  (h[v1]&0x4000)==0 → return 1 (terminal cell). Else obj[42] = b[v1] (record the resolved tag byte onto the
  probe object), reload v1' = w[0x1F8001E0]; if (h[v1']&0x4000)!=0 → LOOP (descend further) else return 1.
  Pure control flow over scratchpad + object memory; ONE object write (obj+42); NO GTE, NO render packets.
- **Ownership**: control flow + the obj+42 write owned native; all three callees stay PSX via rec_dispatch
  (the two grid leaves honor their own owned override identically in the dispatched path; FUN_8004798C's
  deep tree stays interpreted). Returns: 0 only when the query returns 0; otherwise 1 (matching the gen
  delay-slot `addiu v0,zero,1` on every keep path).
- **VERIFIED** with the full RAM+scratchpad A/B gate `gridresolve` (native run → snapshot+rollback →
  rec_super_call → diff): **0-diff over 8000+ live field calls** across both movement directions (press
  right 250 + press left 250). v0 + every persistent RAM word + all scratchpad match exactly. GOTCHA (same
  family as scriptvm/player): the dispatched callee tree runs in BOTH passes and leaves transient values in
  its OWN stack frames below entry sp; because FUN_800498C8's native frame is absent those residual bytes
  differ harmlessly, so the gate excludes the top-of-RAM stack window [sp-0x800, sp) (sp ~0x1FE9xx, RAM end
  0x200000 — far above all game data; a real divergence would alter persistent state below). A first 32-byte
  exclusion exposed exactly this stack residue (diffs all at sp-0x26..sp-0x48), confirming the cause; the
  wider window then went 0-diff. Registered in game_tomba2.cpp alongside the grid leaves.

### later-201 — FUN_8004798C `ov_grid_step_4798c` owned (collision-grid per-step origin/index setup)
Owned `FUN_8004798C` PC-native in `engine/game_tomba2.cpp` — the LAST dispatched callee inside the owned
resolve loop (later-200), completing the collision-grid family (setup 80049968 / query 80047CBC / resolve
800498C8 / step 8004798C all native now). a0 = probe object. Pure scratchpad halfword arithmetic + two
dispatched callees; NO GTE, NO render packets.
- **RE** (`tools/disas.py 0x8004798C`): three blocks over scratchpad base 0x1F800000.
  (1) if obj[42] != byte[0x1FE] (current grid id) → jal 0x80048ecc(a0=obj[42]) (grid reload, dispatched).
  (2) SELECT/RANGE TEST: a "Z vs X" select on (h[0x1AE] u< h[0x1B0]); compute the selected-axis delta of the
  working probe coord vs the grid origin ((h[0x1C0]−h[0x1AC])&0xffff for Z, (h[0x1BC]−h[0x1AA])&0xffff for X)
  and sltu vs the extent (h[0x1B0]/h[0x1AE]); if the probe is past the range → jal 0x80048fc4(a0=obj,a1=1)
  (re-resolve, dispatched). (3) CLAMP+RECOMPUTE on the SAME (h[0x1AE] u< h[0x1B0]) select: Z branch clamps
  0x1C0 into [0x1AC, 0x1AC+0x1B0] then recomputes 0x1BC; X branch clamps 0x1BC into [0x1AA, 0x1AA+0x1AE]
  then recomputes 0x1C0. recompute = cellbase + (((clamped − cellbase2)·pitch[0x1BA]) >> 14), a SIGNED
  `mult`/`mflo` low-word product then arithmetic `sra 14`.
- **Ownership**: control flow + every scratchpad op owned native; the two callees (80048ecc grid-reload,
  80048fc4 re-resolve) stay PSX via rec_dispatch. Registering 0x8004798C means the resolve loop (later-200)
  now routes its `jal 0x8004798C` through this native body instead of the raw recomp.
- **VERIFIED** with the full RAM+scratchpad A/B gate `gridstep` (native run → snapshot+rollback →
  rec_super_call → diff): **0-diff over 8000+ live field calls**, 0 mismatches, across both movement
  directions (press right 250 + press left 250 — exercises both the Z and X clamp branches and the reload /
  re-resolve callee arms). GOTCHA (same grid family as gridresolve/scriptvm): the dispatched callees run in
  BOTH passes and leave transient residue below entry sp (this fn has no native frame there), so the gate
  excludes the top-of-RAM stack window [sp-0x800, sp) — far above all game data; a real behavioral
  divergence would alter persistent state below it. Live (non-verify) field run: Tomba walks normally, no
  crash, reaches stage 0x8010637C.

### later-202 — FUN_80040410 `ov_child_spawn_40410` owned (per-object child-node spawn / sub-object builder)
Owned `FUN_80040410` PC-native in `engine/game_tomba2.cpp` — a callee of the per-object state machine
FUN_80040558's state-0 handler. a0 = obj, a1 = group index (low byte). NO GTE, NO render packets; pure
control flow + object/child-node memory writes with two dispatched callees.
- **RE** (`tools/disas.py 0x80040410`): obj[8]=2 (child count) set unconditionally. GATE — if
  (int16)*0x800ed098 < 2 → obj[4]=3, return 0 (global "not ready" gate; `slti v0, lh, 2` so the compare is
  SIGNED 16-bit). Else: obj[9]=2, obj[13]=0, obj[11]=0, sh obj[84]=obj[86]=obj[88]=0; count = obj[8] (the 2
  just written, re-read from memory). For i in [0,count): node = jal 0x8007aae8() (child-node allocator,
  dispatched); store the node ptr at obj[0xC0 + 4*i]; node[6] = (i-1) as s16 (0xFFFF on the first child);
  node[0/2/4] = u16 tblA[6*i + 0/2/4] (tblA = 0x800a3b1c, stride 6); node[8] = node[0xA] = node[0xC] = 0;
  a2 = lh tblB[2*((a1&0xff) + i)] (tblB = 0x800a3b28, stride 2, base index a1&0xff); jal 0x80051b04(node,1,a2)
  (transform/geom setup → writes node[0x40], dispatched). Return 1.
- **Ownership**: control flow + every memory write owned native; the allocator 0x8007aae8 and the setup
  0x80051b04 stay PSX via rec_dispatch (each honors its own override identically in the super-call path).
- **VERIFIED** with the full RAM+scratchpad A/B gate `child40410` (native run → snapshot+rollback →
  rec_super_call → diff over full RAM 0x200000 + scratchpad 0x400 + v0): **0-diff over 28 live field-spawn
  calls** (press right 250 + press left 250). This is a spawn-time INIT handler — called once per
  object-spawn, NOT per-frame — so 28 is the natural exercise count for the field; each of the 28 is a
  complete state-equivalence check (whole RAM + whole scratchpad + return). GOTCHAs caught while RE'ing:
  (1) the child count is the value JUST stored (obj[8]=2), re-read from memory, not a constant; (2) node[6]
  uses the PRE-increment loop index (delay-slot ordering — s2++ lands after the node[6]=s2-1 store but before
  the tblA stores complete); (3) the gate is a signed 16-bit compare. GOTCHA (same family as grid/scriptvm):
  the 2 dispatched callees run in BOTH passes and leave transient residue below entry sp (FUN_80040410's own
  48-byte frame is also dead there) → the gate excludes the top-of-RAM stack window [sp-0x800, sp), far above
  all game data. Live (non-verify) field run: Tomba walks normally, no crash, reaches stage 0x8010637C.

### later-203 — FUN_80040558 `ov_sm40558` owned (per-object STATE-MACHINE HEAD — the dispatcher above child40410)
Owned `FUN_80040558` PC-native in `engine/game_tomba2.cpp` — the per-object state machine whose state-0
handler calls the just-owned child-spawn FUN_80040410 (later-202). Owning the HEAD advances the whole
behavior family (higher leverage than the isolated leaf). a0 = obj; returns void. NO GTE, NO render packets;
pure control flow + object byte/halfword writes + global/scratchpad READS, with EVERY `jal` (24 distinct
sub-behaviors, incl. overlay code 0x80114xxx/0x80120xxx/0x8012xxxx) kept PSX via rec_dispatch.
- **RE** (`tools/disas.py 0x80040558`, full 1280-byte body disassembled past the jump-table `jr v0` traps;
  three inner jump tables dumped from MAIN.EXE: 0x800152e0 state-0/obj[5]==1 ×8, 0x80015300 state-1/obj[5]
  ×6, 0x80015318 state-1/obj[94] ×8, 0x80015338 state-2/obj[5] ×5). Dispatch on the state byte obj[4]:
  - **STATE 3** (@a40): jal 0x8007a624(obj); return.
  - **STATE 0** (@5ac): sub-dispatch on obj[5]. obj[5]==0 (@5cc): v0=jal 0x80040410(obj, a1=obj[3]); if
    v0!=0 → obj[5]++; then ALWAYS sh obj[128]=64, obj[130]=128, obj[132]=obj[134]=150, sb obj[41]=obj[43]=
    obj[95]=obj[70]=0. obj[5]==1 (@620): v1=obj[94]; v1<8 → jt 0x800152e0 (jal one of 0x8003fbc4/fc00/
    0x801286f4/-/0x8003fc78/0x80120188/0x8003fc8c/0x801146e8; entries 0-5 force v0=1, 6/7 take the callee's
    v0 and @6b8 returns early if v0==0), else v0=1 (@6c0); @6c4 sb obj[4]=obj[0]=v0, sb obj[5]=obj[41]=0.
  - **STATE 1** (@6d8): early-out gates on *0x800bf870 (==18 needs *0x800bfa59!=0; ==19 returns if
    *0x800bf871==19). @720 obj[5] sub-dispatch (jt 0x80015300, jal 0x8003fd10/fed8/ffcc/0x4022c/0x40390/
    0x80114934). @7a8 obj[94] sub-dispatch (jt 0x80015318: [0,1,3,4,6]→@7e0, [2]→@888, [5]→@8c0, [7]→@7d8
    =@7e0 prefixed by jal 0x8012b118). @7e0: if *0x800bf816!=0 && *0x800bf817==(s16)obj[106] && (obj[40]&
    0x80) → sb obj[1]=1, jal 0x80077e7c, jal 0x800517f8; else @834: if obj[40]&0x80 → done; else v0=jal
    (*0x800bf870==8 ? 0x8012e168 : 0x8007778c); if v0!=0 → jal 0x800517f8. @888: v0=u8 *(*obj[16]+1); sb
    obj[1]=v0; if v0!=0 → jal 0x8012866c, jal 0x80077e7c. @8c0: jal 0x801201e0. All paths sb obj[41]=0.
  - **STATE 2** (@8d4): obj[5] sub-dispatch (jt 0x80015338: [1]→@904, [2]→jal 0x8003fe00, [3]→jal 0x8003fed8,
    [0]/[4]→@964). @904: if obj[3]==0 && *0x800bfad1==0 → jal 0x80040b48(a0=56); if obj[94]==2 → *(*obj[16]+
    94)=1. @964: if obj[94]==2 → sb obj[1]=u8 *(*obj[16]+1), jal 0x8012866c, jal 0x80077e7c; else @99c mirrors
    state-1's @7e0..@834 tail (same global checks + jal 0x8012e168/0x8007778c + jal 0x800517f8).
- **Ownership**: control flow + every memory write native; all 24 callees stay PSX via rec_dispatch.
- **VERIFIED** with the full RAM+scratchpad A/B gate `sm40558` (native run → snapshot+rollback → rec_super_call
  → diff full RAM 0x200000 + scratchpad 0x400; void return, no v0): **0-diff over 11200+ live field calls**
  (press right 250 + press left 250). A transient state histogram (built with -DSM40558_HIST, then reverted)
  confirmed this seaside scene drives **state 0 (28 calls, the spawn-init path) + state 1 (~11000 calls, the
  hot active-behavior path)**; states 2 and 3 do NOT fire here (rarer transition/special paths), so they are
  transcribed-from-disasm and will verify the moment a scene drives them (same posture as scriptvm/gridstep's
  rare branches — the gate-off live path runs the identical code, so any state-2/3 slip surfaces under the
  gate). GOTCHA (same family as child40410/grid/scriptvm): every dispatched callee runs in BOTH A/B passes
  and leaves transient residue below entry sp (FUN_80040558's own 24-byte frame is dead there) → the gate
  excludes [sp-0x800, sp), far above all game data. GOTCHAs caught while RE'ing: (1) state-0 obj[5]==1 entries
  6/7 take the callee's return as v0 and @6b8 returns WITHOUT writing obj[4]/[5]/[0]/[41] when v0==0 — entries
  0-5 force v0=1 and never early-return; (2) state-1 jt 0x80015318[7]→0x800407d8 is the SAME block as [0,1,3,
  4,6]→0x800407e0, only PREFIXED by jal 0x8012b118 (the prefix is the only difference, so obj[94]==7 alone
  runs that callee); (3) the obj[817]==obj[106] compare is against the SIGN-EXTENDED s16 lh obj[106].
  Live (non-verify) field run with the override active: Tomba walks normally — master X 0x0F640000→0x17704900
  holding right, no crash, reaches stage 0x8010637C.

### later-205 — JT1[1..5] not autonomously verifiable; pivoted to FUN_80051128 `ov_xform51128` (child-node transform loop)
**First half — JT1 frontier exhausted for autonomous verification.** The prior session's recommended next
target was `FUN_80040390` (sm40558 STATE-1 obj[5] jump-table JT1[4]). I RE'd it (clean: no GTE, no render
packets, 2 dispatched non-trivial callees 0x80027144/0x80074590, one obj[4]=3 write + one *0x800bf850
decrement gated on obj[94]==5) and wrote the native body + a full-RAM+scratchpad `fd390` A/B gate. But it
**never fires in the seaside scene** — a -DFD390_FIRE entry counter logged ZERO firings over 800 frames of
movement. Instrumenting sm40558's own obj[5] dispatch with a JT1 histogram (then reverted) showed why:
**obj[5] is ALWAYS 0 in this scene** — only JT1[0] (the already-owned fd10) is ever dispatched; JT1[1..5]
(0x8003FED8/FFCC/4022C/40390/80114934) get exactly 0 calls. So none of the JT1 siblings can be 0-diff-verified
autonomously here. Per the standing rule (don't force a trivial/unverified port), I removed the unverifiable
fd390 override and pivoted to the §F resident hot-list.

**Second half — owned `FUN_80051128` `ov_xform51128` PC-native (engine_submit.cpp), 0-diff verified.** A fresh
profile (newgame + skip 650, press right 300 + left 300) put `FUN_80051128` at **3.7% of field hot time** —
a clean (no GTE in body, no render packets), frequently-exercised resident transform orchestrator and the
highest-value autonomously-verifiable target left. It is a near-twin of the already-owned `FUN_80051464`
ov_xform_propagate: a per-object CHILD-NODE TRANSFORM loop. RE (`tools/disas.py 0x80051128`):
- GUARD: if node[9]==0 return. Loop s2 in [0, node[8]) with a DUAL-BOUND continue check on node[9] (identical
  idiom to xform_propagate).
- Per child = node[0xC0 + 4*s2]: seed the scratchpad work matrix @0x1F800000 (zero +4/+0C/+14/+18/+1C;
  +0=(s16)child[56], +8=(s16)child[58], +10=(s16)child[60], the +56/58/60 sign-extended via lhu→sll16/sra16),
  then ov_rotmat(child+8 euler → 0x1F800020), ov_mat_mul(0x1F800020 × 0x1F800000 → 0x1F800040).
- Parent select on sentinel=(s16)child[6]: ==-1 ROOT (parent = this node: ov_mat_mul(node+152 × 0x1F800040 →
  child+24), ov_apply_matlv(child → child+0x2C), child[0x2C/30/34] += node[0xAC/B0/B4]); else SIBLING (parent =
  node[0xC0 + 4*sentinel]; same ops with p+24 / p[0x2C/30/34]).
Every callee is an already-owned native primitive (ov_rotmat 0x80085480 / ov_mat_mul 0x80084110 /
ov_apply_matlv 0x80084220), so the body is pure orchestration + scratchpad seeding + integer translation
adds; the primitives are rec_dispatch'd in the recomp's EXACT jal order to preserve the matmul→MVMVA GTE-CR
coupling (ov_mat_mul CTC2's R→CR0-4 → the following ov_apply_matlv reads the right matrix).
GOTCHAs: (1) child[56/58/60] sign-extended; (2) the sentinel is sll'd by 2 in the branch delay slot, so the
sibling parent ptr is node[0xC0 + sentinel*4].
**VERIFIED** with the `xform51128` gate (same scheme as `xformverify`: snapshot each touched child sub-struct
+0x18..+0x37 + the scratchpad work matrix + GTE data regs, run native, restore, run rec_super_call, diff;
GTE FIFO regs 12-15 + LZCR reg 31 excluded — the comparator can't round-trip a FIFO restore and neither path
writes them): **0-diff over 3000+ live field calls (~22000 child transforms), 0 mismatches**, exercising BOTH
branches (a transient root/sib counter, since reverted, showed root=4748 / sibling=17268 children) across
both movement directions (press right 400 + press left 400). Live gate-off run: Tomba walks normally (master
X 0x0F640000→0x17704900 holding right), no crash, reaches stage 0x8010637C. Registered in game_tomba2.cpp
next to ov_xform_propagate. NEXT clean §F targets I see: `FUN_80026C88` / `FUN_8003F024` (pure per-object
dispatcher loops over the 40-entry, 64-byte-stride 0x800ec188 table — no GTE, leaf-ish, full RAM+scratchpad
A/B verifiable). AVOID `FUN_80027A4C` (16% but it builds GP0 packets with cop2/lwc2/swc2 — render-boundary).

## later-207 — SAVE / LOAD FLOW owned native (engine/save.cpp): FUN_80036DFC head dispatcher
USER task: own the game's save/load FLOW (NOT the memory-card hardware — that's already native in
runtime/recomp/memcard.cpp) as a clean PC-game save module engine/save.cpp.

**RE — the save system is a 6-state machine.** The FLOW HEAD is `FUN_80036DFC` (an earlier scan labelled it
0x80036E00 = the first store after the `addiu sp,-0x30` prologue). It is the body of the active save-menu
task, reached from the title "Load Game" and the in-game pause menu "Load data". It:
- saves s0/s1/s2/ra in a 0x30 frame; sets s1 = a0 = the save-menu TASK struct, s0 = 0x800D1E68 = the
  save-system CONTEXT struct;
- reads SUBSTATE = task[1] (lbu); bounds `sltiu <6`; if ≥6 → epilogue 0x800376D4 (restore+return, no-op);
- else `handler = lw table[substate]` (table @0x80010668, stride 4); `jr handler` — a TAIL-CALL (a0/s0/s1/s2/
  ra + the 0x30 frame all live; the page handler unwinds the frame itself and returns to the dispatcher's
  caller). Handlers: [0] 0x80036E48 LOAD-SELECT, [1] 0x80036E58 LOAD-RUN (slot Up/Down via pad edges
  0x800E7E68, confirm/cancel), [2] 0x800371D4 SAVE-CONFIRM, [3] 0x80037360 SAVE-EXECUTE (sets save flags
  0x800BF809/0A/0B + 0x800BF83A, kicks the writers via 0x800364AC/800368D0/8004D650/800525D0), [4] 0x800375E0
  FORMAT, [5] 0x80037638 DELETE.

**No discrete game-side serialize/deserialize fn in this title.** The save data IS the game's live progress
block; persistence is libmcrd writing/reading card frames directly via the BIOS file API (open/read/write
through the 0xB0/0xA0 BIOS-call trampolines). That whole path is library/PLATFORM, reached ONLY via
trampolines — proven: the file-API stubs 0x800808B8/C8/D8/E8/F8 (open 0x32 / lseek 0x33 / read 0x34 / write
0x35 / close 0x36) have ZERO direct jal callers; the libmcrd internals FUN_8009Bxxx/8009Cxxx (incl. the
checksummed frame R/W FUN_8009C2B0 + dir writer FUN_8009C3F4) are reached via 0xB0 trampolines; and the
SAVE-EXECUTE handler 0x80037360 contains only flag writes + sub-calls, NO bulk game-RAM→buffer copy. It's all
already owned by memcard.cpp's HLE (B0:0x4E/0x4F frame R/W + SwCARD completion, later-94). So the
ENGINE-ownable save/load LOGIC is precisely this FLOW state machine — which engine/save.cpp owns; the page
handlers + the card I/O leaf stay PSX via rec_dispatch. (Ruled-out dead ends: the "Tomba MEMORY CARD" filename
@0x800106d4 + the UI-text table 0x800a294c..0x800a29c0 are UI strings, NOT a serialize-buffer layout; the
0x8001cc00 region is the card SIO/DMA hardware init, platform.)

**Module:** engine/save.cpp — `save_dispatch_native` (faithful prologue + bounds + table resolution +
rec_dispatch the handler), `ov_save_dispatch` (the override + saveverify gate), `save_register()` (ONE line
into game_tomba2.cpp init: `rec_set_override(0x80036DFC, ov_save_dispatch)`). Added to run.sh +
tools/build_port.sh SRC lists (in sync). Same shape as ov_render_cmd (render_cmd_dispatch) / ov_disp_26c88.

**VERIFY:** `saveverify` channel = a NON-DESTRUCTIVE dispatch-decision gate. The page handlers can YIELD
across frames (SAVE-EXECUTE commits the card write over multiple frames), so a snapshot/rollback/super-call
A/B that DOUBLE-RUNS the handler is UNSAFE (a yield in the native pass longjmps out before the rollback) —
so the gate instead confirms the native body resolves the same handler the recomp `jr table[substate]` reads
(by construction identical) + that the override fires at sane substates; the handler runs exactly ONCE.
**EXERCISE COVERAGE — HONEST:** the dispatcher is NOT reliably reachable headless. Its only trigger is
interactive title/pause-menu navigation INTO the file flow (cursor → "Load data" → confirm → spawn the file
machine), which docs/driving-the-game.md §5 flags as only partly solved; several headless pause-menu nav
sequences (tap down/x) did not enter it (0 HITs). The dispatcher is RESIDENT MAIN.EXE (0x80036xxx) so the
override IS registered; verified inert + no regression during boot/field (builds + links clean; field stage
0x8010637C reached cleanly, task0 state=2). The dispatch body is transcribed faithfully from the disasm; the
`saveverify` gate fires + verifies the moment the menu IS navigated. (Build gotcha: the shared scratch/obj
object cache was contaminated by a parallel worktree's game_tomba2.o → undefined `sound_register`; built into
a private object dir to isolate — the parallel-subagents OBJDIR rule.)
## later-207 — SOUND front-end owned as a PC-game audio module (engine/sound.cpp)
Lifted the game's sound-TRIGGER API (the SFX/BGM trigger functions the game logic calls, wrapping libsnd)
into a clean PC-game audio module `engine/sound.cpp` (`sound_play_sfx`/`sound_play_bgm`/`sound_stop_bgm`,
registered via `sound_register()` — one line in game_tomba2.cpp). The SPU hardware was already native; this
is the trigger/state layer above it.

RE (tools/disas.py; jump tables dumped from MAIN.EXE):
- **FUN_80074590 = the SFX / song-id ROUTER** (`t2_call3(0x80074590, id, ...)`). id>=0x70 → a fixed 16-entry
  id->song map (jump table 0x80016c04, all entries just call FUN_80074BF8 with a constant song: id 0x70→bgm2,
  0x71→bgm3, … 0x79→bgm13, 0x7d/0x7e→ret0, 0x7f→FUN_80074eec). id<0x70 → the real per-effect SFX path
  (reads descriptor tables 0x800a4d18/0x800a4ef8 for pan/vol, `jal 0x80075e04` submit leaf). **OWNED native**
  — pure control flow; the id router + bounds reimplemented in C, the descriptor sub-path kept dispatched
  (it funnels into the same 0x80075e04 leaf → identical SPU). `soundverify` gate (full RAM+scratchpad A/B vs
  rec_super_call) = **0-diff over 800+ live calls** (menu/cursor/action SFX firing while walking the seaside
  field). FIRES: per-frame on object actions; the 0x72→bgm4 etc. BGM entries route through the BGM override.
- **FUN_80074BF8 = BGM START** / **FUN_80074E48 = BGM STOP**. Full RE in the module comment: classify the
  requested song vs current song (state writes to 0x800bed80 song + 0x800be22a request byte), stop the old
  seq, then drive the libsnd sequencer leaves (SsUt bank-set 0x800963a0, SsSeqSetVol 0x80091f50, SsSeqPlay
  0x80090560, SsSeqStop 0x80091af0) per the seq-slot table 0x800be368 (stride 8: +0 handle, +4 vcount whose
  low byte is the bank) and reset the per-voice table 0x800be238 (stride 12, word0=-1) / voice-count 0x800bed78.

BOUNDARY CALL (not a bandaid — documented): I first reimplemented the BGM start/stop bodies fully native
(control flow + state writes owned, the 5 libsnd leaves rec_dispatched in exact order). The `soundverify`
A/B caught a REAL divergence I could NOT close: for vcount-14 songs the per-voice table 0x800be238 ends up
HALF-written through the *dispatched* SsSeqPlay (the leaf decides per-voice keep/kill from its own internal
voice state, which the gen body sets up via the EXACT inline register/call context; a native re-drive that
dispatches the leaf does not reproduce that voicetab side-effect bit-for-bit). That is exactly the "it's a
leaf, keep it dispatched" case in THE BOUNDARY — BGM start/stop ARE libsnd-sequencer glue. So the OWNED-native
trigger surface is the SFX/song ROUTER (pure control flow, 0-diff), and BGM start/stop are **engine-glue
super-call wrappers**: the wrapper owns the ENGINE-facing contract (the instant-CD dialog-music cut hook
`xa_music_cut_if_dialog`, moved here from native_boot ov_bgm_start; + the clean API), and the gen body runs
as the live sequencer via rec_super_call. The native BGM bodies + their RE are retained in sound.cpp as the
documented reference for a future deeper pass (own the libsnd voice allocator too), clearly marked NOT
registered. Verified live: `bgm 4` sets song@0x800bed80=4, `bgmstop` clears it to 0xFFFF; Tomba walks normally
(master X → 0x17704900 holding right).

Cleanup: removed the now-superseded `ov_bgm_start`/`ov_bgm_stop` wrappers + their registrations from
native_boot.cpp (the dialog-cut hook + REPL bgm/bgmstop routing moved to / through sound.cpp). Added
engine/sound.cpp to run.sh + tools/build_port.sh SRC lists (in sync). DEAD END recorded: dispatching the
libsnd SEQ leaves does not bit-reproduce their voicetab bookkeeping — don't re-attempt a native BGM-start
body without ALSO owning the SsSeqPlay/voice-allocator leaf.

## later-200: 60fps dynamic-shadow STROBE root-caused + fixed (interp pass zeroed the shadow stream)

SYMPTOM: with 60fps interpolation + dynamic shadows ON, the shadow strobed at 30Hz.

ROOT CAUSE (mechanical, not visual): the live 60fps tier is the actor-transform VK path
(`fps60_present_vk` / `build_lerp` — the OLD SW `s_interp` re-rasterizer is already retired). It presents
each logic frame TWICE: PASS 1 = interpolated in-between (movers at the A/B transform midpoint, everything
else snapped), PASS 2 = the real frame. The dynamic-shadow GEOMETRY is captured ONCE per logic frame into
the host-persistent `s_shadow_vbuf` (count `s_shadow_n`) during the engine submit walk — at the real
frame-B world positions. Both passes go through `panel_render(s_tex)`, and the gate
`if (shadows_on() && tgt == s_tex) shadow_pass()` DID fire on both. BUT PASS 1's
`gpu_fps60_present_pass` → `gpu_gpu_frame_end` → `frame_end` ZEROED `s_shadow_n`, so PASS 2 (the real, final-
presented frame) rasterized an EMPTY shadow map. Probe `PSXPORT_DEBUG=fps60shadow` showed the smoking gun:
`shadow_pass: s_shadow_n=2565 / 0 / 2565 / 0 …` (interp had the shadow, real had none) → 30Hz strobe.
NOTE the brief's guess (`shadows only run for s_tex, never for the interp frame`) was the INVERSE of the
real mechanism — both passes share s_tex; the bug was the stream reset, not the gate.

FIX: don't let the interp present-pass consume the shadow stream. Added
`GpuGpuState::frame_end(..., int keep_shadow=0)` + `gpu_gpu_frame_end_keepshadow`; the interp pass calls the
keepshadow variant (resets only the draw batch s_tri/tex/semi, KEEPS s_shadow_n). Both passes now rasterize
the SAME shadow map (B positions) → probe shows `2556 / 2556` on every frame. This matches the user's chosen
cheap design: the dynamic shadow is NOT interpolated — it comes from the real composite (B) on both displayed
frames (a mover's shadow may lag it by up to half a frame on the in-between; explicitly accepted, no strobe).

2D OVERLAYS: not a bug in the live actor-transform tier — 2D/HUD carry fps_world=0, snap identically, and are
emitted from the same RenderQueue snapshot on both passes (byte-identical). The brief's "2D re-raster diverges"
was the OLD retired SW `s_interp` path's problem, already gone. Verified: before_interp == after_interp (0 px).

VERIFY: mechanical `s_shadow_n` 2565/0→2556/2556 (proven by toggling the fix in/out); `before_real` (no
shadow) vs `after_real` (shadow) diff (16.8% px) lights up exactly the hut/foliage/ground shadow regions
(scratch/screenshots/diff_real_shadow.png). USER to eyeball the live run for no strobe. Diagnostics added
(cfg-gated, no behavior change): `PSXPORT_DEBUG=fps60shadow` (per-pass s_shadow_n) and
`PSXPORT_FPS60_INTERPSHOT="path[:fence]"` (dump the interp frame's s_tex in isolation). Repro: set
`shadows=1 light=1 fps60=1` in psxport_settings.ini (or PSXPORT_SETTINGS), run the field.

## later-201: 60fps rearchitected to ONE pipeline — interp frame = the same full queue, twice (keep_shadow + HUD-bypass REMOVED)
USER DIRECTIVE: "There shouldn't be any difference between how 60fps renders and how 30fps does — integrate
into the 3D pipeline." The later-200 `keep_shadow` flag and the HUD's direct `gpu_gpu_draw_tritri` bypass were
SYMPTOMS of a SEPARATE interp path: a subsystem that lands on only one of the two presents needs a per-feature
patch to also run on the other. The fix is to make the in-between go through the IDENTICAL pipeline, not to
patch each subsystem.

REARCHITECTURE: the render queue is now THE COMPLETE FRAME; each 60fps present emits the whole queue and runs
the full present pipeline (panel_render = opaque/semi + shadow_pass + ssao_pass + 2D). Two things were moved
INTO the queue so they ride both passes by construction:
- HUD → queue. `engine/hud.cpp` `hud_quad` now calls `rq_push_2d_quad` (new, gpu_native.cpp) → an RqItem on
  layer RQ_HUD, instead of a direct `gpu_gpu_draw_tritri`. The HUD is part of the snapshot, emitted on BOTH
  passes → no flicker.
- Dynamic shadow geometry → queue. RqItem gained host-only `sh_cast` + `sh_vx/vy/vz[4]` (the opaque world
  prim's view-space verts). `gpu_draw_world_quad` takes an `sv` arg; the GT3/GT4 submitters (engine_submit.cpp
  `shadow_verts`) and native_terrain.cpp fill it instead of calling `gpu_gpu_shadow_push_tri` directly.
  `gpu_emit_rq_item` re-pushes the shadow tris on EVERY emit → the shadow map rebuilds identically on each
  pass. Shadows stay un-interpolated (build_lerp leaves sh_v* at B), per the user's design.

REMOVED: `keep_shadow` param on `GpuGpuState::frame_end`, `gpu_gpu_frame_end_keepshadow` (header + both defs),
and the keepshadow call in `gpu_fps60_present_pass` — `frame_end` now resets the shadow stream every present,
refilled by the next queue emit. No per-feature special-casing remains.

VERIFY: (1) clean `build_port.sh all`, boot, 0 bad opcodes. (2) DEFAULT 30fps field guest-RAM dump (run 8)
BYTE-IDENTICAL to a pre-change baseline (render changes don't touch guest RAM); deterministic across runs.
(3) `debug fps60pass` (new REPL diagnostic, cfg-gated) at the field with fps60+shadows+light: interp and real
pass report IDENTICAL counts every frame — `prims=942 HUD=3 shadow_cast=569` on BOTH — proving the HUD and the
shadow casters are in the queue and emitted on both presents (no flicker/strobe). after_real shot shows the
field with the 3 HUD indicators + shadows. USER eyeballs the live run for motion smoothness.

GOTCHA (cost a long debugging detour): RqItem lives in render_queue.h, included by game.h, so it sizes `Game`.
Changing RqItem changes `sizeof(Game)` and every member offset — an INCREMENTAL `build_port.sh <files>` that
misses a TU including game.h leaves a stale .o with the OLD layout → an ODR size mismatch → `core->game->fps60`
computes a garbage `this` → SIGSEGV deep in unrelated code (it crashed in rate_tick with `d` pointing into SDL
symbols). ALWAYS `build_port.sh all` after touching render_queue.h / game.h / core.h. (Confirms the existing
"core.h/game.h edits need build_port.sh all" note — extend it to render_queue.h.)
ALSO: `PSXPORT_DEBUG=<chan>` env does NOT enable channels (no startup getenv→cfg_dbg_set wiring); channels are
REPL-only via `debug <chan>` — already in memory, re-confirmed here.

---

## later-208 (2026-06-21) — SPAWN subsystem fully owned + a latent empty-list bug; HUD item-popup RE (HP-gauge correction)

**SPAWN subsystem — DONE (engine/entity_spawn.cpp).** Owned the remaining pieces so the engine now owns
object spawning end-to-end:
- `FUN_80079F90` / `FUN_8007A12C` / `FUN_8007A2C8` — the three remaining pool spawn primitives (dispatcher
  classes 2/3/4). RE'd from disas: each is the pool-2 pop+link+stamp with a different free-list head +
  count byte, and returns 0 (jr ra; v0=zero) when its pool is empty — NO delegation (unlike pool-2, which
  delegates to 79F90). Free-head/count: var2=(0x800F2398,0x800ED8CC), var3=(0x800ED8D4,0x800ED8C5),
  var4=(0x800ED8D0,0x800ED8C4). All share `spawn_link_stamp`. Gate `spawnvarverify` 0-diff.
- `FUN_8007AA38` — spawn-RELATIVE-to-object dispatcher (table 0x80016E64). Guards `(u8)obj[+0x0a]==a3`,
  then `cls=(a1>>8)&0x7f; type=a1&0xff; if(cls>=5) return 0; return SPAWN_VAR[cls](obj,type,a2,a3)` (the 5
  thin handlers 0x8007aa90..aad0 each `a1&=0xff; jal SPAWN_VAR[class]`). Unlike FUN_8007A980 it passes the
  caller's mode (a2) and list (a3) through. Gate `replacedispverify` (not exercised at seaside, 0 calls,
  like pool2verify — RE-verified). Commits 3a67eb6 + 71b63f1.

**REAL BUG fixed in the shared `spawn_link_stamp`:** when the target active list is EMPTY, the recomp
initializes BOTH end pointers — a head-insert also writes `*tail`, a tail-insert also writes `*head`. The
native body only set the one end, so an empty-list insert diverged at the list head ptr 0x800F2624 (caught
by spawnvarverify; the seaside-only spawnverify never hit an empty active list). The fix applies to all
five primitives (79C3C/79DDC were latently wrong too, just never exercised empty at seaside).

**HUD item-popup RE — and a CORRECTION to an HP-gauge mis-read.** While hunting Tomba's HP global via code:
the HUD gauge/counter renderer is `FUN @0x80039110` (helper FUN_8007E938; 12 callers found by scanning for
`jal 0x8007e938`). It sets `s3 = 0x800BF870` (save block B) and reads B+0x1c..0x1f + flags B+0x11.
INITIALLY mis-read B+0x1c as "HP segment count". WRONG — verified against the item-event dispatcher
`FUN @~0x8004a3xx-0x8004a7xx`: that handler, per acquired item, writes `B+0x1c = item id` and `B+0x1d =
category (1 or 2)` and calls the give-wrapper 0x8004d4c4. So **B+0x1c..0x1f are the RECENTLY-ACQUIRED-ITEM
HUD popup (icon id + category), NOT a persistent HP/AP gauge** (FUN_800376ec draws an item icon by id, not
a segment bar). Consistent with the inventory "recently-acquired ring" (B+0x13 len, B+0x14 data). DO NOT
treat B+0x1c as HP.
- The persistent **HP / AP gauge global is still UNFOUND**. Live-confirming it is also BLOCKED in the
  reachable seaside field: B+0x11 (the HUD-enable flags the drawer gates on) reads 0 after `newgame; skip
  700; run 240` AND after `tap start` drives — i.e. the HP/AP gauge is NOT active there (same reachability
  wall as the apple-on-a-log scene; matches docs/reference/issues/issue6_12_re.md "AP-gauge state not
  reachable headless").
- **LEAD for the user's "red=1 / blue=2":** the item category byte B+0x1d ∈ {1,2} set per item in the
  0x8004a3xx dispatcher is a 2-way distinction worth chasing — but it is a CATEGORY, not confirmed to be an
  HP/AP heal amount. NEXT: find the shared HEAL fn (writes the real HP/AP gauge global, area-independent, in
  MAIN.EXE engine/player code) by locating the gauge global first (find the damage/death path, or a live
  state where B+0x11!=0 with the gauge on), then the apple/fruit overlay handler that calls it.

**later-208 cont. — DESPAWN owned (FUN_8007A624).** Inverse of spawn: unlink from active list + free-list
push (5 trivial handlers 0x8007a718..a7a8 whose pools = the spawn descriptors; cls4 also jal 0x8007ADDC) +
deactivate epilogue. GOTCHA: the deactivate epilogue at 0x8007a7d0 clears MANY node header words
(0/4/8/c/10/14/18/38) + bytes (0x29/0x2a/0x2b/0x5e), not just node[0]/[4] — first native attempt diverged at
node+0x0a (the list-id byte, inside the 0x8 word). RE'd the full epilogue. `despawnverify` 0-diff over 100+
live despawns (commit 2cc6478). Object-pool alloc/free lifecycle now COMPLETE + PC-native.

**later-209 — OBJECT RECORD + RENDER-STATE subsystem owned (shared by collectables).** Per the user
(2026-06-22: "stop chasing HP/AP, keep owning game subsystems until HP/AP reveal themselves naturally").
Owned 3 shared engine routines the collectable/entity handlers call:
- `FUN_8007AAE8` render-record BUMP ALLOCATOR (cursor 0x800E7E74 over a record-ptr array, count 0x800ED098;
  record=*cursor; cursor+=4; cnt--; empty→0). Gate recallocverify 0-diff 1000+.
- `FUN_80051B70` per-object render-record INIT (alloc + zero/init record scale 0x1000, stamp obj render
  fields obj[+0xc0]=record etc., rec[+0x40]=table-driven data ptr from 0x800ECF58; pool empty→obj[+4]=3,
  ret 1). Gate recinitverify 0-diff 80+.
- `FUN_800517F8` per-object RENDER-STATE UPDATE (hot, 8000+/run): FUN_80085480 transform build + snapshot
  int16 pos obj[+0x2e/32/36]→32-bit obj[+0xac/b0/b4] + FUN_80051300; 2 callees kept content. Gate
  rendupdverify 0-diff 8000+ (also a perf win). All in engine/entity_spawn.cpp (commits 8bec1e7, 7547512).

**later-209 cont. — more shared object/spawn helpers owned (all engine/entity_spawn.cpp):**
- `FUN_80077B38` set object geometry-block ptr (obj[+0x38]=*(tbl+idx*4); obj[+0x0e]=ent[+2]&0x3fff). Gate
  setgeomverify 0-diff 5000+.
- `FUN_8006CBD0` set object transform block (copies a1[0..2]→scratchpad 0x1F8000D2/D6/DA, a1[3..5]→obj
  +0x3a/3e/42). Gate setxblkverify (0 calls@seaside, RE-verified leaf).
- `FUN_8003116C` spawn-and-init helper: guard pool0 cnt>=7, spawn type6/list1 via owned FUN_8007A980, seed
  pos from a1, init via FUN_80028E10 (content). Gate spawninitverify 0-diff 120+.
Method (user 2026-06-22): "keep owning game subsystems until HP/AP reveal themselves naturally" — work the
shared engine routines the collectable handlers call (ranked by call count in scratch/decomp/collectables.c).
Owned this run: spawn variants 79F90/A12C/A2C8, dispatchers 7AA38/7A980-already, despawn 7A624, record
alloc/init 7AAE8/51B70, render-state 517F8, geom/xform setters 77B38/6CBD0, spawn-init 3116C. NEXT shared
candidates (unowned, by count): FUN_8004BD64 (10, multi-mode pos midpoint), FUN_80083F50 (10, quadrant sine
LUT → belongs in mathlib.cpp), FUN_80083E80 (8), FUN_80054D14 (5), FUN_80040CDC (5).

## later-212 (2026-06-22) — FUN_80073CD8 owned `ov_beh_73cd8` (resident sibling per-object behavior SM)
Continuing the per-object behavior-handler descent (later-211 owned the first resident handler 0x800739AC).
Owned the second resident/generic handler the seaside placement table installs at node+0x1c:
**`FUN_80073CD8` → `ov_beh_73cd8` (engine/objbeh_73cd8.cpp).** Same state-byte shape as 0x800739AC but ~558
instrs: STATE 0 (init) builds the cull-record (FUN_80051B70, a2 = `(s16)DAT_800a4c94[area]`) + box/size, then a
per-`node[3]` sub-switch (jump table 0x80016B68, index node[3]-2 in [0,30]) seeding node+0x56/0x80..0x86/8/0xb
(case 0x11 bumps node+0x32 += 100 when `DAT_800bfe56 & 0x10`). STATE 1 calls the cull FUN_8007778C **and ignores
its result** (key difference vs 739ac, which early-returns on a cull miss), then runs a node[5] sub-machine (JT
0x80016BE8, [0,6]) → FUN_8007E110 (scene id from `DAT_800a4ca8[node[3]]`, special-cased for node[3]==2 via
DAT_800bf907/8c3), pad-edge `DAT_800e7e68 & DAT_1f800174` (scratchpad!), FUN_80042728, per-type confirm
FUN_80040B48(0x4e/0x4f/0x50). Tail: special-area (2/7/0x14) release of node+0x14, then node[0x2b]=0 + render
FUN_800517F8. Control flow + all node/global writes owned native; every sub-behavior call rec_dispatched (no GTE,
no render packets). RE'd 1:1 from disas (full function incl. both jump tables) — see docs/engine_re.md.
VERIFY: `obj73cd8verify` full-RAM (minus the callee stack window) + scratchpad A/B vs rec_super_call = **1400+
live seaside-field calls 0-diff, 0 MISMATCH, 0 bad opcode**; plain (non-gate) run renders the field clean.
Wired into run.sh + tools/build_port.sh SRC + game_tomba2.cpp registration. NEXT: the scene-overlay handlers
(0x8012/0x8013xxxx) the placement table installs; then Item 3 (FUN_800520e0 callees).

## later-213 (2026-06-22) — FUN_800741DC owned `ov_beh_741dc` (3rd seaside-resident per-object behavior SM)
Continued the behavior-handler descent (later-211/212 owned 0x800739AC / 0x80073CD8). To pick the next
VERIFIABLE target (rather than the documented scene-overlay handlers, which only run in non-seaside scenes
and can't be A/B-exercised headless), I added a throwaway counting override over candidate resident handlers
{0x800741dc, 0x80052078, 0x800499e8, 0x8004c930} and ran the seaside field: **only FUN_800741DC fires there.**
Owned it: `ov_beh_741dc` (engine/objbeh_741dc.cpp) — an item/pickup scene trigger, same state-byte shape as
the siblings but its state-1 dispatch is a plain if-chain (no jump table). State 0: cull-init (FUN_80051B70
a1=1, a2=0x18), box/size, node+0x56 = `DAT_800a4cec[node[3]]`. State 1 node[5] machine: scene-register
(FUN_8007E110 keyed DAT_800a4cf8), pad-edge (DAT_800e7e68 & DAT_1f800174 scratchpad), bounded child-spawn
(FUN_8007413C vs DAT_800a4d04[node[3]]/DAT_800bf874, else node[5]=99 re-arm), and case-4 completion (2×
FUN_80027144 packet emit + SFX + per-type collected bit DAT_800bfa23 / 0x1f reward). Like 73cd8 it calls cull
FUN_8007778C and IGNORES the result. WRINKLE: case-4 builds a 3-field struct on the guest stack for
FUN_80027144 — so `ov_beh_741dc` wraps the body in the recomp's own `sp -= 0x30` frame (restored on return)
so the buffer at sp+0x10 sits above the rec_dispatch sub-call frames exactly where the recomp puts it (and
the writes fall inside the verify's excluded [sp-0x800,sp) window). Control flow + node/global writes owned
native; every sub-call rec_dispatched (no GTE, no packets). VERIFY: `obj741dcverify` full-RAM+scratchpad A/B
vs rec_super_call = **500+ live seaside-field calls 0-diff, 0 MISMATCH, 0 bad opcode**; plain run renders
clean. Wired into run.sh + build_port.sh + game_tomba2.cpp. The seaside-RESIDENT behavior-handler set
(739ac/73cd8/741dc) is now EXHAUSTED; remaining placement handlers are scene-overlay (0x8012/0x8013xxxx) that
need their own scenes (and a reliable cross-area drive) before they can be A/B-verified — a real blocker for
autonomous headless progress on that branch.

## later-214 (2026-06-22) — PC-native object DEPTH from real world position (fixes invisible objects)
USER REGRESSION: after the spawn/object-ownership chain (c5ddfb9…) spawned the FULL object set (~145 vs ~12),
Tomba and many objects went INVISIBLE in Fisherman Village (`newgame; skip 650`). ROOT CAUSE (not a bandaid —
this is the real engineering fix the user asked for): every object's render-queue DEPTH was tagged with
`proj_obj_center_ord()`, which projects the object ORIGIN (0,0,0) through the **live GTE CR0-7** — i.e. whatever
camera×object transform was composed LAST. That makes object depth RENDER-ORDER-DEPENDENT (a PSX-ism leaking
in): billboards got a wrong/too-far view-Z (~0.05) and lost the depth test to terrain (correct per-vertex
~0.25–0.48, GREATER_OR_EQUAL = nearer wins) → vanished under the ground. The user's diagnosis was exactly
right ("depth must come from the object's real 3D world position, not PSX OT / order").
FIX: `obj_world_ord(c,node)` (engine/engine_submit.cpp) projects the object's REAL spawned WORLD position
(node+0x2e/0x32/0x36 — the fields entity placement stamps and movement keeps live) through the STABLE scene
camera, published once per frame at terrain draw (`camview_publish` in native_terrain.cpp; `proj_camview_world_ord`
/ `camview_valid` in gte_beetle.cpp — Rcam from scratch 0xF8 /4096, camT from 0x10C). Render order can no
longer leak into depth. Replaced `proj_obj_center_ord()` at ALL 7 object depth-tag sites (the universal
render-cmd chokepoint ov_render_cmd, submit_perobj_render, submit_render_walk default case, the snapshot walk,
ov_collectable_quad, +2). Falls back to the old live-GTE projection only before the scene camera is known
(first frame / no terrain). VERIFY: Fisherman Village headless `shot` — Tomba + crates + cart-wheels now render
visible at correct depth/occlusion (scratch/screenshots/fv_depthfix2.png), vs the prior all-invisible field.
146 obj_depth spans = the full object set. NEXT (the larger PC-native rebuild): the deferred-OT span-match
routing (obj_depth_lookup keyed by packet address) still exists only to carry this depth to billboards that
rasterize in the OT walk; owning the remaining billboard drawers (0x8003C8F4 + the unowned render modes) to
draw IMMEDIATELY PC-native from world position will retire the span hack and give per-collectable identity
(fixes the objid overlay merging all collectables into one box).

## later-215 (2026-06-22) — GHIDRA decomp pipeline (USE IT) + GAME area-load / overlay-class RE
**GHIDRA IS SET UP — use it for any RE instead of squinting at raw disas (USER directive).**
- Ghidra 12.0.4 headless: `<HOME>/dev/ghidra_12.0.4_PUBLIC/support/pyghidraRun -H` (Ghidra 12 needs the pyGhidra launcher for Python scripts). Reusable script
  `<local-notes>/skills/decomp-port/DecompDump.py` (skill `decomp-port`, SKILL.md §1-2 for usage).
- **Full MAIN.EXE decomp already exists: `scratch/decomp/ram_f1000_all.c`** (2MB, every resident FUN_*).
  Grep it FIRST — readable C beats hand-disassembly. Overlay decomps under `scratch/decomp/{a00,game,sop}/`;
  Ghidra projects under `scratch/ghidra/{A00,GAME,SOP}` (re-decompile is cheap).
- Decompile an OVERLAY: import raw with `-processor MIPS:LE:32:default -loader BinaryLoader
  -loader-baseAddr <guest base>` (PSX MIPS LE). Use `scratch/ghidra/` for the project +
  `-Djava.io.tmpdir=scratch/ghidra/tmp` (NEVER /tmp — quota'd tmpfs). file_off = vaddr - base.
- `tools/disas.py 0x<addr> --ram <dump>` stays the quick cross-check; the RAM dumps (ram_game.bin =
  derail w/ NO overlay; ram_derail2.bin = A00 loaded @0x80108f9c) are the ground-truth images.

**GAME-stage area-load RE (the new-game derail frontier):**
- GAME.BIN = BIN/GAME.BIN, LBA 1882, size 0x2d74, base 0x80106228, ENDS exactly at 0x80108f9c.
- Two overlay CLASSES load RAW (no reloc) to the SAME slot **0x80108f9c** (the byte right after GAME.BIN):
  - **MODE overlays** (SOP/OPN/CRD/DEMO/START.BIN) — have a real FUNCTION at **0x80109450** (the per-frame
    game-mode state machine: `switch(gamestate sm+0x50)`, fade/spawn/…). GAME.BIN's running sub-mode-0
    handler (FUN_8010882c, sm[0x4a]==0) does `jal 0x80109450` every frame = a CROSS-OVERLAY direct call
    into the resident MODE overlay (link-time symbol map; not a trampoline, nothing patches it).
  - **AREA overlays** (A00..A0L.BIN) — have a DATA jump-table at 0x80109450 (per-object-type dispatch,
    entries into FUN_80113700 object-list walker). Their header @0x80108fa0 = count(10) + state-handler
    entry pointers (e.g. A00 hdr[0]=0x8010a3ac into the actor state machine FUN_8010a33c).
- Per-area file table @ **0x800be118** (stride 8, (LBA,size); 25 entries). A00..A0L at table idx 3..23,
  indexed by **(area_id+3)&0xff**. The area-CODE overlay loader = `FUN_80045080(dest=0x80108f9c, area_id+3)`
  = `cd_dc40_sync(0x80108f9c, table[idx].LBA, table[idx].size)` (raw, no reloc). Called from `FUN_800452c0`
  (task-1, the cooperative area-load that never spawns in our port). Full FUN_800452c0 body in
  ram_f1000_all.c:24267 (code overlay → FUN_8007566c per-area cel/scene → FUN_80044f58 texgroup →
  DAT→0x8018a000 + reloc → done flags 0x1f80019b=1). Fresh new-game first area = **area 0 (seaside) = A00**
  (task0[0x6e]=0 at GAME entry; idx 3; LBA 374, size 0x459A8).
- **CONCLUSION (do NOT patch 0x80109450):** loading the AREA overlay (A00) to 0x80108f9c is the WRONG
  overlay class for the sub-mode-0 `jal 0x80109450` path — that path needs a MODE overlay resident. The
  new-game derail is an **overlay-SEQUENCING** issue: GAME runs the MODE-overlay machine (sub-mode 0) with
  no mode overlay loaded. NEXT: find which MODE overlay the new-game opening needs + what selects/loads it
  (FUN_8005082c arms flags 0x800ea0d4/0x800ec144 + 0x800bf8a4..7 = mode/transition request; find consumer).
- TOOLING ADDED: REPL `newgame` now FREEZES at the GAME prologue (native_boot.cpp: `continue` before
  native_step_frame) so immediate `r`/`dumpram` see a clean GAME-entry state (was derailing same frame).

## later-215b (2026-06-23) — SOP mode-overlay load address fix (derail -> understood hang)
ROOT CAUSE of the new-game GAME derail (was "jal 0x80109450 into empty/zero region"): the native DEMO s0
owner (engine_demo.cpp ov_demo_stage_main) loaded the SOP MODE overlay (BIN/SOP.BIN, file-table idx 2) to
the WRONG dest **0x80118f9c** instead of **0x80108f9c** — a `lui 0x8011 | 0x8f9c` decode slip vs the real
`lui 0x8011 + addiu -28772` = 0x80108f9c (the overlay slot immediately after GAME.BIN). So SOP sat 0x10000
too high; 0x80108f9c..0x80109xxx stayed zero; GAME's running sub-mode-0 `jal 0x80109450` hit zeroed RAM.
- KEY RE (later-215 + agents): TWO overlay classes load to the SAME slot 0x80108f9c — MODE overlays
  (SOP/OPN/CRD idx 0/1/2, a real fn at 0x80109450 = the per-frame field-mode state machine) and AREA
  overlays (A00.. idx 3+, a data jump-table there). New-game needs SOP (idx 2). SOP loads in the DEMO
  stage (s0), survives into GAME, runs via `jal 0x80109450`; later the AREA overlay overwrites it.
- FIX: engine_demo.cpp:395 dest 0x80118f9c -> 0x80108f9c. VERIFIED: SOP now resident from DEMO frame 1
  (0x80109450 = 3C021F80 = `lui v0,0x1f80`, the SOP mode machine prologue); newgame reaches GAME with NO
  derail. (Also fixed an earlier-attempted A00 forced-load experiment which was the WRONG overlay class.)
- REMAINING (next): SOP state-0 (FUN_80109450 sm+0x50==0) spawns the cooperative area-DATA load via
  FUN_80044bd4(&LAB_80109164,0,0,3) and yield-polls 0x1f80019b; the spawned task never completes (no-IRQ
  coop scheduling) -> hangs at interp PC 0x80051f80 (the yield). Per user: the ENTIRE call chain must be
  PC-owned (override system is gone) -> own the area-load synchronously / run the coop scheduler natively.
  SOP mode-machine map: scratch/sop_mode_re.md; level layout: scratch/level_layout_re.md; overlay seq:
  scratch/overlay_seq_re.md. eng_load_game_area_code (engine_stage.cpp, PSXPORT_OVLIDX-gated) is a WIP probe.

## later-215c (2026-06-23) — RE-WIRE the cooperative yield (ChangeThread -> ov_switch) — scheduler restored
The override-system removal (later-212) silently disconnected the cooperative TASK-SWITCH: FUN_80080880
(ChangeThread) was the universal yield/task-end primitive (FUN_80051f80 yield + FUN_80051fb4 task-end both
`jal 0x80080880`), and it USED to be an address-keyed override -> ov_switch (saves the task's resume regs +
longjmps to the native scheduler). With the table gone and 0x80080880 outside the platform-HLE window,
ChangeThread ran as pure interpreted kernel code that never longjmped -> EVERY GAME-stage per-frame yield
(FUN_80051f80) spun forever at PC 0x80051f98 (the scheduler ran slot 0 once and never came back). DEMO was
unaffected (it uses the native per-frame dispatcher ov_demo_frame, no coroutine yield), so this only bit at
the GAME stage — the first coroutine-yielding task.
- FIX: re-wire via the surviving platform-HLE table (sync_overrides.cpp): widen plat_in_window to include
  the kernel/BIOS window 0x80080000-0x8009E000 (kernel thread primitives at 0x80080xxx are platform, not
  game logic), and `plat_register(0x80080880, ov_switch)`. Made ov_switch non-static (native_boot.cpp).
- VERIFIED: GAME slot 0 now yields+resumes (resume_pc=0x80051FA4); the cooperative AREA-LOAD task SPAWNS and
  RUNS (slot 1 @0x80109164 = SOP state-0's loader LAB_80109164); slot 2 (XA reader @0x8001CFC8) spawns.
  Front-end unaffected (DEMO reaches s2 clean, no derail/hang). This is the general fix for ALL cooperative
  tasks, not just the area load.
- NEW blocker (next layer): the area-load task spins in libcd at 0x8008a720 (a CD-command wait that uses
  VSync 0x80085900 as its clock) — an un-HLE'd libcd CdRead path. Per FAIL-FAST: make that CD read native +
  synchronous (extend cd_override / sync_overrides). NB earlier pitfall "don't force-sync FUN_8001D940" was
  about a bad manual restart; the proper fix is native-synchronous CD I/O so the wait is never reached.

## later-215d (2026-06-23) — restore the orphaned CD-subsystem HLEs — NEWGAME REACHES GAMEPLAY
With the cooperative yield re-wired (215c), the area-load task ran but slot 2 (the cooperative XA/BGM
streaming reader FUN_8001cfc8) spun forever in libcd CdSync (FUN_8008a6ec) — wedging the whole scheduler
frame. ROOT CAUSE: the override-table removal ORPHANED the entire CD-subsystem native HLE set (only
FUN_8001DC40 had been migrated to platform-HLE). Recovered the original set from the removal commit
(faeb436) and re-registered via platform_hle_register in cd_overrides_init:
  0x8001D2A8 ov_voice_play, 0x8001CF2C ov_voice_stop, 0x8001DB8C ov_cd_loadfile, 0x8008AC34 ov_cd_command,
  0x8008A6EC ov_cd_sync (CdSync->complete), 0x8001CE90 ov_cd_cmd_stream (streaming GetlocL pos in range),
  0x8008C1EC ov_cd_read. Two DELIBERATE omissions: 0x8001D940 (ov_cd_async_read — the shared async core,
  force-syncing corrupts the coop area overlay, verified pitfall) and 0x8008B2D8 (CdInit, owned by
  sync_overrides ov_cdinit_hs). (platform-HLE window already widened to 0x80080000-0x8009E000 in 215c.)
- **VERIFIED: newgame now boots ALL THE WAY into gameplay** — DEMO->GAME->SOP mode machine runs its state
  flow (sm[0x50] 0 LOAD -> 1 FADE -> 2 GAMEPLAY per scratch/sop_mode_re.md); 200+ frames with ZERO
  derail/stuck/fault; renderer processes hundreds of prims (projprim ~490, 2D-prim ~980). Headless `shot`
  shows the new-game OPENING CUTSCENE rendering (intro narration "Tomba is living peacefully... Tabby has
  disappeared" over the scene) — scratch/screenshots/newgame_field.png. USER eyeballed.
- This completes the boot->gameplay chain the override removal broke. The full causal stack was: (215b) SOP
  mode overlay loaded to wrong addr -> (215c) cooperative yield disconnected -> (215d) CD HLEs orphaned.
- NEXT: drive into the actual field (skip the intro), exercise movement/camera; verify render fidelity on
  the live windowed build (USER); the OT-walk-enumeration retire + 3D-position ordering remain (later-214).

## later-215e (2026-06-23) — sync the area-DATA streaming reader -> SKIP-CUTSCENE FIELD RENDERS (black screen fixed)
After 215d, newgame reached the intro cutscene, but SKIPPING it left a BLACK field. ROOT CAUSE: the
cutscene->field transition triggers the cooperative AREA-DATA load (area machine sm[0x4a]=1/sm[0x4c]=2 ->
FUN_80044cd4 -> task1 FUN_800452c0 -> FUN_8001db38 -> FUN_8001d940). FUN_8001d940 (the libcd async
streaming reader) loops `FUN_80051f80(1) yield; poll status&count` until the per-sector IRQ callback drains
_DAT_1f8001f4 — but no IRQ fires, so the count never reaches 0, the read never completes, the load-done flag
0x1f80019b stays 0, and the area machine cycles forever -> 0 prims -> black.
- FIX: register FUN_8001D940 -> ov_cd_async_read in cd_overrides_init (the existing synchronous replacement:
  reads the descriptor 0x1f8001f0/f4/f8, delivers all sectors via disc_read_sector, zeroes the count).
- **The OLD "do NOT sync the shared core FUN_8001D940 — it corrupts the overlay" pitfall is FALSIFIED /
  STALE.** It dated from BEFORE the cooperative-yield fix (215c): with the scheduler broken, the core ran in
  a wrong/incomplete context. Now that the scheduler runs tasks correctly, syncing the core is CORRECT and
  safe — VERIFIED: 0x1f80019b set, NO derail/corruption over 400 post-skip frames, the seaside/village field
  RENDERS (Tomba + hut + terrain + objects + "Go to the Burning House!" banner — scratch/screenshots/
  after_skip2.png, field_stable.png). USER eyeballed.
- STILL OPEN: cutscene BGM doesn't play (audio path — ov_voice_play/xa_stream; investigate next). And the
  area machine sm[0x4e] cycles (intro/fade states) — confirm it settles into walkable play with input.

## later-215f (2026-06-23) — cutscene/area BGM investigation (voice works, music silent) — NOT headless-verifiable
USER (windowed ./run.sh): intro/fisherman cutscene VOICE plays correctly, but the MUSIC doesn't.
Findings (headless trace, PSXPORT_XA_DBG):
- The music IS triggered: ov_voice_play fires `chan=4 [84515..97979] loop=1` (BGM.XA, the looping area
  music) at f104; it goes active (xa_stream act=1 loop=1), is briefly suppressed by a dialog tone
  (song=4, the dialog-vs-music coordination mod), then RESUMES (f154) and stays active over 500 frames.
- Sectors are VALID: `discdump subhdr` shows BGM.XA (music) and DEMO.XA (voice) are BOTH file=1 with
  channels 0-7 interleaved, so the chan-4 music sectors exist and PASS the decoder filter
  (file==1 && chan==4 in xa_stream.c). So the filter is NOT the cause.
- ROOT-CAUSE NOT YET FOUND, and NOT headless-verifiable: the XA decode (xa_decode_next_sector) is
  PULL-driven by the real SPU/audio output (Beetle spu.c CDC_GetCDAudioSample). Headless drives NO XA
  decode at all (0 sector traces for voice AND music), so headless cannot distinguish working voice from
  silent music — needs the windowed audio build to diagnose.
- NEXT (needs USER's audio build): run `PSXPORT_XA_DBG=2 ./run.sh`, reach the field, and read the
  `[xa]  sector LBA ... chan=4 ... n=<samples>` lines — if n>0 for chan-4 sectors the DECODE works and it's
  a mix/volume issue; if chan-4 sectors are absent/filtered or n<=0 it's a decode/stream issue. Also suspect
  the dialog-music coordination mod (cd_override.cpp xa_dialog_coord) over-suppressing in the field, and the
  music_fade_in ramp. (The chan-4 area music had prior trouble — see the EOF-early-loop note in xa_stream.c.)

## later-216 (2026-06-23) — PC-NATIVE AUDIO ENGINE: directive + Phase-0/1 (ADPCM/VAB decode PROVEN)
USER DIRECTIVE: drop the PSX sequencer + XA, replace with a PC-native audio engine. Order: (1) load
SEQ+VAB+XA natively, (2) native sequencer + voice synth, (3) fold XA into the native mixer, (4) retire
Beetle spu.c for game audio. ONE behavior, no env A/B; verify by listening + offline WAV A/B vs Beetle.
- DIAGNOSTIC (supersedes later-215f's "sequenced audio silent" framing): captured the SPU MIXED output
  headless via PSXPORT_WAV (forces spu_render even with no audio device). menu-cursor SFX (pure SEQ, no XA)
  rendered REAL audio (peak 11753, RMS at the tap frames); intro rendered continuous content. So libsnd SEQ
  voices DO synthesize in Beetle — the pipeline is not dead (play flag set, rd ptr advances, VAB DMA'd to
  SPU RAM 46144 words, KON fires with nonzero voice bits). The windowed silence is likely output/coord, not
  a dead pipeline. Moot under the native rewrite.
- RE COMPLETE (formats are all STOCK PSX): TOMBA2.SND container = u16[24] VAB sector table + concatenated
  SEP sequences (resident 0..0x51800 -> RAM 0x80182000) + 24 VABs ('pBAV' ver7) from 0x51800. SEP='pQES'
  ver1 ppqn384 tempo-us/quarter BE, MIDI running-status events. VAB=VabHdr32 + ProgAttr[128]*16 +
  ToneAttr[ps*16]*32 + VAG-size-table(256 u16) + ADPCM body. Full spec: scratch/native_audio_spec.md;
  container detail: scratch/snd_container_re.md. Raw file: scratch/bin/snd/TOMBA2.SND.
- BUILT + VERIFIED: tools/snd_render.c (standalone) parses the container + all 24 VABs (sane prog/tone/vag
  counts, ToneAttr vol/pan/centre/adsr/vag all correct) and decodes PSX ADPCM. VAB0 VAG1 -> clean TONAL
  waveform (peak 30704, rms 8497, 23.7% zero-crossings = instrument note, not noise). The riskiest piece
  (ADPCM 2-tap filter math) is PROVEN. Filter coeffs pos{0,60,115,98,122}/neg{0,0,-52,-55,-60}.
- NEXT (in scratch/native_audio_spec.md §STATUS): voice synth (ADSR+pitch+vol/pan) -> SEP sequencer ->
  offline full-song render + A/B vs scratch/wav Beetle captures -> wire into ov_frame_update (stop the
  interpreted libsnd tick) -> fold native XA into the native mixer -> retire Beetle spu.c for game audio ->
  delete the cd_override dialog-coordination bandaid.

## later-218 (2026-06-24) — DEMO front-end: ATTRACT (s7) owned native; freeze fixed, render works
USER REPORT: native ./run.sh FREEZES on the idle->attraction-demo transition (and on Load data); PSX
fallback (PSXPORT_GATE=1) works. Both are deep-yielding DEMO substates not owned by the native per-frame
dispatcher ov_demo_frame (engine_demo.cpp), which only handled s1/s2/s3/s5/s6 — substate 7 (attract) hit
the `default` case and silently spun (bumped the frame counter, no progress, FAIL-FAST violation).
- ROOT CAUSE (attract): at idle, the title sub-machine (s2) returns outcome 1 -> sm[0x48]=7. The s7
  trampoline 0x80106668 calls the 3-phase machine 0x80106C24 (phase0 launch+area-load, phase1 per-frame
  engine update + attract render counting down sm[0x5a], phase2 teardown -> restart at sm[0x48]=0). phase0's
  area-load and phase1's update deep-yield in the guest; the native per-frame model can't resume a guest
  coroutine, so it must run them synchronously.
- FIX (engine_demo.cpp): added demo_frame_s7 (faithful to 0x80106C24, title_ram disasm) + wired ov_demo_frame
  case 7. phase0 owns the field writes + cursor table {0,1,3}@0x8010770c selection, runs the area-load
  SYNC via native_transition_area_load (the 0x800452c0 callback body — the area selector comes from
  sm[0x6d]/sm[0x6e], not the 0x80044bd4 latch), then the reinit jals; phase1 rec_dispatches the return-based
  per-frame update (0x80106e28 gameplay / 0x80106ee4 type-3) — one frame each, no yield; phase2 teardown +
  restart. Also extracted demo_frame_s0 (was inline in ov_demo_stage_main) and wired ov_demo_frame case 0 so
  the attract->title restart reloads the menu resources. VERIFIED: no more freeze — phase 0->1 advances,
  engine update runs each frame (no yields caught), attract RENDERS real gameplay (seaside village, Tomba +
  HUD, camera pans — headless shots scratch/screenshots/attract_f600/f800.png; USER confirmed gameplay shows).
- REMAINING FRONTIER (shared root cause): the recorded-input/attract subsystem is NOT armed. Recording-active
  byte 0x800bf4f8 == 0 during the native attract, so (a) the blinking "DEMO" text overlay never draws (its
  handler, one of 8 in the jalr table dispatched by 0x80026368, runs with flag 0x1f80019a=1 but the recording
  state it needs isn't set), and (b) 0x800524b4 (end-of-recording check, reads 0x800bf4f8) never fires, the
  sm[0x5a] timer is huge (=0x8007982c's return), so the attract NEVER cycles back to the title (plays the demo
  area forever — not a freeze, but wrong). The arm is the a2=1 attract-mode path of the spawn
  FUN_80044bd4(0x800452c0, entry, a2=1, 2): the spawn latches a1->task[0x6e], a2->task[0x6d] so the load task
  sees sm[0x6e]=entry, sm[0x6d]=1 (vs the GAME path's a2=0). native_transition_area_load runs with the DEMO
  task's sm (cursor), so it loads a valid area but takes the GAME (a2=0) path and never arms the recording.
  NEXT: find the recording-arm (writer of 0x800bf4f8, struct-relative so not absolute-offset scannable; the
  consumer is SOP 0x80102530) and the demo-recording file load; transcribe the a2=1 attract load faithfully.
  Diagnostic added: PSXPORT_DEBUG=demoflag (interp.cpp) logs which demo-flag 0x1f80019a reader PCs execute.
- Load data freeze: same class (deep-yielder substate, likely s4/load sub-page) — not yet root-caused.

## later-223 — door-enter freeze (X≈14656 seaside house) root-caused; state-3 transition path owned native
USER bug: walk RIGHT into the X≈14656 treehouse (#DB80 @ 14656,-2636,1676) → screen fades to black, frozen
(first house works). Repro headless: skip into field, `w 800e7eac 39400000` (tp Tomba X→14656), press right,
run ~900 — black, frame loop still runs (watchdog won't trip; HUD draws, 3D scene empty). Full chain:
- Walking right flips the field running sub-machine **sm[0x4c] 2→3** (mid-transition handler). Owned native
  this session: ov_field_run_x (0x801070b4) / ov_field_frame_x (0x80108be4), wired into ov_game_submode1
  case 3 (engine_stage.cpp). Behavior identical, now traceable C — no regression (field 98% non-black).
- Screen-transition sequencer **FUN_80026ad0** (caller 0x80116870, found via new REPL `trace` cmd — it's
  indirect, not an entity handler, not a static jal) runs ~1×/frame in state 3. Door case sets sm[0x4c]=3 +
  **bf818(0x800bf818)=2**, then waits at substate 3 for **bf818==3** to reach substate 4 (which sets
  sm[0x4c]=2 = done). bf818 stays 2 → stuck forever → black. (gate bf80f=0, not the blocker.)
- **bf818=3 = "destination area READY" handshake**, written only by FUN_80073300 ← FUN_80073328's node[6]
  state machine (gated on node[6] reaching states @0x800734a0/0x80073518 + bf80f==0 + bf818==6). In the stuck
  window (trace): FUN_80073300 fires 0×, area-load body 800452C0 0×, FUN_800782F0 0×, trigger behavior
  FUN_800739AC (owned native objbeh_739ac.cpp; its warp/confirm sub-states are transcribed-but-untested) 0×.
  Teardown 8001cf2c ran 1×. So the transition is HALF-done: torn down, destination-area LOAD + ready-signal
  NEVER run → handshake never completes. SAME FAIL-FAST class as the Load Game freeze / later-215c/d (a
  spawned cooperative load task whose scheduling is orphaned post-override-removal).
- NEXT: own FUN_800782F0 (area transition the trigger calls) + the door load path native+SYNC (mirror
  native_transition_area_load); find where the destination-area load is meant to be spawned in state 2
  (trace a tight window before the 4c→3 flip). Entity lists UNCHANGED across the freeze (NOT objects-cleared).
  Full detail + addresses: scratch/handoff_door_freeze.md. DEAD END: objid_split.png is the START area, not
  this house; #DB80 = Tomba's own node handle.

## later-224 — PC-NATIVE world-coord render module (engine_project) + live field-render chain MAPPED
USER directive (this session): build a COMPLETELY PC-native renderer, fully decoupled from the PSX, based on
objects' REAL WORLD COORDINATES — no fallback; put the code in a proper engine module (NOT gte_beetle).
- **New module `engine/engine_project.{h,cpp}`** — the PC-native object render transform & projection.
  `eproj_compose_object(cmd)` composes camera × object in FLOAT from the object's real world matrix (cmd+0x18)
  + world position (cmd+0x2c) + the scene camera (scratch 0x1F8000F8 / 0x10C); `eproj_compose_camera()` does
  the camera-only case (world-space verts). `eproj_vertex()` is the full RTPT in float — NO gte_op, NO CR0-7
  read for the picture. `eproj_set/clear/active` + `eproj_vertex_active` drive the submitters; `eproj_active_cr`
  packs the float xform back to CR0-7 for the 60fps midpoint reproject. Projection consts (OFX/OFY/H) still
  read from CR24-26 (the camera's frame-constant projection; a future engine camera can own them).
- **engine_submit converted**: `submit_perobj_flush` (gen_func_8003CDD8) no longer GTE-composes — it builds the
  float world xform via eproj and calls `native_gt3gt4` directly. NO FALLBACK: the old `native_dispatch` →
  gen_func_8003F698 per-mode PSX dispatcher is removed; every per-object geomblk is submitted as generic
  GT3/GT4 through the world-coord projection. The GT3/GT4 native submitters project via `eproj_vertex_active`
  (the resident byte-packed emitter submit_poly_gt4_bp still uses proj_native_xform/GTE — its upstream compose
  is still-PSX field code). `fps60_stamp_world_cr` added so 60fps works off the float xform.
- **KEY DISCOVERY — the foundation is DORMANT at the walkable field; the live render is a DIFFERENT chain.**
  Instrumented (PSXPORT_DEBUG=eproj / subc) + the REPL `trace`: at the seaside walkable field, submit_perobj_flush
  and ALL native submitters fire **0×**. The live per-object render is **0x8010810c (ov_field_frame render
  submit, a jump-table SM) → 0x8003D074 (per-object render, 76×/frame) → 0x8003F698 (per-mode dispatch) →
  0x8003F6E8 → 0x80146478 (overlay GT3/GT4 caller = ov_gt3gt4_caller, ORPHAN) → the overlay submit twins** —
  ALL interpreted. The render-walk list 0x800F2624 is EMPTY here (ov_render_walk no-ops). So owning the visible
  field render means owning THIS chain top-down (0x8010810c down), routing it through native_gt3gt4 + eproj.
- `ov_field_entity_render` (native 0x80109fe0, the SOP-mode entity loop, world-coord) is written + ready but
  UNWIRED — the SOP path isn't the walkable field, so it's unverified; own the live chain first.
- VERIFIED no regression: SOP intro narration + the walkable field both render identically (the new code is
  built in but dormant). Tooling: PSXPORT_DEBUG=eproj (compose count), subc (submitter counts).
- NEXT: own the live chain top-down from 0x8010810c → 0x8003D074 → 0x8003F698, set the camera-only/world
  eproj xform, route geometry through native_gt3gt4. That makes the VISIBLE field render fully PC-native from
  world coords. RE 0x8010810c (jump-table SM) + 0x8003D074 from the field dump (scratch/bin/field_game_ram.bin).

## later-225 (2026-06-24) — LIVE field render owned PC-native: orchestrator + world-coord render walks wired
Continues later-224. Goal: the VISIBLE walkable-field render runs through PC-native, world-coord code.
- **CORRECTION to later-224's chain.** The handoff said the live per-object render hangs off the overlay
  render-submit SM **0x8010810c** (sm[0x6b]). It does NOT. The render-submit SM at the seaside field is in
  state sm[0x6b]==0 (a guard/transition state — verified live: *0x1f800138=0x801fe000, [0x6b]=0). The real
  per-object render driver is **MAIN.EXE 0x8003f9a8**, called DIRECTLY by the native ov_field_frame
  (`if(*0x1f800136<2) d0(c,0x8003f9a8)`), and its transition twin **0x8003fa44** (from ov_field_frame_x).
  Found by extending the interp call tracer to log COMPUTED jumps (jr jump-tables were invisible — interp.cpp
  now trace_call's the computed-jump case too) and back-tracking who reaches the render chokepoint 0x8003cca4.
- **Render chain (all MAIN.EXE, resident, disas.py-visible):** 0x8003f9a8 (11-pass orchestrator) →
  0x8003bf00 / 0x8003eec0 / 0x8003bb50 / 0x8003bcf4 (the per-object render-queue WALKS) → per-type render
  handlers → 0x8003cca4 (per-object render dispatch) → 0x8003cdd8 (the GTE-compose flush) → 0x8003f698
  (render-cmd dispatch) → submit twins. The walk over list 0x800f2738 (count 0x1f80015c, jump table
  0x80014d38) drives ~12 per-object renders/frame.
- **KEY: those 4 render walks ALREADY had PC-native bodies** in engine_submit.cpp — ov_rwalk_aux_bf00 /
  ov_rwalk_aux_eec0 / ov_render_walk_snapshot / ov_rwalk_aux_bcf4 — written under the old override model and
  ORPHANED since the override table was removed (codemap: all [ORPHAN], "only the removed override table").
  Each walks its queue, dispatches every live object's per-type renderer (still-PSX content via rec_dispatch),
  and tags the produced packet span with the object's PC-native WORLD-POSITION depth (gpu_obj_depth_add) —
  i.e. engine-owned render ordering from real world coords, not the PSX OT. They just needed a native parent
  to call them.
- **DONE — new module engine/engine_render.{h,cpp}** (render code OUT of gte_beetle, per directive): owns the
  render orchestrator `ov_render_frame` (0x8003f9a8) + `ov_render_frame_x` (0x8003fa44), which the native
  ov_field_frame / ov_field_frame_x now call DIRECTLY (was rec_dispatch). The orchestrator wires the 4 orphan
  walks back into the LIVE render; 0x8003b588 (no real native, only a subcnt diagnostic) + the 5 non-walk
  passes (4fd30/25d98/d0bc/f024/df04/c048) stay rec_dispatch. Built + run-list updated (build_port.sh+run.sh).
- **VERIFIED (headless `shot`):** A/B at the seaside field — orchestrator-native + walks-rec_dispatch renders
  4:3 98.4% non-black (== baseline); orchestrator-native + walks-NATIVE renders the SAME scene correctly in
  WIDESCREEN 428×240 (90.5% non-black + HUD) — the world-coord depth path now drives the engine-owned render
  extent. Stable 320+ frames, zero derail/caught-yield. Shots: scratch/screenshots/render_walk0.png (4:3) vs
  render_final.png (widescreen). USER eyeball pending (`git pull && ./run.sh`). No A/B env/compile gate kept
  (ONE behavior — the bring-up WALK_NATIVE switch was removed before commit).
- **NEXT:** descend the render walks — own the per-type render handlers (0x8003cca4 + 0x8003c2d4/c464/c5f8/
  c788) and the per-object flush 0x8003cdd8 native, routing geometry through eproj/native_gt3gt4 so the actual
  VERTEX PROJECTION is world-coord float (not GTE). Those natives (ov_perobj_render 0x8003cca4, ov_render_cmd
  0x8003f698) also exist ORPHANED in engine_submit.cpp — wire them as the frontier reaches each. Contiguity:
  the walks are native now, so 0x8003cca4 is the next ownable node.

## later-226 (2026-06-24) — per-object VERTEX PROJECTION now world-coord float (eproj) in the LIVE render
Continues later-225. The render walks now drive the per-object render through the NATIVE dispatch
`submit_perobj_render` (0x8003cca4) instead of rec_dispatching the PSX body. submit_perobj_render's
flush-only case (the only one that fires at the field) runs the native `submit_perobj_flush`, which
composes the camera×object transform in FLOAT from the object's REAL WORLD coordinates (world matrix
cmd+0x18 + world position cmd+0x2c + scene camera) via engine_project (eproj) and submits every geomblk
through native_gt3gt4 → ov_submit_poly_gt3/gt4, which project each vertex with `eproj_vertex_active`
(float RTPT) — NO gte_op, NO CR0-7 read for the picture. This was the DORMANT later-224 foundation; it is
now LIVE.
- WIRED (engine_submit.cpp): the 4 render-walk per-type dispatch sites that called rec_dispatch(0x8003CCA4)
  — aux_bf00_case 0x8003BFAC, aux_eec0_case 0x8003EF20/0x8003EF30, aux_bcf4_case 0x8003BDAC, rq_dispatch_case
  0x8003BC00 — now call submit_perobj_render(c) (a0=node already set). The double depth-tag (walk session +
  submit_perobj_render's own session) is benign: PktSpanSession MERGES nested sessions and both tag the same
  span with the same obj_world_ord. Secondary-effect cases (rare, not at the field) still fall back to the PSX
  body via rec_super_call=rec_interp.
- VERIFIED (headless `shot` + USER-eyeball-ready): the seaside field renders IDENTICALLY to the GTE output
  (perobj_native.png == later-225 render_final.png — float eproj matches the PSX RTPT) AND under a moved
  camera (perobj_move.png, Tomba relocated: treehouse/bridge/terrain/sea/HUD all correct depth/occlusion).
  `debug eproj` confirms native compose FIRES (Tview reasonable, H=350) — it was 0× (orphaned) before.
  Stable, zero derail.
- NEXT: own the remaining per-type render handlers (0x8003c2d4/c464/c5f8/c788 — the non-flush-only object
  render variants) and 0x8003cdd8's secondary-effect cases native; then retire the PSX GTE compose from the
  per-object path entirely (eproj_active_cr already bridges fps60). Also: the resident byte-packed emitter
  submit_poly_gt4_bp still uses proj_native_xform/GTE for its (still-PSX) upstream field compose — own that
  compose to make terrain/BG projection world-coord too.

## later-227 (2026-06-24) — master render-list walk owned native; resident terrain → world-coord float
Continues later-226. Owned the MASTER phase-2 render-list walk **0x8003c048** native (the orchestrator now
calls ov_render_walk = submit_render_walk instead of rec_dispatch) and routed the field TERRAIN renderer
(node+24 == 0x8002AB5C) to the PC-native **ov_terrain → terrain_render_pc** (float transform, real per-pixel
depth via gpu_draw_world_quad, NO GTE / NO packet) — previously orphaned.
- RLIST (head 0x800F2624, table 0x80014DB8): at seaside only 2 LIVE nodes — 0x800fc5c0 (type32, renderfn
  **0x8013e9d8** = an OVERLAY drawer, the main ground/BG — still PSX) and 0x800edb80 (type32, renderfn
  0x8002AB5C = resident terrain). submit_render_walk's pre-scan passes (both RCASE_DEFAULT), native walk runs.
- VERIFIED: `debug rwalk,terrgte,terrpc` confirms NATIVE walk active → ov_terrain(node=800EDB80) →
  terrain_render_pc drew 1 quad (H=350). Renders correctly static AND under camera motion (terr_native.png,
  terr_move.png — Tomba relocated, terrain/props/sea all correct). Zero derail.
- FINDING: at seaside, the resident terrain 0x8002AB5C is only **1 quad** (geomblk 0x8009FAE8 has 1 record;
  ctl<=0 terminator on rec0). The big visible GROUND is the overlay node renderfn **0x8013e9d8** (still PSX
  GTE) — that's the next terrain target. terrain_render_pc holds static MODEL-space verts (no per-frame writes
  to 0x8009FAE8 observed via PSXPORT_CW) and projects them through the fresh camera each frame, so the static
  buffer is correct for static geometry. OPEN QUESTION (verify at a SCROLLING area): if 0x8002AB5C is meant to
  rebuild the geomblk per-frame at scroll-areas and nothing does now, a scrolling resident-terrain strip could
  go stale — revisit when a scrolling area is reachable. (At seaside: correct.)
- NOTE: `cfg_dbg` channels are set via the REPL `debug <a,b>` command ONLY — NOT the PSXPORT_DEBUG env var
  (that does nothing for cfg_dbg). Use `debug` in the REPL stream.
- NEXT: own the overlay ground drawer 0x8013e9d8 (the main seaside terrain) world-coord; then the billboard-
  quad handlers 0x8003c2d4/c464 → 0x8003c8f4 (pickups, still GTE-project their 4 corners).

## later-228 (2026-06-24) — the MAIN seaside GROUND now renders world-coord float (overlay node owned)
Continues later-227. Owned the seaside ground/BG node renderer **OVERLAY 0x8013E9D8** native (ov_bg_render,
engine_submit.cpp) and routed the master render-list walk's default case to it (fn == 0x8013E9D8). The recomp
wrapper: stack a position triple (*(node+0x14)) + node[0x4e/50/52], call the GTE visibility/bound setup
0x8013DD34 (kept PSX via rec_dispatch — writes only scratchpad cull temps 0x1F8000C0/0x1F800080, not the
per-command transform; the recomp calls the render UNCONDITIONALLY after it), then call the per-object render
dispatch 0x8003CCA4 = native submit_perobj_render -> submit_perobj_flush (world-coord eproj).
- This node (0x800FC5C0) carries **12 render commands** = the main ground geometry (geomblks 0x801e68xx..),
  so this is what makes the visible seaside GROUND render PC-native from world coords.
- VERIFIED: `debug bgr` confirms ov_bg_render fires (node=800fc5c0, cmds=12). Render is **BYTE-IDENTICAL** to
  the GTE output (bg_native.png vs terr_native.png: 0/102720 pixels differ — float eproj == PSX RTPT for the
  whole ground) and correct under camera motion (bg_move.png, Tomba relocated). Zero derail. `debug eproj`
  shows the BG command range composing world-coord.
- STATE OF THE FIELD RENDER now: per-object MESH (Tomba/props), the resident terrain quad, AND the main
  overlay GROUND all project world-coord float via eproj — NO GTE for those pictures. Remaining GTE users at
  seaside: the billboard-quad PICKUP handlers 0x8003c2d4/c464 -> 0x8003c8f4 (4-corner RTPT, but already
  world-coord DEPTH), and the byte-packed terrain-prop emitter submit_poly_gt4_bp's upstream compose.
- NEXT: own 0x8003c8f4 (billboard quad) projection via eproj to finish the per-object render GTE-free; then
  audit any remaining RTPS/RTPT callers at the field (the PSXPORT_RTPCALLER histogram is gated on the GPU
  present frame %50 and didn't fire headless — fix its trigger or add a per-frame gte_op RTP counter).

## later-229 (2026-06-24) — ROOT CAUSE of the missing crane/figure + the 6.96% diff: the VISIBLE FIELD IS STILL ~99% PSX 2D-BAND
Continues later-228. Chased the harness 6.96% diff (crane device + a figure + a pickup OCCLUDED/invisible on the
RIGHT in native; present in PSX). The chase OVERTURNS the later-225..228 story that "the field render is owned":
**the bulk of the visible seaside field is still drawn by STILL-PSX passes that emit GP0 packets → gp0_exec →
the flat 2D OT-BAND (no real depth)**. Only a small per-object-mesh subset goes through the real-depth owned
path (eproj → gpu_draw_world_quad). So the native field is essentially PSX flat layering with a thin real-depth
layer hidden underneath — which is exactly why it nearly matches PSX (6.96%) and why the user said "you need the
ownership of game's 3D OBJECTS, not sorting layers."
- HARD EVIDENCE (new diagnostics, all via cfg_str so REPL `debug`/env both work):
  - `PSXPORT_PRIMAT="x,y"` (gpu_native.cpp, gp0_exec + gpu_emit_rq_item): logs EVERY prim covering a DISPLAY
    pixel with is3d/bg/billboard/dep/layer/order_mode/node/tp/clut/col — point-in-triangle, sees BOTH the gp0
    OT-walk prims AND the queue/world prims (provat is BLIND to VK polys: it tracks CPU s_vram, polys tee to VK).
  - `PSXPORT_PAINTFG=1`: force every gp0 2D-FG poly to solid magenta. Result at the seaside field: the WHOLE
    ground/tree/house/Tomba turns magenta → the visible field IS gp0 2D-band, NOT owned real-depth geometry.
  - `debug ndepth`: real-depth(3D) gp0 prims = **3/frame**, OT-band(2D) = **337/frame** (3D% = 0.9%). The
    native submitters DO fire (subc: gt3/gt4_native ~56/frame) but that output is a minority, mostly hidden
    UNDER the 2D-band field. (ndepth counts only gp0_exec prims; gpu_draw_world_quad prims bypass it.)
- WHERE THE 337 2D-BAND PRIMS COME FROM (bisect via the temp PSXPORT_SKIPPASS mask over ov_render_frame's
  rec_dispatch'd non-walk passes): skipping ALL non-walk passes → 2D-band drops 337→0. Per-pass:
  **0x8003d0bc emits ~220 prims (the GROUND/terrain — a 22-case MODE dispatcher keyed on *0x800BF870, jump
  table 0x80014EF0, arg a0=0x800F2418), 0x8003b588 emits ~117 prims** (NOT "diagnostic-only" as later-225
  claimed — that note is FALSE; fix it). The billboard handlers (0x8003C2D4/C464/C5F8/C788) are NOT the
  source (SKIPBB → still 337) and the per-object flush is not either (g_perobj_psx no-change, later handoff).
- THE CRANE specifically (#DF38, a QUAD/billboard object, rtype=0x11, world (5727,-1912,5548)): its tan quad
  (col 208,192,144, tp=(320,0) 4bpp clut=(496,177)) reaches gp0 as a 2D-FG (HUD-band) prim and is queued
  topmost, but is CULLED before raster (PSXPORT_PAINTFG leaves the pixel sky-blue — not a texture/occlusion
  issue; the VK quad is dropped, likely winding/degenerate from the un-owned GTE projection). The fix is NOT
  to debug the 2D-FG cull — it is to OWN these objects so they project world-coord with REAL per-vertex depth.
- THE FIX (next session): OWN 0x8003d0bc and 0x8003b588 PC-native — RE each, route their per-object geometry
  through eproj + the native GT3/GT4 submitters (gpu_draw_world_quad, real depth), exactly like
  submit_perobj_flush / ov_field_entity_render. That moves the ground + the 117-prim pass + the crane/figure/
  pickup off the flat 2D-band onto real depth → the occlusion resolves itself and the 6.96% collapses. Verify
  with `tools/render_cmp.py` (diagnostic gate; the TARGET is real-depth ownership, NOT matching PSX).
- TOOLS LEFT IN-TREE: PSXPORT_PRIMAT + PSXPORT_PAINTFG (gpu_native.cpp, cfg_str, documented in gfx-debug.md).
  The temp PSXPORT_SKIPPASS getenv mask + bb_dispatch/SKIPBB bisect scaffolding were REMOVED before this commit
  (raw getenv is banned); re-add a cfg_str-based pass/handler skip if the next session needs to re-bisect.

## later-230 (2026-06-24) — ENTITY-BEHAVIOR ownership: the 4 hottest field behaviors wired LIVE (were written-but-orphaned)
User directive: own the ENTITY/OBJECT SYSTEM overall (not just its render). Mapped the current state (subagent):
the entity LIFECYCLE is already ~90% native (pool/free-lists, spawn `ov_entity_spawn` + 5 variants + 2
dispatchers, despawn `ov_despawn`, placement `ov_place_objects`, the per-frame walk `ov_objwalk`, cull) — all
0-diff verified earlier. What is STILL PSX is the per-object BEHAVIOR layer: the node+0x1C handlers `ov_objwalk`
dispatches via rec_dispatch (the AI/state/physics logic = the object system's actual logic).
- ENUMERATED the live behavior set (new cfg diag `debug behhist` — tallies distinct node+0x1C handlers in
  engine_tomba2.cpp call_handler): **46 distinct handlers** at the seaside field (a few resident 0x800xxxxx,
  most overlay 0x801xxxxx). Hottest over 300 walks: 0x80040558 ×4312 (per-object state machine), 0x8012EB54
  ×3708 (overlay), 0x80124E74 ×2772, 0x80133C14 ×2162, 0x80073CD8 ×1236, 0x800739AC ×618, 0x800741DC ×308…
- KEY FINDING: four of those (the resident ones) ALREADY had byte-exact native bodies — `ov_sm40558`
  (0x80040558, entity.cpp), `ov_beh_739ac`/`ov_beh_73cd8`/`ov_beh_741dc` (objbeh_*.cpp) — but were ORPHANED:
  the three objbeh bodies sat in ANONYMOUS NAMESPACES reachable only via the now-dead `objbeh_*_register()`
  override-era stubs, so `call_handler` always rec_dispatched the raw guest address. They never ran natively.
- WIRED them LIVE: added exported `ov_beh_*_run` entries (kept the empty register stubs game_tomba2.cpp still
  calls) + a `dispatch_native_behavior()` switch in call_handler (engine_tomba2.cpp) that routes those 4 guest
  addresses to the native bodies; everything else still rec_dispatches. Native-only when the verify channel is
  off, A/B-vs-recomp when on.
- VERIFIED byte-exact: `debug sm40558,obj739acverify,obj741dcverify,obj73cd8verify` (set BEFORE the field so
  the static gate latches ON) → **0 MISMATCH** with thousands of matches across all four (the gates do a full
  RAM+scratchpad A/B vs rec_super_call each call). `tools/render_cmp.py` unchanged at 5343px/6.96% (identical
  game state → identical frame). So the hottest entity behaviors now run native with zero behavioral drift.
- NEXT: own the remaining ~42 behaviors top-down by hotness — start with the overlay hot ones 0x8012EB54,
  0x80124E74, 0x80133C14 (RE via `tools/disasm_overlay.py` on a fresh `dumpram`; resident via tools/disas.py).
  Each: RE 1:1, add a verify gate, wire into dispatch_native_behavior, confirm 0-diff. The render-side 2D-band
  ownership (later-229: 0x8003d0bc/0x8003b588) is a SEPARATE track. New diag: `debug behhist`.

## later-231 (2026-06-24) — IMPLEMENTATION SPEC: own the two field object-render passes as real-depth 3D (the crane fix)
User clarified the target: the crane (and the rest of the field's right side) are REAL 3D game objects whose
geometry the PSX flattens to depth-less OT-ordered quads (PSX has no depth). To render them natively with true
depth we must RE the game OBJECTS' real 3D geometry + world transform and reproject in float via eproj — i.e.
own the two still-PSX passes from later-229. Full RE done (subagent); implementation-ready spec:

CRITICAL ORDERING CAVEAT — own BOTH passes together. Pass A runs before Pass B in ov_render_frame. If only one
is converted to real-depth while the other stays 2D-FG (band ~0.94 = very front), the 2D-FG pass will OCCLUDE
the newly real-depth objects (0.94 > their real depth). So convert A and B in the SAME change (or the field
will look worse mid-way). Verify with tools/render_cmp.py + USER eyeball.

PASS A — 0x8003B588 (~117 prims) — NEARLY FREE (reuses the already-native real-depth path):
  It is a wrapper around node 0x800E7E80 (a single render node, node+8/9 = 17 commands, cmd-ptr array at
  node+0xC0[i]); it does state bookkeeping then `jal 0x800597AC(node)` (PSX setup — keep rec_dispatch) then,
  if node[1]!=0, `jal 0x8003CCA4(node)` = submit_perobj_render = ALREADY NATIVE → submit_perobj_flush →
  native_gt3gt4 → real depth. Disasm (tools/disas.py 0x8003B588) of the bookkeeping to port 1:1 (s0=node):
    - @5a0: v1=node[0xd]; if((v1&0xD0)==0) node[0xd]=0 (goto @698→@69c); else node[0xd]=v1|0x02; if(v1&0x20)
      skip to @69c; elif(v1&0x10) {…reads 0x1F800247, may set node[0x18]=208 and return-ish} else {…};
      the @5f4/@62c/@64c arms compute a byte via `jal 0x80083E80(a0=(0x1F800247&0xf)<<7)` (keep rec_dispatch;
      it returns v0, then v0=(v0<<16)>>22 +0x30/+0x10/… ) → node[0x18], and node[0x19]=0x20, node[0x1a]=v0.
      PORT THESE BYTE WRITES EXACTLY (node[0x18..0x1a], node[0xd]) — they feed the render/anim. Full arm
      decode is in the disasm; transcribe verbatim.
    - @69c: rec_dispatch(0x800597AC, a0=node); if(node[1]!=0){ s1=node[8]; if((node[0x17e]&0x20)&&node[0x179]
      && ...) node[8]=node[9]; submit_perobj_render(node); node[8]=s1; }
  The existing engine_submit.cpp:1662 ov_rwalk_b588 is a DIAG STUB — replace it with this, then in
  engine_render.cpp call ov_rwalk_b588(c) instead of d0(c,0x8003b588u) in BOTH ov_render_frame/_x.
  VERIFY: A/B the BOOKKEEPING bytes only (node[0xd],[0x18..0x1a],[8]) vs rec_super_call(0x8003B588) — the
  packet/OT writes WILL differ by design (native real-depth vs PSX packets); gate the compare to those bytes.

PASS B — 0x8003D0BC case 0 → handler 0x801401B8 (~220 prims = the GROUND) — needs TWO new submitters:
  0x8003D0BC: mode=*0x800BF870 (==0 at seaside; CONFIRM live with `r 0x800bf870` — other field sub-states use
  cases 1→0x80132358,2→0x80124CB8,4→0x801185F0,5→0x8013606C, RE those handlers too if they fire), table
  0x80014EF0, a0=0x800F2418 passed through. Handler 0x801401B8 (OVERLAY — disasm_overlay.py on field dump)
  walks a STATIC SCENE TABLE exactly like ov_field_entity_render (engine_submit.cpp:671):
    es=0x800F2418; count=u8[es+6](=132); base=*(es+0xC)(=0x801A5724); ot=*0x800ED8C8; load scratch
    0x1F8000F8→CR0-7 (scene camera = what eproj_compose_camera reads); u16 idx array @es+0x10, count entries;
    per idx: cmd=base+idx*4; s0=*cmd; GT3(0x8013FB88, a0=cmd+4, count=s0&0xff)→ret; GT4(0x8013FE58, a0=ret,
    count=(s0>>16)&0xff). Verts are WORLD-SPACE (single camera load) → use eproj_compose_camera + set_active
    once before the loop.
  The GT3/GT4 record layouts DIFFER from the existing native submitters → add submit_poly_gt3_ov (stride
  0x24/36B) and submit_poly_gt4_ov (stride 0x2C/44B), copies of submit_poly_gt3/gt4_native but with these
  offsets (decoded from 0x8013FB88 / 0x8013FE58 lwc2/mtc2):
    GT3 36B: +0x00 rgb0|code(op=>>24,bit25=semi), rgb1 hi→rgb2=rgb1<<4; +0x10 VXY0; +0x14 VZ0lo|VZ1hi;
             +0x18 VXY1; +0x1C VXY2; +0x20 VZ2; uv|clut & uv|tpage in +0x00..+0x0C region (re-confirm uv offs
             against the disasm before trusting).
    GT4 44B: +0x00 rgb0|code(rgb1=rgb0<<4); +0x04 uv0|clut; +0x08 uv1|tpage; +0x0C uv2|uv3; +0x14 VXY0;
             +0x18 VZ0lo|VZ1hi; +0x1C VXY1; +0x20 VXY2; +0x24 VZ2lo|VZ3hi; +0x28 VXY3.
    Reuse eproj_vertex_active + engine_shade_face + gpu_draw_world_quad + the NCLIP/frustum cull from the
    existing natives. New `ov_ground_render(c)` mirrors ov_field_entity_render; in engine_render.cpp replace
    d1(c,0x8003d0bcu,0x800f2418u) with ov_ground_render(c) (gate by mode==0; else rec_dispatch the dispatcher).
  CAUTION: the GT3/GT4 _ov field offsets above are the subagent's first decode — RE-VERIFY each lwc2/mtc2/lw
  offset directly against tools/disasm_overlay.py of 0x8013FB88 & 0x8013FE58 before shipping (byte-exact).
  Existing precedents to copy: submit_poly_gt3_native (engine_submit.cpp:430), submit_poly_gt4_native (:479),
  submit_poly_gt4_bp (:557, the separate-layout precedent), ov_field_entity_render (:671).

### later-231b — Pass B FIRST ATTEMPT (reverted): ground VANISHES via ov_field_entity_render — projection bug to chase
CONFIRMED LIVE: mode byte *0x800BF870 == 0 at seaside; table 0x800F2418 has count@+6 = 0x84 (132),
base@+0xC = 0x801A5724. RE-VERIFIED the GT3/GT4 record layouts directly (disasm_overlay 0x8013FB88 /
0x8013FE58): **they are IDENTICAL to submit_poly_gt3_native / submit_poly_gt4_native** (GT3 VXY0@+0x10,
VZ@+0x14, VXY1@+0x18, VXY2@+0x1C, VZ2@+0x20; GT4 VXY0@+0x14, VXY1@+0x1C, VXY2@+0x20, VXY3@+0x28) — the
subagent's "different layout / need new submitters" claim was WRONG. So Pass B == ov_field_entity_render with
a0=0x800F2418 (same OT-base 0x800ED8C8, same camera CR0-7 from 0x1F8000F8). NO new submitters needed.
- ATTEMPT (reverted): replaced `d1(c,0x8003d0bcu,0x800f2418u)` in ov_render_frame with `ov_ground_render`
  (mode==0 → ov_field_entity_render(0x800F2418)). RESULT: the GROUND/tree/house VANISH — the sea backdrop
  shows through; render_cmp jumps to 90.23%. So it is BROKEN, not merely "different from PSX."
- DIAGNOSIS so far: the geometry IS submitted (`debug subc`: gt3/gt4_native fire ~98k, huge jump) but
  mis-projects. `PSXPORT_PRIMAT` at a grass pixel shows SOME real-depth world prims (layer=1, depth~0.08,
  gray) at plausible coords, yet the ground isn't visible and BG node 800FC5C0 projected OFF-SCREEN (155,-81).
  Loading the GTE CR0-7 camera in the native pass (hypothesis: downstream PSX passes need it) made ZERO
  difference (identical 69294px) → NOT a downstream-GTE-state issue. So `eproj_compose_camera` /
  ov_field_entity_render itself mis-projects the ground's world-space verts. eproj_compose_camera was
  UNVERIFIED (later-224) and this is its first live use.
- NEXT-SESSION LEAD (do this with FRESH context): instrument ONE ground record — log eproj's px/py/pz for its
  verts vs the PSX GTE RTPT SXY/SZ for the SAME record (run the PSX 0x801401B8 once via rec_dispatch with a
  capture, or read CR results). Compare against eproj_compose_OBJECT which WORKS (submit_perobj_flush renders
  the tree/props correctly). Candidate bugs: (a) eproj_compose_camera rotation packing/scale differs from the
  PSX ctc2 load order; (b) the ground verts are NOT pure world-space — RE 0x801401B8 FULLY past 0x8014022C
  (after the CR0-7 load) to confirm no per-table/per-record pre-transform or a vert SCALE the camera-only path
  omits; (c) a 4096 / fixed-point scale mismatch in T. The repo is back to the WORKING 6.96% state (Pass B
  reverted); re-apply ov_ground_render once eproj_compose_camera is fixed, and own Pass A in the SAME change
  (ordering caveat above).

## later-232 (2026-06-24) — 3 more game-object behaviors owned (the hottest overlay handlers), byte-exact
Continues later-230 (own the entity/object SYSTEM's behavior layer). RE'd + reimplemented the 3 hottest
still-PSX field object behaviors 1:1 native, following the objbeh_* pattern (anonymous-namespace impl + a
full RAM+scratchpad A/B verify gate + an exported ov_beh_*_run; all jal callees kept as rec_dispatch leaves):
- 0x8012EB54 (×3708/300walks) — engine/objbeh_8012eb54.cpp — node[4] state machine + node[5] jump-table
  sub-machine (table 0x80109dec, 6 entries); range 0x8012EB54..0x8012ED80.
- 0x80124E74 (×2772) — engine/objbeh_80124e74.cpp — node[4] SM + 7-way node[3] jump table (0x80109B88) with
  inner node[6]/node[5] sub-switches; range 0x80124E74..0x801252BC.
- 0x80133C14 (×2162) — engine/objbeh_80133c14.cpp — node[4] SM, no sub-table, data table 0x8014A6E4[node3];
  range 0x80133C14..0x80133D68.
Wired into engine_tomba2.cpp dispatch_native_behavior + build lists (build_port.sh/run.sh). VERIFIED:
`debug obj8012eb54verify,obj80124e74verify,obj80133c14verify` (set before the field) → 0 MISMATCH, thousands
of matches each; render_cmp unchanged at 5343px (identical game state). 7 field behaviors now native total
(later-230's 4 + these 3). NEXT: continue the behhist top-down (remaining overlay handlers 0x80138FC8 ×1854,
0x8013259C ×1848, 0x80145230 ×1848, 0x801395C0 ×1232, 0x80124E74-family, …) same pipeline. The render-side
object real-depth (later-231/231b: own 0x8003b588/0x8003d0bc; the eproj_compose_camera ground-projection bug)
is the SEPARATE track.

## later-232b (2026-06-24) — +2 more game-object behaviors (0x8013259C, 0x80145230); 0x80138FC8 has a bug, deferred
Continues later-232. Batch of 3 RE'd (parallel subagents); 2 verified byte-exact and wired LIVE:
- 0x8013259C (×1848) — engine/objbeh_8013259c.cpp — node[4] SM, comparison-chain sub-dispatch (no JT).
- 0x80145230 (×1848) — engine/objbeh_80145230.cpp — node[4] SM, slti/beq ladders, spawn via on-stack struct.
Both: `debug obj8013259cverify,obj80145230verify` → 0 MISMATCH; render_cmp unchanged 5343px. 9 behaviors now
owned (later-230's 4 + later-232's 3 + these 2).
- 0x80138FC8 (×1854) — engine/objbeh_80138fc8.cpp WRITTEN but the A/B gate shows 40 MISMATCH at node+1 /
  scratch+0x148 in state 1 sub 0 (obj 800efa98). NOT wired (dispatch case commented out; runs PSX). The
  state-1 global-gate front-end (0x800BF816/0x800BF817 → FUN_80077ebc/8007703c/cull) or a node+1 write is
  mis-RE'd. Sent the RE agent a fix request with the exact mismatch. Re-wire once `obj80138fc8verify` is clean.

## later-233 (2026-06-24) — camera ownership status: owned-as-code, blocked on contiguity (selector is indirect-called)
User asked to own the camera. FINDING: the camera UPDATE is ALREADY reimplemented PC-native and verified —
engine/engine_camera.cpp owns the per-mode orchestrators ov_cam_orch_e0f0/e228/e3f4 (0x8006e0f0/e228/e3f4)
calling the 9 owned sub-fns (ov_cam_track_xz/_y, _rotbuild, _dist_solve, _angle_step, _pitch, _y_floor,
_heading, _lookat) — all 0-diff via `PSXPORT_DEBUG=camverify` (later-173..180). BUT they are ORPHAN (codemap):
written under the now-removed override model, never wired into the live call tree → the camera currently runs
PSX each frame.
- BLOCKER to wiring it live: the camera-mode SELECTOR (picks one orchestrator/frame, ABI a0=cam a1=target) is
  called INDIRECTLY — grep of generated/shard_3.c/shard_6.c finds NO direct `gen_func_8006E0F0(...)` call site;
  the orchestrators are reached via a function-pointer/jump table the selector indexes by camera mode. And the
  selector itself is invoked from the still-PSX per-frame field-update chain (FUN_801092b4 → … per later-224),
  which is NOT native yet. Unlike the per-object behaviors (wired at the native ov_objwalk call_handler site),
  there is NO native call site to intercept the camera at — the override table is gone.
- TO OWN IT LIVE (next): (1) find the selector address — add a one-shot log in rec_dispatch/the indirect-call
  path when target ∈ {0x8006e0f0,e228,e3f4} to capture the caller `ra`, or find the orchestrator-pointer table
  in MAIN.EXE (search for the 3 addresses as DATA); (2) own the selector native (it's a small mode dispatch),
  exporting ov_cam_select_run; (3) walk UP from the selector to the first already-native ancestor and route
  there — i.e. own the field-update chain down to the camera call, same top-down contiguity rule as everything
  else. The camera math is DONE; this is purely a wiring/contiguity task. (camverify still gates correctness.)

## later-232c (2026-06-24) — +4 more game-object behaviors owned (13 total); 0x8004C238 has a bug, deferred
Continues later-232b. Batch of 5 RE'd (parallel subagents); 4 verified byte-exact + wired LIVE, and the
earlier-deferred 0x80138FC8 was FIXED (inverted cull-branch polarity: state-1 node[3]==2 gate, 0x1F800207<0x1d
→ FUN_8007778c cull else FUN_8007703c — was backwards) and is now LIVE too:
- 0x80138FC8 (×1854) engine/objbeh_80138fc8.cpp (FIXED+wired), 0x8012D4EC (×1848) engine/objbeh_8012d4ec.cpp,
  0x8012D404 (×1232) engine/objbeh_8012d404.cpp, 0x801395C0 (×1232) engine/objbeh_801395c0.cpp.
All four: `debug obj80138fc8verify,obj8012d4ecverify,obj8012d404verify,obj801395c0verify` → 0 MISMATCH; render
unchanged 5343px. **13 field behaviors now owned native.**
- 0x8004C238 (×1232, RESIDENT) engine/objbeh_8004c238.cpp WRITTEN but A/B gate → 40 MISMATCH; NOT wired
  (dispatch case commented). Sent RE agent a fix request. Re-wire once obj8004c238verify is clean.
NOTE: running 5 full-RAM A/B gates at once is very slow (each = 2MB snapshot+restore+compare per call) — verify
in groups of ~4 and exclude any gate that's spamming mismatches.

## later-234 (2026-06-24) — DIRECTION (user): DECOUPLED renderer + NATIVE object model mirrored to guest RAM
User rejected the byte-exact per-object behavior transcription as the focus ("focusing on the crane feels
wrong — we're making an engine DECOUPLED from PSX"). Byte-exact-vs-recomp reimplementation is COUPLING (it
defines correctness as "matches PSX" and operates on guest-RAM nodes at PSX offsets). The chosen target is the
**DECOUPLED RENDERER** (AskUserQuestion) PLUS the user also endorsed the **native object structs mirrored to
guest RAM** idea. Synthesis = the decoupled-engine object/render architecture:
- **Native object model**: an engine-owned `EngObject` struct array = the SOURCE OF TRUTH for world entities
  (id, type, world transform = euler+translation+scale, geometry/model ref, state). The engine owns it.
- **Mirror to guest RAM**: each frame copy the native struct fields INTO the guest entity node (at the PSX
  offsets the still-PSX AI/physics/quest content reads) so un-owned content keeps working — native→guest. For
  fields the engine doesn't own yet, populate native FROM guest (guest→native) so the model is complete.
- **Decoupled renderer**: draw the scene by iterating the NATIVE object model — real geometry + world
  transform projected in float with real per-vertex depth (eproj + gpu_draw_world_quad) — NOT the PSX
  OT/packet render passes. The PSX render passes (the orchestrator 0x8003f9a8 + its walks/passes) get RETIRED
  in favor of one native scene renderer driven by the native model. Recomp render = visual REFERENCE only.
CURRENT RENDER STATE (what to build on): per-object MESHES already draw via eproj real-depth
(submit_perobj_flush → eproj_compose_object → gpu_draw_world_quad — WORKS, tree/props correct). The GROUND +
scenery (PSX pass 0x8003d0bc → 0x801401B8 → scene table 0x800F2418, ~132 entries, WORLD-space verts) and the
crane/figure/pickup (still-PSX passes 0x8003b588 etc.) are NOT owned → flat PSX 2D-band (later-229). 
PARKED BLOCKER (later-231b): routing the ground through ov_field_entity_render/eproj_compose_camera made it
VANISH. New finding (debug `groundproj` in ov_field_entity_render): compose_camera produces a SANE camera
(T=(-4612,-524,3072), normalized R), but my manual record decode read absurd model verts (-30976,18688) — the
scene-table record striding/offsets I assumed are WRONG (or the GT3-skip is). NEXT-SESSION: instrument INSIDE
submit_poly_gt3/gt4_native (the REAL decode, not a manual skip) to log the first ground record's model verts +
eproj px/py/pz; confirm the cmd+4 header size + GT3/GT4 strides for THIS table (vs native_gt3gt4's 16-byte
header). Then build the native object model + decoupled scene renderer per the architecture above.
NB the behavior ports (later-230/232/232b/232c, 13 owned) are NOT wasted — they're the content-interface and a
step toward eventually moving object DATA native — but they are NOT the decoupling deliverable; the renderer is.

## later-232d (2026-06-24) — A/B behavior-verify CAVEAT: false mismatches for overlay-calling handlers
0x8004C238 (the crane handler) showed 40 A/B mismatches at node+0x29, but RE re-audit vs the recompiler's
OWN emitted body (generated/shard_0.c gen_func for 0x8004C238) proved the native transcription is BYTE-EXACT
across all 16 JT-B cases — there is no behavior bug. ROOT CAUSE = a verify-HARNESS artifact: the gate's oracle
is `rec_super_call` = `rec_interp` (the flat interpreter), which INLINES a `jal` to the overlay sub-fn
(0x80118B10, the failing case-6 path) via a flat `pc=tgt` jump (interp.cpp ~563-573), whereas the native body
runs that overlay via `rec_dispatch` in a SEPARATE run context (its own stop_ra sentinel). The two contexts
handle the overlay's deep jal/jalr (and possible yields — interp.cpp notes rec_interp-via-rec_dispatch "dies
on a deep yield's longjmp", later-168) differently, and the first divergent byte surfaces as node+0x29. So the
A/B gate can FALSE-POSITIVE for any owned handler that rec_dispatches an OVERLAY sub-fn. The 13 wired behaviors
that verified 0-diff are still fine (they didn't mismatch); but a mismatch in such a handler must be confirmed
against the gen_func emitted body before assuming a transcription bug. 0x8004C238 left UNWIRED (moot under the
later-234 decoupled-renderer pivot; its native body is correct if ever needed). Fix-if-pursued: own 0x80118B10
too, or make the verify oracle use rec_dispatch for sub-calls.

## later-235 (2026-06-24) — GROUND DECODE IS CORRECT (later-231b/234 blocker was a RED HERRING); the REAL blocker is the 2D sea/water backdrop ORDERING
Picked up the later-234 "parked blocker" (routing the ground scene-table 0x800F2418 through ov_field_entity_render
makes the ground VANISH; later-231b blamed an eproj_compose_camera PROJECTION bug + a record-stride misread).
Both diagnoses are WRONG. Instrumented the REAL submit path (new `debug groundprobe`, engine_submit.cpp
ov_ground_probe — decodes the table through the EXACT GT3/GT4 record layout the native submitters use):
- **The records DECODE and PROJECT CORRECTLY.** First ground entries: model verts (713,-128,1854),
  (2549,-128,2626)… — all on the ground plane (y=-128), sane world x/z; camera T=(-4612,-524,3072) H=350,
  normalized R; they project to on-screen px/py with POSITIVE depth pz~2500-3100. The later-231b "absurd verts
  (-30976,18688)" came from the *manual* GT3-skip decode in the old groundproj probe — a bug in THAT probe, NOT
  the real path. eproj_compose_camera is FINE. So later-231b's "next-session lead" (chase the projection /
  4096-scale / rotation packing) is a DEAD END — do not re-walk it.
- **Why the ground still vanishes (the true cause):** routing the ground real-depth makes grass/house/tree
  disappear and reveals the SEA underneath. The seaside SEA/SKY/WATER is a **2D backdrop** (the grass+scenery
  are geometry drawn ON it). Natively the grass becomes layer-1 real-depth, but the 2D sea prim
  (`pktnode=800BFEB8 op=3C tp=(576,256)`, fails bg_2d's ¾-coverage test → mis-classified HUD/foreground)
  composites OVER it. Same root cause both ways: **the sea backdrop must classify RQ_BACKGROUND** (CLAUDE.md's
  "sea-drawn-on-top = order inherited from PSX" bug). The decode work is DONE; this is a render-ORDERING task.
- **The backdrop drawers:** `debug passpool` (pool-delta per pass) → the sea/sky/water comes from passes
  **0x80025d98** (sky/upper sea, node 800BFEB8) + **0x8003b588** (water reflection, node 800C0B04, ~117 prims).
- **Why fixing it is HARD (three mechanisms tried, all failed — record so they're not re-walked):**
  1. **bg_2d coverage** — sea prims are ~216px wide (<¾ of 320), so never classified backdrop. (Bandaid to
     lower the threshold; rejected.)
  2. **During-pass flag** (`g_in_backdrop_pass` set around the passes, checked in gp0_exec) — NO effect, because
     **gp0 classification is DEFERRED to the OT walk** (runs long after the pass returns). Only node-ADDRESS
     provenance (node_is_bg) survives the deferral.
  3. **node_is_bg pool-span provenance** (the ov_bg_tilemap mechanism: bracket the pass, register its packet-pool
     span as RQ_BACKGROUND) — FAILS for two compounding reasons: (a) the backdrop packets are **PERSISTENT** —
     built at scene-LOAD and only rarely rebuilt, so a per-frame pool-delta capture (0x800BF544 before/after)
     mostly sees an EMPTY delta and misses them (confirmed: first non-empty capture only at s_frame≈214); (b)
     pass 0x8003b588 **MIXES** the water backdrop with FOREGROUND content (Tomba/scenery) in ONE pool span, so
     tagging the whole span bg pushes Tomba/HUD behind the sea too (verified: Tomba vanished).
  4. **Self-learning texpage** (seed the backdrop texpage from node_is_bg hits — sky+water = tp(576,256),
     Tomba = tp(576,0) — then bg any opaque 2D prim on that page) — sound IDEA, but blocked by (3a): the seed
     never reliably fires because node_is_bg is never seeded (persistent packets). Reverted.
- **CONCLUSION / next step:** the fix must IDENTIFY the backdrop robustly without per-frame pool-delta. Options:
  (i) scan-on-load for the sea/sky DRAWER by code signature (like ov_bg_tilemap does for the green tilemap) and
  register its STABLE packet region once per scene; (ii) the proper decoupled answer — the engine OWNS the
  sea/sky/water as a NATIVE background layer drawn explicitly behind the world (RE the drawer, render a native
  backdrop, retire the PSX backdrop passes). (ii) is the later-234 direction. Either way the ground geometry is
  already proven-correct and will show the moment the backdrop sorts behind it.
- TOOLS LEFT IN-TREE (gated, OFF by default, repo still at the 6.96% baseline — verified render_cmp unchanged):
  `debug groundprobe` (ov_ground_probe: decode+project the ground table, no draw) and `debug groundnative`
  (route the ground real-depth via ov_field_entity_render — shows the backdrop-ordering bug live). The seabg/
  texpage/passpool experiments were REMOVED (didn't work; documented above).

## later-236 (2026-06-24) — RE: the seaside has TWO seas — REAL 3D water (0x8003b588) vs a 2D ATLAS backdrop (0x80025d98)
User flagged (correctly) that the field has BOTH "real water" and a "background atlas water" and they must NOT
be conflated. Deep static RE (subagent, exhaustive trace) settled which op-0x3C source is which:
- **REAL WORLD WATER = pass 0x8003b588 → node 0x800C0B04.** Trace: 0x8003b588 (bookkeeping + jal 0x800597AC
  setup) → `jal 0x8003cca4` (= submit_perobj_render) → jump table 0x80014ec8 (node[0xd]&0x0b) → GTE projector
  **0x8003cdd8** (40 COP2 ops: loads scene-camera CR0-7 from scratchpad **0x1F8000F8** via ctc2, Tcam from
  **0x1F80010C**, runs RTPT/RTPS `4a49e012`/`4a486012` on world-space model verts from node+0xC0[i], swc2 the
  SXY/SZ). This is the SAME camera the ground + objects use, and 0x8003cdd8 = the per-object render path ALREADY
  owned native (engine_submit.cpp submit_perobj_render → submit_perobj_flush → eproj_compose_object →
  gpu_draw_world_quad). So the real water is genuine 3D world geometry; OWN IT real-depth by routing 0x8003b588
  through its native body (this is exactly later-231 "Pass A": 0x8003b588 wraps node 0x800E7E80 → submit_perobj_
  render). Then it sorts by real depth like everything else.
- **ATLAS BACKDROP (sky/upper-sea) = pass 0x80025d98 → node 0x800BFEB8.** Exhaustive call-graph from 0x80025d98
  = 42 reachable fns, ZERO indirect (jalr) sites (so the static graph is COMPLETE). Only ONE fn touches COP2
  (0x80084080: mtc2 LZCS/mfc2 LZCR = the CLZ divide helper — NOT projection). **No RTPT/RTPS, no camera ctc2,
  no lwc2/swc2 of vertex coords anywhere in the pass.** The packets are built by 2D screen-space TEMPLATE
  blitters: sky drawer 0x80025b78 computes integer SCREEN coords from descriptor bytes (e.g. `X = u8[node+6]+160`,
  `Y = 212` constant, column = u8[node+8]±1) and calls 0x8007e6dc/0x8007e938 (sprite op 0x64/0x66) + 0x8007e1b8
  (FT4 op 0x2C), copying a 16-byte template from the STATIC table **0x80017334**, patching only color + scrolling
  UV (signed-byte deltas), linking into OT 0x800ed8c8 / pool 0x800bf544. Texpage tp=(576,256). So the atlas is a
  genuine 2D SCREEN-SPACE backdrop; its "perspective" is BAKED into the atlas art + the static tile-coordinate
  table, NOT produced by any camera transform.
- **RESOLVES the user's uncertainty** ("the atlas has its own native 3D projection, maybe it's part of the
  world — I don't know"): the 3D-projected sea is the REAL WATER (0x8003b588); the ATLAS (0x80025d98) is NOT
  projected and NOT part of the world — it is a flat screen-space background. So "statically put the atlas
  behind" is CORRECT (it has no camera projection to honor); the later-235 ordering struggle was a CLASSIFICATION
  problem (couldn't identify the source), not a projection problem — and we now KNOW the source is pass 0x80025d98.
- **PLAN (own both, retire the PSX backdrop):** (1) route 0x8003b588 native (later-231 Pass A) → real water gets
  real depth, sorts under the grass cliff / over the far backdrop automatically. (2) Own the atlas backdrop as an
  explicit native 2D BACKGROUND layer (RQ_BACKGROUND): reimplement the descriptor+0x80017334-table tile blit, or
  at minimum tag pass-0x80025d98 output RQ_BACKGROUND now that the source is known. No new ART needed — the atlas
  texture IS the game's sky/sea art; we own its RENDERING, not recreate it. Then re-enable ground real-depth
  (groundnative) and the grass/house/tree show correctly over the atlas, under nothing.
- Key addrs: water 0x8003b588 → 0x8003cca4 (JT 0x80014ec8) → 0x8003cdd8, cam 0x1F8000F8/Tcam 0x1F80010C, geom
  node+0xC0[i]. Atlas: dispatcher 0x80025d98, sky drawer 0x80025b78, sprite 0x8007e6dc(op64)/wrapper 0x8007e938,
  FT4 0x8007e1b8(op2C), static coord/UV table 0x80017334.

## later-237 (2026-06-24) — CONFIRMED: the visible sea IS the 2D atlas backdrop (op-0x65 SPRITE, builder 0x8007e6dc). primat MIS-REPORTS its op as 3C.
A 2nd RE pass raised a scare ("pass 0x80025d98 emits no op-3C, so node 800BFEB8 (which primat shows op=3C) can't
be the atlas — it must be a GTE/world path"). RESOLVED it definitively; the scare was a primat bug, not a real
mis-attribution:
- **Skip-pass bisect (new gated PSXPORT_SKIPPASS, engine_render.cpp) FAILED** to remove node 800BFEB8 by skipping
  ANY ov_render_frame pass (the 5 d0 passes AND all native walks AND the ground/terrain pass). REASON: the
  backdrop packets are PERSISTENT — built once at scene-LOAD, linked into the OT, re-walked every frame; skipping
  the building pass mid-run can't un-link an already-built packet. (Same persistence that made later-235's
  passpool pool-delta unreliable. Pass-skip is the wrong tool for persistent packets — use WWATCH.)
- **PSXPORT_WWATCH=0x800BFEB8,0x800BFEE0 (writer trace, mem.cpp) is DECISIVE:** the sea packet is written by
  **0x8007e6dc** (pc 0x8007E838..0x8007E8B0) + tpage 0x80083de0 + OT-splice 0x80083c30 + clip-rect 0x80081cf8 —
  i.e. EXACTLY the atlas SPRITE-builder infrastructure (the RE's 0x80025d98 → 0x80025b78 → 0x8007e6dc path).
- **`r 0x800BFEB8` = `27 F8 00 65 …` → word0 high byte 0x65 = GP0 textured SPRITE** (opaque, blend). So the packet
  is a SPRITE (op 0x65), NOT POLY_GT4. **primat's `op=3C` for this prim is a MIS-READ** (it decodes the op from
  the wrong word for sprite packets) — that false "op=3C" is what sent the 2nd RE down the "can't be the atlas"
  path. FIX primat's op decode for sprites if it keeps misleading (gpu_native.cpp primat block).
- **NET: later-235/236's conclusion STANDS and is now hard-confirmed.** The visible cyan sea = the 2D ATLAS
  backdrop (op-0x65 sprites, builder 0x8007e6dc, dispatcher 0x80025d98, tp(576,256)), persistent. The REAL 3D
  water is the SEPARATE 0x8003b588 GTE path (node 800C0B04). They are two different things, as the user insisted.
- **PLAN unchanged (later-236):** own the atlas backdrop as RQ_BACKGROUND (native reimpl per the RE spec below),
  then own the real water real-depth (0x8003b588 Pass A), then re-enable groundnative.
- **ATLAS native-reimpl spec (RE agent, implementation-ready):** dispatcher 0x80025d98 routes on
  u8[0x800ED061]&3 / mode u8[0x800BF870] to arms 0x80025744 (FT4 op-2C) / 0x80025934 (FT4+sprite) / 0x80025b78
  (3-tile sprite sea-band). 0x80025b78 emits a HARDCODED 3 sprite tiles (no count/terminator) at screen
  X=(s8)u8[0x800ED05E]+160 (±32/+64 for the 3), Y=212, column=u8[0x800ED060]±1 (horizontal sea-band scroll lives
  in these node bytes, NOT a UV register — UVs are static per cell). Blit-source template ptr = *(0x800ED094).
  Per-tile cells walked by 0x8007e6dc from records via pointer table 0x80017334[idx] (idx = lh[0x800AD284 +
  u8[blitsrc+34]*4 + 2]); each 16B cell: +0 UV|clut, +6 OT-z, +14/+15 signed X/Y delta, +10 UV. Emit each cell as
  a textured 2D quad (screen rect + UV sub-rect, tp(576,256) clut(624,510), semi if op 0x66) into RQ_BACKGROUND
  via rq_push_2d_quad(core, RQ_BACKGROUND, …) (gpu_native.cpp:722) — NO PSX packet. Read the per-frame WH from the
  0x800ED094 blit-template live. CAUTION: also check arms 0x80025744/934 (counter-driven FT4) don't include a
  2D FOREGROUND element (sea-foam/spray) that must NOT be reclassified — eyeball before blanket-bg.
- Diagnostics left in-tree (gated, OFF by default): PSXPORT_SKIPPASS (engine_render.cpp, with the persistence
  caveat in-comment). render_cmp unchanged at 6.96%.

## later-238 (2026-06-24) — CORRECTION: the "sea/sky backdrop" is the GROUND scene-table (0x801401B8), NOT the atlas drawer or the water pass. later-235/236/237 mis-attributed it.
Followed the user's directive (port TOP-DOWN, it reveals itself) instead of the handoff's bottom-up "replace
the atlas pass" plan — and the handoff/journal attribution chain was WRONG. Hard evidence (live PRIMAT + WWATCH
on the actual screen-filling cyan, with `debug groundnative` ON so the bug shows):
- The visible cyan "sea" that covers the field in groundnative mode is **pktnode 800C0ED4, op=3C, tp=(576,256)
  clut=(624,510), is3d=0** — a TEXTURED POLY (GT4), not a sprite.
- **WWATCH 0x800C0ED4 → builder pc=0x8013FBE8 / 0x8013FED4 / 0x8013FEC4 = the GT3/GT4 scene-table submitters
  0x8013FB88 / 0x8013FE58**, dispatched by the GROUND handler **0x801401B8** (the 0x8003d0bc pass, scene table
  0x800F2418). So the sky/sea backdrop lives in the SAME scene-table as the grass/house/tree and is drawn by
  the GROUND pass. It is NOT the atlas sprite drawer 0x80025d98/0x80025b78 (that builds op-0x65 SPRITES — node
  800BFEB8 — a SEPARATE, thin element) and NOT the water pass 0x8003b588.
- This RESOLVES every prior confusion: skipping/replacing 0x80025d98 did nothing because the cyan backdrop
  isn't drawn by it (later-237's WWATCH watched node 800BFEB8 = the sprite band, and conflated it with the
  poly backdrop 800C0ED4). The "sea on top" in groundnative is because ov_field_entity_render projects the
  ENTIRE table uniformly, so the screen-space backdrop quads (tp 576,256, baked perspective) mis-project and
  cover the world instead of staying behind it. The grass entries project fine (later-235 groundprobe was
  right about THOSE) — but the table also contains backdrop entries that must NOT be camera-projected.
- DEAD ENDS now firmly closed: (a) reclassify/own the atlas sprite drawer 0x80025d98 as RQ_BACKGROUND — wrong
  target, doesn't touch the cyan backdrop; (b) own the water 0x8003b588 to fix the backdrop — water poly is a
  different node, owning it (Pass A, below) left 800C0ED4 untouched. The atlas SPRITE band and the water are
  real but minor/separate; do NOT chase them for the "sea on top" bug.
- THE REAL FRONTIER: own 0x801401B8 (the field ground+backdrop scene-table renderer) faithfully — it must
  distinguish the WORLD-space ground entries (camera-project via eproj → real depth) from the SCREEN-space
  backdrop entries (sky/sea on tp 576,256 → draw as RQ_BACKGROUND 2D, no camera projection). Need the
  per-entry 3D-vs-backdrop discriminator from 0x801401B8's decode (RE in progress). Then groundnative becomes
  correct and the grass/house/tree show over the backdrop.

### later-238 LANDED: Pass A (field WATER) owned native real-depth (engine_submit.cpp ov_rwalk_b588).
Replaced the orphan diag stub with the faithful body of 0x8003B588 (node 0x800E7E80): node-byte bookkeeping
(@0x8003b5a0..698, ported 1:1 from disas), rec_dispatch the PSX transform-setup leaf 0x800597AC (no render),
then route the per-object render through NATIVE submit_perobj_render (node+0xD=0 → render-case
0x80014EC8[0]=0x8003CD00 = the eproj FLUSH case → real per-vertex depth). Wired into ov_render_frame/_x
(both twins) replacing d0(c,0x8003b588u). VERIFIED: default seaside view (groundnative off) unchanged
(passA_default.png == baseline) — no regression. This is correct top-down progress (water now sorts by real
depth) but it is NOT the "sea on top" fix (that is 0x801401B8, above).

### later-238 (cont.) — the cyan backdrop is built OUTSIDE ov_render_frame; classified at the DEFERRED OT-walk; attribution tooling is the blocker
Pursued the cyan backdrop (tp 576,256, op-3C, is3d=0) with reliable C-level tools instead of WWATCH:
- **The ground table 0x800F2418 is GRASS/terrain ONLY** — new `debug groundprobe` full-table scan
  (`[groundprobe-tp]`) shows its GT4 records are all on tp_y=0 pages (640,0 grass, 576,0, 704,0, 768,0,
  896,0, 960,0), eproj-projecting to SANE positive depths (pz ~195..4474). NO tp(576,256) entries. So
  groundnative draws the grass correctly; later-235 "ground decode correct" CONFIRMED. The cyan backdrop is
  NOT in this table.
- **WWATCH "pc=" is UNRELIABLE for the field render.** It logs g_interp_pc, which only updates in the
  INTERPRETER; the field render runs through recomp/native code, so the pc is stale garbage (e.g. it pointed
  at 0x80115xxx which is a 0x7FFF7FFF DATA buffer, not code). later-237's "writer pc=0x8007E838" and all
  pool-address WWATCH attribution (mine + the journal's) are suspect. Pool addresses also churn (a node addr
  is reused by different builders frame-to-frame), so node-address attribution is doubly unreliable.
- **Built a per-pass attribution probe** (g_render_pass_tag stamped per pass in ov_render_frame + ov_field_frame;
  PSXPORT_BDTAG histograms it in the gp0 classifier). Result: the tp(576,256) backdrop is ALWAYS tagged "?"
  (the initial value) — i.e. it is classified when NO pass tag is set. CONCLUSION: gp0 classification is
  DEFERRED to the present/OT-walk (double-buffered: present runs at loop-top BEFORE ov_field_frame sets any
  tag), DECOUPLED from the building pass. So per-current-pass tagging CANNOT attribute these prims (re-confirms
  later-235 #2). The backdrop is also built OUTSIDE ov_render_frame entirely (skipping every ov_render_frame
  pass never removed it — earlier SKIPPASS tests), i.e. in the field-update phase or a separate BG/backdrop
  render. (The probe was reverted; it proved a negative. `groundprobe-tp` kept.)
- **NET STATE:** the "sea on top" cyan backdrop = a tp(576,256) screen-space 2D element, built outside the
  owned ov_render_frame, classified at the deferred OT-walk → lands is3d=0 foreground → covers the native
  world when groundnative moves the grass to RQ_WORLD. The atlas SPRITE band (0x80025d98) and the water
  (0x8003b588, now owned) are SEPARATE and NOT this backdrop.
- **THE REAL BLOCKER / NEXT STEP:** need RELIABLE BUILD-TIME prim→builder attribution that survives the
  deferred OT-walk + pool churn. The infra exists: PktSpanSession / g_pkt_track (engine_submit.cpp:832) records
  a pool-write span; bracket each ov_field_frame call (0x80059d28, 0x80069b28, 0x80026368, ov_objwalk,
  0x80025588, 0x8004fe84, ov_disp_26c88, 0x80022a80, 0x8006ec44, 0x80050de4, 0x8001cac0, 0x8010810c) with a
  span capture, then at gp0 classify look up which bracket's span the packet falls in (NOT a current-pass
  global). That identifies the backdrop's builder reliably. Then own that builder TOP-DOWN and either draw its
  sky/sea as RQ_BACKGROUND (if screen-space) or eproj real-depth (if world-space). Determine which by dumping
  its scene-table records' verts once the builder is known. (Faster alt to localize the builder: temporarily
  no-op each ov_field_frame call one at a time, rebuild, shot with groundnative — see which removes the cyan.)

### later-238 (cont. 2) — FOUND IT: the backdrop builder is FUN_80109fe0 (SOP entity render over 0x800f2418), reached via ov_sop_field_mode
Built a RELIABLE build-time prim→builder attribution harness (PSXPORT_BDTAG): bracket a call with
ffspan_begin()/ffspan_end("name") to record its pool-write span (g_pkt_lo/hi while g_pkt_track=1, mem.cpp);
at the deferred gp0 OT-walk, ffspan_lookup(packet_addr) returns the INNERMOST bracket that wrote it
(earliest-recorded span wins — inner brackets end before outer, and outer spans merge their children, so
the first containing span in record order is the tightest). Reset once per frame at native_step_frame top.
Climbed the call tree top-down with nested brackets:
  native_step_frame → **scheduler** → **gameframe** (ov_game_frame) → **submode0** (ov_game_submode0, s4a==0
  — NOT submode1/ov_field_frame!) → ov_sop_field_mode → ov_sop_field_update → **entrender** = the SOP entity
  render loop `d1(c, 0x80109fe0u, 0x800f2418u)` (engine/sop.cpp). 50/50 backdrop prims attribute to entrender.
KEY CORRECTIONS:
- The seaside walkable field renders through the **SOP field-mode path** (ov_game_submode0 → ov_sop_field_mode
  → ov_sop_field_update), NOT the ov_field_frame / ov_render_frame path (sm[0x4a]==0 SOP sub-mode does the
  actual per-frame render; sm[0x4a]==1 / ov_field_frame is a DIFFERENT/secondary path). This is why owning the
  water (ov_rwalk_b588 in ov_render_frame) and groundnative (ov_field_entity_render in ov_render_frame) did
  NOT touch the backdrop — they're in the wrong render path for this field.
- The sop.cpp:203-206 comment ("this SOP path isn't exercised by the walkable field; ov_field_entity_render
  ready but UNWIRED, renders via 0x8010810c→0x8003D074→0x8003F698") is **WRONG**. The walkable field DOES use
  ov_sop_field_update, and 0x80109fe0 (entity render over 0x800f2418) is the live builder of the sky/sea
  backdrop (and the scene). ov_field_entity_render is the native reimpl of 0x80109fe0 — wire it HERE.
- NB: groundprobe-tp's GT4 decode of 0x800f2418 showed only tp_y=0 records (grass) — so the tp(576,256)
  backdrop is NOT a standard GT4 record in that table; 0x80109fe0 must draw it via a different entry-type /
  per-entity path. RE 0x80109fe0 fully before wiring native (it draws BOTH the grass scene AND the backdrop;
  the backdrop is screen-space → must become RQ_BACKGROUND, the grass → eproj real-depth).
- THE OWNERSHIP TARGET IS NOW PINNED: own FUN_80109fe0 (engine/sop.cpp ov_sop_field_update's entrender call)
  natively. That single function owns the field's whole picture; owning it correctly (backdrop→RQ_BACKGROUND,
  world→eproj depth) fixes the "sea on top" AND gives real depth, in the RIGHT render path.

### later-238 LANDED — owned FUN_80109fe0 (the live field render) native; backdrop now real-depth, "sea on top" root cause eliminated
RE'd the SOP submitters: FUN_801099b4 (GT3, scratch/decomp/sop/801099b4.c — stride 36, RTPT-projects, vert
offsets +16/+20/+24/+28/+32) and FUN_80109c80 (GT4, 80109c80.c — stride 44, RTPT+RTPS, +20/+24/+28/+32/+36/+40)
are BYTE-IDENTICAL in layout to submit_poly_gt3_native / submit_poly_gt4_native, and FUN_80109fe0's loop
(es=0x800f2418, CR0-7 from 0x1f8000f8, idx array @+0x10, GT3-then-GT4 per cmd) is identical to
ov_field_entity_render. So ov_field_entity_render IS the faithful native reimpl. CRUCIALLY: both submitters
GTE-PROJECT every record (RTPT/RTPS) — the sky/sea backdrop is real WORLD GEOMETRY, not a flat screen-space
overlay. It only LOOKED like a foreground "backdrop" because, run as PSX, its packets were is3d=0 (the native
renderer couldn't recover their projected verts) → flat 2D foreground → drew over the native world.
FIX (engine/sop.cpp ov_sop_field_update): replaced `d1(c, 0x80109fe0u, 0x800f2418u)` (PSX) with
`c->r[4]=0x800f2418u; ov_field_entity_render(c)` (native). Now every field record — grass, house, tree, AND
the sky/sea — projects through float eproj with REAL per-vertex depth and sorts in RQ_WORLD by the depth
buffer. VERIFIED on the live game: sky pixel (160,20) is now [primat-rq] layer=1 om=0 depth~0.04-0.085 (native
world), NO gp0 is3d=0 backdrop prim remains; scene renders correctly (matches baseline), STABLE 380+ frames
with motion (Tomba walks, sign prompt appears, gameplay progresses), zero derail. The "sea on top" root cause
(backdrop classified is3d=0 foreground while world is native) is GONE — all field geometry is native real-depth
in the RIGHT (SOP) render path. (USER eyeball pending: sop_native_stable.png.)
NEXT: own the SOP entity UPDATE FUN_8010a0e0 (positions) + the BG draws FUN_8010bffc/8010c26c (parallax/scroll,
on 0x800ed018) + Tomba update 0x8007b008 — the rest of ov_sop_field_update's PSX leaves — as the frontier
advances. The ov_render_frame water-ownership (later-238 ov_rwalk_b588) is dormant in this SOP path; revisit if
ov_field_frame becomes live. BDTAG attribution harness (ffspan_*, PSXPORT_BDTAG) left in-tree (gated) for reuse.

### later-238 CORRECTION — the FUN_80109fe0 native render is the SOP-INTRO path, NOT steady gameplay (verified)
Honesty fix to "later-238 LANDED" above (which overstated it as the live field render). VERIFIED with a
per-path one-shot counter (debug pathdbg, since reverted): at the seaside, **ov_sop_field_mode (the SOP path,
sm[0x4a]==0, where FUN_80109fe0 / my native ov_field_entity_render lives) fires exactly ONCE — the intro
frame** — then **ov_field_frame (sm[0x4a]==1, the area-machine path) fires EVERY steady frame** (#1,121,241,
…,721 over run 250). sm[0x4a] is a stable 1 from ~run 30 on (dumped 30/120/250). So:
- The STEADY, user-visible gameplay field is rendered by **ov_field_frame → ov_render_frame** (engine_stage.cpp
  / engine_render.cpp) — STILL PSX (atlas pass 0x80025d98, ground 0x8003d0bc, the rwalks, 0x8003f024/df04).
  It renders CORRECTLY today because it's all-PSX (PSX OT order). My SOP FUN_80109fe0 ownership is CORRECT code
  but only exercises the 1-frame SOP intro — keep it (it owns the SOP-area render path, used for SOP intros),
  but it does NOT own steady gameplay and did NOT change the steady picture.
- The water ownership (ov_rwalk_b588) DOES live in ov_render_frame → it IS in the steady path (good).
- Steady-field BDTAG map (the real frontier, run 200): is3d=0 PSX prims built by fieldframe/fieldrun on tp
  (576,0),(640,0),(704,0),(768,0),(896,0),(960,0),(320,0),(448,256),(512,256),(320,256),(960,256),(384,0).
  NB there is NO tp(576,256) backdrop in the steady field (that was the SOP intro). The steady field's own
  sky/backdrop is among these (e.g. op-3e tp(320,256)/(960,0)). RE which when owning them.
- THE REAL FRONTIER (corrected): own the STEADY field render = ov_render_frame's still-PSX passes natively
  (engine_render.cpp), top-down, each producing eproj real-depth or RQ_BACKGROUND as appropriate — same method
  as the SOP path. Start where the BDTAG map shows the heaviest builders. The "decouple the steady gameplay
  field" goal is this path, not the SOP one.

## later-240 (2026-06-24) — AUTO_SKIP into real free-roam FIXED; and the field renders via INTERPRETED overlay, NOT ov_render_frame (corrects later-238)
Two findings while making "reach gameplay headless" actually work.

### 1. `PSXPORT_AUTO_SKIP` was DEAD; reimplemented to drive into real CONTROLLABLE free-roam
`PSXPORT_AUTO_SKIP` / `PSXPORT_AUTO_GAMEPLAY` / `PSXPORT_AUTO_NEWGAME` were referenced only in docs and **read
by NO code** (`git grep` confirms). So a "no-input" run never mashed anything — it just sat in the **attract
DEMO** (`stage=0x801062E4`, the game playing predetermined input). That demo is PSX-rendered playback, not the
GAME free-roam field; using it to judge rendering is a trap. Reimplemented `PSXPORT_AUTO_SKIP=1` as a
self-contained auto-drive state machine in `runtime/recomp/native_boot.cpp`:
- (0) tap **Cross** until task0 enters the GAME stage (`0x8010637C`);
- (1) wait for the post-NewGame **intro cutscene** to start — the cutscene-active flag `*(0x1F800137)` goes 1
  (verified: 1 throughout the scripted camera-pan/dialog cutscene, 0 in free-roam; an early loading 0-window
  precedes it, so we wait for the first 1 before treating 0 as free-roam);
- (2) the cutscene does NOT end on its own — pulse **Start** (every ~40f) WHILE the flag is 1 (Start ends it;
  takes a few taps until it reaches a skippable point). Stop once the flag has been 0 for ~60f (~2s), which
  also lets the cutscene-END FADE finish so a Start right after hand-off opens the pause menu (not mid-fade).
  Pressing Start ONLY while the flag is 1 keeps us out of the pause menu (Start in free-roam opens it).
VERIFIED controllable: at hand-off (~f216) idle frame-to-frame Δ≈0px (cutscene auto-pans 83%), holding Right
pans the camera ~70k px, and a Start tap opens the Options/Load/Quit pause menu. Recipe + dead-var note are in
`docs/driving-the-game.md` ⭐. Gotcha that cost time: the manual `newgame` REPL path FREEZES task0 at the GAME
prologue (`continue`), and a Start tap there does NOT skip the cutscene — only the live auto-drive path does.

### 2. The steady GAME field renders via INTERPRETED overlay code, NOT the native ov_render_frame (corrects later-238)
later-238's "steady gameplay = ov_field_frame → ov_render_frame" is **FALSIFIED**. Probe `debug rfprobe`
(counter at the top of `engine_render.cpp ov_render_frame`): in the real free-roam field `ov_render_frame`
runs **0–1 times over 700 frames** (dormant) — both in the attract demo AND in the player field. The field is
drawn by interpreted OVERLAY code, attributed by BDTAG to `'scheduler'` (outside even the `gameframe`
bracket), i.e. it runs in the cooperative guest path, not under the native `ov_game_frame`.
Traced the actual steady builders (WWATCH on a terrain packet `0x800c367c` → builder PCs `0x80146xxx`/
`0x8013Fxxx`; these are OVERLAY addresses, beyond MAIN.EXE, dumped live + disassembled with capstone):
- **Entity-render loop `0x801401b8`** — BYTE-IDENTICAL in structure to `ov_field_entity_render`
  (count@+6, base@+0xc, u16 idx array @+0x10, CR0-7 from scratchpad `0x1f8000f8`, otbase from `*0x800ED8C8`,
  GT3-submit-then-GT4 per cmd). Takes the scene table `es` in a0.
- **GT3 submitter `0x801465ec`** and **GT4 submitter `0x8013fe58`** — RTPT/RTPS-project each record and emit a
  GT3/GT4 packet to the pool at `*0x800bf544`; structurally == `submit_poly_gt3/gt4_native`. (Also a second GT3
  `0x8013fb88` the entity loop calls.) The packets classify **is3d=0** (flat) only because the PSX emitter
  discards view-Z; the geometry IS real per-frame GTE-projected world geometry.
- **Caller `0x8003d0bc`** is a SCENE-TYPE DISPATCHER: `*(0x800bf870)` (<0x16) indexes a jump table at
  `0x80014ef0`; one case → `0x8003d0f4` jal `0x801401b8`. (This is why the `groundnative` flag — which routes
  the *native* `ov_render_frame` ground pass `0x8003d0bc(0x800f2418)` to `ov_field_entity_render` — had ZERO
  effect on the field: `ov_render_frame` never runs here, so its passes are moot.)
**Implication for the frontier:** owning `ov_render_frame`'s passes (later-238 plan) does NOT touch the visible
steady field. The real target is the interpreted overlay render path — `0x801401b8` (== `ov_field_entity_render`)
and its submitters — reached top-down from whatever cooperative driver runs it. Per the user (2026-06-24): as
world objects are traced and these functions identified, **each should be DECOMPILED into the PC port** (the
overlay GT3/GT4 submitters + entity loop are direct equivalents of natives we already have). Decompiles cached
under `scratch/dual/*.asm` (capstone flat-dump disassembler `scratch/dual/mdis.py`).

## later-243 (2026-06-24) — Phase-1 DECOUPLED native world-data render LANDED (debug scenenative); backdrop is the remaining gap
Per the user's render north star ([[one-native-render-path-decoupled]]): ONE native render path driven from
WORLD DATA, PSX vanilla, fully decoupled. Phase 1 landed (commit 6f01015): `ov_scene_native`
(engine_submit.cpp), wired in `ov_draw_otag` (game_tomba2.cpp); when `debug scenenative` is on the PSX OT
walk is SKIPPED and the field renders from game data — native render walks (→ov_terrain), scene table
0x800F2418 (ov_field_entity_render), and every object in the 3 entity lists (node+0xC0→geomblk→native GT3/GT4,
eproj+D32+lighting). VERIFIED headless (AUTO_SKIP free-roam): terrain/hut/tree/Tomba/Swine/see-saw/water all
render real-depth, stable 200+ frames of motion, no derail.
- **0px root cause (solved):** native 3D world prims live in depth [0,0.9375]; the PSX 2D-FG band is (0.9375,1]
  and the compare is GREATER_OR_EQUAL, so the PSX flat layer WON and covered the native world. Skipping the PSX
  walk (decoupling) fixed it — confirms the native path must OWN the frame, not share a queue with PSX.
- **REMAINING = the BACKDROP** (the only black area: sky + distant parallax hills). [CORRECTED by later-244:
  the "NOT FUN_80115598 / 0x8004fd30 sky-fill / 0x8003df04 hills" attribution below was WRONG — the backdrop
  IS the tilemap drawer FUN_80115598. The 0x80115598 signature search missed it because this field's texpage
  differs; the flat-fill 0x8004fd30 is fully covered (invisible). See later-244.] ~~Candidates among the
  still-skipped passes: 0x8004fd30 (sky fill) and 0x8003df04 (distant hills).~~ Then the 2D HUD, then retire
  the PSX walk here and make scenenative the behavior (ungate), and confirm 60fps/ires/lighting ride one path.
- Tools added this session: REPL `ents` (enumerate objects), `PSXPORT_PCTRAP=0xADDR`(+_SKIP=N) guest-call-chain
  tracer, `PSXPORT_AUTO_SKIP=1` (reach real free-roam). RE: docs/engine_re.md ★ GAME-STAGE OBJECT PIPELINE.

## later-249 (2026-06-25): title "top-left quadrant" corruption is macOS/MoltenVK-specific — NOT uninitialized VRAM
Investigated the handoff's bug #1 (user's macOS title menu shows a garbled top-left quadrant: blocky
texture-page data + noise, rest of title fine). Could NOT reproduce on Linux (RADV + Wayland). Findings:
- **Headless `shot` reads `s_tex` back DIRECTLY** (`vk_readback_to_rb` → vkCmdCopyImageToBuffer from s_tex,
  gpu_gpu.cpp:2208), bypassing the windowed `present.frag` sampling path. So NO headless capture can ever
  show a windowed/present bug. Every headless title shot is clean for this reason — not because the bug is absent.
- **DISPROVEN: "uninitialized VRAM image" theory** (handoff guess). Added a temp diagnostic that fills
  s_tex/s_tex_b/s_vram_tex with loud-magenta poison at creation (PSXPORT_VRAM_POISON, since reverted) → the
  title still renders perfectly clean. Reason: the title's displayed region (VRAM rows <512) is FULLY uploaded
  every present from zero-init CPU `s_vram` (panel_upload first-use full upload covers rows 0..511; s_vram_tex
  gets a full VRAM_W×VRAM_H upload each panel_render). So the display region is defined on macOS too. (NB the
  FB scratch rows ≥512 ARE left undefined on 2D frames — but a 2D title samples rows <512, so that's not this bug.)
- **RADV windowed is CLEAN** through the entire title/intro via present.frag: captured SCEA, Whoopee Camp logo,
  Tomba!2 logo fade-in, and the attract cutscene with `spectacle` — all clean, no quadrant garbage.
  ⇒ The corruption is **MoltenVK/macOS-specific**, not in the committed logic path.
- Remaining suspects (NOT verifiable from Linux): (a) the cross-stage OVERLAPPING push-constant range used by
  the tri/tritex pipeline layout `s_pll` — vertex uses bytes [16,48), fragment uses [0,16)∪[48,48..64); MoltenVK
  mishandles overlapping/disjoint push ranges more often than desktop drivers; (b) the R16_UINT integer sampler
  / MUTABLE_FORMAT aliased image (s_tex carries both R16_UINT and A1B5G5R5_UNORM views) under Metal; (c) a
  macOS-only asset/CD upload gap leaving CPU s_vram garbage in that region (macOS HLE has known CD-fs gaps,
  see tomba2-hle-irq.md:220) — but that would be black-after-clear on RADV, not seen here.
- BLOCKER: any #1 fix needs the user's macOS build to verify (no MoltenVK on this Linux box; user is the visual
  authority). Did NOT ship a speculative MoltenVK change blind (would be an unverifiable guess). Tree clean.

## later-250 (2026-06-25): title-menu corruption ROOT-CAUSED — demo→menu return, render doesn't follow reloaded menu state (windowed-only)
Supersedes later-249's "macOS-specific" conclusion — the USER REPRODUCED IT ON LINUX (windowed). It is NOT
macOS-specific. The corruption = the StartGame/Options main menu drawn with raw VRAM texture-page garbage in
the top-left + cyan speckle over the title art. User's behavioral description (decisive): "it does two fade-ins,
one in corrupt menu then immediately again in the cutscene back to back, but the cutscene is already started
when the corrupt menu fade is happening — it's supposed to just jump from menu to fade in straight."

MECHANISM (mapped this session):
- The front-end is the DEMO/menu stage machine (engine/engine_demo.cpp, sm=*0x1F800138, sm[0x48]=substate).
  Cycle: menu s1 → attract demo s7 (countdown sm[0x5a]) → timer expires → **s0 init: reloads the menu texgroup
  (meta 0x800FB170)** → back to menu s1. The corrupt menu re-appears here, while the attract demo's scene/VRAM
  is still resident.
- The intro/transition screen fade is the recomp FUN_8002655c (bg scene-transition SM) calling FUN_8007e9c8
  (PSX fade-rect builder), confirmed via PSXPORT_PCTRAP=0x8002655c / 0x8007e9c8 (both fire in the intro).
  FUN_8007e9c8 builds a PSX OT rect that the engine's PC-native render IGNORES (engine owns ordering) → the
  fade is "fake"/not applied = the user's "fake fade". The native equivalent that routes through engine_fade_set
  (engine/bg_scene_transition_sm.cpp, ov_bg_scene_transition_sm) is ORPHAN — NOT wired into this intro path.
- HEADLESS SHOWS THE CORRECT BEHAVIOR: captured the full fade sequence with the new `fadeshot` channel (interp
  hook on FUN_8007e9c8): call0 white init (0xFFFFFF) then 31 steps 0xF8→0x08 = ONE clean fade-in of the cutscene
  emerging from the menu — exactly the "jump menu→fade straight" the user expects. The buggy EXTRA corrupt-menu
  fade-in does NOT occur headless because headless collapses the real-time menu display + attract loop.
- At the demo→menu return in headless, s0-init reloads the menu texgroup but the RENDER STAYS ON THE DEMO SCENE
  (no menu drawn) — i.e. render-doesn't-follow-reloaded-state, just a different visible outcome than windowed.

WHY IT'S WINDOWED-ONLY / UNREPRODUCIBLE HEADLESS: the bug needs the real-time menu↔attract cadence. Headless is
uncapped and collapses it; the corrupt interleave (menu fade rendering while attract VRAM is resident) never
forms. Windowed iteration is minutes-per-cycle (the attract loop is long real-time) → slow + needs the user's
eyeball (can't self-verify a render). Tools tried: frame-indexed shots (drift, unreliable), PAD axis, bgshot
hook on the orphan native SM (never fires), fadeshot (works, but only the headless single-fade).

ROOT CAUSE (model, needs windowed confirm): on the demo→menu return the menu texgroup reload + menu render are
not sequenced/gated against the still-resident attract scene, so the menu fades in over stale demo VRAM (raw
texture pages). This is the "native render doesn't follow scene/sub-scene state" class (standing directive). The
fix is to make the menu render own/clear the scene on reload (and ideally route the transition fade through
engine_fade_set so it's actually applied) — NOT to reproduce PSX OT order.

TOOL ADDED: `debug fadeshot` (interp.cpp) — on every recomp FUN_8007e9c8 screen-fade call, log color+ra and
capture s_tex to scratch/screenshots/fade_NNN.ppm. Deterministic capture of a transition's fade frames.
NEXT: reproduce the demo→menu return WINDOWED with fadeshot (run long enough for a full attract loop, or skip
the demo via input), capture the corrupt menu frame, identify the exact VRAM region it samples vs the menu
texgroup target, then gate/sequence the menu render on the reload. Verify on the live windowed game (user).

---

## later-252 (2026-06-25): FIXED — opening-cutscene narration renders nothing on the native FIELD path (2D overlay drop)

THE BUG (user-reported): selecting New Game → the opening STORY cutscene ("Tomba is living peacefully in the
country when Zippo finds a mysterious...") drew NOTHING on native; the prior menu's stale VRAM showed through.

ROOT CAUSE (definitive): the cutscene runs in the FIELD/GAME stage (0x8010637C). ov_draw_otag's field branch
(game_tomba2.cpp) runs ov_scene_native (PC-native 3D world) and SKIPPED the PSX OT walk ENTIRELY — exactly the
frontier note it carried ("scenenative skips ALL the PSX 2D for the field"). The narration is PSX 2D glyph
SPRITES submitted into the OT (verified live: scene classifier saw rect=60 + 1 vramcopy on the field OT, with
the glyph prims linked at OT[1]); the field path never walked them, so all 2D was dropped.

THE PORT (the fix — 2D-overlay enumeration on the native field path):
- runtime/recomp/gpu_native.cpp: new `g_ot_2d_only` mode for the OT walk (gpu_dma2_linked_list/gpu_gp0). When
  set, the prim classifier DROPS all guest-OT POLYS (the GTE 3D world — OWNED by ov_scene_native, redundant in
  the OT; and is3d is UNRELIABLE here: projprim has no records on the native field path, so is3d==0 for every
  poly — keeping them re-emits the whole world as flat HUD → render-queue overflow + the free-roam crash) and
  keeps only the 2D HUD SPRITES (cutscene text, dialog, item bubbles, HUD). bg sprites are dropped too.
- engine/game_tomba2.cpp: the field branch now runs ov_scene_native THEN a 2D-only OT walk
  (g_ot_2d_only=1; gpu_dma2_linked_list; g_ot_2d_only=0) — queued as RQ_HUD on top of the native world. This is
  THE behavior (not a debug channel). scenenativehud kept as a DIAGNOSTIC (full walk incl. world).
- VERIFIED LIVE (windowed, real New Game flow): the narration renders correctly on black, no stale-menu garbage.

ALSO FIXED (separate real bug, runtime/recomp/dbg_server.cpp): the debug server's write() to a socket raised
SIGPIPE with the DEFAULT disposition = TERMINATE THE WHOLE GAME, so a dropped/timed-out dbgclient connection
killed the live port (masqueraded as a "crash" entering the New-Game cutscene). Now signal(SIGPIPE, SIG_IGN).

FRONTIER LEFT: 2D-POLY overlays (gradient/fade panels) and world-billboard SPRITES are not yet discriminated on
the field 2D-only walk (provenance — projprim/obj_depth — is empty there); the cutscene text + common HUD are
sprites and work. A separate pre-existing DERAIL on one New-Game→free-roam entry path (bad opcode at pc=8) was
observed and is NOT caused by this change (the OT walk executes no guest code); track separately.

---

## later-253 (2026-06-25): FIXED — SBS `both` mode freezes on the intro OP.STR movie (guest STR streamer spin)

SYMPTOM: `PSXPORT_SBS_MODE=both ./run.sh` froze right after `[fmv] MOVIE/OP.STR -> LBA 152238`, printing
the game's own `time out in strNext()` repeatedly (seconds apart). NOT caused by later-252 (the cutscene
2D fix) — it's in the FMV/CD path. PSXPORT_NO_FMV=1 does NOT help (that flag is read by NATIVE code; the
PSX core runs guest recomp that never sees it).

ROOT CAUSE (definitive, traced live + via decomp): the OP.STR opening movie is OWNED by the native FMV
player, which is ALREADY skipped in SBS (native_fmv.cpp:677 `if (g_sbs) return 0`), so core A is fine.
The PSX core (B) runs the GUEST demo machine (FUN_80106f80, DEMO.bin overlay). Its STR streamer
strNext (FUN_8010755c) waits for CD-streamed STR sectors that are NEVER fed on the interp path (no
async STR ring-buffer fill). strNext busy-polls: outer loop 0x7d0 (2000) x inner FUN_801075f8 0x7d0
(2000) calls to StGetNext (FUN_8008d030) = ~4M INTERPRETED polls per attract cycle, a NON-YIELDING
multi-second spin that stalls the SBS lockstep (step_core A, step_core B, present) = the "frozen" window.
StGetNext just dequeues a decoded frame from the StPlay ring (0x80102728, idx 0x80102714, 32-byte slots,
slot[0]==2 means frame ready); the ring is never filled -> always returns 1 (no frame) -> the spin.

KEY SUPPORTING FACTS (so nobody re-derives them):
- CdSync (FUN_8008a6ec) IS already HLE'd sync: cd_override.cpp:397 -> ov_cd_sync sets v0=2 (CdlComplete).
  So the demo teardown's `do { FUN_80089e1c(9,0,0)=CdControlB(CdlPause) } while(==0)` loop COMPLETES (it
  returns FUN_8008a6ec(..)==2). The teardown is NOT the hang.
- The platform-HLE table (sync_overrides.cpp / cd_override.cpp platform_hle_register) is consulted on
  the INTERPRETER path (interp.cpp coro_native_call / hle.cpp) for every jal. Runtime is INTERPRETER-ONLY
  (dispatch.cpp: rec_func_index always -1; generated/shard_*.c are offline analysis, NOT linked). So HLE
  fires on the PSX core too — strNext just doesn't reach a HLE'd leaf in its hot poll (StGetNext is plain
  guest code at 0x8008d030; faking its frame output is a MINEFIELD: success re-enters the decode-wait
  spin at FUN_80106f80 case 6 `do{}while(_DAT_1f800034==0)` 0x7fffff iters, and corrupts SM[0x4a] 7->8).
- The game's OWN skip-request flag DAT_1f80019d, when set, makes the demo machine prologue force the
  teardown sub-state: guest FUN_80106f80 (`if (DAT_1f80019d) SM[0x4a]=7`) AND native demo_menu_machine
  (engine_demo.cpp:401 `s4a = mem_r8(0x1f80019d) ? 7 : SM[0x4a]`). It's the Start-skip; case 7 clears it.
- Only ONE guest caller of StGetNext (DEMO.bin.asm:1278 = the demo OP.STR fetch). In-game cutscene FMV +
  XA voice/BGM are native (native_fmv / xa_stream), so skipping the guest demo FMV affects nothing else.

FIX (sbs.cpp step_core): while THIS core is in the DEMO stage (0x801fe00c==0x801062E4) and its demo SM
sub-state SM[0x48]==1 (the intro-FMV phase s1), set DAT_1f80019d=1. The demo prologue then forces the
teardown path and the never-satisfiable STR streaming is skipped SYNCHRONOUSLY (the movie is owned
natively). Confined to the DEMO stage so the GAME-stage (0x8010637C) opening cutscene — the SBS
comparison target — is NEVER skipped. VERIFIED headless: `time out in strNext` 0 occurrences (was
repeated), 3s frame-progress watchdog never fires (frames advance), no abort.

ALSO (this session, separate, later-252 commit): SIGPIPE killed the whole game when a debug-server client
disconnected mid-reply — now signal(SIGPIPE, SIG_IGN) in dbg_server.cpp.

## later-254 — PIVOT: interpreter REMOVED → static-recomp shards ARE the runtime substrate (fail-fast on miss)
**User directive (2026-06-30):** "remove the interpreter, every recomp miss should crash the game and give
us a log … no legacy, we don't keep legacy." REVERSES the later-101..103 interpreter-only pivot.

**What changed.** The flat interpreter (`runtime/recomp/interp.cpp`) is DELETED. The statically-recompiled
shards (`generated/shard_*.c` from `tools/recomp/emit.py`) are now LINKED and are the execution substrate
for every non-native guest function. `shard_disp.c` generates `rec_dispatch` (an address→`func_<addr>`
switch); a recompiled body runs as a plain C call. A MISS (overlay code, a non-recompiled address, a
computed/fn-pointer jump target) falls through `rec_dispatch_miss` (hle.cpp) which now FAILS FAST — prints
`[recomp-MISS] no recompiled fn for 0x… (caller ra, a0)` + a guest-stack backtrace and `abort()`s. There is
NO interpreter fallback. BIOS A0/B0/C0 vectors and the platform-HLE sync table (sync_overrides.cpp) still
resolve natively first; only genuine non-native RAM code aborts.

**Files:** removed `interp.cpp`; `dispatch.cpp` rewritten to shims (`rec_super_call`/`rec_interp`/
`rec_coro_run`/`stub_dispatch` → generated `rec_dispatch`; `rec_coro_redirect` setter kept) + holds
`g_override_tgt`; `hle.cpp` `rec_dispatch_miss` RAM path → abort+`guest_backtrace_to`; `cmake/
tomba2_port.cmake` drops the `PSXPORT_SUBSTRATE` option (shards always linked, interp.cpp never compiled);
`run.sh` regenerates shards whenever missing/stale (build REQUIRES them; generated/ is gitignored).

**PC is now PER-CORE (OO).** Removed the global `g_interp_pc`. Added `uint32_t pc` to `R3000` (r3000.h);
each recompiled wrapper sets `c->pc = <its guest addr>` on entry (emit.py). Diagnostics that read the old
global now use `c->pc` (mem.cpp store/SPU-DMA logs, sync_overrides trap). The watchdog signal handler drops
its PC line — the C backtrace already names the `gen_func_<addr>` guest call chain.

**Status:** builds clean; boots stub→native crt0→recompiled MAIN, then FAILS FAST at the first miss:
`0x80085CB4` (caller gen_func_80085B20 → `lw v0,*(0x800abda0); jalr v0+0xC`, a0=0x8010622C GAME overlay).
That is an INDIRECT (fn-pointer) call target jal-discovery can't see — the first worklist item. Backtrace is
clean (`gen_func_80085B20 → func_80085B20 → rec_dispatch → rec_dispatch_miss → abort`).

**NEXT (miss-resolution loop):** each miss is either (a) a resident MAIN fn reached only indirectly → seed in
emit.py EXTRA_SEEDS + regen; (b) overlay code → needs overlay static recompilation (the big phase); or (c) a
fn to port native. Grind the log top-down.

## later-255 — substrate boot: auto-seed indirect/vtable fn targets; reach the overlay boundary
Resolving the later-254 fail-fast misses top-down. The boot's early misses were all RESIDENT MAIN
functions reached ONLY via a function pointer (jalr through a vtable / callback), invisible to emit.py's
direct-jal discovery. Added TWO binary-only discovery scans (tools/recomp/emit.py), unioned into the seed
set so they're recompiled up-front instead of aborting one-by-one:
- `pointer_table_funcs`: scan the whole EXE image for WORDS that point at a function entry in text
  (`is_func_entry`: `addiu sp,sp,-N` prologue OR preceded by `jr ra` — the latter catches stackless leaf
  fns). Catches static fn-pointer tables / vtables baked in data.
- `constructed_func_pointers`: scan code for `lui rD,H; addiu/ori rD,rD,L` that builds an in-text
  function-entry address — catches vtable slots whose pointer is BUILT IN CODE then stored (so the
  address never appears as a single data word). e.g. FUN_8008651C (installed via code, word nowhere).

Result: recompiled set 1238 → 1533; the boot now runs native crt0 → init → loads START.BIN overlay →
native frame loop → **2 frames of the DEMO stage (0x801062E4)**, then fails fast at the first OVERLAY
miss: `ov_demo_frame → rec_coro_run(0x80106F80)`. 0x80106F80 is inside the stage overlay loaded at
0x80106228 (NOT in MAIN.EXE), so the static recompiler doesn't cover it.

**FRONTIER = overlay static recompilation.** The stage overlays (\BIN\{START,DEMO,GAME,...}.BIN, loaded
raw to 0x80106228+) hold the DEMO/field code and the render submitters. They must be recompiled as their
own module(s) keyed at the overlay base, with rec_dispatch routing 0x80106228.. to the currently-loaded
overlay. (The stub is already emitted as a separate module — same pattern.) Until then, any call into
overlay code fails fast by design.

## later-256 — overlay-recompilation design notes (the FRONTIER after later-255)
The substrate boots to DEMO frame 2 then fails fast at `0x80106F80` (overlay code). To run overlays
under the substrate (no interpreter) they must be STATICALLY RECOMPILED. Key facts gathered:
- **Overlays OVERLAP** — DEMO.BIN / GAME.BIN / SOP.BIN all load RAW to the SAME base **0x80106228**
  (engine_re.md:284/387; journal:514 "aliases GAME.BIN, distinguished by the root prologue sig"). So a
  given address (e.g. 0x80106F80) is DIFFERENT code depending on which stage overlay is resident →
  static address→fn mapping is ambiguous; need runtime CURRENT-OVERLAY dispatch.
- Two overlapping overlay SLOTS (at least): (1) stage slot @0x80106228 — START/DEMO/GAME/SOP/OPN/CRD.BIN
  (small, 1.6–17 KB); (2) area/field slot @ ~0x80113000+ — the big A0*.BIN area overlays (75–234 KB)
  holding the field render submitters (0x8013e9d8/0x8013fae0/0x8013f4dc/0x8013fb88 etc.).
- emit.py ALREADY emits a SEPARATE module for the boot stub (STUB_NAMES: stub_func_/stub_dispatch) — the
  same pattern is the template for per-overlay modules.

**DESIGN (N64Recomp overlay model):** (a) emit.py: recompile each .BIN as its own module, functions keyed
at base+offset, with its own dispatch table (reuse the Names/emit_module machinery). (b) Runtime registry:
map an overlay-range address → the CURRENTLY-loaded overlay module; cd_loadfile_native(dest,...) sets the
current overlay for the dest range (it knows the file). (c) rec_dispatch for an overlay-range address →
current overlay's func; still a MISS (fail fast) if no overlay covers it. First impl task: instrument
cd_loadfile_native to log (file, dest, size) over a boot→field run to get the EXACT per-overlay bases.

## later-257 — overlay STATIC RECOMPILATION: DEMO+GAME+SOP run under the substrate (no interpreter)
Implemented the overlay model (the later-256 frontier). The stage/mode overlays \BIN\*.BIN are now
statically recompiled as their OWN modules and run under the substrate — the boot drives DEMO → GAME →
SOP through the full intro/fade/gameplay cycle with ZERO recomp miss (was: fail-fast at DEMO frame 2).

**Recompiler (tools/recomp/emit.py).**
- Per-overlay module emission: each .BIN → emit_module with a PsxExe(load=base) and its own Names
  (ov_<tag>_*), keyed at base+offset, with its own dispatch switch. Seeds = pointer_table_funcs ∪
  constructed_func_pointers ∪ code_pointer_tables ∪ func_entries_after_return (jr-ra boundary scan) ∪
  internal jal targets.
- GLOBAL ROUTER split: `Names.router` ("rec_dispatch", shared) is what a recompiled body CALLS for any
  out-of-module target; `Names.dispatch` is the module's OWN switch (main_dispatch / ov_<tag>_dispatch).
  Hand-written rec_dispatch (runtime/recomp/overlay_router.cpp) range-routes: MAIN range → main_dispatch;
  an overlay-slot address → the CURRENTLY RESIDENT overlay (identified by a 32-byte content signature of
  guest RAM at the slot base, cached); else rec_dispatch_miss (fail-fast).
- code_pointer_tables(): seed vtable/handler-table targets (runs of ≥4 consecutive in-text code pointers)
  that is_func_entry misses (stackless leaves). EXCLUDES switch_table_spans() so a switch jump-table
  (array of in-function labels) isn't mis-seeded as a vtable and shred the containing fn (the printf
  parser 0x8009A76C class). Caught the resident vtable handler 0x8007E2F8.
- find_jump_tables() generalized to the `addu B,idx,tbl` idiom (table addr built in a SEPARATE reg from
  the index reg) — the form the overlays emit; recovered the DEMO menu machine 0x80106F80's switch.
- EXACT overlay bases from the CD load-log (OVERLAY_BASES, evidence-documented, NOT magic): STAGE slot
  0x80106228 (START/DEMO/GAME, mutually exclusive); MODE slot 0x80108F9C (SOP, loaded right after GAME
  which stays resident). cmake links the dynamic TU set via generated/rec_sources.cmake.

**Platform-HLE bypass fix (the "CD timeout" / VSync spin).** The HW-sync primitives (libcd/libetc/libmdec
VSync/CdSync/DecDCTinSync …) are RECOMPILED MAIN fns, so a call routed main_dispatch → the recompiled
BUSY-WAIT body, never reaching rec_dispatch_miss where platform_hle_lookup intercepts → spun to "CD
timeout"/"VSync: timeout". Fix: platform_hle_register() now ALSO wires each into the recomp OVERRIDE table
(shard_set_override) — func_<addr>'s wrapper checks g_override FIRST, so the native sync resolves before
the recompiled wait runs. (User report: "CD timeout … should be trapped like vsync, everything is sync.")

**Status:** boot → DEMO (menu machine + substates) → GAME stage transition (GAME.BIN swaps in at 0x80106228,
routed by signature) → SOP field-mode (LOAD/FADE/GAMEPLAY sm[0x50] 0→4, area intro) → area machine advances
→ FAILS FAST at the field AREA-CODE overlay: a 285096-B blob @ LBA 374 loaded to 0x80108F9C (MODE slot,
swaps out SOP), holding the field render submitters (0x8013xxxx). NEXT FRONTIER = extract + recompile that
area overlay (and the other A0*.BIN field overlays) at base 0x80108F9C.

## later-258 — recompiler TDD harness; field area overlay A00; recomp transforms (no flood-fill)
Continued the overlay frontier into the FIELD area code overlay, and added a TDD suite for the
recompiler (user directive: "put some TDD in the recompiler too" / "add game-specific tailorings").

**Recompiler TDD (tools/recomp/test_emit.py).** Two layers, runnable standalone or via pytest:
- STRUCTURAL: a tiny MIPS assembler (`Asm`) + asserts on the static analyses — find_jump_tables BOTH
  idiom variants (A: `lui base;addiu;addu base,base,idx`, B: `addu B,idx,tbl` overlay form), is_func_entry,
  code_pointer_tables seeding a vtable while EXCLUDING switch_table_spans (the printf-parser class).
- EXECUTION (differential TDD): assemble a fn → emit_func → C → compile against a minimal Core → RUN on
  concrete reg/mem → assert state. Covers basic ALU, a loop, a recovered jump table, branch-into-delay-
  slot, shared-epilogue register restore, tail-call dispatch, and a 200k-iteration TAIL-JUMP LOOP that
  only completes in O(1) stack with sibling-call optimization (pins the build-flag requirement). 11 tests.

**Field area overlay.** A0*.BIN are the per-area FIELD CODE overlays; each loads to the MODE slot
0x80108F9C (swapping out SOP), holding the 0x8013xxxx render submitters (cd-log: A00 = 285096 B @LBA374 ->
0x80108F9C). emit.py recompiles A00 (OVERLAY_BASES + `A0[0-9A-Z]` rule). Boot now drives DEMO -> GAME ->
SOP intro/fade -> A00 area load with no miss until the shared-epilogue gap below.

**Transforms kept (tested):** find_jump_tables `addu B,idx,tbl` variant (overlay menu machine 0x80106F80);
code_pointer_tables (vtable targets is_func_entry misses) + switch_table_spans exclusion;
merge_early_return_boundaries (a `jr ra` mid-body is an early return, not a function end — merges the false
split so a branch past it stays an in-fn goto; fixes A00 0x80131600->0x801316C4); branch-into-delay-slot
labels (MAIN 0x80084080). Build flags for the generated shards: `-foptimize-sibling-calls` (a guest tail-
jump loop -> `dispatch(c,x);return;` must be a real tail call or the C stack grows -> SIGSEGV) +
`-fno-strict-aliasing -fwrapv` (recomp-safety).

**Tried + REVERTED — CFG flood-fill.** Rewrote emit_func to emit each function as the CFG closure from its
entry (auto-duplicating shared epilogues, following `j` chains). It MIS-recompiled the resident vtable
state machine (0x8007E2F8 <-> 0x8007E620, register-based jump table) into an INFINITE LOOP (plain smoke hung
at DEMO frame 5; HEAD's linear emit runs it clean to frame 90). Too many subtle pitfalls (emission order /
entry-must-be-first, fall-through between disjoint blocks, merge-back into the main body). Reverted to the
proven linear [lo,hi) walk. Lesson: the linear contiguous-body model is the source of truth; handle cross-
function shared tails by seeding/merge, not by recomputing the whole-function CFG.

**FRONTIER = cross-function shared epilogue.** A00 0x80113100 branches into 0x80113314's epilogue at
0x80113328 (compiler tail-merged a common epilogue across sibling dispatch handlers). Linear emit routes the
cross-fn branch to the dispatcher -> fail-fast. Needs a TESTED surgical fix (seed the shared-epilogue target
+ make its owner tail-call it on fall-through, scoped to genuine epilogues only — NOT the global flood-fill).

## later-259 — shared-epilogue tail duplication (additive, TDD'd) — past the A00 0x80113328 frontier
The cross-function shared-epilogue gap (later-258 frontier) is fixed with ADDITIVE tail duplication —
the opposite design from the reverted flood-fill. emit_func keeps the proven LINEAR [lo,hi) body
UNCHANGED (entry always first, state machines untouched) and only APPENDS duplicated tail blocks for
branch/jump targets that land OUTSIDE [lo,hi) in a sibling's range (a tail-merged shared epilogue) —
collect_tail_dups follows out-of-range, in-module, NON-entry targets (a sibling ENTRY stays a real tail
call), never re-entering [lo,hi); a tail that flows back into the body emits an explicit `goto`. TDD first:
test_exec_cross_function_shared_epilogue (a branch to an epilogue past `hi` must still restore s0) — red,
then green. Verified on the game: plain headless smoke CLEAN (frame 90, 0 misses — the vtable state machine
that flood-fill broke is unaffected); autoskip now runs DEMO->GAME->SOP->A00 PAST 0x80113328 to a new
frontier at 0x8018BD30 (code in the area-data region 0x8018xxxx — another loaded overlay to map/recompile).

## later-260 — OPN base fix → past 0x8018BD30; FRONTIER = coroutine resume (GAME field task)
OPN.BIN's load base was wrong in OVERLAY_BASES (guessed 0x80108F9C). cd-log shows it loads to the AREA
slot 0x8018A000 (its header fn-ptrs 0x8018A348.. confirm), shared by-signature with the big area data.
Fixed OPN -> 0x8018A000; the field run now passes the OPN miss (0x8018BD30) and advances to the next,
architectural, frontier.

**FRONTIER = mid-function COROUTINE RESUME.** The field run fails fast at 0x801063F4 — caller `ra` ==
target, dispatched directly by the native scheduler (native_boot), and 0x801063F4 is MID-function in the
GAME task loop (`lw v0,0x138(s1)`, not a function entry). This is the cooperative-task RESUME the handoff
flagged: task-0 yields once per frame (FUN_80051f80 -> ov_switch longjmps OUT to the scheduler, which works
— it unwinds the C stack), but the scheduler then resumes the task at the saved mid-function PC, and the
recompiled substrate has NO resumable mid-function entry (a recompiled body is a plain C function; you
can't jump into its middle). DEMO sidestepped this because engine_demo.cpp owns its substates natively
(each synchronous, one frame, no mid-fn yield). GAME's field loop genuinely yields mid-body.
Two ways forward (a real decision, NOT a hack):
  (a) OWN the GAME field task loop natively (top-down, like engine_demo owns DEMO) so it never yields
      inside a recompiled function — the cooperative yield becomes a native frame boundary; or
  (b) STACKFUL coroutines for the substrate (run the recompiled task on a ucontext/separate stack that can
      be suspended at the yield and resumed) — a general fix, larger.
Overlay STATIC RECOMPILATION (the later-256 goal) is essentially done: DEMO/GAME/SOP/A00/OPN all recompiled
and running under the substrate; the remaining blocker is this resume model, a separate workstream.

## later-261 — GAME field runs under the substrate: cooperative loop = PC per-frame re-entry (NO MISS)
The coroutine-resume frontier (later-260) is solved the PC-game way (USER: "make a PC game, don't limit
yourself by PSX constraints"). The GAME cooperative task loop yields once per frame and the PSX scheduler
resumes it at the saved mid-yield PC — which the recompiled substrate can't continue (the loop's C frame is
longjmp'd away at the yield). Instead we treat the per-frame yield as a PLAIN PC FRAME BOUNDARY: the
scheduler now RE-ENTERS the recompiled loop at its TOP every frame.
- emit.py OVERLAY_EXTRA_SEEDS: seed the GAME loop top 0x801063F4 (a documented re-entry point; mid-fn, so
  no scan finds it — the prologue 0x8010637C is owned natively by ov_game_stage_main). gen_func for it =
  the loop body (dispatch the SM, then yield).
- SchedulerState.game_coop[] (game.h) + native_boot.cpp: when ov_game_frame returns "not owned" (the field
  state), mark the slot game_coop and, every frame, resume it at 0x801063F4 with the loop's callee-saved
  regs (s0=s1=0x1f800000, s2=1) — NOT the saved yield PC. All loop state lives in guest RAM, so re-entry ==
  continue; cleared on area transition (base+0xc changes).
Result: PSXPORT_AUTO_SKIP drives DEMO -> GAME -> SOP -> A00 field and runs the GAME field loop for 1500
frames with ZERO recomp-MISS (deepest the port has run); sm[0x48] advances across the run. Plain headless
smoke still clean (frame 90, 0 misses); recompiler tests 12/12. RENDER correctness (does the field draw)
is the next thing to eyeball — separate from this mechanical milestone.

## later-262 — run.sh extracts A0* area overlays; emit overlay word-align fix (portability)
USER hit a DIFFERENT recomp-miss than me on macOS (0x800810F0 vs my 0x800739AC) on a plain `./run.sh`:
root cause = run.sh extracted only the 6 stage overlays, NOT the A00..A0L field area overlays, so
overlay_funcs() seeded fewer resident MAIN fns (the area overlays jal into MAIN) → fewer MAIN fns
recompiled → a different MAIN miss per box. Fixed: run.sh now extracts A00..A0L too; emit.py word-aligns
overlay data (`data[:len(data)&~3]`) — a non-4-aligned A0* size overran a scan (IndexError). emit now
generates 28 overlay modules clean. NEXT (handoff scratch/handoff_ensure_recomp_and_attract.md):
(1) USER directive — move all recomp provisioning into a single HASH-CHECKED `tools/ensure_recomp.py`
that run.sh just calls ("ensure all recomp is there and matches a hash"); (2) resolve the attract-path
(plain, no AUTO_SKIP) misses 0x800739AC (indirect jalr → seed) and 0x800810F0 (coroutine resume → maybe
another game_coop-style loop). The AUTO_SKIP field path is clean and renders.

## later-284b — intro-cutscene FREEZE + red corruption FIXED by removing the redundant PSX-render-underneath
The later-284 root-cause (PSX-render-underneath `d0(0x8003f9a8)` recursing deep on task0's ~2KB guest
stack → sm[0x48]=17 clobber → freeze; game_coop r29 SP-leak → red-diagonal corruption) is fixed by
DELETING the underhood re-render from ov_field_frame (engine_stage.cpp). Key empirical finding: the
handoff assumed we must FIRST make ov_render_frame guest-memory-free and then delete BOTH the underhood
AND the dv_snapshot/restore rewind. Not so — `dv_restore_pre` already restores the FULL post-gameplay
guest state (2MB RAM+scratchpad+GTE), so removing ONLY the redundant re-render (and keeping dv_restore
as the decoupling mechanism) fixes the freeze + corruption with guest state provably correct. VERIFIED:
PSXPORT_SELFTEST=oraclediff stays convergent native-vs-oracle (only the ~26 benign baseline bytes) through
all narration checkpoints AND free-roam onset (native f1131, oracle f1133); nothing consumed the PSX-built
OT/packets (native display re-derives from node data). Screenshots fx_700 (narration text+Tomba), fx_1000
("And then…" cliff/sea), fx_1145 (walkable field, Tomba on the fishing line) render clean — the opening the
original NEVER reached (it froze on the fishing-line pose). NEXT FRONTIER (exposed by the fix): free-roam
aborts ~f1184 at `jal 0x80109450` with A00 resident in the MODE slot — A00's 0x80109450 is a jump-table,
not a fn (SOP's is a fn); the GAME-stage dispatcher at 0x8010882c drives it when sm[0x4c]==0 && sm[0x4e]==1.
See docs/findings/render.md ("Free-roam recomp-MISS: jal 0x80109450"). Making ov_render_frame write ZERO
guest memory (native-float A00 object render → drop the dv rewind for perf) remains a valid FOLLOW-UP.

## later-285 — free-roam recomp-MISS root cause CORRECTED: gameplay object-graph recursion (native ~4× deeper than oracle), NOT the render
later-284c blamed the A00 per-object RENDER chain overflowing task0's ~2.5KB guest stack, and prescribed
"own the A00 render native-float." That diagnosis is FALSIFIED. Evidence (this session):
- Prototyped the native A00 render (native ov_a00_node_render = submit_perobj_render for the model +
  faithful 0x8013DD34 marshalling, routed from the master walk RCASE_DEFAULT + rq_dispatch_case). It did
  NOT stop the crash (still aborts ~f1184 at `jal 0x80109450`). Reverted (correct-by-RE but tangential).
- `debug skip3d` (new probe) skipping the ENTIRE ov_render_frame orchestrator + the submit 0x8010810c
  STILL crashes at the same frame → the deep recursion is NOT in the render pass at all.
- A `g_in_scene_native` scope flag + a min-sp probe (`debug lowsp`) in rec_dispatch/interp show the deepest
  dispatches are `in_scene_native=0` — task0's own GAMEPLAY update, specifically the object-graph recursion
  through the indirect thunk 0x80022AB8 (`jalr v0`, ~10 levels), interleaving the object 2D-marker projector
  0x8013DD34 and the libgpu OT/GS-sort walker 0x80082D04↔0x80082734, kicked off from ov_field_frame's
  gameplay pass `d0(c,0x80022a80)`.
- DECISIVE: `PSXPORT_SELFTEST=oracle PSXPORT_DEBUG=lowsp` runs the pure interpreter through the whole opening
  to f4235 (free-roam A00) with global-min task0 sp = 0x801FE7A8 (uses ~0x258 bytes), SAME task0 layout
  (obj 0x801FE000, top 0x801FEA00). The native port at the same free-roam scene reaches 0x801FE078 (~0x988,
  ~10 levels) — ~4× deeper. The stack is NOT too small; the native port RECURSES TOO DEEP.
NEXT: bisect WHY native recurses ~4× deeper — (A) a native override in the object-update path leaks guest
sp (like the game_coop r29 leak fixed in a766217), or (B) a native spawn/placement divergence builds a
longer/looping child chain than the oracle. Diff native-vs-oracle object graph + recursion backtrace at the
~f1181 divergence point. Full detail: docs/findings/render.md correction block (later-285).

## later-290 (2026-07-01) — camera restructured to `class CutsceneCamera`; SOP snap-follow wired+verified; free-roam-camera premise falsified
The handoff's premise ("0x8006E3B0 is the live free-roam camera, wire it into ov_field_frame/objwalk") was
FALSIFIED by measurement: instrumented rec_dispatch (camtrace) + recdep on A00 free-roam WITH real player
movement (`press right`, player X 0x800E7EAC advanced 0x0F64→0x1770) show ZERO dispatch of any resident
camera fn (0x8006c800–0x8006e480) across 200 moving frames. The ~967/1000 figure was the SOP INTRO CUTSCENE
(frames 0–216), where sop.cpp natively drives 0x8006e3b0 each frame. The free-roam camera is in the MODE-slot
FIELD OVERLAY (hot recdep cluster 0x8013xxxx). → docs/findings/camera.md.
DONE regardless (the restructure is the directive): rebuilt the orphaned register-convention statics of
the old `engine_camera.cpp` into `game/camera/cutscene_camera.{h,cpp}` = `class CutsceneCamera` — methods over named state (Core*+cam_
members, named MASTER_X/Y/Z + G/S blocks, enum-ish mode methods), NO `c->r[4]=cam` register convention, guest
accessors (r8/r16/r32/camR*/…) behind names. 9 sub-ops + 4 orchestrators (added snapFollow=0x8006e3b0). Wired
`CutsceneCamera::snapFollow` into sop.cpp via `cam_snap_follow` (replaces both `d2(c,0x8006e3b0)` sites). VERIFIED:
`PSXPORT_DEBUG=camverify` A/B vs recomp oracle (rec_interp 0x8006e3b0) = **0 mismatch over 51+ live SOP calls**
(cam struct + full 1KB scratchpad) — exercises trackXZ/trackY snap + the full lookAt matrix builder (isqrt/
ratan2/MulMatrix0/ApplyMatrixLV/CopyMatrix + cpu_div). No regression (free-roam still reached f216; the boot
`[miss 0]` at ~f115 is PRE-EXISTING — same in the substrate build). WORKFLOW FIX: `tools/codemap.py` didn't
index C++ class methods (DEF_RE only matched `void ov_*(Core*`); extended it (METHOD_RE + def-line `// FUN_`
tag association, only when the method owns an address) so `--addr` finds `CutsceneCamera::lookAt` etc. — required as
subsystems move to `class Foo`. NEXT: RE the free-roam MODE-slot overlay camera (0x8013xxxx) and own it, or
formalize ObjectWorld (pc-subsystem-rebuild.md's ranked #1; behaviors already ~149/150 native).

## later-291 (2026-07-01) — CutsceneCamera oracle UNIT TEST (all 13 methods, 0-diff); free-roam camera correction
Added game/camera/cutscene_camera_test.cpp — a deterministic, render-free oracle unit test for the whole
`class CutsceneCamera` (PSXPORT_SELFTEST=camera). It seeds thousands of synthetic guest states (sweeping the
mode selectors G+0x164 / 0x800bf870 / cam+0x72 etc. so every switch arm is hit), runs each native method,
snapshots outputs, restores inputs, runs the guest fn via rec_interp on identical state, and diffs the full
cam struct + 1KB scratchpad + touched globals. Result: **0 mismatching words over 39000 runs** across all 13
methods — the restructure preserved behaviour exactly, including the latent modes (dist/pitch/heading/rotBuild/
mainFollow/…) the live SOP scene never exercises. 390 iters oracle-SKIPPED where the recompiler's jump-table
discovery can't evaluate the synthetic state (added hle.cpp `g_rec_miss_tolerant` test hatch so a genuine miss
skips instead of fail-fast-aborting; default 0 = fail-fast everywhere else). Found+recorded a recompiler gap:
yFloor 0x8006C80C render-mode 1 target 0x8006C844 isn't in the recompiled switch (docs/findings/camera.md).
CORRECTION to later-290: the reachable free-roam camera matrix (scratchpad 0x1F8000F8/0x10C, what
scene_build.cpp read_camera consumes) is rebuilt each frame via the resident matrix leaves (MulMatrix0/MR_init/
ApplyMatrixLV/CopyMatrix) with caller ra=0xDEAD0000 = a NATIVE caller — NOT overlay code. So the free-roam
camera is native, not the 0x8013xxxx overlay (that cluster is field render/objects). ov_80078610 (pool.cpp)
does per-area-load view setup calling resident lookat 0x8006D02C; the per-FRAME native driver isn't pinned yet.
NEXT: pin that native per-frame camera driver (wwatch 0x1F8000F8 → native caller of MulMatrix0), rework it to
call CutsceneCamera::lookAt directly; then reach a real post-intro area to see the true gameplay camera.

## later-293 (2026-07-01) — RESOLVED: free-roam camera IS the resident snapFollow; recdep is blind to intra-MAIN calls
Root-caused the later-290..292 confusion. A GUEST-STACK backtrace (PSXPORT_WWATCH on the camera view matrix
0x1F8000F8 + guest_backtrace_to) at a moving free-roam frame showed the live chain: field frame 0x80022xxx →
0x8006EEF8 (resident camera driver @0x8006ec4c) → 0x8006E3B0 (snapFollow) → 0x8006E3E0 (post-lookat return) →
MulMatrix0. So the resident camera IS the free-roam field camera. Why camtrace/recdep showed "zero camera
dispatch in free-roam": the recompiler emits **intra-MAIN calls as DIRECT C calls** `func_8006E3B0(c)` (see
generated/shard_3.c:16799, shard_4/5), NOT rec_dispatch — so any hook in rec_dispatch (recdep, camtrace) is
STRUCTURALLY BLIND to resident→resident calls. "0 in recdep" ≠ "dead". Recorded as a tooling caveat
(docs/findings/tooling.md) and the camera finding rewritten (docs/findings/camera.md).
CONSEQUENCE: `CutsceneCamera::snapFollow`/`lookAt` (owned, camverify 0-diff, oracle-unit-tested 0-diff over
39k runs) ARE the free-roam field camera; it currently runs as the equivalent substrate gen_func in free-roam.
There is NO separate overlay/native free-roam camera. "CutsceneCamera" is a misnomer (it's the general field
camera; rename to class Camera deferred, low priority). The handoff's original premise was CORRECT; my
later-290 refutation was the artifact. NEXT (top-down ownership, not a hunt): own the camera dispatcher
0x8006ec4c (calls snapFollow at 0x8006eef0) + 0x8006ea7c (mode selector, 21-entry jump table on 0x800bf870)
as CutsceneCamera::update(), wired from the native field frame; verify via the oracle unit test.

## later-294 — camera DISPATCHER owned native: CutsceneCamera::update() + init() (0-diff oracle)
Owned the per-frame camera DRIVER and mode selector, completing the camera tree top-down (leaves were
already owned). CORRECTION to the handoff: the recompiled driver entry is **0x8006EC44**, not 0x8006ec4c
(gen_func_8006EC44 exists; 0x8006EC4C is 8 bytes into it and isn't a separate fn). The driver is ARG-LESS:
it hardcodes the camera object at 0x800E8008 and reads its outer state from cam[0] (0=init/1=run/else idle),
runs the cam[1] sub-state machine, dispatches on cam[0x64]&0x3F (18-entry table @0x80016A44) to a follow
orchestrator / substrate leaf / field overlay, then always runs tail 0x8006C988. init()=0x8006EA7C: field
reset + render-mode-keyed (0x800BF870) 21-entry table @0x800169EC → initial mode, optional mainFollow path,
scripted-follow post-check (0x1F800236 ∈ {5,6}). Owned orchestrators called directly; unowned resident
leaves (E294/E360/E2FC/E918/CBA8/C988) + all field overlays (mode 0/1 render table @0x800A4AA0; modes
9/10/17 = 0x8018B924/0x8010D89C/0x80111AB4) via substrate rec_dispatch (sub()), like trackFollow. VERIFIED:
oracle unit test cases init+update = 0 mismatching words over ~10k verified iters (overlay modes MISS since
no overlay is loaded → skipped, expected; the test's check() now tolerates a native-run miss too). NOT wired
yet (reached via camera-object behaviour ptr; substrate runs it 0-diff meanwhile) — wire when the object
walk that dispatches the camera node is native. docs/findings/camera.md updated. Also fixed the stale
"cutscene-only, free-roam is a separate overlay" comments in cutscene_camera.{h,cpp} (superseded later-293).

## later-294b — camera driver modes 2/3/4 owned native (snap-follow variants, 0-diff)
Continuing top-down from the dispatcher: owned the three scripted SNAP follow orchestrators the driver
picks for modes 2/3/4 — snapFollowA (0x8006E294, mode 2 + init post-check), pitchFollow (0x8006E360, mode 3),
snapFollowB (0x8006E2FC, mode 4) — plus the trivial snap-accumulator leaves snapAccXZ (0x8006D934) / snapAccY
(0x8006D950); snapFollow refactored onto them. The scripted look-angle builders they call (0x8006DC38/DF88 for
A, 0x8006DAD8/DEF0 for B — rsin/rcos heading+pitch into the S block) stay substrate as a cohesive future unit.
VERIFIED: oracle unit test cases snapFollowA/pitchFollow/snapFollowB = 0 mismatching words, 0 skips (no overlay
path). pitchFollow passing confirms pitch() is arg-independent (hardcodes its G reads). The driver's modes
2/3/4 now call native methods; the resident camera follow tree is native except the look-angle builder unit.

## later-294c — scripted look-angle builder unit owned; camera follow tree fully native (0-diff)
Finished the camera follow subsystem: owned the 4 scripted look-angle builders snapFollowA/B call (were
substrate) — posBuildA (0x8006DC38: rcos/rsin place, overwrite S+0/S+8, cam66|=1), posBuildB (0x8006DAD8:
place + yaw/dist ACCUMULATE tail identical to lookatTail → extracted shared yawDistAccumulate(dx,dz)),
headBuildA (0x8006DF88), headBuildB (0x8006DEF0: heading step with ±10 snap). BIT-PRECISION the oracle
pinned: posBuildB truncates its P to int16 (posBuildA keeps 32-bit); posBuildB reads S positions lhu
(unsigned) vs posBuildA lh (signed). VERIFIED: oracle unit test all 4 = 0 mismatching words over 8000 iters
each, 0 skips; snapFollowA/B still 0-diff with native builders wired. Resident camera follow tree now fully
native (orchestrators + sub-ops + builders). Remaining camera substrate: init subs 0x8006E918/0x8006CBA8,
post-mode tail 0x8006C988, and true field overlays (modes 0/1 render, 9/10/17).

## later-294d — init() fully native: initPlace (0x8006E918) + initSeedGrp (0x8006CBA8) (0-diff)
Owned init()'s last two substrate deps. initPlace (0x8006E918): places the camera X/Z base S+0x02/S+0x0a
from the heading (rcos/rsin of G+0x140 + cam[0x52] ± cam[0x56] scaled by −radius 0x1F8000EE, over base
G+0x2e/G+0x36). initSeedGrp (0x8006CBA8): copies a 3-halfword group from a source struct into the FIXED
driver cam 0x800E8008 (cam[0x3a/0x3e/0x42]) — hardcodes 0x800E8008. Oracle unit test both = 0 mismatching
words over 4000 iters, 0 skips. init() is now 100% native. ONLY camera substrate left: the post-mode tail
0x8006C988 (per-frame, big/indirect-jump-heavy — next unit) + true field overlays (driver modes 0/1 render
dispatch, 9/10/17). Whole camera tree (driver/init/orchestrators/sub-ops/builders) native + oracle-verified.
