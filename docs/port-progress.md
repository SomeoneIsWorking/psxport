# Tomba2Engine — PORT PROGRESS TRACKER (the single source of truth, boot → gameplay)

**This file is the authoritative, ORDERED map of the engine port. Read it FIRST every session; UPDATE it in
the same commit whenever you own a function or learn a boundary fact. Goal: port the engine top-to-bottom in
EXECUTION ORDER — boot → menu → gameplay → per-frame — owning every ENGINE function as it runs, keeping only
game CONTENT as PSX.** Don't cherry-pick; advance the FRONTIER (§ "CURRENT FRONTIER" below).

Pointers (detail lives there, STATUS lives HERE): RE map `docs/engine_re.md`; findings/dead-ends
`docs/journal.md`; driving/automation `docs/driving-the-game.md`; render `docs/render-arch.md`; the boundary
& directives `CLAUDE.md`.

## How to own a function (the LOOP — same every time)
1. **Find it in EXECUTION ORDER** (this file's spine). Take the first node marked ☐ TODO at the frontier.
2. **RE it:** `tools/disas.py 0x<addr>` (function-scoped — it stops at `jr ra`; `--mem` = just loads/stores
   with resolved addr+WIDTH). Note args (a0=r[4], a1=r[5], …), every mem read/write (addr+width), the return
   (v0=r[2]), and any `jal` (callee = keep-as-reference or own-too). Record the RE in `engine_re.md`.
3. **Classify** (see § BOUNDARY): ENGINE → reimplement PC-native in `engine/`. CONTENT → leave PSX. PLATFORM
   → native platform service in `runtime/recomp/`.
4. **Reimplement** as `ov_<name>(Core* c)` (ABI `void(Core*)`). Read args from `c->r[]`, do the work via
   `c->mem_r*/w*`, set `c->r[2]`. Register with `rec_set_override(0x<addr>, ov_<name>)` (or
   `rec_set_interp_override_auto` for OVERLAY fns, flushed on unload). Add the .cpp to BOTH `tools/build_port.sh`
   and `run.sh` SRC lists.
5. **VERIFY by RUNNING the game (see § VERIFY).** There is NO oracle to diff against and NO override-OFF
   A/B build — env gating is banned (ONE PC-native behavior). Drive the live port through the REPL to the
   scene that exercises the function, then check the result:
   - **CONTENT-INTERFACE fns (the guest RAM the still-PSX content/cull/render reads):** inspect that RAM via
     the REPL (`r`/`rw`/`dumpram`) and reason about correctness against the recomp REFERENCE you read while
     porting. The output must be what the retained PSX content expects to read back; a wrong write corrupts
     the content (e.g. native terrain made Tomba fall through the ground, later-158).
   - **Pure ENGINE / RENDER result fns:** gate on the observable result — the world/menu/scene built and the
     picture drawn. The USER eyeballs a build (the agent builds, runs, sends a `shot`, and asks).
   - **ALWAYS confirm the override actually FIRES** (a `cfg_dbg` log on entry, enabled via REPL `debug`). A
     faithful override registered where it's never invoked looks fine while doing nothing (later-170 trap).
6. **Update THIS file** (status ☐→✅ + file/symbol) and `journal.md` (later-NNN), commit + push.

## VERIFY — drive the live PC game via the REPL (there is NO oracle to diff against)
The methodology changed (2026-06-20): the Beetle oracle (`wide60rt`), all diff/compare tooling (`gpu_differ`,
`drive.py`, `dualcore.py`, `ram_region_diff.py`, …), and every env A/B toggle are GONE. You verify by RUNNING
the ONE native port (`scratch/bin/tomba2_port`) and observing it — never by diffing two builds.

Drive it with the REPL (`PSXPORT_REPL=1`, commands piped on stdin; or the live TCP debug server
`PSXPORT_DEBUG_SERVER=1` + `tools/dbgclient.py` for a windowed run):
```
# headless, drive into the field, then inspect the state the function produces:
printf 'newgame\nskip 500\npress r\nrun 150\ndumpram scratch/bin/state.bin\nr 0x800bf4fa 4\nshot scratch/screenshots/now.ppm\nquit\n' \
  | PSXPORT_REPL=1 PSXPORT_VK_HEADLESS=1 scratch/bin/tomba2_port
```
Key REPL commands: `run N` (advance N frames), `newgame` (pulse to the GAME prologue), `skip N` (pulse Start
to push through the intro into the field), `press`/`release`/`tap <btn>` (input), `r`/`rw`/`w` (read/write
memory), `dumpram <path>` (+ a `.spad` scratchpad sidecar — so dumps cover scratchpad too), `shot <path>`
(VK-readback PPM, works headless), `debug <chans|all>` (enable diagnostic channels at runtime — replaces the
old `PSXPORT_DEBUG` env var), `stage`, `regs`, `seq`, `quit`.

Pick a scene that actually EXERCISES the output: the idle field exercises boot/steady systems, but a
MOTION scene (drive in then `press r` to free-roam walk) is needed for camera/anim accumulators — a stopped
scene leaves them unchanged and hides a bug. Confirm correctness by reading the relevant guest RAM / scene
state with `r`/`rw`/`dumpram` and comparing it to what the recomp REFERENCE says it should be; for the
picture, have the USER eyeball a build (`shot` + ask). Do NOT conclude from a single cherry-picked still —
verify on the running game. Tools that remain: `tools/disas.py` (RE a function), the REPL/debug-server
inspection commands above, and the F1 imgui overlay for live-tuning enhancements.

## THE BOUNDARY (one line; full text in CLAUDE.md)
PSX keeps ONLY game CONTENT: per-character/enemy **AI & behavior**, **physics/collision response**, **level
data**, **quest/event/progression rules**. EVERYTHING else is PC-owned (engine): boot, menus, loading, scene/
world/object model, placement/spawn, camera, projection, **render ordering & the renderer**, save/load, 60fps.
The recomp body is the behavioral ORACLE for engine fns (reimplement, don't transcribe) and the live RUNTIME
for content fns (call it). Do NOT mimic PSX hardware (GTE/GP0/OT) — remove Beetle by porting its CALLERS.

---

# THE EXECUTION SPINE — boot → gameplay  (✅ owned · ◐ partial/staged · ☐ todo · ⬛ keep-PSX content)

## A. Boot / init  (MAIN.EXE resident)
- ✅ `FUN_800896E0` crt0 (SP/gp/heap/BSS) — platform, via `native_boot.cpp`.
- ✅ `FUN_80050b08` main = `ov_game_main` — `native_boot.cpp`.
- Init prefix of ov_game_main (classified in engine_re.md table):
  - ✅ platform stubs: CdInit, ResetGraph, SetDispMask, VSync, SPU/sound, DMA/pad — native platform services.
  - ✅ `FUN_80050a0c` frame-state init = `eng_init_framestate` (engine_init.cpp).
  - ✅ `FUN_800509b4` display + GTE projection = `eng_init_display` (engine_init.cpp).
  - ✅ `FUN_80050a80` camera init = `eng_init_camera` (engine_init.cpp).
  - ✅ `FUN_80075130` **font / text system init** = `ov_font_init` (engine_font.cpp) — ORCHESTRATION + direct
    writes + the 3 engine-state callees OWNED PC-native; the 8 LIBGPU/sound callees KEPT as `rec_dispatch`
    in-context (later-182b nested-dispatch risk). Owned: `ov_font_bank_select` (FUN_800963a0, store
    0x80105cec), `ov_font_bank2_store` (FUN_80096370, store 0x80105d28), `ov_font_glyphclass_fill`
    (FUN_800752b4, 24-entry table @0x800be238+8), plus the direct writes (0x800bed78/bed80, 0x800be358 +
    14× sh loop @0x800be3d6.., the sp+16 FntOpen struct, 0x800be22a/b). Mirrors the sp-48 frame so the
    dispatched FntOpen (0x80098330/0x80098d30) reads its struct at sp+16. VERIFIED: called directly from
    native_boot (replaces rc0); boot dump (run 2) main-RAM + scratchpad 0-diff vs scratch/bin/initref.bin.
    Registered in game_tomba2.cpp (orchestrator + 3 callees) for any other dispatcher.
  - ◐ `FUN_800520e0` **engine subsystem init** = `eng_init_subsystems` (engine_init.cpp) — ORCHESTRATION owned
    PC-native (later-183): the 6 direct engine-flag writes (*0x800bf4fa=0xffff, *0x800ecf4a/4c/4d/4e/4f=0) +
    the 4-callee sequence. Boot A/B (frame 50) main-RAM + scratchpad 0-diff. Callee descent:
    ✅ `FUN_8007b328` entity-pool init = `eng_init_entity_pool` (later-183b, boot A/B 0-diff: the 8-byte header
    @0x800fb160 + FUN_8007b2c0(0) scratchpad scale params, both inlined native).
    ✅ `FUN_80086620` mode ctrl = `eng_init_mode_ctrl` (later-184: full logic reimplemented native; verified a
    NO-OP at the init call — a0=1 with both mode flags 0x800abe88/8c + counters still BSS-zero → early-return;
    boot RAM+scratchpad IDENTICAL to the recomp-running reference dump).
    ✅ `FUN_80087a60`→`FUN_80086970` input init = `eng_init_input` (later-184: own the direct writes + the
    7-call orchestration native — FUN_80080890/80087400/800873f0/80085b10/800808a0 + the dispatch handler
    *0x800abe3c twice — rec_dispatched in-context; boot RAM+scratchpad IDENTICAL to reference).
    ✅ `FUN_80088b00` allocator/dispatch-table = `eng_init_alloc` (later-184: install the 6-entry mode
    dispatch table 0x800abe38.. + build the 480-byte heap @0x80102500 native, rec_dispatch the 3 callees
    FUN_80089160/8009a340/80086738 in-context; boot RAM+scratchpad IDENTICAL to reference). **FUN_800520e0
    is now FULLY OWNED — all callees native.** (Reference-cmp caught a lui+addiu sign-extension slip in the
    table addrs — 0x8009 lui + negative addiu = 0x8008xxxx — which is exactly the gate working.)
  - ◐ `FUN_80051e00` scheduler task-table init (rc0'd at boot, native_boot.cpp:371) + `FUN_80051f14` register
    task 0. The native frame loop replaces the per-frame stepper.
- ✅ Native cooperative frame loop `native_scheduler_step` (replaces `FUN_80051e60`) — `native_boot.cpp`.
  Thread/critical-section primitives owned: ov_open/close/change_thread, ov_switch (0x80080880).

## B. Stage sequencer / loader  (task 0)
- ✅ `FUN_800499e8` task-0 stage sequencer = `ov_task0_boot` (engine_level.cpp) — resolves START.BIN.
- ✅ `FUN_80052078` stage transition = `ov_stage_transition` (engine_level.cpp) — restart task at new stage.
- ✅ `FUN_800450bc` level/overlay LOADER = `ov_load_stage`/`eng_load_stage` (engine_level.cpp, later-162).
- ✅ CD loadfile path native (ov_cd_loadfile 0x8001DB8C synchronous) — `cd_override.cpp`.

## C. Stages (task 0 cycles START → DEMO → GAME)
- ☐ **START.BIN** boot splash (SCEA etc.) — un-ported PSX boot path (EXPECTED-broken until ported; don't debug).
- ☐ **DEMO stage `0x801062E4`** = TITLE / front-end MENU state machine (8 substates) — **the big front-end
  engine system, INTERPRETED.** Title→New Game flow dr(driving-the-game.md). The first large un-owned system
  in execution order between boot and gameplay.
- GAME stage `0x8010637C` (overlay GAME.BIN, AUTO-registered in engine_stage.cpp via stage_scan_overlay):
  - ✅ top-level prologue = `ov_game_stage_main`; ✅ sm[0x48] handlers 0/1/2 (`ov_game_s48_0/1/2`);
    ✅ running dispatcher sm[0x48]==2 (coro-redirect handshake, later-169).
  - ◐ `ov_game_s4c` (sm[0x4c] area machine `0x80106478`) — STAGED, UNVERIFIED (never entered on field/boot;
    needs a real area transition sm[0x4a]==2; see § OPEN). NOT registered.

## D. Per-frame GAMEPLAY systems (inside the GAME stage loop)
- ✅ `FUN_800788ac` frame update = `ov_frame_update` (pad read + present + audio kick) — game_tomba2.cpp.
- ✅ `FUN_8007a904` object/entity WALK = `ov_objwalk` (engine_tomba2.cpp) — the per-frame object driver.
- ✅ `FUN_8007712c` per-object CULL / LOD = `ov_object_cull` (game_tomba2.cpp). **BODY now PC-native
  (later-188)** — was a `rec_super_call` WRAP (recomp body ran hot, ~11.2% of sampled interp time); now
  `cull_native_body` reimplements the full decision (RE'd from the disasm: jump table 0x80016cc0, 5 state
  handlers + state-0 typed sub-dispatch). dist=eng_isqrt16(dx²+dy²+dz²); per-state {near,far,fov} cone test
  KEEP iff near≤dist<far AND (fwd·d)/(dist*4)≥fovthr (MIPS signed `div`, C `/` matches); writes the visible
  flag @obj+1 + pushes the obj ptr onto 3 type-keyed render queues (A/B/C @0x1f80013c/48/54, cap 24/40/28).
  VERIFIED 0-diff over 60000+ live calls via the `cullverify` gate (predict native pure, recomp body does
  the writes, compare obj+1/state/queue-delta/pushed-ptr) incl. directional motion. Field 27.06M→25.02M
  insns (−7.5%; cumulative −41.7% from baseline). Margin re-include (margin_collect) still fires.
- ✅ Gameplay-overlay pure LEAF `0x8013fae0` = `ov_tile_lookup` (engine_submit.cpp, later-188b) — 2D table
  lookup `tab[52*a1+a0] & (mask16<<4)` (tab=*0x8014c804, mask=*0x8014c800). Was the single hottest overlay
  piece (4.25%, 39.9k calls), mis-bucketed under FUN_8013F0DC until the prof_report overlay-resolution fix.
  Signature-registered in engine_scan_overlay; `tileverify` gate 0-diff 60000+ calls. Field 25.02M→24.46M.
- ✅ `FUN_80051C8C` per-object TRANSFORM build = `ov_build_xform`.
- **Camera update (engine_camera.cpp):**
  - ✅ position X/Z `FUN_8006d960` = `ov_cam_track_xz`; ✅ position Y `FUN_8006da54` = `ov_cam_track_y` (later-174).
  - ✅ rotation / LOOK-AT builder `FUN_8006e464` = `ov_cam_rotbuild` (engine_camera.cpp, later-175) — 2 jump
    tables + common look-at tail; owns control flow + arithmetic native, CALLS libgte rsin/rcos/ratan2/isqrt
    via rec_dispatch. Output 0x1f8000d0/d8 is SCRATCHPAD (main-RAM diff is blind) → gated by per-call
    comparator `PSXPORT_DEBUG=camverify` (native vs recomp oracle, 0-diff, d0 accumulating on motion scene).
  - ✅ DISTANCE / ZOOM solver `FUN_8006d2ac` = `ov_cam_dist_solve` (engine_camera.cpp, later-176) — first
    sub-fn of orchestrator FUN_8006e0f0. Picks a target (X,Z) (13-entry jump table on G[+0x164] + cam[+0x72]&2
    path), derives the camera→look-point planar distance (isqrt) + angular error (ratan2), smooths cam[+0x14]
    toward ±0x280000 (±65536/frame), accumulates into cam[+0x58], places camera planar pos cam[+0x08]/[+0x10].
    Outputs are MAIN-RAM cam fields → gated by per-call comparator `PSXPORT_DEBUG=camverify` (0-diff over 1800+
    calls on the motion scene). GOTCHA: the far/near branch's DELAY SLOT (slti angd,2048) reloads v0, so the
    NEAR-path final test NEGATES s0d when angd<2048 — a missed delay-slot effect (caught by the trig-spy diff).
  - ✅ angle-accumulator step `FUN_8006e010` = `ov_cam_angle_step` (engine_camera.cpp) — ±8 rate-limit of
    cam[+0x34] toward a mode-selected target; main-RAM output, camverify 0-diff (1000+ calls, motion scene).
  - ☐ remaining sub-fns of `FUN_8006e0f0`: 0x8006d654 (pitch smoother), 0x8006c80c (Y-floor clamp),
    0x8006dcf4 (heading tracker), 0x8006d02c (orient/look-at matrix builder, uses GTE leaves). Full per-fn
    port specs (inputs/outputs/hazards/pseudocode/verify) in scratch/handoff_camera_subfns.md (this session).
  - ☐ per-MODE orchestrators `FUN_8006e0f0` / `FUN_8006e228` / `FUN_8006e3f4` (call the smoothers; multi-mode).
- ✅ Math helper `FUN_80077FB0` = `ov_isqrt` (engine_math.cpp, later-186) — 16-bit ROUNDING integer sqrt
  (libgte-style leaf). Was 8.41% of all interpreter instructions; owning it native dropped the field run
  42.93M→38.99M insns/300fr (−9.2%). Bit-exact: 65000+ live calls 0-diff vs recomp (`mathverify` gate).
- ✅ Math helper `FUN_80084080` = `ov_gte_norm` (engine_math.cpp, later-186) — table-based fixed-point sqrt
  (LZCR leading-bit count → normalize → LUT @0x800a6310 → exponent shift). GTE used ONLY as a CLZ → pure
  fn of a0, ported native with `__builtin_clz` (the GTE caller no longer needs Beetle). Bit-exact 0-diff
  15000+ live calls. (Was mis-attributed as 9% by 256B buckets; real cost 0.5% — the profiler bucket fix.)
- ✅ GTE 3x3 MATRIX MULTIPLY `FUN_80084110` = `ov_mat_mul` (engine_math.cpp, later-186) — P=R×M via MVMVA
  (sf=1, rot matrix, lm=0). Was 16% of hot interp time + top freq leader (55k calls). Ported PC-NATIVE per
  USER 2026-06-21 (GTE is PSX hardware → make the math native C, no gte_op/Beetle): plain-C MVMVA (44-bit
  accum, >>12, signed clamp16;
  last element via SWC2 = sign-extended). GTE-EXACT verified: a2 output + MVMVA GTE regs (IR/MAC/input) 0-diff
  over 115000+ live calls (FIFO/LZCR regs excluded — comparator can't round-trip FIFO writes; neither path
  touches them). Field run 38.77M→35.21M insns (−9.2%; cumulative −18% from baseline). First of the cluster.
- ✅ GTE RotMatrix `FUN_80085480` = `ov_rotmat` (engine_math.cpp, later-187) — Euler angles (SVECTOR a0)
  → 3x3 rotation matrix (MATRIX a1, also returned). Was ~17% of hot interp time (the hottest after the
  matmul leaf). Ported PC-NATIVE: SIN/COS LUT @0x800a6490 (sin low / cos high, sign re-applied to sin)
  + the GTE GPF op (cmd 0x3d, sf=1: MAC_i=(IR0·IR_i)>>12, IR_i=clamp16) reimplemented as a clamped
  scalar×vector multiply, interleaved with 2 native >>12 mults (cy·cx, cy·sx, truncated not clamped).
  GTE-EXACT verified: 5 output words + ALL GTE data regs (incl. the RGB FIFO the 4 GPFs push) 0-diff
  over 55000+ live calls (FIFO/LZCR excluded — same as matmul). Field run 35.21M→31.90M insns (−9.4%;
  cumulative −25.7% from the 42.93M baseline). Second of the cluster.
- ✅ GTE-transform cluster REMAINDER `FUN_80085050`/`FUN_80084EB0`/`FUN_80084D10` = `ov_rot_z`/`ov_rot_y`/
  `ov_rot_x` + `FUN_80084220` = `ov_apply_matlv` (engine_math.cpp, later-187). The three rot_* are ONE
  generic kernel (`rotpair`): compose an axis rotation onto a 3x3 matrix — rowA'=(cos·rowA−sin·rowB)>>12,
  rowB'=(sin·rowA+cos·rowB)>>12, differing only in (rowA,rowB) offsets + the Y-variant's inverted sin
  sign (80085050 rows +0/+6 posSin+1; 80084EB0 +0/+12 posSin−1; 80084D10 +6/+12 posSin+1). PURE (no GTE,
  SIN/COS LUT @0x800a6490 + native mults; multu→MFLO = signed product for 16-bit operands). ov_apply_matlv
  = MVMVA (sf=1, ROT matrix from GTE CR0-4, vec from a0) → IR1-3 sign-extended to a1, GTE-exact. VERIFIED
  0-diff: rotX 55k+, ov_apply_matlv 75k+ (output + all GTE data regs), rotZ 35k+ live calls; rotY only 1
  live call (Y-axis rotation barely fires in 2.5D seaside) but is the same kernel verified at scale on its
  siblings + offsets/sign read directly from the disas. Field run 31.90M→27.06M insns (−15.2%; **cumulative
  −37% from baseline**). The whole GTE transform cluster is now OWNED + gone from the hot-list.
- ✅ GTE RotMatrix VARIANT `FUN_80084A80` = `ov_rot84A80` (engine_math.cpp, later-189). Build a 3x3 matrix
  from 3 Euler angles (a0 SVECTOR vx/vy/vz, a1 MATRIX out + v0). PURE — SIN/COS LUT @0x800a6490 + native
  16-bit mults (multu/MFLO), NO GTE ops, so no GTE side-effects to mirror. Different element layout/rotation
  order than 80085480 (m20=−s1; m00=c1c2; etc — full layout in the fn comment). Intermediates kept 32-bit
  (asm's `sra ,12` stays register-width before re-multiplying); only the final `sh` stores truncate to 16.
  VERIFIED 0-diff 5000+ live field calls (forced-verify build: resident-math overrides latch `v` at boot,
  so the REPL `debug mathverify` line arrives too late — verify by registering `*_verify` unconditionally
  for the one-time gate, then revert to the gated form). Was 4.4% of field hot time, now gone from hot-list.
- ✅ Render submit: geom GT3/GT4/gt4_bp, per-object render `0x8003CCA4`, render walk `0x8003C048` — engine_submit.
- ✅ MORE per-frame transform/cull leaves OWNED (later-189):
  - `FUN_8007778C` = `ov_cull_wrapper` (game_tomba2.cpp): camera-relative delta (obj−cam, wrapping s16) +
    zero the two cull scratchpad flags (0x1F800080/84) → `rec_dispatch` the owned cull body 0x8007712c
    (routes through ov_object_cull for current-object tracking + margin). Verified 0-diff 40000+ via `cullwrap`.
  - `FUN_80051464` = `ov_xform_propagate` (engine_submit.cpp): child-node transform propagation. Orchestrates
    the owned rot_x/y/z + matmul(80084110) + MVMVA(80084220) in the recomp's exact jal order (preserving the
    matmul→MVMVA GTE-CR coupling), seeds the scratchpad identity work matrix, accumulates parent translation.
    Parent = node itself (sentinel c[6]==-1) or sibling node[0xC0+4*c[6]]. Verified 0-diff 6000+ via `xformverify`.
  - `FUN_800517BC` = `ov_settrans` (engine_math.cpp): SetVector 0x20-byte block (a1/a2/a3 s16 at +0/+8/+16,
    rest 0). Pure leaf. Verified 0-diff 30000+.
  - `FUN_800851F0` = `ov_rot851F0` (engine_math.cpp): CPU RotMatrix twin (non-GTE companion to 80085480);
    pure LUT trig, distinct layout, two elements negate-before-shift (nsh12). Verified 0-diff live (rare path).
    A dependency of FUN_800597AC. NOTE: resident-math overrides latch their verify-gate `v` at boot (the REPL
    `debug mathverify` lands too late), so verify them with a one-time FORCED-verify build, not the channel.
- ✅ WORLD-TRANSFORM orchestrator OWNED (later-190):
  - `FUN_80084360` = `ov_compmatlv` (engine_math.cpp): libgte CompMatrixLV — compose M ← R × M IN PLACE.
    Identical product/clamp/leftover to `ov_mat_mul` (P=R·M), only in-place into a1 + v0=a1. GTE-exact,
    0-diff via forced-verify (thin coverage in seaside, raised by its consumer FUN_800597AC). ALSO: made
    `ov_mat_mul` + `ov_compmatlv` faithfully CTC2 R→CR0-4 (the real bodies do; was relying on a prior
    80084470's CR write) so a following `ov_apply_matlv` (reads CR0-4) gets the right matrix robustly.
  - `FUN_800597AC` = `ov_orch597AC` (engine_submit.cpp): per-object world-transform orchestrator (3.8% field
    hot). Builds node+0x98 render matrix (SetVector + RotMatrix + CPU-rot + matmul + CompMatrixLV + 80084470),
    optional SECONDARY transform (node[0x145]/0x146 gated → 0x1F800060 + trans 0x1F800074..7C), then child
    propagation over node[0xC0+4i] with parent select (child[6]<0 → this node, via s6/s7 picking node vs the
    secondary matrix; child[6]>=0 → sibling node[0xC0+4*child[6]]). node[8] temp-forced to node[9] for the
    loop, restored on exit. rec_dispatch'd primitives in exact jal order (preserves CR coupling). Verified
    0-diff 2800+ live calls via `orchverify` (lazy first-call gate → REPL `debug orchverify` works).
- ✅ AUXILIARY render walks `0x8003BCF4` / `0x8003BF00` / `0x8003EEC0` = `ov_rwalk_aux_*` (engine_submit.cpp,
  issue #4): faithful per-node lift of each recomp body + per-node `gpu_obj_depth_add(world-pos depth)` so
  flame/rope/effect billboards occlude by real world depth (was: flat 2D band → drew over foliage). Field
  verified safe (renders intact, ndepth obj_depth spans 3→5 = the 2 aux queues now contribute). The FLAME
  occlusion itself awaits the flame-hut scene + USER eyeball (not headless-reachable yet).
- ✅ Render ORDERING: engine RenderQueue owns VISIBILITY (3D = real D32 depth buffer; 2D = enumeration order).
  Prims enumerated in OT LINK order (later-178) — NOT to honor PSX visibility (engine owns that) but to apply
  GP0 STATE (E1 texpage/E2 texwindow) in DRAW order so each 2D sprite binds the right texpage. later-172's
  linear packet-pool walk was REVERTED: memory order ≠ draw order broke 2D (title bg sprites went black).
  DrawOTag `0x80081560` = `ov_draw_otag`. Native projection `proj_native_vertex` (float matrices, real depth).

## E. PLATFORM services (native; the "remove Beetle by porting callers" axis — NOT game logic)
- ✅ CD/disc (cdc_native, cd_override, disc), ✅ XA stream (xa_stream), ✅ libsnd BGM (ov_bgm_*) + SPU voice.
- ⬛ Still-linked Beetle: `gte.c` (GTE — used by the still-PSX cull NCLIP + content collision; goes when those
  callers are ported), `mdec.c` (FMV), `spu.c`. These vanish only by porting their CALLERS, never by re-emulating.

## ⬛ CONTENT — keep as recompiled PSX (do NOT reimplement)
Per-enemy AI / per-character behavior; physics & collision response (Tomba move/jump/land); level data payloads;
quest / event / progression / game-rule logic.

## F. PERF PROFILER (the #1-priority lever: hot interpreted fn → own it native → faster + more PC-native)
The port is interpreter-only, so every un-owned engine fn + all CONTENT runs through `interp_flat`. The
in-port profiler (later-186, `interp.cpp`) gives the TIME + FREQUENCY histograms to pick the next fn to own.
- **Drive:** REPL `prof start` / `prof stop` / `prof dump <path>`. Cost: one predictable branch when OFF.
  Buckets are 16 BYTES (aligned to function starts — 256B straddled adjacent small fns and mis-attributed,
  e.g. lumped the hot FUN_80084110 under FUN_80084080; fixed later-186).
- **Report:** `tools/prof_report.py <dump> --top N` aggregates PC buckets by enclosing function (resident list
  `tools/recomp/tomba2_funcs.txt`, which runs to 0x8018FBCC; addrs past MAIN.EXE file end are overlay code).
  Top-200 16B buckets cover only the hot tail (TIME % is of the sampled buckets, not all insns) — the RANKING
  is what matters; the FREQUENCY list (call counts) is exact.
- **ACCURATE HOT-LIST (field, newgame+skip 600, 300 frames — later-186; don't re-profile to re-confirm):**
  - **GTE matrix/vector TRANSFORM cluster ≈ 52% of hot time — THE dominant lever + the "remove Beetle GTE"
    axis. NEXT BIG ARC.** All resident libgte ApplyMatrix/RotTrans-family helpers that CTC2-load a matrix
    + run MVMVA/RTPS, write to a2: ~~`FUN_80084110` 16.2%~~ ✅ OWNED native (ov_mat_mul, GTE-exact 0-diff
    115k calls — §D; the PROVEN RECIPE for the rest: decode GTE cmds, reimplement MVMVA from gte.c, verify
    a2 + GTE output regs via the comparator excluding FIFO/LZCR). **THE WHOLE CLUSTER IS NOW ✅ OWNED +
    GONE FROM THE HOT-LIST (later-187):** ~~80085480 16.9%~~ ov_rotmat, ~~80085050 9.3%~~ ov_rot_z,
    ~~80084EB0 8.25%~~ ov_rot_y, ~~80084D10 7.5%~~ ov_rot_x, ~~80084220 2.3%~~ ov_apply_matlv (+ the earlier
    80084110 ov_mat_mul). Field run cumulative **42.93M→27.06M insns (−37%)**. (Historical note — the orig.
    plan said porting native needs: (1) a native fixed-point GTE-math layer (MVMVA with
    sf/lm/IR-saturation/MAC-overflow matching mednafen gte.c), (2) GTE-REGISTER inspection tooling (the RAM
    dump is BLIND to GTE regs — must verify GTE-state LEAKAGE that downstream RTPS reads), (3) per-fn a2 +
    GTE-reg comparator. Big but the single highest-value perf + 100%-PC-native move.
    **USER DIRECTIVE 2026-06-21 (the GTE is PSX HARDWARE; this is a PC game → that math must be PC-NATIVE,
    not a mimicked/emulated GTE):** order is **(1) PC-NATIVE the GTE MATH FIRST** — reimplement each cluster
    fn as plain C arithmetic (NO `gte_op`, NO Beetle). This is NOT a "tradeoff vs the boundary" and NOT the
    same as the later-171 NCLIP/AVSZ revert: later-171 was a RENDER op (the engine already projects PC-native
    with real depth, so replicating PSX NCLIP was pointless mimicry → bypass, don't replicate). These cluster
    fns instead feed retained PSX CONTENT (object position), so the C math must produce values in the game's
    fixed-point format — i.e. it must match the GTE's NUMERIC result so the content reads correct data (the
    content-interface gate, CLAUDE.md). That is correctness, not GTE-hardware emulation. Verify a2 output +
    the GTE data regs the fn leaves that a still-PSX gte_op reader would consume (the comparator excludes the
    FIFO/LZCR regs neither path writes — can't round-trip a FIFO restore). **(2) CALLER ANALYSIS after**
    (FUN_80084110 ← 80084470 ← 80051980 = object rotated-displacement → world-position integration); **(3) a
    VULKAN/render PERF CHECK** (the profiler only measures interp insns, not GPU/present — needs a frame-time
    probe). End state: once the content consumers are ported too, even the fixed-point interface format goes
    away. The transitional `gte_write_data` of leftover regs is just to keep the remaining gte_op readers correct.)
- **PROFILER OVERLAY RESOLUTION (later-188b):** prof_report.py now merges the FREQUENCY call-targets into
  the TIME boundary set, so merged overlay buckets split at real function starts (`ov_<addr>(>enclosing)` =
  call-target-resolved, VERIFY with disas before porting — may occasionally be a jump-table re-entry, not a
  fn start). This corrected "FUN_8013F0DC 13.9%" → 4 distinct fns. **Run `prof_report.py <dump> --top N`.**
- **HOT-LIST after the GTE cluster + cull + tile-leaf (field, later-188b; cumulative total now 24.46M
  insns):** the SECOND ARC is the OVERLAY render. % is of sampled buckets:
  - `FUN_80115598` **39.4%** — **THE lever.** OVERLAY 2D scrolling-tilemap RENDERER (reads tile dims +16/+17,
    screen pos +40/+42 centered 160/120; builds GP0 sprite packets + OT). RENDER-boundary → reimplement as a
    PC-native 2D layer feeding the engine RenderQueue (NOT a packet/OT transcription). Register via the
    SIGNATURE scan in engine_scan_overlay (engine_submit.cpp) — the gameplay overlay loads at 0x80108F9C+
    0x459A8. Can't headless-verify render → USER eyeballs a build. Biggest single fn.
  - `FUN_8013F0DC` **4.2%** — OVERLAY per-object anim/transform STATE MACHINE (0x8013f0dc..0x8013f4d8, 21-way
    jump table @0x8010a088 → 4 handlers 0x8013f178/1b8/228/234; writes scale @0x800a3f90/94, dispatch byte
    @0x1f800207). Mechanically verifiable (predict-compare). `ov_8013F988` 4.0% + `ov_8013FA80` 1.7% are
    sibling overlay fns in the same region.
  - ~~`FUN_8007712C` 11.2% per-object CULL~~ ✅ OWNED (later-188, cull_native_body, §D). ~~`ov_8013FAE0`
    4.25% tile-lookup leaf~~ ✅ OWNED (later-188b, ov_tile_lookup, engine_submit.cpp; pure `tab[52*a1+a0]
    & (mask<<4)`, signature-registered, tileverify 0-diff 60000+ calls).
  - resident leaves still visible: `FUN_800931C0` 6.5% (font/text → render), `FUN_801401B8` 4.1% (overlay),
    `FUN_8003F698` 3.0% (26k×2 calls). ~~`FUN_80051464` 3.8%~~ ✅ OWNED native
    (later-189, ov_xform_propagate: child-node transform propagation orchestrating the owned rot/matmul/MVMVA
    primitives; verified 0-diff 6000+ via `xformverify`). ~~`FUN_800597AC` 3.6/3.8%~~ ✅ OWNED native
    (later-190, ov_orch597AC: per-object world-transform orchestrator; needed new leaves 0x80084360
    CompMatrixLV — ALSO owned (ov_compmatlv) — 0x800517bc + 0x800851f0 already owned later-189; verified
    0-diff 2800+ via `orchverify`). With the orchestrator + its deps owned, the resident transform/cull
    cluster is essentially exhausted — the remaining BIG levers (FUN_80115598 ~49% tilemap, FUN_800931C0
    ~9% font) are RENDER-boundary (REIMPLEMENT as PC-native 2D layers, USER eyeball — see §B targets below).
    ~~`FUN_80084A80` 4.4%~~ ✅ OWNED native (later-189, ov_rot84A80 RotMatrix variant; verified 0-diff 5000+).
    ~~`FUN_8007778C` 3.4%~~ ✅ OWNED native (later-189, ov_cull_wrapper: camera-relative delta + flag reset →
    rec_dispatch the owned cull body; verified 0-diff 40000+ via the `cullwrap` channel).
  - ~~`FUN_80084080` 9% (mis-attributed; real 0.5%)~~ ✅ OWNED native (later-186, ov_gte_norm) — table sqrt via
    LZCR→LUT, GTE used only as CLZ; verified 0-diff 15000+ live calls. ~~`FUN_80077FB0` isqrt~~ ✅ OWNED. See §D.
  - `FUN_80076D68` 3.6% (no GTE) = animation-sequence VM stepper (control flow + 3 callees 80075f0c/80076904/
    80075ff8; verify needs full-RAM diff + caution: untested command types = false-confidence risk).

---

# CURRENT FRONTIER (work these, in this order)
**USER REDIRECT 2026-06-20 (later-177) — supersedes the camera items below.** Port top-down boot→gameplay;
the ASSET PIPELINE must be fully PC-owned so assets reconstruct with the emulator OFF. Acceptance test:
reconstruct the sea backdrop bit-identical.
**STATUS: the texture asset pipeline is now FULLY PC-OWNED and the acceptance test PASSES (later-177).**
Owned this session: the unpack post-step (libgs DrawSync dropped from the synchronous native upload path), the
per-group LOADER ORCHESTRATION `FUN_80044F58` → `ov_load_texgroup` (header+archive CD-load, unpack, metadata,
terminal yield), and the OFFLINE reconstructor `tools/tex_reconstruct.c` that rebuilds the seaside VRAM atlas
from raw compressed bytes with NO PSX — 99.95% bit-identical to live VRAM, residual = runtime-animated CLUTs only.
**title/menu BG render: FIXED (later-178).** The title's two full-screen background sprites rendered BLACK
because later-172's linear packet-pool walk decoupled each 2D sprite from its E1 DR_TPAGE (memory order ≠
draw order); restoring OT LINK-ORDER enumeration in `gpu_dma2_linked_list` binds the texpage correctly. The
title now composites the full TOMBA!2 logo/brick/characters/menu; field unchanged. (NOT a missing-submission
or asset-pipeline bug — the assets were in VRAM all along; later-177b's root-cause was wrong, FALSIFIED.)
**Menu BG EXPORTED OFFLINE from the CHD (USER request) — `tools/menu_bg_export.cpp`.** Opens the disc,
resolves `CD/TOMBA2.IDX`/`CD/TOMBA2.IMG` by name, decodes the title set (set 2: LZ + unpack), composites the
320×224 title via 8bpp+CLUT sampling, writes a PNG — game NOT run, no OT, no runtime. Output matches
`fl_06.ppm` (sans the runtime menu-text prims). Run: `build/tools/menu_bg_export <disc.chd> out.png`.
**REMAINING under this redirect:** the on-screen 288×576 backdrop COMPOSITION (engine-owned 2D layer/sort,
ties into render-ownership, NOT the asset pipeline). (Camera sub-fns below are DEFERRED behind this.)


1. **Camera update system — DONE (later-180).** All `FUN_8006e0f0` sub-fns owned PC-native in
   `engine/engine_camera.cpp`: 0x8006c80c Y-floor, 0x8006d654 pitch, 0x8006dcf4 heading, 0x8006d02c
   orient/look-at matrix (rec_dispatches libgte/GTE as the math library per the RENDER boundary), 0x8006e010
   angle-step, plus the earlier 0x8006e464 rotbuild / 0x8006d2ac dist-solve / 0x8006d960/da54 track xz·y. The
   three per-mode ORCHESTRATORS `FUN_8006e0f0` (active follow) / `FUN_8006e228` / `FUN_8006e3f4` are collapsed
   native too. camverify is now an end-to-end gate (native-everything vs a PURE-gen oracle, sub-fn overrides
   cleared around the orchestrator rec_interp): e0f0 0-diff over 1000+ calls (AUTO_SKIP=500 AUTO_WALK=r);
   e228/e3f4 are latent alternate modes, faithfully transcribed, verify when a scene drives them. Two e228
   sub-fns (`FUN_8006dad8`/`FUN_8006def0`, a0-only) still route via rec_dispatch — own them if/when e228 is exercised.
2. **DEMO / front-end MENU stage `0x801062E4`** — the big un-owned system in execution order between boot and
   gameplay. Title→New Game. Own its substate machine PC-native. **FULLY RE'd (later-181) — see docs/engine_re.md
   "DEMO / front-end MENU stage" section**: root dispatcher + per-frame loop + the 8 substate handlers
   (s0..s7), their inner menu sub-machines, sm field map, transitions, and callees are all mapped.
   **PARTIAL — OWNED (later-182): substates s1/s2/s3/s6 owned PC-native in `engine/engine_demo.cpp`** (the
   ones whose only sub-call is SYNCHRONOUS — verified yield-free via the new `tools/yield_reach.py`). They
   rec_dispatch the inner machine, then own the transition LOGIC (sm[0x48] selection + field writes) natively
   and coro-redirect to the guest TAIL (like ov_game_s4c). Registered AUTO from the overlay-load scan
   (`demo_scan_overlay`, distinguishes DEMO from the GAME.BIN that aliases the same base via the root prologue
   sig 0x27bdffd0). **A/B gate PASSED**: override-on vs override-off at a steady menu frame (REPL `run 150`)
   = main-RAM 0-diff and **scratchpad 0-diff** except ONE word — task-0's saved-ra slot 0x801FE9CC
   (CORO_SENTINEL vs the guest return-PC), the inherent coro-redirect artifact, not behavioral state.
   `dumpram` now also dumps a `.spad` sidecar so the A/B covers scratchpad (was blind before).
   **s0 NOW OWNED (later-185)** via the coro-redirect-INTO-the-yielder handshake: it has genuine pre-yield
   engine state (sm[0x68]=0, sm[0x48]++, sm[0x4a]=0) owned native, then redirects to its first loader jal
   0x801063E4 (yields) so the loaders + fall-through to s1 run in-context. A/B (run 150) main-RAM + scratchpad
   **0-diff, no saved-ra artifact**. **s4/s5 STAY GUEST (final):** verified (later-185) the override table is
   consulted ONLY on jal/j/jalr/computed-jr targets, NOT on `jr ra` returns — so s4's only logic (the sm[0x6b]
   branch post-yield at 0x8010658C, reached by jr ra) is unreachable by an override; s5 is one stage-transition
   call. (The earlier "post-yield override" idea was retracted as not implementable.) **NEXT (this item): s7 —
   OWNABLE.** Its `jal 0x80106C24` is an override-checked jal target and 0x80106C24's phase-selection prologue
   (sm[0x4a]) is pre-yield + phase2 teardown is all-SYNC; own selection + phase2, redirect yielding phase0/1.
   Needs reaching s7 (confirm a menu option) to A/B-verify. Overlay disasm via
   `tools/disas.py <addr> --ram scratch/bin/tomba2/ram_menu.bin`.
3. **Init-prefix remainder:** `FUN_800520e0` engine subsystem init — ORCHESTRATION OWNED (later-183,
   eng_init_subsystems, boot A/B 0-diff); NEXT = descend into its 4 callees (8007b328/80088b00/80086620/87a60).
   `FUN_80075130` font/text init — ✅ DONE (ov_font_init, engine_font.cpp; 3 engine callees + memsets owned,
   libgpu callees kept dispatched in-context; boot run-2 0-diff vs initref.bin). See §A.

# OPEN / BLOCKED (not on the critical path)
- **ov_game_s4c verification** needs an in-game AREA transition (sm[0x4a]==2). Free-roam IS reachable headless
  (AUTO_SKIP=500 + AUTO_WALK) but the seaside exit isn't a plain walk/jump (hut door blocked by a barrel) —
  needs visual steering or RE of the exit trigger in GAME.BIN handler `0x801088d8`. (NB: there is NO pause menu
  — the reliable `PSXPORT_DEBUG=state` task-slot probe confirms 0 menu tasks; an earlier "pause menu" claim was
  a broken probe.) Controls (which button = jump) are not yet identified — leave it; it reveals itself in play.
