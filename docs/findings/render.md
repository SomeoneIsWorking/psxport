# Findings — render / engine submit

## perobj_billboard cluster (C2D4/C464/C8F4) — BUF base wrong + register-faithfulness gap (2026-07-09, OPEN)

- **how found**: the oracle-gate fix (commit 5483a83, `engine_override_thunk`) made SBS honest for the
  `g_override[]` render clusters. Post-fix `PSXPORT_SBS_MODE=full` immediately surfaced 19 `[sbs-div]`
  at f117 in the packet pool (0x800BFFxx) + scratchpad — exactly the writes the false 0-div had hidden.
- **bisected** with `PSXPORT_THUNK_FORCE_GEN` (force a cluster to gen even on core A): disabling the
  billboard leaves cleared f117 → billboard; `C2D4`-only force-gen left packet data divergent →
  `billboardEmit` (C8F4, reached direct-C++ on core A so the thunk hit-counter showed 0 native).
- **bug 1 — RESOLVED (commit a457082)**: the whole cluster wrote its MATRIX-compose + projected-coord
  buffer to MAIN RAM `0x800C0000`, but `gen_func_8003C2D4`/`8003C8F4` base `r16/r17 = 8064<<16 =
  0x1F800000` (SCRATCHPAD). Emitted packets (copied BUF+4..+36) therefore differed from the substrate.
  Single-constant fix: `BUF = 0x1F800000`. Packet-pool divergence eliminated.
- **bug 2 — OPEN (register-faithfulness, same family as frameTick / NodeXform)**: with bug 1 fixed, f117
  now diverges in the GUEST STACK at 0x801FE8D8 (signature A=80 1F / B=00 00). `gen_func_8003C8F4`
  spills the CALLER's `r16..r22/ra` at `sp+64..+92` (its own prologue); native `billboardEmit`'s
  `GuestFrame(96)` only allocates the frame, doesn't spill. The spilled "caller r16" further differs
  because the caller `billboardCompose1` (C2D4) in gen reassigns `r16=MAT_OUT`/`r17=MAT_A`/`r18=flag`/
  `r19=node` but native C2D4 uses C++ locals and never writes `c->r[16..]`. Callees of C8F4
  (`func_8003B220`, `func_8003B054`) are leaves that spill NO callee-saved regs, so the fix is purely
  the prologue spill bytes + the C2D4/C464 mid-body register assignments — the frame-mirror treatment
  per docs/findings/animation.md (NodeXform's named frame structs). Two layers: (1) C8F4 spills caller
  r16..ra at sp+64..92; (2) C2D4/C464 set c->r[16..] to gen's reassigned values so C8F4's "caller r16"
  matches. More f117+ divergences likely stack behind this (other render clusters: quad_rtpt,
  overlay_gt3gt4, overlay_ground_gt3gt4) — chase them the same way once billboard is clean.
- **refs**: game/render/perobj_billboard.cpp (BUF constant; `Render::billboardCompose1/2/billboardEmit`;
  `billboardComposeTail`); oracle `generated/shard_4.c gen_func_8003C8F4` (L4365) + `generated/shard_0.c
  gen_func_8003C2D4` (L4507); commits 5483a83 (oracle gate), a457082 (BUF fix).

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
