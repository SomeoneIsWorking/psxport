# Findings — render / engine submit

## ires (internal resolution) 2D/HUD blur (bug #55) — band split + coverage gate (2026-07-15, PARTIAL FIX)

- **symptom (USER):** at ires>1 the whole picture, including HUD/menu TEXT that should be pixel-identical
  regardless of the 3D scale, looks visibly blurrier — not the expected "sharper 3D edges, unchanged 2D".
- **root cause #1 (2D shares the 3D-scaled target):** `RQ_OM_2D_BG`/`RQ_OM_2D_FG` content (HUD, menus,
  dialog/fade panels) was batched into the SAME tri/tex/semi vertex buffers as the 3D world (gpu_gpu.cpp
  `render_geom`, pre-fix), so at ires>1 it rasterized into the ires-scaled target and suffered the seed-
  blit's LINEAR-upsample (lossy for sharp pixel art/text) + box-filter downsample round trip, even though
  2D has nothing to do with the ires scale. Confirmed with pixel evidence: a pause-menu capture (Options/
  Load data/Quit game) diffed non-zero between ires=1 and ires=4 despite unchanging content.
- **fix #1:** 2D content batched SEPARATELY per band (`GpuGpuState::s_tri2d_buf/s_tex2d_buf/s_semi2d_buf`,
  indexed `GGS_2D_BG`/`GGS_2D_FG`) and rendered in 3 ordered passes: 2D_BG native-res → 3D world (scaled +
  composited back, unchanged) → 2D_FG native-res, AFTER the 3D composite-back so it's NEVER touched by the
  round trip. **Provably pixel-exact for RQ_OM_2D_FG** (nothing runs after it) — verified via a HUD-region
  crop pixel-diff and a `PSXPORT_ONLYWORLD=1`/pure-2D-frame check.
- **root cause #2 (composite-back has no coverage test):** even with the band split, `RQ_OM_2D_BG` content
  is drawn BEFORE the 3D world, so band 2's composite-back — which unconditionally overwrote the WHOLE
  display sub-rect — still clobbered band 1's own pixels wherever this frame's 3D geometry didn't
  rasterize a fragment there. The ORIGINAL single-pass code got per-pixel occlusion for free from one
  shared real depth test; splitting into sequential passes across different render targets lost it.
- **fix #2 (narrowed, not closed):** `ires_downsample.frag` now samples the ires depth target per
  destination sub-texel and, where no OPAQUE 3D fragment landed, substitutes a native-res snapshot of
  `s_vram_tex` taken right after band 1 (`GpuGpuState::s_ires_bg_snap`) instead of the lossy upsampled
  seed. A second depth-only pass (`semi_cover.frag`/`s_semi_cover_pipe`) re-rasterizes the semi buckets so
  TRANSLUCENT 3D coverage also registers (Pass B itself never writes depth by design).
- **PARTIAL / open — the verification pause-menu capture itself is NOT fully fixed:** its text is
  classified `RQ_OM_2D_BG` (node_is_bg/sprite_is_bg_texpage provenance, not `RQ_OM_2D_FG` as first
  assumed), and the "ghosted" 3D world visible behind the paused menu still measurably blurs it at ires>1
  even with BOTH coverage gates active. `PSXPORT_DEBUG=ires` depth-visualization (temporarily rendering
  `u_depth` as grayscale instead of compositing) showed near-zero coverage across most of that region from
  BOTH the opaque and semi-stamp tests, yet the real (correct, ires=1) picture is unmistakably drawn there
  — meaning some draw path is painting that content without the depth signal my coverage gates rely on.
  NOT root-caused within this change's scope; needs further RE into which draw path paints it (the
  `tri`/`tex`/`semi` counts logged for the frame don't obviously account for the full picture) before the
  coverage gate can close it fully. Do not re-attempt the SAME depth-based coverage angle without new
  evidence — it was tried twice (opaque-only, then opaque+semi) and both left this specific scene
  unchanged.
- **verified clean:** ires=1 byte-identical to pre-fix baseline (`md5sum` match); 3D edges visibly
  smoother/AA'd (not blurrier) at ires=4 (character silhouette + grass crop); SBS-full 0-diff (host-only
  render change, `PSXPORT_SBS_AUTONAV=combat`, f18300+, 190s bound); fps60=1 + ires=4 renders clean, no
  crash/corruption.

## Coplanar z-FIGHTING on barrel/decoration surfaces (black chunks flicker) — paint-order depth tiebreak (2026-07-10, PARTIAL FIX)

- **symptom (USER):** on the waterpump/seesaw barrel a black chunk flickers through the red basket top
  (`scratch/screenshots/reports/zfight_pump.png`), and barrel top/bottom surfaces generally shimmer;
  same expected on indoor wall decorations. A dark interior/back face wins the depth test over the red
  top face in a large patch, and the patch changes as the barrel/camera moves.
- **mechanism (root cause, first-principles — NOT quantization we can "fix"):** the barrel is INTEGER
  model geometry projected through the GTE fixed-point pipeline, so per-vertex view-Z (`proj_native_xform`
  → `tmp2_unshifted/4096`, gte_beetle.cpp) has 1/4096 granularity — the finest this pipeline offers (an
  earlier fix already moved off the even-coarser integer SZ; comment there flags "#5 barrel"). For a small
  near object the real per-face view-Z spread is ≤ 1/4096, so near-coplanar detail faces/quads collapse
  onto a FEW discrete depth buckets (measured: three values 0.096767/0.096782/0.096798, ~1.5e-5 apart in
  ord, overlapping). The D32 `GREATER_OR_EQUAL` test then orders them by these unstable buckets — which (a)
  can disagree with the paint order and (b) flips as sub-pixel motion nudges the depths across a bucket
  boundary → the black chunk pops. There is NO finer depth to recover: the surfaces are genuinely coplanar
  at the geometry's native resolution. **PSX has no depth buffer — it resolves these purely by OT/paint
  order (last drawn wins uniformly), which is stable.** So paint order is the only correct disambiguator.
- **fix (host-render-only; game/, runtime/recomp/gpu_gpu.cpp):** a minimal deterministic PAINT-ORDER depth
  bias. For 3D-band prims the per-vertex depth = `ord3d(depth) + emit_order * ZBIAS_UNIT` (clamped to the
  3D band, cap `ZBIAS_MAX=1.5e-3`), so on a (near-)equal-depth tie the LATER-emitted prim wins `>=`
  uniformly and motion-invariantly — reproducing PSX's paint-order resolution. `ZBIAS_UNIT` default 4e-7
  (tunable `PSXPORT_ZBIAS`); span ≈ 1e-3 at ~2500 prims, an order of magnitude below measured genuine
  world depth separations (~4.5e-3), so real occlusion is UNCHANGED (verified: world renders identically
  except the coplanar-contest pixels).
- **why this does NOT violate the no-OT-inheritance rule:** the engine still owns PRIMARY ordering (real
  per-pixel depth decides all genuinely-separated geometry). The paint/emit order is used ONLY as a
  sub-ULP TIEBREAK between prims the depth buffer cannot distinguish (genuinely coplanar at geometry
  resolution) — the one case where PSX's own answer is paint order. It is a bounded epsilon on the depth
  we own, not a read of the guest OT/draw-order/GP0.
- **verification:** host-only → SBS-full **0-diff BOTH legs** (combat AUTONAV + WATCH_CUT, f11640/f12400,
  0 divergence — the bias never touches guest RAM). New auto-finder `PSXPORT_ZFIGHT[=eps]` (render_queue.cpp
  `zfightScan`) SW-rasterizes opaque depth prims into a per-pixel top-2 buffer and reports contests +
  paint-order stability + a heatmap; it measures the fix resolving **~87% of true ties** (gap<1e-5) to
  deterministic paint order at the safe U=4e-7 (incl. the barrel-detail contests, Δemit~140), up from ~70%
  raw. Evidence: `scratch/screenshots/zfight/` (heatmaps, before/after crops, stability logs
  `scratch/logs/stability2.log`).
- **PARTIAL / open:** a GLOBAL paint-order bias cannot resolve 100% of ties at a world-safe magnitude —
  per-step × prim-count = span, so raising the per-step enough to beat the largest tie gaps (needs U≈4e-6)
  blows the span past genuine world separations. ~13% of ties (small-Δemit adjacent-mesh SEAMS + contests
  near the eps boundary) remain depth-resolved. **Full elimination needs the architectural fix:** route
  intra-object detail prims to ONE shared object depth (like `obj_depth_lookup` billboards) so they are
  EXACT ties → paint-order-resolved with no bias and no span budget. Deferred (needs object grouping for
  the guest-OT-walk detail polys, which arrive with per-vertex projprim depth + dbg_node=0).
- **refs:** `runtime/recomp/gpu_gpu.cpp` (`gpu_zbias_unit`/`set_order` bias, `ord3d_b`, draw_tri/tex_emit),
  `runtime/recomp/gpu_gpu_internal.h` (`s_depth_bias`), `game/render/render_queue.cpp` (`zfightScan` diag),
  `runtime/recomp/gte_beetle.cpp:218` (the 1/4096 depth source). Config: `PSXPORT_ZBIAS`, `PSXPORT_ZFIGHT`.
## Weapon "ball but no CHAIN" (#28 follow-up) — chain-LINK billboards already render; only a minor grab/dust semi-quad is dropped (2026-07-10, could-not-reproduce a significant chain)

- **symptom (task/USER):** under the default config (pc_render), Tomba's weapon mid-swing shows the BALL
  but not the CHAIN. Ball renders since f6aebd4 (the field's 2D-only OT pass keeps obj-depth-tagged
  billboard prims — `runtime/recomp/gpu_native.cpp` `billboard` keep in the poly path + `objz` keep in the
  sprite path). Premise: the chain segments are still dropped.
- **repro used:** REPL nav to the first field (pipe 8×{run 12/tap x}, 25×{run 40/tap start}, 15×{run 20/tap
  x}), then `tap sq 8`; shots every 2 frames. This drives Tomba's **basic square attack (the crouch-grab)**,
  matching the earlier session's `scratch/screenshots/vischeck/atkcmp_atk*.png`.
- **prim anatomy at the swing (all coords display 320×240, pinned via `PSXPORT_PRIMAT="x,y,frame"`):** the
  weapon is built into the **0x800C0000-region per-instance GPU packet buffer** (NOT the main pool
  0x800BF544; see the "0x800C0000-0x800C8FFF is the packet pool" finding below). Two distinct prim families
  cover the wrist/ball:
  1. **0x800C0xxx** — small textured tris/quads (GP0 op **0x34/0x3c**), tp=(448,256). These are the ball
     **and the chain LINKS**. **Every one is `is3d=1 bb=1`** (obj-depth-tagged via the billboardEmit family
     `perObjRenderDispatch`/`billboardCompose1/2`/`billboardEmit` = 0x8003C2D4/C464/C8F4) → KEPT and drawn.
     Verified: a full `PRIMAT` scan at the swing found **zero** 0x800C0xxx prims with `bb=0`.
  2. **0x800C7xxx** — large **semi-transparent** textured quads (GP0 op **0x2E**), tp=(320,256), covering
     Tomba's body/hands. `is3d=0 bb=0` → DROPPED by the 2D-only filter. This is a minor grab/dust/shadow
     smear, NOT the chain (see below).
- **builder of the dropped 0x800C7xxx quads (RE'd via a write-watch backtrace + `generated/` caller trace):**
  the packet writer is `gen_func_80027A4C`, called from `gen_func_80027E5C` (a0=node sprite/quad builder),
  reached through the master render-walk's DEFAULT case (`0x8003C29C`, `rec_dispatch(node+24)`) — the object's
  own `node+24` render fn (contains 0x8003DF80). A persistent object (node 0x800FB218) also reaches it via
  `gen_func_80033080`. `QuadRtptSubmit::submitQuad` (0x8003B320, the rope/flame RTPT quad path) is NOT
  involved here (0 calls during the swing).
- **DEAD END — wrapping the builder in RenderObserver does not visibly help:** added `gen_func_80027E5C`
  (and separately 0x80033080) to `game/render/render_observer.cpp`'s obs-wrap set so a `PktSpanSession`
  tags its packets with `obj_world_ord(node)`. Result: the 0x800C7xxx quads become `bb=1` (kept) but render
  **fully occluded — 0 pixel change** (the object's world depth places them behind Tomba's opaque body).
  Routing them to `RQ_HUD` instead (drawn on top, painter-style) makes them visible but **overshoots**:
  same-exec (`PSXPORT_GATE=1`) pc-vs-psx weapon-region diff goes 1540px → 1581px (WORSE, not better). Their
  total contribution is ~40px — negligible. Reverted; NOT shipped (tagging the wrong thing / adds a hot-fn
  wrap + SBS risk for no visible gain = bandaid).
- **conclusion:** in the current build (HEAD w/ f6aebd4) the discrete weapon **chain-link billboards
  already render** — they are the same 0x800C0xxx billboard class as the ball, which f6aebd4's keep already
  covers. A same-exec `GATE` pc-vs-psx compare shows the ball+wrist weapon present in BOTH; the residual
  ~1540px weapon-region difference is dominated by pc_render's global **lighting/shading style** (deferred,
  expected), not a dropped chain prim. **Could not reproduce a significantly-missing chain with this repro.**
- **most likely gap (for the next session / operator):** the task's repro drives Tomba's **basic grab**, not
  a distinct equipped **ball-and-chain WEAPON**. If the user's "chain" is a purchasable/equipped weapon, a
  fresh headless newgame can't reach it — the operator should confirm the exact weapon/scenario (or provide a
  save-state at the equipped step), then re-run this PRIMAT/`bb`-flag check on that weapon's prims.
- **refs:** `runtime/recomp/gpu_native.cpp` (2D-only keep rules L881/L1058, `obj_depth_lookup`/`_add`),
  `game/render/render_observer.cpp` (obs-wrap set), `game/render/submit.cpp` (PktSpanSession rationale),
  `generated/shard_6.c` gen_func_80027E5C/80033080; evidence shots `scratch/screenshots/chain/` (G_pc_*/
  G_psx_* same-exec pairs, wrist_*, csg_*). Diagnostics used (all reverted): PSXPORT_DEBUG chaintrace/
  chainshow/poolwatch/cw7/odlk/obswrap.

## Fisherman's-hut interior "much different than oracle" (2026-07-10, OPEN — repro UNBLOCKED 2026-07-14)

- **symptom (USER):** entering the fisherman's hut (fish-painted door, first field) shows something much
  different under the default config (`./run.sh`, pc_skip + pc_render) vs the oracle. Given entry position
  X=4002 Y=-1372 Z=2352, entry = `press up` ("Use ↑ to jump to" prompt).
- **status:** the interior divergence could **NOT be reproduced headless** this session, for two concrete
  reasons — recorded so the next session doesn't re-walk them:
  1. **The hut EXTERIOR renders byte-identically** default vs oracle (evidence:
     `scratch/screenshots/hut/cmp_grid.png` — 3 matched points, pixel-identical). So the divergence is
     inside/after entry, not the field render at that spot.
  2. ~~quest-gated~~ **FALSIFIED (USER 2026-07-14): the hut interior IS reachable in a fresh game — a pad
     recording enters it** (`replays/scene-transitions/` hut-entry captures; play back with
     `PSXPORT_PAD_REPLAY=<file>` under SBS to verify). The earlier 28-spot sweep's 0 area-id writes only
     proved the sweep's spots/inputs never triggered the door, not that a gate exists. Use the replay as
     the repro.
- **why this matters / render-arch hypothesis (unverified):** if the interior is a separate area, its field
  code runs a per-mode renderer selected by the area's mode byte `0x800BF870` through the 22-entry table at
  `0x80015268` (`game/render/perobj_dispatch.cpp`). Several table entries are per-scene OVERLAY submitter
  variants (`0x8013xxxx`) that pc_render does not rebuild — same class as the fixed billboard drop. That
  would explain "much different" (pc_render draws nothing / wrong for the interior's mode). **Cannot confirm
  without reaching the interior.**
- **the unblock path = clean cross-area warp** (Part 2, `docs/engine_re.md` "DOOR RECORD"): warp directly
  into the hut interior area to A/B it. Blocked in turn by the A0X MODE-overlay residency gap (below /
  engine_re.md) — a future session with that gap closed, or a save-state at the enterable quest step, can
  reproduce and root-cause. Filed as a GitHub issue.
- **refs:** `scratch/screenshots/hut/` (cmp_grid, nudge_grid, back_grid, cmd_w0), `game/render/
  perobj_dispatch.cpp`, `docs/engine_re.md` "Area WARP / DOOR RECORD".
- **UPDATE (2026-07-11): USER confirmed this is a TRANSITION issue, not a render issue** — "PC still not
  showing hut interiors, it still shows the outside area when in hut." The area transition either doesn't
  fire or fires but the destination area's A0X overlay never loads (the MODE slot stays at A00). Root cause
  identified in `fieldRun` case 6 (trig==3): it enters submode1 at `sm[0x4c]==1`, SKIPPING case 0 (the
  overlay-load state). A first fix attempt (loading FUN_80045080 in case 6) was REVERTED — it caused a
  massive f154 regression because it fired during normal gameplay, not just cross-area transitions. The fix
  needs proper gating (only fire when destArea != currentArea during an actual cross-area transition).

## fps60 redesigned as TRUE per-object interpolation (2026-07-10, RESOLVED)

- **symptom (USER)**: the 60fps tier "looks like a hack, especially how the billboards move; many things
  are jittery." The old `game/render/fps60.cpp` was a post-hoc SCREEN-SPACE layer: it snapshotted the
  resolved `RqItem` render-queue prims, matched them across frames by a material fingerprint, reprojected
  mesh SCREEN verts at a crude packed-CR midpoint, screen-translated billboards by an anchor delta, and
  per-tile-shifted backdrop tiles. Billboards translated in screen space (wrong perspective); terrain +
  backdrop juddered because they SNAPPED to frame B while meshes interpolated.
- **fix — interpolate at the OBJECT level, render the in-between through the REAL native pipeline**:
  - Each logic frame the native scene render CAPTURES, host-side, every object's WORLD transform (rotation
    matrix + position, keyed by render-command ptr `cmd`) and the scene CAMERA (view R/T + OFX/OFY/H), plus
    the backdrop scroll and each billboard's WORLD anchor (node+46/50/54). All guest READS.
  - The mid-present RE-RUNS `Render::sceneNative()` (read-only, `DisplayPassGuard`) with a MIDPOINT provider
    armed (`Fps60::mInterp`): the camera choke `Fps60::sceneCam` (routed through `projComposeCore` /
    `projComposeCamera` / `native_terrain`), the object choke `Fps60::objXform` (in `projComposeObject`),
    and `Fps60::bgScroll` (backdrop) all return the `(prev,cur)` t=0.5 lerp instead of the raw guest value.
    Terrain, scene-table, meshes AND backdrop therefore pan through the SAME interpolated camera the real
    projection uses — no screen-space reproject, no per-layer judder.
  - Billboards (guest OT 2D quads, not produced by sceneNative) are re-emitted from the captured queue and
    re-anchored by projecting their WORLD anchor through the real projection at the interpolated world
    position + interpolated camera (`projWorld` = `view = Rcam·w + Tcam`, same math as `native_terrain`).
  - `RqItem` tagged `fps_scene` (armed around `sceneNative()` in `drawOTag`): the mid-present rebuilds the
    `fps_scene=1` prims fresh and re-emits the `fps_scene=0` (2D/HUD/billboard) prims.
  - Retired: the whole screen-space matcher (`build_lerp`, `fps60_reproject{,_anchor}`, `fps60_compose_mid`,
    the SXY→obj grid, `XObj` capture, `stampWorld{,Cr}`, the backdrop median, the `fps_cr`/`fps_mv`/
    `fps_world`/`current_object`/`fps_cur_key` members). Kept: the logic-rate detector.
- **billboard registry timing bug (also RESOLVED)**: `Fps60::mBbCur` (the OT-node→entity span map the OT
  walk uses to re-anchor billboards) was reset lazily at `RenderQueue::push()`'s first per-frame push —
  which is inside `drawOTag`, AFTER `fieldFrame`'s substrate render already recorded the spans (in
  `pcSched.step`). The reset wiped that frame's billboards before the OT walk could stamp them, so
  `bb moved=0` (billboards never re-anchored). Moved the reset to the top of `Engine::frameUpdate` (the
  true frame boundary, before `fieldFrame` records) → `bb moved≈570/frame`.
- **verification**: build OK; `PSXPORT_SETTINGS=…fps60=1` field free-roam →
  `tools/preseq_flicker.py scratch/screenshots/preseq_new` PASS (0/4 bands, alt-frac=0.00 — no 30Hz
  oscillation); consecutive frames show the purple gem + black item-ball billboards translating smoothly.
  SBS-full both legs 0-diff (fps60 off → the camera/object choke reads are byte-identical to the old inline
  reads; every capture is a host-only guest read). `bb moved=574 kept=115 snapped=0`, objs=438/frame.
- **DEAD END avoided**: NOT "write interpolated transforms into guest objects and restore" (violates the
  READ-ONLY OVERLAY invariant + races SBS). The provider is a host-side override consulted by the native
  projection during the interp pass; guest RAM is never written (DisplayPassGuard-gated).
- **note**: `proj_native_xform_cr` (runtime/recomp/gte_beetle.cpp) is now dead (was the old CR-midpoint
  reprojector) — left in place, harmless; remove on a future gte_beetle pass.
- **refs**: game/render/fps60.{h,cpp}, projection.cpp, native_terrain.cpp, render_walk.cpp,
  render_queue.{h,cpp}, submit.cpp, game_tomba2.cpp (drawOTag mSceneTag + frameUpdate bbFrameReset).

## fps60 billboard anchor must be PER-PARTICLE, not per-manager-node (2026-07-10, gems RESOLVED)

- **symptom (USER)**: with the redesigned fps60 tier, 2D GEMS (and the objective banner) "still react
  poorly at 60fps — definitely not done per objects." The per-object redesign re-anchors obj-depth-tagged
  billboards through the real projection, but a whole CLASS of billboards still snapped/jittered.
- **root cause**: what the fps60 registry treated as "one billboard object" (fps_key = the entity NODE) is
  actually a MANAGER node whose particle sub-list holds MANY visible sprites (all the score-gems / effect
  quads of that class). `Fps60::recordBillboardSpan`/`stampBillboard` keyed the anchor by node and used the
  node's single world position (node+46/50/54), so EVERY sprite of a manager shared ONE anchor. In the
  mid-present all those sprites got the SAME rigid screen-translation — losing each particle's own animated
  bob. Each particle carries its OWN planar offset at `particle+14/+15` (s8), which `func_8003B220` (shard_4,
  the quad-corner builder) scales ×5 (`x<<2 + x`) and builds the quad corners around; that offset is
  animated per frame by the gem/effect behaviour, so a shared node anchor + rigid translate is exactly wrong.
- **fix**: `billboardEmit` (perobj_billboard.cpp) now records ONE anchor PER PARTICLE it emits, keyed by the
  particle's guest ADDRESS (stable while the sprite lives). The anchor is the manager node's world position
  + the node rotation (`MAT_OUT` rows, /4096) applied to the particle's own 5×(p[14],p[15],0) offset — the
  SAME 5×offset the corner builder uses — so each sprite's anchor moves with its own animation. New registry
  `Fps60::mBbPart[]` (recordBillboardParticle); `billboardForNode` searches it BEFORE the node-level spans,
  so a gem sprite's OT packet resolves to its particle, not its manager. Node-level `mBbCur` stays as the
  fallback for single-sprite billboards. Host-only (guest READ, host WRITE) — no guest-state change.
- **evidence**: `tools/preseqobj_check.py` per-object gate — the new `preseqobj` channel (see below /
  docs/config.md) logs each emitted RqItem per present; the tracker groups by object identity and flags
  oscillation / stall-step. Post-fix, billboardEmit particles carry per-particle keys (new addresses in the
  particle-data band, e.g. 0x801Cxxxx/0x80158xxx) and track smoothly; field capture PASSes (0 flagged over a
  24-present walk, `scratch/logs/cap6.log`). SBS-full 0-diff BOTH legs (combat→f9330, watch_cut→f15120):
  the anchor recording is gated off in SBS (fps60 is off; env-driven) and is a pure host read regardless.
- **DEAD END / still OPEN**: the *stationary field PICKUP gem* I could see renders via a different path (a
  layer-0 sprite + layer-1 key=0 sparkles), NOT billboardEmit — this fix does not cover it. The live field
  also carries ~775/present guest-OT projected WORLD polys (key=0 layer=1 scene=0) that are re-emitted
  unchanged on the interp present (they are world geometry, not discrete objects; the tracker skips scene=1
  rebuilt mesh but these are scene=0). The objective BANNER could not be triggered in a fresh headless
  newgame to characterise it. Extending per-object anchoring to those paths is follow-up work.
- **refs**: game/render/perobj_billboard.cpp (billboardEmit per-particle record), game/render/fps60.{h,cpp}
  (mBbPart / recordBillboardParticle / billboardForNode), game/render/render_queue.cpp (preseqobj emit log),
  runtime/recomp/gpu_gpu.{h,cpp} (gpu_gpu_preseq_present_index), tools/preseqobj_check.py, generated/shard_4.c
  gen_func_8003B220.

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

## overlay_ground_gt3gt4 cluster (8013FB88/8013FE58/801401B8) — depth >>2 + range gate (2026-07-09, packet-pool RESOLVED; f118/f62 register-faithfulness RESOLVED 2026-07-10; f179 task-0-stack mode==2 mirror-offset RESOLVED 2026-07-10 — see bottom)

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
- **re-verify 2026-07-14 (full-narration path, scene-checklist row 5)**: fix holds — no garbage. On a
  full `newgame` playthrough (narration NOT auto-skipped; beat-reset→scene-flip gap at f1124-1128
  default / f1136-1142 GATE) BOTH default and the oracle hold the frozen last-narration frame through
  the 2-6 frame gap and roll straight into the fisherman scene — no black-hold and no "Loading....."
  card appears on THIS path in either config (the card in the original capture belongs to the
  AUTO_SKIP/debug-server window timing above). Default matches oracle ⇒ no bug on this path.
  Shots: scratch/screenshots/row5_{default,gate}_f11*.png.

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

## f179 task-0-stack `0x801FE924` — `OverlayGroundGt3Gt4::gt3/gt4` mode==2 SZ-mirror used the WRONG stack offset (2026-07-10, RESOLVED)

- **symptom:** SBS-full frontier moved to f179 after the packet-pool write-order fix (commit 79f50a5)
  closed f158: `[sbs-div] f179 [?] 0x801FE924..0x801FE928 (4 B) A=00 00 80 1F B=BC 13 00 00`, recurring
  every ~3 frames at the same address. Core A always held `0x1F800000` (a scratchpad-base pointer
  constant, LE); core B held a small slowly-incrementing counter.
- **root cause:** `OverlayGroundGt3Gt4::gt3`/`gt4` (`game/render/overlay_ground_gt3gt4.cpp`) each have a
  `mode == 1u || mode == 2u` branch that mirrors the 3 (gt3) / 4 (gt4) `gte_read_data` SZ values onto the
  function's OWN real guest-stack frame before calling `sz3_minmax`/`sz4_minmax`. Ground truth
  (`ov_a00_gen_8013FB88`/`ov_a00_gen_8013FE58`) is the ORIGINAL compiler having inlined the SAME
  sz-minmax computation TWICE at compile time, once per `mode` value, each copy writing its own dead
  scratch to a DIFFERENT stack offset: `mode==1` writes to (new-sp)+0/4/8[/12]; `mode==2` writes to
  (new-sp)+12/16/20 (gt3, 3 words) or +16/20/24/28 (gt4, 4 words) — NOT the same offsets. A prior draft
  of the native port always used the `mode==1` offsets regardless of which mode fired, so `mode==2`
  records never touched (new-sp)+12..+23 (gt3) — exactly `0x801FE924` when a `mode==2` ground quad
  reuses this call's guest frame (`sp=0x801FE910`, offset +0x14=+20). Once a LATER, unrelated function
  reused that same shared task-0 stack address later the same frame (`ActorTomba::matrixComposeAttached`,
  guest `FUN_800597AC`, spilling the scratchpad-base constant `0x1F800000`), core A's version — which
  never overwrote that byte via the mode==2 mirror — kept the STALE `0x1F800000` at frame end where core
  B's real mode==2 mirror write left its own live counter. This is the same class of bug CLAUDE.md's
  "MIRROR THE GUEST STACK" rule targets ("never exclude a slot because it looks like dead scratch") —
  even though this specific stack region is never READ by the game (it is genuinely dead-but-real
  scratch from a compiler inlining artifact), a LATER, unrelated function's spill onto the SAME reused
  stack address makes the two engines' end-of-frame bytes diverge unless the dead write happens at the
  gen-faithful offset too.
- **method:** `PSXPORT_SBS_PREWATCH=0x801FE924` (raw per-store log, no `WW_ONVALUEDIVERGE`) captured
  BOTH cores' full write history to the address across f0..f179; the LAST writer differed — core A:
  `pc=800597AC ra=8003B6A4` (matrix-compose, unowned substrate on both sides — same code, ran
  identically); core B: `pc=8013FB88 ra=80140278` (gt3's own frame, ONE more write after the same
  matrix-compose call that core A also made). Cross-referencing `generated/ov_a00_shard_0.c
  ov_a00_gen_8013FB88` / `ov_a00_shard_1.c ov_a00_gen_8013FE58` instruction-by-instruction found the
  duplicated-inline mode==1/mode==2 stack-offset split; the native port's `sz3_minmax`/`sz4_minmax` call
  sites used a single fixed offset for both modes. `ActorTomba::matrixComposeAttached`/`Render::
  perObjRenderDispatch`/`overlayTypeDispatch`/`entityLoop` were all re-verified as correctly
  register-faithful and NOT the cause (their own call counts and register state matched B exactly) —
  the bug was purely a dead-scratch offset choice one level below, inside gt3/gt4 themselves.
- **fix:** branch the guest-stack mirror write's BASE offset on `mode` (mirroring gen's real per-mode
  offset) while keeping the `sz3_minmax`/`sz4_minmax` VALUE computation unchanged — `game/render/
  overlay_ground_gt3gt4.cpp` `OverlayGroundGt3Gt4::gt3`/`gt4`.
- **verified:** `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_NOPAUSE=1` gate, 95s window
  (autonav headless): zero `sbs-div`/`VIOLATION` hits; run stays byte-exact through f9210 (the prior
  frontier was f179), SIGINT-terminated by the 95s watchdog, no crash.
- **refs:** `game/render/overlay_ground_gt3gt4.cpp` (`OverlayGroundGt3Gt4::gt3`/`gt4`); oracle
  `generated/ov_a00_shard_0.c ov_a00_gen_8013FB88`, `generated/ov_a00_shard_1.c ov_a00_gen_8013FE58`;
  repro: `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_PREWATCH=0x801FE924` (raw log) or
  `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_NOPAUSE=1` (gate count).

## libgpu GPU-DMA completion-queue cluster + DrawSync/ClearOTagR — wide-RE wiring pass (2026-07-10)

Promoted the 4 banked `game/render/wide_re_gpu_dma_queue.cpp` drafts (`GpuDmaQueueEnqueue`
0x80082D04, `GpuDmaQueueDrain` 0x80082FB4, `GpuDmaQueueSync` 0x80083364, `GpuDmaSend` 0x80082424)
and the 2 sibling drafts in `game/render/wide_re_libgpu_leaves.cpp` (`DrawSync` 0x80080F6C,
`ClearOTagR` 0x80081458) from wide-RE draft to verified ownership per `docs/fleet-workflow.md` §9.
Re-verify found and fixed real bugs in 5 of the 6 functions; SBS-full stayed 0-diff through the
wiring (verified to f9690+, 95s autonav window).

- **status: RESOLVED — 6/6 wired 2026-07-10 (Enqueue's residual closed, see follow-up entry below).**
- **bugs found at re-verify (statistics for §9):**
  1. `GpuDmaQueueDrain`/`GpuDmaQueueSync`/`GpuDmaQueueEnqueue` — 9 combined missing
     branch-delay-slot `r31`-mirror bugs: every call site to a non-leaf callee (`Drain` itself,
     the ISR-register leaf `func_80085B80`, the caller-supplied `fn`/ring-entry/completion-handler
     dispatch targets) needs `c->r[31]` set to gen's literal return-address constant BEFORE the
     call, because the callee spills the caller's ra into ITS OWN guest-stack frame — omitting it
     means the spilled byte diverges from gen (CLAUDE.md "mirror the guest stack"). `func_80085C9C`
     (int-mask set) and the GPU-DMA timeout arm/chk HLE no-ops are true leaves and don't need it.
  2. `GpuDmaQueueEnqueue` — `GPU_QSTAT_ACTIVE` (0x800A59A8) must be written **unconditionally**
     every call (a MIPS branch-delay-slot store — `generated/shard_5.c:13837-13838`); the draft
     gated it on `started==0`, so on every call after the first, ACTIVE never got rearmed. Root
     cause: literal transcription missed that the delay-slot instruction executes regardless of
     the branch outcome. 1-byte SBS-fatal diff at 0x800A59A8 from frame 0.
  3. `DrawSync` (0x80080F6C) — the draft's comment claimed "no stack frame, leaf, sp untouched";
     gen_func_80080F6C actually pushes a -24 frame and spills s0/r16+ra. Missing entirely.
  4. `DrawSync` AND `ClearOTagR` — both read `GPU_SYS_TABLE + offset` with a SINGLE dereference;
     `GPU_SYS_TABLE` (0x800A5998) is actually a POINTER FIELD to the real jump table and gen
     dereferences it TWICE (`r2=mem_r32(base+22936); r2=mem_r32(r2+60)`). The single-deref version
     read garbage (observed 0xFFFFFFFF / stale 0x0101000A-style scratch) and dispatched into
     nowhere — corrupted the ENTIRE downstream OT array (6109+ bytes diverged at frame 0), then
     crashed via render-queue-overflow within a few frames. This was the most severe bug: 629-diff
     crash on the full run, traced with `PSXPORT_THUNK_FORCE_GEN` bisection down to ClearOTagR
     alone, then pinned exactly via a temporary stderr print of `tableSlot44` (showed 0xFFFFFFFF).
- **`GpuDmaQueueEnqueue` (0x80082D04) — WAS left unwired here with an open residual at 0x801FF154;
  CLOSED 2026-07-10, see "GPU-DMA Enqueue residual CLOSED" below for the root cause (a
  register-liveness bug in this file, not in the then-undrafted streamer) and the fix.**
- **ovhit caveat:** `PSXPORT_DEBUG=ovhit` (5-frame REPL run) shows `Sync`/`Send`/`DrawSync`/
  `ClearOTagR` FIRE with matching native/oracle counts (real gate coverage: 1045/1045, 1020/1020,
  1045/1045, 1020/1020). `Drain` shows `native=0 oracle=0` — it is INSTALLED and its 0-diff is
  real, but NOT exercised by this playthrough's autonav coverage, because Enqueue's ring/deferred
  path (the only thing that calls Drain) is dead code while STARTED stays 0. Drain's correctness
  therefore rests on the line-by-line RE re-verify (bug #1 above), not the SBS gate, per §9's rule
  against claiming gate-verification for a leaf that never fired.
- **method:** `PSXPORT_THUNK_FORCE_GEN=<comma-list>` bisection (force individual addresses back to
  the gen body while keeping others native) isolated each bug to one function; `PSXPORT_SBS_
  PREWATCH=<addr>` + the write-site host/guest backtrace dump pinned the exact write instruction
  for the two hardest bugs (ClearOTagR's double-deref, Enqueue's residual).
- **verified:** `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_NOPAUSE=1`, 95s window:
  zero `sbs-div`/`VIOLATION`, byte-exact through f9690+, SIGINT-terminated by the watchdog (no
  crash). Per-address isolation runs (Drain/Sync/Send/DrawSync/ClearOTagR individually, and all 5
  combined with Enqueue left on gen) each independently confirmed 0-diff over 5000+ frames.
- **refs:** `game/render/wide_re_gpu_dma_queue.cpp`, `game/render/wide_re_libgpu_leaves.cpp`;
  install sites `gpu_dma_queue_install()` / `gpu_libgpu_leaves_install()`, called from
  `games_tomba2_init()` in `game/game_tomba2.cpp`.

## GPU-DMA Enqueue residual CLOSED — LoadImage streamer (0x80082734) wired + register-liveness bug fixed (2026-07-10)

Follow-up to the entry above. The LoadImage-style FIFO streamer (0x80082734) that Enqueue's fast
path dispatches into is now drafted (`game/render/wide_re_gpu_loadimage_streamer.cpp`, a separate
wide-RE wave — 48-byte frame, spills ra/s0-s5, W/H rect-clip + chunked-FIFO/async-DMA-handoff, full
struct map in the file header). Re-verifying it line-by-line against `generated/shard_5.c:13663
gen_func_80082734` (every branch polarity, every delay-slot write, both pointer double-derefs, and
the decrement-then-test PIO-remainder loop's net iteration count) found **zero discrepancies** — the
draft was already byte-faithful.

- **status: RESOLVED.** `GpuDmaQueueEnqueue` (0x80082D04) and the streamer (0x80082734) are both
  wired via `engine_set_override_main` (`Render::gpuDmaQueueEnqueue()` / `Render::
  gpuLoadImageStream()`). All 6 addresses in this cluster (Enqueue/Drain/Sync/Send/DrawSync/
  ClearOTagR) plus the streamer are now native.
- **real root cause of the 0x801FF154 residual (NOT the streamer):** a register-liveness bug in
  `GpuDmaQueueEnqueue` itself, one level more subtle than the branch-delay-slot / ra-mirror bugs
  already fixed in this cluster. Diagnosed by re-running the isolation exactly as planned — wire
  Enqueue+streamer, `PSXPORT_SBS_PREWATCH=0x801FF154` — which this time pinned the write to the
  STREAMER's OWN prologue spill of its caller's `r17` (`c->mem_w32(c->r[29]+20, c->r[17])`, its
  first instruction after the frame descent): core A wrote a stale pointer-shaped value, core B
  (real gen) wrote `0x00000008` — which is exactly Enqueue's `sizeBytes` argument (confirmed via
  `PSXPORT_DISPWATCH=0x80082D04`: every Enqueue call reaching the streamer passes `a2=8`, an 8-byte
  RECT16 payload size, not a coincidence).
  - **the actual bug:** gen's real MIPS execution keeps s0-s3 (r16-r19 = argValOrPtr/sizeBytes/
    arg3/fn) genuinely LIVE in the CPU register file for Enqueue's whole body — `r16=r5; r17=r6;
    r18=r7; r19=r4` are real register writes at entry, generated/shard_5.c:13805-13815, and they
    stay in those physical registers (by MIPS callee-save convention) across every nested call
    Enqueue makes, including the fast-path dispatch to `fn`. Any callee reached via that dispatch —
    here, the streamer — is itself a function with its OWN callee-save prologue, and that prologue
    unconditionally spills WHATEVER is currently in `c->r[16..19]` to ITS OWN stack frame, exactly
    as real MIPS hardware would. The `GpuDmaQueueEnqueue` **draft** captured `fn`/`argValOrPtr`/
    `sizeBytes`/`arg3` as plain C++ `const uint32_t` locals and never wrote them back into
    `c->r[16..19]` — so those emulated registers kept whatever STALE content they held from
    *Enqueue's own caller's* frame (`gen_func_80081218`'s live `r17` = a VRAM-scratch destination
    pointer, not `sizeBytes`). The streamer's prologue then spilled that stale pointer instead of
    Enqueue's real `sizeBytes=8` — an SBS-fatal 1-byte diff from frame 0, on a call chain that
    otherwise (args, sp, dispatch target) was byte-identical on both cores, exactly matching the
    prior session's "some memory content already differs before this call, reason not isolated"
    note.
  - **why this is a DIFFERENT bug class from the delay-slot/ra-mirror fixes above:** those fixed
    `c->r[31]` (ra) before a call so the callee's ra-spill matched gen. This is the SAME principle
    (CLAUDE.md "mirror the guest stack" — a live value must be live in the register file, not just
    a local, whenever a nested dispatch can observe it) applied to the OTHER callee-save registers
    (s0-s3), which is easy to miss because C++ locals "look correct" until a downstream callee's
    OWN prologue spills them.
  - **fix:** `Render::gpuDmaQueueEnqueue()` now writes `c->r[19]=fn`, `c->r[16]=argValOrPtr`,
    `c->r[17]=sizeBytes`, `c->r[18]=arg3` directly (matching gen's exact assignment order) and uses
    `const uint32_t&` aliases into `c->r[]` for the rest of the body, so every nested dispatch call
    (the ring-full retry's `Drain`, the fast path's `fn`, the deferred path's `ISR_REGISTER`/
    `Drain`) sees the SAME live values gen's real registers would hold.
  - **same bug found (by inspection, before it could surface) in 2 more functions in this file**,
    both already claimed "verified" by the prior wiring pass — fixed alongside the Enqueue fix:
    - `GpuDmaQueueSync` (0x80083364): the mode!=0 poll path computes `depth` and keeps it live in
      s0/r16 across a nested `Drain` call (`generated/shard_0.c:13000-13003`); the draft used a
      plain local. Fixed the same way (`c->r[16] = depth` before the `Drain` call).
    - `GpuDmaQueueDrain` (0x80082FB4): keeps READY_BIT/BUSY_BIT live in s1/r17 and s0/r16 across the
      drain loop's nested `ISR_REGISTER` and ring-entry `fn` dispatch calls (`generated/
      shard_6.c:14644-45`); the draft's header explicitly (and wrongly) claimed "only the STACK
      BYTES need to match, not host-register contents" — corrected. NOTE: per the existing ovhit
      caveat, Drain's ring-drain path is still dead code under this playthrough's autonav coverage
      (STARTED never gets set), so this fix rests on the RE re-verify, not a live SBS exercise of
      the drain loop — same honesty caveat as before.
    - `GpuDmaSend` (0x80082424) was checked too and does NOT have this bug: its only nested calls
      are the native-HLE timeout arm/chk leaves, which don't read or spill any register.
  - **lesson for future wide-RE wiring passes:** "only the stack bytes need to match" is true ONLY
    when nothing downstream ever dispatches through a value the function computed while that
    register's physical slot is being reused as scratch. Any function whose body computes a value
    into what gen treats as a callee-save register (s0-s7/r16-r23) and THEN makes a nested
    `rec_dispatch`/direct call MUST write that value into `c->r[N]` (not just a C++ local), because
    the callee's own prologue can and will spill whatever is currently there. This is the same
    class of finding as `docs/findings/render.md`'s "renderWalk r16-r23" note and object_table.cpp's
    guest-stack mirroring — but framed at the REGISTER level rather than the stack-slot level.
- **verified:** `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_NOPAUSE=1`, 95s window:
  zero `sbs-div`/`VIOLATION`, byte-exact through f3690 (SIGINT-terminated by the watchdog, no
  crash — watchdog uses `_exit()` so the per-address ovhit atexit dump doesn't fire on this run
  path; fired via `PSXPORT_DISPWATCH=0x80082D04`/`0x80082734` instead). Dispatch-count parity
  confirmed directly: Enqueue fired ~10017/10011 (A/B, small count skew is run-length/cutoff
  timing, not a divergence — sbs-div stayed 0), streamer fired 6361/6361 (exact match).
- **refs:** `game/render/wide_re_gpu_loadimage_streamer.cpp` (`Render::gpuLoadImageStream`,
  `gpu_loadimage_streamer_install()`), `game/render/wide_re_gpu_dma_queue.cpp` (Enqueue/Sync/Drain
  fixes), `game/render/render.h`. Repro: `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1
  PSXPORT_SBS_PREWATCH=0x801FF154` (now clean) or `PSXPORT_SBS_NOPAUSE=1` for the gate count.
## PutDrawEnv cluster + Font::drawText/glyphEmit + Str::length — wiring pass (2026-07-10)

- **symptom:** frontier wiring session — promote the banked PutDrawEnv chain (0x800815D0 + 4 leaf
  DRAWENV-field builders) and the two hottest banked font drafts (Font::drawText 0x80079374,
  Font::glyphEmit 0x80078CA8) + Str::length (0x80079528) from wide-RE drafts to VERIFIED, WIRED
  ownership per fleet-workflow.md §9.
- **status:** WIRED. All 9 addresses installed via `engine_set_override_main` (oracle-gated thunk,
  SBS core B keeps running the pure `gen_func_*` body). Two real bugs found+fixed during the §9
  re-verify pass; the rest byte-diffed clean.
- **bug #1 — PutDrawEnv's GPU_SYS_TABLE single-deref (game/render/wide_re_gpu_putdrawenv.cpp,
  func_800815D0):** the DMA-send dispatch read `mem_r32(GPU_SYS_TABLE + 24)` /
  `mem_r32(GPU_SYS_TABLE + 8)` directly. `GPU_SYS_TABLE` (0x800A5998) is a POINTER FIELD holding
  the real jump table's base, not the table itself — gen dereferences it TWICE
  (`generated/shard_1.c:15876-15878`: `r3=mem_r32(base+22936); r4=mem_r32(r3+24);
  r2=mem_r32(r3+8)`). Same missing-indirection shape already found+fixed in DrawSync/ClearOTagR
  (wide_re_libgpu_leaves.cpp, prior session) — this is the SAME class of bug landing again in a
  sibling file, exactly as fleet-workflow.md §9 predicts ("routinely contain multiple bugs even
  when they compile"). Fixed: `tableBase = mem_r32(GPU_SYS_TABLE); mem_r32(tableBase + 24/8)`.
- **bug #2 — Font::drawText's fabricated 6th "h" argument (game/ui/font.h / font.cpp,
  Font::drawText):** the original wide-RE draft's signature was
  `drawText(x, y, w, h, str, color)` and packed `a2' = (int16)w | (h << 16)`. Traced every call
  site of `func_80079374` (generated/shard_0.c:11403, shard_6.c:13960/13987/14010/14031,
  shard_7.c:11935/12062/12102/12132/12143, shard_2.c:10751/10776, shard_5.c:13279/13303/13363/
  13371/13394) — the REAL guest ABI is 5 args: `a0=x, a1=y, a2=w, a3=str` (a pointer — e.g.
  `shard_0.c:11405 r7 = mem_r32(r16+12)`), `stack[+16]=color`. `a3`/r7 is never read inside
  `gen_func_80079374`'s body at all; it passes through UNTOUCHED to the tail call, which is only
  consistent with a3 being the string pointer. There is no `h` parameter in the real function —
  the fabricated one corrupted `a2'` (the packed size word `func_80078CA8` reads) whenever `h`
  was nonzero. Fixed: dropped `h` from the signature, `a2' = sign_extend16(w)` only.
- **glyphEmit (Font::glyphEmit, 0x80078CA8) — clean, no bugs found:** re-diffed byte-for-byte
  against `generated/shard_5.c:12298`. Confirms the prior wave's dead-tail-code claim: the live
  body's `return` at gen-C line 210 has no label past it (lines 211-402 are unreachable, a
  hand-unrolled struct copy the recompiler grouped in from an adjacent shard region).
- **Str::length (0x80079528) — clean, no bugs found:** byte-for-byte strlen transcription. Checked
  every call site for an `a0`-leftover dependency (like `Font::measureLineWidth`'s documented
  GOTCHA) — none found; every caller overwrites r4 before its next use, so no leftover-register
  mirroring was needed.
- **PutDrawEnv's 4 leaf DRAWENV-field builders (0x80082240/800822D8/80082370/80082220/8008238C)
  — clean, no bugs found** beyond the shared bug #1 above (which lives in PutDrawEnv itself, not
  the leaves). `func_80081FB0` (the DRAWENV packet packer PutDrawEnv calls) stays MAPPED-not-
  drafted/substrate as the prior session left it — PutDrawEnv reaches it via `rec_dispatch`,
  correct either way.
- **ovhit (PSXPORT_DEBUG=ovhit, 5-frame REPL+SBS-full run, before any melee encounter):** all
  wired addresses except Str::length fired with MATCHING native/oracle counts — real gate
  coverage: `0x800815D0` 1020/1020, `0x80082240` 2100/2100, `0x800822D8` 2100/2100, `0x80082370`
  1020/1020, `0x80082220` 1020/1020, `0x8008238C` 1020/1020, `0x80079374` 273/273, `0x80078CA8`
  273/273. `Str::length` (0x80079528) showed `native=0 oracle=0` in this short window — installed
  and 0-diff, but NOT exercised (its heavier call sites are UI/menu text, not hit in the first few
  frames of intro autonav). Per §9: Str::length's correctness rests on the RE re-verify above, not
  gate coverage, in this particular short window — a longer/different playthrough should confirm
  it fires.
- **unrelated timing-sensitive finding (NOT caused by this pass, ruled out by A/B comparison):**
  running `PSXPORT_DEBUG=ovhit` together with `PSXPORT_REPL=1 PSXPORT_SBS_AUTONAV=1` (an unusual
  combination the standard gate command doesn't use) triggers a real SBS divergence around f1019
  inside `Trig::ratan2` (0x80085690, already-owned) with mismatched return addresses between core
  A/B — a control-flow divergence one frame upstream, in melee-encounter code. Confirmed this is
  PRE-EXISTING and unrelated to this pass: built a `main`-tip (8b7cafb) baseline binary and ran the
  identical `PSXPORT_REPL=1 PSXPORT_SBS_AUTONAV=1` command (without `ovhit`) — 0-diff through
  f11760 in 150s; this session's binary ran the SAME command 0-diff through f12060 in the same
  window (further than baseline, not less). The divergence only manifests when the extra `ovhit`
  bookkeeping overhead perturbs autonav's timing enough to reach a melee fight earlier — it
  reproduces on unmodified `main` too. Matches the already-tracked "stack-depth OPEN" note (commit
  69a1fb3, `docs/findings/render.md`'s billboard/overlay_ground entries) — not re-investigated
  here, out of this pass's scope.
- **verified:** `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_NOPAUSE=1`, 95s window
  (the standard gate command): zero `sbs-div`/`VIOLATION`, byte-exact through f8880+,
  SIGINT-terminated by the watchdog (no crash, no divergence).
- **refs:** `game/render/wide_re_gpu_putdrawenv.cpp`, `game/ui/font.{h,cpp}`, `game/core/str.cpp`;
  install sites `gpu_putdrawenv_install()` / `font_wide_re_install()` / `str_wide_re_install()`,
  called from `games_tomba2_init()` in `game/game_tomba2.cpp`.

## `PSXPORT_MIRROR_VERIFY=all` generalization — reproduces the tracked register-faithfulness/
## stack-depth cluster mechanically, plus surfaces new OPEN addresses in the same chain (2026-07-10)

- **task:** generalize `PSXPORT_MIRROR_VERIFY` (previously required a hand-placed `MV_CHECK` per
  call site) to `=all` — automatic per-invocation native-vs-substrate byte-compare for EVERY
  address wired via `engine_set_override_main`/`_a00` or `EngineOverrides::register_`, hooked at
  the two central dispatch points instead of at each call site (`runtime/recomp/
  engine_override_thunk.cpp`, `runtime/recomp/engine_overrides.cpp`; mechanism + knobs in
  docs/config.md "Mirror TDD gate"). See docs/fleet-workflow.md §9a for the new mandatory-step rule.
- **validation run (unmodified `main` + this tooling change, `PSXPORT_AUTO_SKIP=1
  PSXPORT_MIRROR_VERIFY=all PSXPORT_MIRROR_VERIFY_EVERY=50 PSXPORT_MIRROR_VERIFY_CONTINUE=1`,
  headless free-roam, ~3000 target frames, watchdog-limited to ~90s wall):**
  - **Confirms already-tracked bugs, not harness noise:** the dominant mismatching addresses —
    `0x8003CCA4` (perObjRenderDispatch), `0x8003C2D4`/`0x8003C464` (billboardCompose1/2),
    `0x8003C048` (renderWalk), `0x8003D0BC` (overlayTypeDispatch), `0x801401B8` (entityLoop),
    `0x80082220/240/370/38C/424/734` + `0x80079374`/`0x80078CA8`/`0x80080F6C`/`0x80081458`/
    `0x80082D04`/`0x80083364`/`0x8005950C`/`0x800822D8` — are EXACTLY the perobj_billboard /
    overlay_ground_gt3gt4 register-faithfulness cluster already documented above ("stack-depth
    OPEN", commit 69a1fb3) and in the melee/Str::length entry just above this one. The tool
    reproduces the SAME known-open divergence mechanically, at the exact invocation, instead of
    needing an SBS diff several frames later — this is the validation that it works, not a new bug.
  - **Newly-surfaced addresses not previously tracked by address** (mismatches only, mix of
    ABI-register-only diffs and RAM/scratchpad diffs — same v0/v1-dominant shape as the tracked
    cluster, consistent with the same register-faithfulness bug class rather than a distinct one,
    but NOT individually root-caused in this pass — **OPEN, needs a future triage/fix pass**):
    `0x800205CC`, `0x8003B054`, `0x800420AC`, `0x80051C8C`, `0x80075D24`, `0x80077B5C`,
    `0x801241BC`, `0x801360F4`, `0x80139838`, `0x8013A730`, `0x8013AC34`, `0x8014047C`,
    `0x80140544`, `0x80144928`, `0x801465EC`, `0x801467BC`. Also flagged: `0x80091970`
    (`Sequencer::channelNoteInit`, docs/findings/audio.md) — a v0/v1-only mismatch on invocation
    #1 from a caller context (`ra=0x80091B08`) NOT exercised by the earlier narrow single-address
    `MIRROR_VERIFY=0x800909C0` subtree run that certified it 0-diff; `=all`'s central-gate
    injection reaches every caller path, not just the one a hand-targeted run happened to drive.
  - **Ruled out as harness artifact:** re-running with `EVERY=1` (armed for the whole render
    cluster) shows the SAME addresses mismatching from invocation #1 deterministically (not a rare
    flake); the gate's own state handling never perturbs subsequent frames either way (native
    result is what continues execution whether the check runs or not, matched or `CONTINUE`d past
    a mismatch) — so the volume/shape of mismatches reflects real register/RAM divergence in the
    existing native ports, not an artifact of sampling or of arming `=all` itself.
  - **Performance:** baseline (no MIRROR_VERIFY) does 3000 headless free-roam frames in ~6s.
    `=all EVERY=1` could not finish 3000 frames in a 100s watchdog window (some addresses fire
    1000s of times/frame; each checked invocation costs two 2 MB RAM memcpy/memcmp passes).
    `=all EVERY=50 CONTINUE=1` still could not finish in 90s (43.6s CPU / 56s wall before the
    watchdog fired) — the render cluster's invocation volume dominates even at 50x sampling. For a
    targeted single-address wiring-pass gate (the common case, docs/fleet-workflow.md §9a) `EVERY=1`
    over the content that reaches the address is fine; a full-session `=all` soak needs a much
    higher `EVERY` (several hundred) or a bounded frame count.
- **not fixed in this pass** (tooling task; per fleet-workflow.md §6 this is a discovery to log, not
  a mandate to fix everything found) — the newly-surfaced addresses above are OPEN for a future
  RE/fix pass on the render-dispatch chain; the already-tracked cluster's fix is tracked in this
  file's earlier entries.
- **gate:** `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_NOPAUSE=1` (MIRROR_VERIFY
  unset, i.e. the standard gate command) re-run after this tooling change: zero `sbs-div`/
  `VIOLATION` through the 95s watchdog window, unperturbed by the new central-gate hooks (dormant
  when `PSXPORT_MIRROR_VERIFY` is unset — `mirrorSampleGate` returns false immediately).
- **refs:** `game/core/verify_harness.{h,cpp}` (`mirrorSampleGate`, generalized `strictCheck`
  reporting: invocation #, entry sp/ra, `MIRROR_VERIFY_CONTINUE`), `runtime/recomp/
  engine_override_thunk.cpp`, `runtime/recomp/engine_overrides.cpp`.
## Widescreen re-enabled (aspect FOV + present) — SDL_GPU Pass-1 lost the wide present + OFX (2026-07-10)
- **symptom (USER):** widescreen (aspect 16:9/21:9) "no longer works". Enabling 16:9 via the settings
  file showed the plain 4:3 view in the left 320px + raw VRAM texture-atlas GARBAGE in the [320,nw) band;
  after an interim OFX-only fix the whole scene appeared DOUBLED (two Tombas/benches offset by ~54px).
- **status:** FIXED (headless field shots at 4:3 / 16:9 / 21:9: single, centered, genuinely-wider,
  un-stretched world; no garbage margins). Both SBS legs 0-diff (wide is 4:3-gated so the reference is
  untouched). `scratch/screenshots/g43,g169,g219.png` (session artifacts).
- **root cause (multi-part, all from the Vulkan→SDL_GPU renderer rewrite, commit 3fd50c4 "Pass 1"):**
  1. **Present hard-coded 4:3.** `GpuGpuState::present` / `present_image` sampled the 4:3 `s_disp_w` and
     `letterbox(4,3,…)` unconditionally. The wider FB the engine renders (VRAM columns `[sx, sx+nw)`,
     nw=428@16:9 / 560@21:9) was never sampled → cropped to 4:3, and the sampled region was squeezed into a
     4:3 box. The old VK present handled wide; the Pass-1 rewrite dropped it. `s_last_w` (what `shot`/vkshot
     read) was also 4:3, so headless wide captures silently cropped (the docs' "428 present width TODO").
  2. **OFX widen lever lost.** later-117's genuine-wide shifted the projection center OFX 160→nw/2 (214@16:9)
     so the FOV widens symmetrically. That lived in the old per-frame `ov_set_geom_offset`; the OOP rewrite
     folded display init into `Engine::initDisplay` (`game/scene/startup.cpp`) which hard-wrote OFX=160 with
     NO wide shift. So the world never actually projected wider.
  3. **Draw-area clip stopped at 320.** GP0 E4 set `s_da_x1` to the 4:3 FB right edge, so even once the FOV
     widened, wide-side fragments were clipped and the [320,nw) band showed raw VRAM.
  4. **2D backdrop mapping assumed a shader margin that no longer exists.** `ws_2d_local_x` (gpu_native.cpp)
     was written for the VK relocation shader that added `fb_x0=margin`; the SDL_GPU `tritex.vert` adds none,
     so the backdrop stretch (`x*ww/320 - margin`) under-filled the wide frame (21:9 top-right garbage).
- **why the interim OFX-only fix DOUBLED the scene:** the guest OT-walk draws the guest packets at the guest
  GTE's OFX=160, while the natively-owned submit/terrain re-projections were shifted to 214. At 4:3 both
  centers coincide (double-draw, invisible); at wide they separate by the margin → two copies. The unifying
  fix is to widen the projection center at the SOURCE — `Engine::initDisplay` writes CR24 = `nw/2` when wide
  — so the GUEST GTE itself projects wide and every reader (guest packets + all native re-projections) agrees
  on one center. This is an authorized widescreen guest-state write, gated on `gpu_gpu_wide_engine()` (false
  at 4:3 / under `PSXPORT_ORACLE` and in the SBS legs). Per-read `render_wide_ofx` bumps in the native paths
  were tried first but are redundant with the source write AND risk re-introducing the double if a scene
  legitimately uses OFX=160 — removed.
- **the fix (all wide-gated on `gpu_gpu_wide_engine()`):**
  - `Engine::initDisplay` (game/scene/startup.cpp): OFX = `gpu_gpu_wide_engine_ofx()` (nw/2) when wide.
  - `GpuGpuState::present` (gpu_gpu.cpp): sample width = `gpu_gpu_wide_engine_w()`; `letterbox(disp_w,240)`
    (aspect nw:240); store the wide width into `s_last_w` so `shot`/vkshot capture it. `gpu_native_shot`
    (gpu_native.cpp) crops the wide width too.
  - GP0 E4 handler (gpu_native.cpp): extend `s_da_x1` by `(nw-320)` when wide (render-clip only; the GPU
    batch's `i_da`).
  - `ws_2d_local_x` (gpu_native.cpp): backdrop `x*ww/320` (fill), HUD `x+margin` (center) — matches the
    SDL_GPU shader (no shader-added margin). 4:3 (`margin==0`) byte-identical.
- **widescreen cull population (USER 2026-07-10):** the stock cull is tuned for 320px and very aggressive,
  so dynamic props/entities in the wide side-margins pop out (the read-only static-only margin re-include,
  `MarginRenderer::collect`/type-0x03, never re-includes them). Under widescreen the `Cull::objectCull`
  margin re-include now POKES `obj+1=1` for EVERY culled margin object (dynamic included) so the wide
  margins fully populate. This perturbs guest state — explicitly allowed for the widescreen enhancement —
  and is gated on `gpu_gpu_wide_engine()`, so 4:3 keeps the read-only 0-diff margin and the SBS/oracle
  reference is untouched. refs: `game/render/cull.cpp` (`objectCull`).
- **DEAD END / NOTE:** `ires` (internal-resolution scale) is NOT consumed by the Pass-1 renderer
  (`frame_via_fb()==0`, no supersampled scratch FB). The merged Vanilla/X2/X3/X4/Auto selector + the 4x cap
  persist and report correctly, but X2..X4 do not visibly sharpen until the scaled-FB pass lands. Do not
  claim ires sharpening is verified — it is a forward-looking selector.

## Billboard OBJECT MODEL — manager node + per-particle records (RE 2026-07-10, operator)
- **symptom:** gems "react poorly" at 60fps; historic objid rectangle covered ALL visible billboards
  at once (USER, "many moons ago") — because the billboard identity used everywhere (fps_key,
  obj-depth span, anchors) is the MANAGER node, not the individual billboard.
- **model (gen_func_8003B220, shard_4):** one manager node per billboard class; its sub-list
  (node+56 → {count,byteOff}[], base node+60) holds 16-byte PARTICLE records — each particle is one
  visible billboard (gem/effect). particle[10]/[11] = quad w/h; particle[14]/[15] = s8 X/Y offsets
  SCALED ×5; corners are 2D (z=0) around 5×offset; world pos = node composed translation +
  R·(5·p14, 5·p15, 0) via the CR0-7 billboardCompose1/2 loaded. p14/p15 are ANIMATED per frame by
  the owning behavior (bobbing).
- **consequence:** any per-object mechanism keyed on the node (fps60 anchors, objid rects, obj-depth)
  lumps every billboard of the class together. Correct identity = PARTICLE ADDRESS (stable while the
  object lives). Fix direction handed to the fps60-residue agent: per-particle anchor capture +
  interpolation. Score-gem lifecycle traceable via beh_pickup_collect_trigger.cpp (FUN_8007413C spawn).
- **FOLLOW-UP REGRESSION — native backdrop did NOT cover the wide margin → atlas garbage (2026-07-10,
  fixed):** after f831172 the reported garbage (`scratch/screenshots/reports/ws_garbage.png`) REMAINED in
  scenes whose right margin is SKY: a grid of rainbow texture-atlas tiles filled the `[~344, nw)` band
  wherever no opaque geometry happened to overdraw it (clearest behind the entrance-field's big tree, where
  sky shows through the foliage gaps). RE / evidence: the present samples guest-VRAM columns `[sx, sx+nw)`
  (software-rasterized FB, `frame_via_fb()==0`); a full-VRAM dump (`vkvram 0 0 1024 512`) shows the field FB
  at the top-left with the **texture atlas abutting it immediately to the right** (the later-55 VRAM packing).
  So every margin pixel not painted by clear+geometry samples the atlas. The E4 draw-area extend (fix #3
  above) let GEOMETRY reach the margin, but the SKY/PARALLAX layer did not: the native backdrop drawer
  `render_bg_tilemap_native` (game/render/render_walk.cpp) transcribes the PSX tilemap window centred at
  screen-x 160 with a fixed 352px (`0x160`) span — tuned for the 320 view, covering only `~[0,344)`. It
  draws via the native render queue (RQ_BACKGROUND), so `ws_2d_local_x` (which stretches only GP0-path 2D
  sprites) never touched it. A `vkpix` scan of a sky row confirmed sky up to x≈344 then atlas. **Fix:** widen
  the backdrop window to the wide FB — `cx = nw/2`, `winw = nw+0x20`, replacing the literal `160`/`152`/
  `0x160`; reduces to the exact 4:3 constants when not wide (gated on `gpu_gpu_wide_engine()`), so 4:3 stays
  byte-identical. This re-centres the sky/parallax on the wide projection centre (matching the world's
  OFX=nw/2 shift) and tiles it across the full `[0,nw)`. Verified: full first field, both directions, at 16:9
  (nw=428) AND 21:9 (nw=560) — zero atlas garbage anywhere, sky/sea fills all margins, wide margins still
  populated (cull fix intact); both SBS legs 0-diff. refs: `game/render/render_walk.cpp`
  (`render_bg_tilemap_native`); artifacts `scratch/screenshots/wsfix/`.
  - **remaining exposure (future):** only the state-0 tilemap backdrop is owned natively; other fields'
    backdrop states are still-PSX and render black (frontier). If such a field is widescreened its margin
    is uncovered → the safety net is to give each ported backdrop the same full-`nw` coverage. A blanket
    per-frame margin clear was NOT added: it would write into texture VRAM every frame (the margin overlaps
    the atlas, esp. at 21:9) with no observed benefit for the field (the backdrop widening covers it), and
    the correct owner of the margin is the background layer, not a clear.

## CORRECTION (USER, 2026-07-10): throw-chain is a RENDERER bug, not a pc_skip fork
- The earlier inference (chain visible under SBS-full ⇒ pc_faithful has it ⇒ pc_skip init loses it)
  was WRONG — it trusted SBS panes that the user showed are unrepresentative of both real configs.
- USER: the chain bug is a RENDERER bug — the render paths (pc_render / psx_render) still LEAK into
  each other instead of being truly OOP-isolated ("asked a million times"). Same exec state, chain
  appears under one render path and not the other ⇒ the chain's prims exist; pc_render loses them
  through shared/leaked renderer state, not through spawn/init.
- Direction: audit the renderer boundary as an OOP-isolation defect (shared statics/buffers/flags
  crossing between the PSX render path and pc_render — the historical class of leaks), with
  abcompare (docs/abcompare-design.md) as the instrument: same run, flip ONLY the renderer, diff.
- The pc_skip-fork hypothesis in the entry above is FALSIFIED for this bug; kept for the record.

## "Upside-down Tomba" in hooked-fish beat — FALSE ALARM + eprojv tool (2026-07-14, bug #44 closed)

- **symptom (reported then falsified):** row-6 evidence pass flagged Tomba's caught-fish sprite as
  upside-down under pc_render at ~f1370 vs the oracle's "upright" rendering.
- **dead end / lesson:** the comparison was RAW-FRAME-matched across configs with a 12-14 frame exec
  lag (GATE lags default); Tomba swings on the fishing hook, so shots a dozen frames apart show
  opposite orientations. Exec-aligned same-frame A/B (single deterministic GATE run per renderer,
  `newgame; run 1373`) shows an IDENTICAL pose: scratch/screenshots/bug44_ab_{pc,psx}.png. The only
  real diff in the pair is bug #34's missing dialog panel. **Cross-config visual comparisons must be
  state/beat-aligned, never raw-frame-matched** (same rule as MODE=skip pane alignment).
- **positive by-product — `eprojv` diag channel (game/render/submit.cpp, docs/config.md):** dumps the
  composed EObjXform R/T/H per object + final screen-space verts per GT3/GT4 primitive. Fitted against
  the same-run guest OT packet SXY (GATE=1 + pc_render: substrate populates the OT with GTE-computed
  packets while pc_render computes its own floats — one run yields both vertex sets): all 13 rig
  primitives matched under IDENTITY at shift 0 with 0.46-0.94 px RMS. **The native mesh projection
  pipeline (projComposeObject → EObjXform::project → submitPolyGt3/Gt4Native) is pixel-exact vs the
  GTE for this scene — do not re-investigate it for orientation/position symptoms.** Decoder scripts:
  scratch/bug44_decode_ot.py, scratch/bug44_fit_transform.py.

## Quad batching (#45) — evidence pass: scope narrowed to 0x80039F4C (2026-07-14)

- **user report:** quads lack RE; all visible quads share one "billboard" → react badly to fps60.
- **proven from code (not sampled):** provenance is manager-granular by construction — otattr spans
  coalesce on (fn, caller, node); obs_body/withDepthTag register ONE depth span + ONE fps60 identity
  per gen call regardless of how many world objects the call draws.
- **but two of the three suspect emitters are already covered:** 0x8003C5F8/0x8003C788 are thin GTE
  compose wrappers that direct-call 0x8003C8F4 = native Render::billboardEmit, whose per-particle
  loop already records recordBillboardParticle (identity + world anchor per particle), and
  Fps60::billboardForNode searches the per-particle table FIRST (fps60.cpp:161-179) — gem/pickup/
  flame sprites interpolate individually. Occlusion depth remains whole-batch (outer withDepthTag) —
  not user-reported, left as-is.
- **the real gap = 0x80039F4C** (UNOWNED, wrapped by obs_body): a genuine multi-element loop —
  iterates per-element object pointers (iVar7 = param+4k), reads a glyph index per element, resolves
  per-element transform at iVar7+0xC0, emits one quad each via FUN_8003F7D8 (decomp:
  scratch/decomp/bug45.c). This is the score/AP popup / icon-strip renderer. N elements' quads land
  in ONE span with ONE node identity → fps60 reprojects the strip rigidly. Natural per-element
  boundary: each loop iteration's element pointer.
- **fix direction:** port 0x80039F4C native (full ownership) with per-element provenance
  (recordBillboardParticle-equivalent + per-element identity); port framework + port_check gate.
- **outstanding:** live otattr confirmation of 0x80039F4C firing (needs a score/AP popup on screen —
  collect a pickup; the evidence agent's jump-traversal never collected one). Confirm before landing.
- **refs:** issue #45; scratch/decomp/bug45.c; game/render/perobj_billboard.cpp:388,512-529;
  game/render/fps60.cpp:142-196; game/render/render_observer.cpp; ot_attr.cpp:44-49 (coalescing key).
- **FIX LANDED (2026-07-14, 0f78384) — the node-span teleport class:** stampBillboard now
  unprojects each quad's own screen centroid through the base camera at the node's view depth
  (exact inverse of projWorld; R orthonormal → transpose) and stores THAT as the per-quad anchor;
  node-span hits only. Measured with the new `bbanchor` channel: mean anchor error 672px → 5.3px
  (n=115k stamps), centroid round-trip 0.45px mean / 7.45px max; particle-table path unchanged;
  SBS-full smoke 0-diff f29190. USER repro (tree fruit alternating ~150px every frame) awaiting
  eyeball. Residual: per-quad anchor captures camera motion only (object's own motion within the
  span is not interpolated — correct next step remains per-element RE/ownership of the emitters,
  0x80039F4C first).

## 0x8003F9A8 474-prim attribution resolved — 4 substrate list-walkers, no hidden emitter (2026-07-14)

- The field draw dispatcher 0x8003F9A8's misattributed prims are the ALREADY-NATIVE chain
  (cca4→cdd8→f698→80146478→OverlayGt3Gt4::gt3/gt4) reached via four still-substrate object-list
  walkers: 0x8003BB50 (list @0x800F2410), 0x8003BCF4 (@0x800F26C8), 0x8003BF00 (@0x800F2738),
  0x8003EEC0 — direct C calls, invisible to the otattr shadow stack. PORT ORDER: (1) the 4 walkers
  (no new emitter RE; identity = walked object ptr; wire the case-0x15/16/17/20 per-object vtable
  slots through rec_dispatch); (2) FUN_8004FD30+FUN_8004FB4C (self-contained HUD gauge emitter,
  loop identity = 0x800BF548 stride-0x8C index); (3) single-object dispatchers (defer).
- 0x8013CDD4 = the un-owned widescreen-margin OT.LINE/GT3 submitter (prior scoped debt, journal
  later-129/131), reached via the walkers' per-object vtable slots — NOT part of the gt3/gt4 chain.
- Decomps: scratch/decomp/otattr_subs.c, otattr_leaf{,2}.c, otattr_f698.c; Ghidra project
  scratch/ghidra/otattr_census.

### RESOLVED — 4 walkers OWNED native (2026-07-15, game/render/objlist_walk.cpp)

- **status:** all 4 walkers (+ the FUN_8003BED8 shared-tail split of BCF4) ported as `Render::
  objListWalk1..4` + `objListWalk2Continue`, wired via `engine_set_override_main` (oracle-gated; core B
  stays pure gen). SBS-full AUTONAV 300s = **0-diff through f21390** (0 sbs-div, 0 differ checkpoints).
- **walker map:** BB50=objListWalk1 (-40 frame, spills r16/17/18/19/ra; list@0x800F2410, cursor
  0x1F80013C/146, table 0x80014A70, 144-entry); BCF4=objListWalk2 (-40, r16..r20/ra; @0x800F26C8,
  0x1F800148/152, table 0x80014CB0, 33-entry) — MANUAL frame push, pop deferred to objListWalk2Continue
  (=FUN_8003BED8, an independently guest-reachable "continue the walk" trampoline that OTHER substrate
  leaves tail-call, so owned at its own address); BF00=objListWalk3 (-32, r16/17/18/ra; @0x800F2738,
  0x1F800154/15E, table 0x80014D38, 32-entry); EEC0=objListWalk4 (-32, r16/17/18/ra; node+0x24-linked
  chain head *0x800F2738, table 0x80015000, 33-entry). Case-0/0xF → perObjRenderDispatch; per-object
  vtable slots (cmd+0x7C / cmd+0x18 / cmd+0x24 / cmd+0x7C) → rec_dispatch (never dropped).
- **two real bugs found + fixed (both via bisected SBS, baseline forced-gen = 0-diff):** (1) BB50's
  case-value 0x8003BCB4 (Ghidra case 0x16, vtable+0x7C without the preceding billboardCompose1) is a
  SEPARATE directly-reachable switch target, not merely 0x8003BCAC's fallthrough tail — omitting it sent
  those objects to the `default:` early-return, aborting the walk (massive packet_pool divergence from
  f180). (2) register-faithfulness: BCF4/BED8's rec_dispatch targets read the object ptr from **r16**
  (gen never sets r4 for them, unlike BB50/BF00/EEC0 whose cases do) AND every walker keeps its loop
  state (cursor/count/table-base) LIVE in the real callee-saved regs — a downstream substrate leaf
  spills gen's r19=0x80014A70 (BB50 table base) that native left as stale 0 (SBS diff 0x801FE8C4/E4,
  f119..f156). Fix = mirror the live registers in c->r[] (r16=obj, r17=count, r18=cursor, r19=table for
  BB50; analogous for BF00/EEC0/BCF4), not C++ locals.
- **otattr proof (field f405):** attribution byte-IDENTICAL native-owned vs forced-gen baseline (caller
  counts 621×0x8003F9A8 / 161×0x8003BDAC / 129×0 / 18×0x8003CCA4 — exact match). The walker-chain
  per-object provenance (caller=0x8003BDAC = the BCF4-table's perObjRenderDispatch case
  `gen_func_8003BDAC → func_8003CCA4 → func_8003BED8`; caller=0x8003CCA4) is REPRODUCED exactly; emitter
  leaves fn=0x80146478 (227, OverlayGt3Gt4), fn=0x801401B8 (221, OverlayGroundGt3Gt4::entityLoop),
  fn=0x80115598 (353, backdrop). No hidden emitter — confirmed. Residual fn=0x8003F9A8 spans = the
  backdrop / overlayTypeDispatch path (reached as a plain call from 0x8003F9A8, not the walkers).
- **render:** default free-roam + fps60=1 shot PIXEL-IDENTICAL to forced-gen baseline (md5 match, 0/230415
  byte diffs) — no visual regression (expected: walkers write only guest RAM, mirrored byte-exact).
- **port_check caveat:** reports FAIL on all 4 (coarse call-sequence normalizer) — owned sibling methods
  (perObjRenderDispatch/billboardCompose1/2) appear as unmappable `None` targets not `func_XXXX`, and
  objListWalk2's frame-close lives in the split-off objListWalk2Continue. Same known limitation as
  perobj_dispatch.cpp::cmdListDispatch. SBS-full 0-diff is the authoritative equivalence gate.

## 0x8013CDD4 port — ambiguity SETTLED (2026-07-14): load-bearing field reuse, port unblocked

- Hookable cleanly: a00 overlay rec_dispatch leaf (ov_a00_disp.c case 0x0013CDD4, override slot 451,
  engine_set_override_a00) — no higher dispatcher needed. Packet = 13-word GT4, pool 0x800BF544,
  same layout as OverlayGt3Gt4::gt4. Vertices = signed bytes at record offsets (pb-2/0, pb-15, ...)
  <<8 through RTPT + RTPS(V3) + AVSZ4 (generated/ov_a00_shard_1.c:25528-25899, ground truth).
- **SETTLED (same day, static ground truth from generated/ov_a00_shard_1.c:25528-25899 — no live run
  needed):** verdict (a) — record+30 IS V0.y (GTE wiring proof: gte_write_data(0) high half → RTPT →
  SXY0) AND, read a second time sign-extended (no <<8), the SINGLE shared fog delta for ALL FOUR
  vertices' colors: delta = max(0, s8(rec+30) - mem16(obj+86)); R/G = clamp0_255(base - delta);
  B = clamped only when R didn't underflow (branch-delay artifact — replicate as-is). mem8(obj+3)
  is a DEAD load (recompiled scheduling artifact — not live). Full record layout table in the agent
  report (session 2026-07-14): +4 count/plane, +8/+12/+16 packed color/code words, +15/19/23/27
  V0-3.z, +28..+35 packed {X0,X1,Y0,Y1,X2,X3,Y2,Y3}. A native port must extract rec+30 ONCE and use
  it twice with the two treatments; never split into per-vertex fog inputs; never "fix" the B-channel
  asymmetry.
- **superseded stop condition (for the record):** all four per-vertex fog-clamp blocks read their fog
  input from the SAME byte mem_r8(r8+0) — the byte also used as V0.y. Either a load-bearing
  field-reuse the port must replicate, or pb+0 isn't V0.y at all. Settle by fresh targeted Ghidra
  pass + live watch of pb+0 and param_1+0x56 (otattr watch/who works for this) BEFORE writing the
  native color math. No code was written — correct per no-bandaids.
- Priority per the 0x8003F9A8 resolution above: the 4 substrate list-walkers port FIRST; 8013CDD4
  stays under the widescreen-margin debt (journal later-129/131).

### PORTED (2026-07-15) — game/render/widescreen_margin_quad.cpp, WidescreenMarginQuad::emit — 0-DIFF

- **status:** SBS-full AUTONAV **0-diff to f26790+** (895 checkpoints, exits on 250s wall-clock
  timeout, no divergence). `PSXPORT_MIRROR_VERIFY=0x8013CDD4` — **3905+ invocations, every one
  byte-exact** (RAM + scratchpad + ABI regs v0/v1, zero mismatches). Wired via
  `engine_set_override_a00(0x8013CDD4, &WidescreenMarginQuad::emit, ov_a00_gen_8013CDD4)` (oracle-gated).
- **Three bugs found + fixed during bring-up, all via `PSXPORT_MIRROR_VERIFY`:**
  1. **Record array is `mem32(obj+80)` (a POINTER DEREFERENCE), not `obj+80`.** `r19 = obj+80` is a
     SEPARATE register used only to derive `fogBase = mem16(obj+86)`; `r10 = mem32(obj+80)` is the
     actual record-array base. Using `obj+80` directly read a foreign region (every record field
     came back 0). Caught by mirror-verify showing the emitted packet bytes wildly wrong.
  2. **Guest-stack vertex-staging scratch (sp+32..sp+61) must be WRITTEN, not bypassed.** gen stages
     each vertex screen-delta coordinate as a halfword into stack scratch (offsets 32/34/36/40/42/
     44/48/50/52/56/58/60 per `tools/abi_extract.py --contract`), then loads 32-bit words from there
     into the GTE VXY/VZ registers (the gap bytes sp+38/46/54/62 are read as GTE-ignored VZ upper
     halves). The first port draft computed the packing in HOST registers and fed the GTE directly,
     never writing the guest stack → those 13 bytes diverged (the f570 SBS stop). Fix = mirror the
     stack writes exactly (`kVtxScratch_*` offsets), then load the GTE from the stack — guest-stack
     residency per CLAUDE.md "MIRROR THE GUEST STACK", NOT a dead-scratch exclusion.
  3. **v0/v1 return-register residue.** gen leaves v0=`kPktPoolBase` (0x800C0000, the base it derives
     the final pool-cursor store address from) and v1=`kPktTag` (0x0C000000, the POLY_GT4 tag high
     word held live from the last commit). Reproduced exactly (hold the tag in `c->r[3]` during
     commit; compute the exit store via `kPktPoolBase - 2748`); the node==0 early-out reproduces
     v0=`mem8(obj+7)` (gen's surviving r2 on that path). Now register-exact too.
- **Frame:** 104-byte frame, spills r16-r22/r31 at 72/76/80/84/88/92/96/100 via `GuestFrame` +
  `GuestReg` (guest_abi.h). obj+72's node carries 3 angle bytes (×10) fed as a1 to the still-unowned
  "compose object transform into GTE CR0-8" leaf (0x800318A0, docs/engine_re.md cluster 3) via
  `guest_fn` — this port is the FIRST caller to exercise 0x800318A0→0x80084520→rotmat under SBS-full;
  rotmat (0x80085480) is already natively owned and 0-diff, 0x800318A0/0x80084520 stay pure substrate
  on both legs.
- **Gates:** build clean; mirror-verify 3905+ passes 0-diff; SBS-full 895 checkpoints 0-diff to
  f26790+; default free-roam (pc_skip+pc_render) reaches free-roam f216 + shot; fps60=1 ("TRUE
  per-object interpolated 60fps ON") reaches free-roam + shot, no crash.

## ires (internal resolution) modifier is a NO-OP — never wired past the readout (2026-07-14)

- Proven: ires=1 vs ires=4 frames byte-identical (md5 8c4e6a32..., scratch/screenshots/ires{1,4}_before.ppm).
- Chain: mods.ires is read only by gpu_gpu_video_status() (a text readout for the RmlUi overlay). The 3D
  passes rasterize into the fixed 1024x512 VRAM-space texture (gpu_gpu.cpp ensure_targets + hardcoded
  viewport gpu_gpu.cpp:488); frame_via_fb() is a permanent stub — the "Pass 2 scaled scratch FB" its
  comment references was never built. Not a deglobalization regression; never finished.
- Fix = design unit: a separate ires-scaled 3D target (recreated on live toggle) receiving the opaque+semi
  passes, composited/downsampled back over the pixel-exact VRAM-space 2D; redesign the ires_cap clamp.
  Full analysis in the diagnosis report (session 2026-07-14).
- **FIXED (2026-07-15).** `GpuGpuState::s_ires_color/s_ires_depth/s_ires_rgba` (gpu_gpu_internal.h) — a
  `VRAM_W*i x VRAM_H*i` target set, lazily built + torn-down/rebuilt on scale change by
  `ensure_ires_targets()` — receives Pass A (opaque)/decode/Pass B (semi)/encode at `i>1` (gpu_gpu.cpp
  render_geom); same shaders/vertex data, only the viewport is `i`× bigger (tri.vert's NDC divisors are
  fixed to the 1024x512 canvas, so a bigger viewport is a literal "scale by i"). A box-filter downsample
  shader (`ires_downsample.frag`, NOT a plain `SDL_BlitGPUTexture` LINEAR blit) composites ONLY the
  display sub-rect back into the fixed `s_vram_tex`. `i==1` takes the original direct-to-`s_vram_tex` path
  unconditionally (colorTgt/depthTgt/rgbaTgt alias the plain fields, no extra blit/pass) — byte-identical
  to pre-fix (verified: ires=1 shot md5 stays `8c4e6a32...`).
  - **Two real bugs found + fixed during bring-up, both worth remembering:** (1) `tritex.frag`/
    `trisemi_hw.frag`'s draw-area clip (`v_da`) and `tritex.frag`'s manual semi destination sample
    (`vram_at(px,py)`) both used raw `gl_FragCoord` — native-VRAM-unit thresholds compared against an
    ires-scaled fragment coordinate discarded almost everything past `VRAM_W,VRAM_H` regardless of
    scissor/viewport (symptom: only the top-left 1/i×1/i corner of the scene rendered). Fixed by dividing
    `gl_FragCoord` by a new `PC.scale` fragment uniform before either use. `decode.frag`/`encode.frag` had
    the same class of bug via a hardcoded `vec2(1024.0,512.0)` texel-space constant — fixed with
    `textureSize(sampler,0)`. (2) A plain `SDL_BlitGPUTexture` LINEAR-filter downsample for the final
    composite is a single bilinear tap per destination texel — correct for the upsample/seed blit
    (magnify) but aliases badly on a >1:1 minify (confetti noise on grass/leaf detail, confirmed via
    `iresdump`/`gpu_gpu_ires_rawdump`: the raw ires target was clean, only the blitted-down composite
    wasn't). Replaced with a proper NxN box-filter fragment shader.
  - ires_cap redesigned around a real GPU-memory budget (128 MiB/Game, 14 bytes/px across the three
    ires-scale textures) instead of `VRAM_W / native_w` (that clamp existed to keep a scaled FB inside the
    1024-wide VRAM canvas — moot now that the scaled render is a standalone target, not squeezed into it).
  - Live toggle: REPL `setires <0..4>` (new; previously only the RmlUi overlay could flip `mods.ires`,
    windowed-only) exercises the same `mods.ires` mutation and drove `ensure_ires_targets` through several
    teardown/rebuild cycles headless with no crash.
  - Known caveat: 2D content (HUD/menu text, sprites) is submitted through the SAME tri/tex geometry
    pipeline as the 3D world (no separate 2D draw path exists in this renderer), so it technically renders
    through the ires-scaled target too when `i>1`. Empirically this is harmless for OPAQUE 2D (axial,
    integer-positioned glyphs/sprites box-filter back to the identical value — verified: isolating
    glyph-ink pixels in the pause-menu "Select Options" text gave 1 differing pixel out of ~500 opaque ink
    pixels checked, vs ~490 differing pixels in the SAME crop's translucent-panel background, which
    legitimately shows the differently-anti-aliased 3D world through the panel — expected, not a bug). A
    true architectural fix (tag+split 2D-band prims into an unscaled sub-batch) is future work if an
    opaque-2D case is ever found to actually shift.

## Z-fight sweep 2026-07-14 — barrel fix HOLDS; new double-submission class; 2 zfight tooling defects

- Barrel/faceted epsilon fix verified live (7-leg field sweep, 1475 frames scanned, eps=6e-5): no
  coherent contested patch; residual = the documented mesh-seam true-tie tail (99.6% paint-stable
  at shipped ZBIAS; contests are terrain-sentinel adjacent-quad seams).
- NEW: exact-duplicate double-submission (owned node=800FB858 vs node=0 guest-OT leak) — filed as
  a bug; not an epsilon problem, an ownership/one-picture-source problem.
- Tooling defects (fix same session): (1) PSXPORT_ZFIGHT=1 sets eps=1.0 (atof("1")>0 skips the
  6e-5 default) — every overlap counts as a fight; (2) zfightScan never runs under mods.fps60=1
  (flush short-circuits to rq_capture before emitQueue) — the instrument is dead under the user's
  real config.
- Artifacts: scratch/logs/zfight_sweep_*.log, scratch/screenshots/zfight_sweep*/, heat_f*.ppm.

## Hut interior FIXED (2026-07-15) — authored OT sub-scene walked full, not native-field-reconstructed

- Replay-verified (hut-entry-door-freeze.pad): entry = SUB-SCENE swap, sm[4c]==3, area id UNCHANGED
  (the "separate area / mode table" hypothesis above is WRONG for this door). Transition COMPLETES
  (scene.md's FUN_80073328-case-3 permanent-freeze does not reproduce on this replay); SBS-full
  0-diff f44580 through entry + idle inside (the old f389 diverge = voiceMixTick, fixed).
- Symptom = pc_render only: while sm[4c]==3 the picture is Render::frameX's reduced pass driving
  overlay emitter 0x80146478 (room node 0x800FD850 beh 0x8012C910; NPCs via beh_id_routed_dispatch)
  — no native rebuild exists, so pc_render keeps presenting the exterior field pass. GATE+pc_render
  reproduces; GATE+RENDER_PSX draws the interior. Fix = RE+native ownership of the frameX/state-3
  interior pass. Filed as a bug; evidence scratch/screenshots/hut_verify/ + hut_otattr_out.log.
- **UPDATE (2026-07-15): the "missing native rebuild" fix premise is FALSIFIED.** render_field_native_active
  already fires the f1527b5 redirect in sm[4c]==3 (verified PSXPORT_DEBUG=redirect names node 800FD850 room +
  the two NPC nodes), and sceneNative composes all 4 interior objects (PSXPORT_DEBUG=scenenative: objs=4
  cmds=407, eproj transforms sane) — yet the presented frame is still the closed-door EXTERIOR. So #49 is
  DOWNSTREAM of geometry submission: a present/framebuffer-target selection issue OR the twoDOnly OT walk
  (game_tomba2.cpp:209) redrawing stale exterior packets over the correctly-submitted native world. Next:
  diff VK draw-call/present-target selection between GATE+RENDER_PSX (works) and default (broken) at the
  same interior frame. No fix landed (no-bandaids — mechanism not yet named).
- **FIXED (2026-07-15, root-caused live):** the interior room's 3D geometry IS in the guest OT (proven:
  PSXPORT_DEBUG=scenenativehud full-walk shows the room; oracle full-walk shows it). pc_render's field
  branch ran sceneNative (reconstructs the walkable FIELD — draws the stale exterior through the interior
  camera) + a 2D-only OT walk that DROPPED the interior's 3D world. Discriminant (RE-grounded, not magic):
  the game's field submode dispatches per-frame by sm[0x4c] through its own 9-state table (Engine::s4c,
  engine.cpp) — state 2 = walkable field (Render::frame), state 3 = fieldRunX→frameX reduced pass that
  composites an AUTHORED scene into the OT (interior/transition). Fix (game_tomba2.cpp): when field &&
  sm[0x4c]==3, walk the FULL guest OT with no native field render — same treatment as the void beat.
  Verified: interior room renders (matches oracle, no exterior leak); free-roam field (sm[4c]==2)
  unaffected; SBS-full 0-diff f23940. Shots scratch/screenshots/hut_FIXED_*.png.

## Double-submission (#48) FIXED (2026-07-15) — scene-native-owned meshes dropped from the OT walk
- Owned per-object meshes (perObjFlush over the 3 entity-list heads) were ALSO leaking through the
  twoDOnly OT walk as node=0 world polys → exact-duplicate geometry (gap=0.0), overdraw + latent
  flicker. Fix: Render::nativeObjDrawn (render_walk.cpp) re-derives per-frame (read-only guest mirror
  of perObjFlush's own inclusion test — HEADS[3] walk + node+1 marker + node+8/9 counts; a WRITE-side
  registry failed because Render::frame runs before perObjFlush in the same logic frame) the set of
  nodes perObjFlush draws; cmdListDispatch extends the PktSpanSession/gpu_native_cover_add wrap to
  those nodes so gp0_exec drops their guest-OT copies. Uncovered by design: objects on other walk
  lists perObjFlush never visits (Bcf4 aux) keep the redirect's own inline draw.
- Verified (integrated build): owned-vs-node0 exact-dup tie class → 0; free-roam no lost geometry;
  SBS-full 0-diff f21810. Registration-driven (guest state), never address-range/heuristic.

## FUN_800518FC three-way split → NodeXform::buildWithOffset sole owner (2026-07-15)
- DEFECT (3rd dual-ownership from codemap --conflicts): FUN_800518FC (object matrix-compose-with-offset:
  svec-scale build → rotmat/setvec → matMul → applyMatrixLV → world-pos accumulate → propagate) was
  reached THREE ways: (a) direct native NodeXform::buildWithOffset (5 AI behaviors, fully native math),
  (b) direct native Engine::objMatrixCompose (4 SOP-intro callers, same fn via SUBSTRATE leaves
  0x80085480/84110/84470/51128), (c) rec_dispatch(0x800518FC)/guest_leaf + 8 direct substrate
  func_800518FC(c) sites — ALL falling through to the SUBSTRATE because NO override was registered.
- FIX: NodeXform::buildWithOffset is now the SOLE owner. Registered as the 0x800518FC override
  (EngineOverrides + psx_fallback-gated shard_set_override, bare trampoline — buildWithOffset mirrors
  its own 32-byte frame internally, like buildAxis) so (c)'s callers go native. Deleted
  Engine::objMatrixCompose + its engine.h decl; redirected its 4 SOP-intro callers to
  c->mRender->mNodeXform.buildWithOffset (added render/render.h includes).
- GATE (strong): PSXPORT_MIRROR_VERIFY=0x800518FC over the dark-screen replay = 23,937 passes, ZERO
  mismatch — buildWithOffset is byte-exact to substrate gen_func_800518FC. objMatrixCompose was also
  byte-exact (substrate leaves == gen by construction), so all three were equal; the redirect + delete
  is provably equivalent. SBS-full 0-diff to f17370 post-consolidation. Logs: scratch/logs/
  mv_518fc_darkscreen.log, sbs_518fc_final.log.
- BONUS: this ALSO advanced native ownership — the (c) callers that were silently running the substrate
  body now run native (4 substrate-leaf deps removed from the path), MIRROR_VERIFY-proven equivalent.

## codemap --conflicts FALSE POSITIVE — FUN_8002AB5C (terrain display pass) (2026-07-15)
- NOT a duplication bug. Render::terrain (submit.cpp) + NativeScenePass::terrainRender (native_terrain.cpp)
  both attribute to 0x8002AB5C, but per issue #32 (2026-07-07) they are the READ-ONLY pc_render display
  pass (matrices computed in host memory, terrain_obj_matrix_host) — the SUBSTRATE 0x8002AB5C runs
  underneath and owns ALL guest writes (sway/IR0/GTE) by design. Neither native fn is a faithful
  guest-write owner; the authoritative flag comes from a stale pre-#32 override tsv entry. Leave as-is
  (a pc_render read-only overlay legitimately sharing the address with the substrate owner). Documented
  so the next --conflicts triage doesn't re-chase it.

## Native-but-UNREGISTERED leaves → substrate fallthrough (2026-07-15) — native-ization candidates
- Found while auditing the engine.cpp anim-cluster (after deduping animEnvInit + objMatrixCompose).
  Engine::animTick (FUN_8004190C) and Engine::walkStart (FUN_80054D14) are native-implemented and
  called DIRECTLY by some behaviors (c->engine.animTick / c->engine.walkStart, e.g. beh_sop_intro_pilot)
  — BUT they are NOT registered as overrides. So their rec_dispatch(0x80054D14)/callObj1(0x8004190C)
  callers (beh_actor_tomba_proximity_combat ×4, beh_a06_scripted_actor) fall through to the SUBSTRATE
  gen_func_*. Same gap FUN_800518FC had before it was wired this session — a split where direct callers
  run native and dispatch callers run emulated.
- CANDIDATE FIX (next unit, NOT done — new behavior-changing unit, deferred for review): register both
  as EngineOverrides + psx_fallback-gated shard_set_override (guest ABI: obj in r4; animTick returns 1
  in r2, walkStart returns 0/… per gen), then gate with PSXPORT_MIRROR_VERIFY=0x8004190C / 0x80054D14
  over a combat-exercising replay (dark-screen). If MV byte-exact → keep (dispatch callers native-ized,
  substrate dep removed); if MV mismatches → the native body has a latent divergence vs substrate for
  those callers' objects → investigate, do NOT register. walkStart has an early-exit (cur==mode → ret 0
  no frame) — confirm the override thunk reproduces it.
- TOOL IDEA (future): codemap could grow a `--substrate-fallthrough` mode — flag any address that HAS a
  native owner AND is rec_dispatch/guest_leaf-called but is NOT in the override/shard_set tables. That's
  the machine-detectable version of this gap (it's how FUN_800518FC's fallthrough hid). Not built yet.

## Phase-3 fallthrough native-ization — animTick/walkStart wired + gated (2026-07-15)
- Engine::animTick (0x8004190C) + Engine::walkStart (0x80054D14): native Engine methods registered
  NOWHERE, so rec_dispatch callers (beh_actor_tomba_proximity_combat, beh_a06_scripted_actor) + 5/9
  direct substrate func_<addr>(c) shard sites ran the emulated body while direct native callers ran the
  port. Wired via RegisterEngineAnimLeafOverrides (engine.cpp; single psx_fallback-gated thunk covers
  EngineOverrides + shard_set_override, boot.cpp:114).
- GATE: PSXPORT_MIRROR_VERIFY=0x8004190C,0x80054D14 over dark-screen — animTick 27969 passes, walkStart
  1 pass, ZERO mismatch; SBS-full 0-diff f14130. Both byte-exact to substrate. Log: scratch/logs/mv_anim_leaves.log.
- METHOD FINDING (reshapes Phase 3): a fallthrough is only MV-gate-able when its dispatch path is
  EXERCISED by an available replay. NodeXform::build (0x80051844) fires in NONE of dark-screen/hut-entry
  (its callers are beh_seaside_prox_substate / beh_visibility_gate_dispatch) — I registered it, SBS was
  0-diff, but MV never fired, so I REVERTED it (can't oracle-verify → don't ship, per USER's oracle-gate
  standard). Most of the 97 --substrate-fallthrough candidates are like this: blocked on Phase 5 scene
  coverage. Phase 3 proceeds only for combat/movement-path fallthroughs until Phase 5 extends reach.

## fps60 HUT-INTERIOR FLICKER — interpolated frames show the field exterior (2026-07-15, USER inspect)
- SYMPTOM: in the hut interior (sm[0x4c]=3) with the user config (wide + fps60), the picture FLICKERS
  between the correct hut interior and the field EXTERIOR every frame. Confirmed via `preseq` (dumps
  presented frames incl. fps60 interp): presented sequence alternates exterior (even frames, ~30k PNG)
  / interior (odd, ~20k). With fps60 OFF, the interior is stable (all frames ~52k interior). So fps60
  is the cause. Repro: AUTO_SKIP → right34 → up185 (enter hut, sm[0x4c]=3) → `preseq 14 <dir>`; view
  even vs odd frames. Frames: scratch/screenshots/hut/seq/p0000.png (ext) vs p0001.png (int).
- ROOT-CAUSE HYPOTHESIS: the fps60 double-buffer (mCur/mPrev in game/render/fps60.cpp) captures the
  FIELD render pass, but the hut sub-scene overlay (sm[0x4c]==3, drawn via the full OT walk added in
  #49 for the authored sub-scene) is NOT folded into the fps60 tier — so interpolated presents render
  the stale field (exterior). This is the "fps60 must be object-level inside the renderer" gap the USER
  flagged; the sub-scene path bypasses the fps60 capture. NEXT: trace how the sub-scene (sm[0x4c]==3)
  render integrates with Fps60::mCur/mPrev capture + tier1Render; the sub-scene OT walk needs to feed
  the fps60 buffers (or fps60 must present the sub-scene layer on interp frames too).
- WIDE (aspect=2) hut interior: CLEAN — sub-scene in a centered frame, clean black margins, no VRAM
  artifacts (scratch/screenshots/hut/wide_interior.png 560x240). No wide bug here.

## GATING STANDARD — MIRROR_VERIFY is NOT SBS-full (2026-07-15, regression lesson)
- A fallthrough native-ization (registering a native as a guest-address override) gated ONLY with
  PSXPORT_MIRROR_VERIFY can still break SBS-full. MV compares the fn's guest-RAM writes but NOT the
  dead guest-STACK region the substrate body descends into; SBS-full compares ALL RAM. The Trig::rsin
  override (c29e6696) passed MV 102k× yet diverged AUTO_SKIP SBS-full at f560 — substrate rsin descends
  sp-=24 + spills ra@+16 + calls func_80083EBC (own frame); native rsin mirrors none of it, so the
  stack bytes differed. Fixed by unregistering (85233941); the frame-mirrored overrides (animTick/
  walkStart via GuestFrame, buildWithOffset) held clean (AUTO_SKIP f21540, dark-screen f15180).
- RULE: gate every override native-ization with **SBS-full on a path where the address FIRES**, not
  just MIRROR_VERIFY. A native is only a safe override if it reproduces the substrate's full guest-stack
  frame (CLAUDE.md "MIRROR THE GUEST STACK") — a pure-function port (Trig) can't be an override without
  it; it stays native for DIRECT callers only (those run native on both SBS cores → no split).

## #2b New Game -> DEMO s48=3 is the s3 MENU (not the SOP narration) — build-ready
- **symptom:** selecting New Game on the title crashes pc_render `unimplemented native rendering ... stage=0x801062E4 sm[0x48]=3 overlay_sig=0x3C021F80`; user "cursor after New Game doesn't work / Circle goes back"
- **status:** fixed 2026-07-16 (s3MenuNative, data-driven; s3 pc-vs-psx RMSE 0.000)
- **cause:** DEMO sm[0x48]==3 is `Demo::s3` (0x801064E8), which UNCONDITIONALLY dispatches the s3 cursor
  sub-machine (0x80106AC4 = Demo::s3SubMachine) — that draws logos (FUN_80106690) + a 2-item menu
  (FUN_80106824(**param1=1**)) + cursor, exactly like the title (s2), just a second page. It is NOT the SOP
  intro narration (the SOP overlay 0x3C021F80 is merely resident from f6, even during the title menu; the
  actual narration plays LATER at GAME stage, handled by renderSopNarration). So #2b's producer is a MENU
  renderer, structurally identical to titleNative — do NOT route it to sceneNative/renderSopNarration.
- **fix:** build a native s3-menu producer (classifyScene: DEMO stage + sm[0x48]==3 -> new SceneKind).
  Geometry RE'd + validated against the reference (RAM dump scratch/bin/ram_s3menu.bin, ground-truth shot
  scratch/screenshots/s3menu_ref.ppm): logos = FUN_80106690 (same 2 sprites as title). Items = FT4 templates
  **0x90/0x91** at anchors (90,180)/(230,180); FUN_8007e1b8 vertex offset -> final top-left **(43,172)/(194,172)**,
  width=entry[10], height=entry[11] (position decoder VALIDATED: template 0x8e reproduces the title's known
  (50,172) w80 h16). Cursor = template 0x98 at table 0x80107704[param1=1] = {40,180} - 8 -> (32,172)/(172,172? verify), y=168.
  Selected item attr 0 (raw/bright), unselected 0x50 (dim). REMAINING PRECISE-RE STEP: the FT4 **UV/clut/tpage**
  fields of FUN_8007e1b8 (entry[4..7]=uv+clut, entry[0..3]) — decode carefully + calibrate on template 0x8e
  (must reproduce title item0 u=0,clut_y=509,tpage 0x1D) before trusting 0x90/0x91. VERIFY the built producer
  by pixel-diff pc_render-shot vs psx_render-shot at s48=3 (tools/perceptual.py / render_cmp.py) + SBS logic
  0-diff + no-crash. Best structure: own FUN_8007e1b8 + FUN_80106824 natively (data-driven), shared by title
  + s3 + s6, retiring titleNative's decoded constants.
- **refs:** game/scene/demo.cpp Demo::s2/s3/s2SubMachine/s3SubMachine; docs/native-render-2d-panel.md; portmap `newgame-sop-intro (#2b)`

## #5 SOP narration void beat renders ~black under pc_render — vortex object not drawing
- **symptom:** after New Game, the SOP intro (GAME stage, overlay 0x3C021F80) shows a mostly-black screen
  under pc_render; reference shows a large swirling vortex. pixel-diff at the void beat: pc 2.7% non-black
  vs psx 58.9%, RMSE 45.
- **status:** fixed 2026-07-16 (Render::narrationSwirlRender — native swirl producer; void beat 2.7%->59.1% coverage vs ref 58.9%, RMSE 45->20 = within the accepted native-3D band (field itself shows 61))
- **cause:** the frame is the SOP VOID BEAT (*(u8*)0x800BF9B4 == 5, sm4a==0 sm4c==0). renderSopNarration's
  void-beat guard is correct (black bg + object-pass only, terrain/scene-table/backdrop dropped), BUT the
  vortex object (node 0x800FBA68) does not render through sceneNative's object pass under pc_render — so the
  screen stays ~black. The prior #5 note ("caption text 2D pending") UNDER-STATED this: the whole vortex is
  missing, not just text.
- **narrowed (2026-07-16):** the vortex node IS walked — it's in HEADS[1]'s chain (5 nodes) and its gates
  pass (n+1=1 visible, n+8/n+9=0x0F = 15 render commands). fieldObjectsRender -> perObjFlush IS called on
  it and submits all 15 geomblks (cmd+0x40) through the native gt3gt4 path. So the walk/emit is NOT the
  problem — the vortex's geomblks go through gt3gt4 but produce ~no visible output. The bug is in the PRIM
  EMISSION: gt3gt4's handling of the vortex's specific prims (likely a semi-transparent / special prim type
  or a projection issue — the swirl is a large blended effect). Node +0x0c reads 0x00030003 (packed, not a
  geomblk pointer), consistent with a non-standard render-command layout.
- **fix:** (not done, DEFERRED — render fix behind Job #1 per CLAUDE.md) inspect the 15 geomblks' prim
  opcodes + blend flags; find why gt3gt4 emits them invisibly (transparency not blended? projected
  off-screen? unhandled prim). Verify by pixel-diff pc-vs-psx at a fixed void-beat frame. Reaching true
  free-roam (#3b) is BEHIND this narration in the newgame path. Needs USER eyeball for the animated result.
- **refs:** render_walk.cpp renderSopNarration/sceneNative void-beat guard; docs/native-render-rebuild.md #5
- **DEEP DIAGNOSIS (2026-07-16, autonomous):** ruled out, in order — (1) walk/cull: vortex IS walked +
  perObjFlush submits all geomblks; (2) projection-size: prims project x[-766..465] y[-442..273], they
  COVER the whole screen + beyond; (3) backface cull: disabling `area<=0` in submitPolyGt3Native → no
  change; (4) semi-transparency: prims are OPAQUE (code 0x34, semi bit clear); (5) engine_shade_face:
  disabling it → no change; (6) shader texel-0: tritex.frag already `discard`s texel==0 correctly.
  REMAINING ROOT CAUSE: pc draws a bright CENTRAL core + BLACK edges; psx draws bright core + dim-purple
  VARYING (textured, mean~18 stdev~20) edges. Same prims, same tpage 0x19/CLUT. So pc SAMPLES texel-0
  (transparent→discard→black bg shows) at the edges where psx samples dim-purple — i.e. the vortex texture
  content in the VK VRAM SNAPSHOT (u_vram, sampled by tritex.frag) differs from the guest VRAM gpu_native
  reads for psx_render. Hypothesis: a vortex texture-upload path (MoveImage/DMA during the narration) is
  not mirrored into the VK VRAM image. NEXT: dump+compare VK VRAM vs guest VRAM at tpage 0x19 (~576,256);
  if the texture is absent/stale in the snapshot, fix the upload mirror. Deep GPU-backend/VRAM-sync work.

## High-res (ires) looks blurry — it's supersample-to-native, not present-at-high-res
- **symptom:** enabling high-res (internal-resolution / ires mod) makes the image look BLURRIER/softer than
  native, not sharper. USER report 2026-07-16.
- **status:** fixed 2026-07-16 (unified present-at-high-res; verify on-window by USER eyeball)
- **cause:** the ires path renders the 3D world into a scaled target (s_ires_color, e.g. 4x) but
  render_geom (gpu_gpu.cpp) BOX-DOWNSAMPLES it back to the native 320x240 VRAM (s_vram_tex) via
  ires_downsample.frag, and the present pass samples that NATIVE VRAM up to the window (present.frag,
  s_samp_nearest). So ires is SSAA-to-native: the high-res detail is averaged away before present, and the
  box-average softens edges — so more ires = softer output at the SAME ~240-line screen resolution. The
  scaled target itself is sharp (prior `iresdump` proof); it's discarded at the downsample.
- **fix (done):** ONE unified render path (USER 2026-07-16: "just one render path, behavior shouldn't
  differ between ires levels"). render_geom now renders EVERY band (2D_BG/3D/2D_FG) into a single composite
  C at the current scale (C = s_vram_tex @1x, s_ires_color @>1x) and present() samples C directly to the
  window — high-res is genuinely crisp, no downsample-to-native for present. Deleted the whole SSAA
  apparatus (seed blit, s_ires_bg_snap, the bug#55 coverage-mixing downsample); ires_downsample.frag is now
  a plain box used ONLY for the headless `shot`/VRAM readback. The ires level changes only the target SIZE.
  VERIFIED headless: @1x title RMSE 0.000 + void-3D RMSE 20.24 (identical to pre-rework = no regression);
  @4x renders into the 4096x2048 composite, shot box-downsamples to match (void 19.07, title 0.00), no
  crash; SBS gameplay still 0-diff (render is host-only). On-window crispness = USER eyeball (windowed,
  ires>1). See [[oracle-not-ground-truth-for-render]].
- **refs:** runtime/recomp/gpu_gpu.cpp render_geom (band 1/2/3, ires downsample ~L826) + present (~L840);
  shaders_gpu/ires_downsample.frag, present.frag

## fps60 2D flicker (#67) — blanket "tier1 owns RQ_WORLD" dropped guest-time drawables from interp presents (2026-07-16, RESOLVED)

- **symptom (USER)**: "2D things flicker at fps60" — gems/flames/pickups/weapon quads strobe at 30Hz.
- **cause**: unified-path step 2b made `Fps60::isTier1Owned` own ALL of `RQ_WORLD`, so the interp present
  skipped every world prim in mRqCur assuming tier1Render's re-run reproduces them. But RQ_WORLD has
  GUEST-EXECUTION-TIME producers the display-pass re-run never re-emits: the guest-OT obj-depth billboard
  walk (gpu_native.cpp is3d/objz) and the #65 dual-emit records (rq_push_ft4_record from billboardEmit +
  quad_rtpt_submit). They drew on real presents only → 30Hz flicker. Same class as the #54 RQ_BACKGROUND
  blanket exclusion.
- **fix** (game/render/fps60.cpp): tier1-own only what the re-run reproduces — exact discriminator is
  `has_xyf` (every re-run prim goes through drawWorldQuad → has_xyf=1; every guest-time record is
  screen-space → has_xyf=0, presented VERBATIM from mRqCur on both presents). Also: ineligible frames
  (hut) no longer consult the stale mSink nor skip owned prims (the #50 class); tier1Render mirrors the
  real frame's world gates (Render::worldVoidBeat / fieldAreaInit — factored to one definition each in
  render_walk.cpp). preseqobj log emits `scene=` (has_xyf) again so preseqobj_check.py skips rebuilt mesh.
- **verify**: headless field walk (fps60=1, `preseq 24` + `debug preseqobj`): key=0 RQ_WORLD prims on ALL
  24 presents (interp included; pre-fix absent on interp); preseq_flicker.py PASS 0/4 bands under camera
  pan; preseqobj_check 402→7 flags — the 7 are STALL-STEP on the #65 dual-emit billboards (drawn on both
  presents at frame-N position: 30Hz stepping, no flicker). Residual by design until each guest-time
  emitter becomes a display-pass producer deriving from game state (the REDIRECT doctrine / USER
  principle: real and interp frames identical derivation, lerp only in the inputs).
- **refs**: game/render/fps60.{h,cpp} isTier1Owned/present_vk/tier1Render, render_walk.cpp
  worldVoidBeat/fieldAreaInit, render_queue.cpp preseqobj scene=, bug #67, scratch/logs/bug67_gate_s.log

## billboardEmit particles now a DISPLAY-PASS producer — dual-emit deleted, true fps60 interpolation (2026-07-16, #67 RE work)

- **why**: USER principle — real and interpolated frames must be MADE identically, both derived from
  game state. The #65 dual-emit pushed billboardEmit's GTE-computed SXYs verbatim (guest-execution-time
  picture), so the fps60 interp present could only replay them → 30Hz stepping (7 stall-step tracks on
  the windmill/contraption nodes 800EDF38/800EDE28/800EDDA0/800EDD18 after the #67 flicker fix).
- **RE basis (all already native/RE'd — no new Ghidra needed)**: corners = rect from particle p+14/15
  (s8 offsets) × p+10/11 (w,h), all ×5, z=0 (func_8003B220 = hitbox.cpp's mislabeled
  `hitbox_build_3b220`); rotation = MAT_OUT (scratch BUF+0x40: rotZ(node+90), compose2 ∘
  seedBlock(node+122/124/126)); anchor = node+46/50/54; **CAM2 = scratchpad 0x1F8000F8 = EXACTLY the
  view matrix Fps60::sceneCam reads** (the perobj_billboard header's "main-RAM 0x800C0000" note was
  stale — corrected); GTE compose: CR rot = MAT_OUT.R, CR trans = CAM2·anchor + CAM2.t; material =
  particle uv words + node+92/node+13 patches (already mirrored natively in billboardEmit).
- **change**: billboardEmit records one host-side `Render::BbRec` per emitted particle (corners,
  MAT_OUT rotation, anchor, RESOLVED material words) — `rq_push_ft4_record` dual-emit DELETED
  (break-first). New `Render::billboardsRender` (perobj_billboard.cpp), called at the end of
  fieldObjectsRender (field + hut + tier1 interp re-run all reach it): float-projects each record
  through the sceneCam choke (fps60-lerped) and emits drawWorldQuad-convention RQ_WORLD prims
  (has_xyf=1 → tier1-owned, dbg_node=node = real identity). At the interp re-run (mObjOverrideOn)
  each record component-lerps against the previous frame's record keyed by particle addr
  (mBbRecsPrev, rotated in present_vk like mObjCur) — anchors AND per-particle animation interpolate.
  Records reset per logic frame in native_boot after frameUpdate (present consumed them).
- **verify**: SBS-full 0-diff BOTH legs (combat f7620, watch_cut f27600 — capture is host-only, guest
  packet emission untouched); headless field walk fps60=1: preseqobj gate PASS 0 flagged (was 7
  stall-step), preseq_flicker 0/4 bands; screenshot bug67_native_bb.png — gems/sprites/contraption
  render correctly via the new producer.
- **residual / next emitters**: QuadRtptSubmit::submitQuad (weapon-class quads) still dual-emits its
  GTE SXYs (rq_push_ft4_record's remaining caller) — same display-pass treatment applies; then the
  0x80039F4C score strip + the otattr census remainder (docs/fps60-rework.md REDIRECT list).
- **refs**: game/render/perobj_billboard.cpp (BbRec capture + billboardsRender), render.h,
  render_walk.cpp (fieldObjectsRender tail), fps60.cpp (bbSwapPrev), native_boot.cpp (bbFrameReset).

## submitQuad quads (flame/rope classes) now display-pass — composed-CR camera factorization (2026-07-16, #67 RE work cont.)

- **probe first (`debug quadcr`)**: at the seaside field the live submitQuad caller is the A00-overlay
  emitter (ra=0x80134168/0x801341CC, node 0x800F1008) under COMPOSED CRs (≠ scene camera) — B704 and
  case188 do not fire there. So a camera-only float re-projection was not enough.
- **mechanism — factor the composed CR against the scratchpad camera** (pure at emit time; per-object
  composes touch only the GTE regs): CR = cam∘obj, tr = cam·pos + cam.t ⇒ objR = camᵀ·CR/4096,
  objT = camᵀ·(tr − camT). `Render::WqRec` captures node + per-node seq + model corners (the xf words)
  + factored world transform + resolved material words. `billboardsRender` re-composes with the
  (fps60-lerped) camera: corner_view = Rcam·(objR·corner + objT) + Tcam — exact at the endpoints for
  ANY CR content, world-glued in between, ONE mechanism for every submitQuad caller class (no
  per-emitter RE needed for the picture). Per-record prev/cur lerp keyed (node, seq).
- **break-first**: `rq_push_ft4_record` DELETED entirely (render_internal.h — submitQuad was its last
  caller). Both record kinds emit via a shared `emitRecQuad` that decodes texpage/clut RAW from the
  record words with neutral tw/da (GpuState's live s_tp_*/s_da_* are stale at display time — latent
  hazard in the first BbRec emit, fixed here).
- **verify**: SBS-full 0-diff both legs (combat f8220, watch_cut f27840); fps60 field gate PASS
  (preseqobj 0 flagged, preseq_flicker 0/4); default-config 30fps shot renders identically.
- **refs**: quad_rtpt_submit.cpp (quadcr probe + WqRec capture), perobj_billboard.cpp
  (emitRecQuad + WqRec loop), render.h (WqRec), render_internal.h (helper deleted), docs/config.md quadcr.

## FUN_80039F4C text-label renderer OWNED (Render::textLabelEmit) + BbRec lerp removed (2026-07-16, #67 RE work cont.)

- **RE** (scratch/decomp/quad_emitters.c + textlabel.c; ground truth generated/shard_1.c): 39F4C = the
  per-character 3D TEXT-LABEL renderer (renderWalk case 0x8003C0E8). FUN_8003F174 first (mesh pass —
  loads CR0-7 DIRECTLY from the PRE-COMPOSED matrix at cmd+0x18, a different cmd layout from
  cmdListDispatch's camera∘object compose), then per char: glyph template V(-3,-7,-1)(5,-7)(-3,9)(5,9)
  z=-1 in the guest stack, UV from FUN_80039E80 (u=char*8, v=((char+32)>>5)*16+8; space skips),
  projected by FUN_8003F7D8 under SetRotMatrix/SetTransMatrix(cmd+0x18) — **FUN_80084660/84690 ARE
  libgte SetRotMatrix/SetTransMatrix** (the render_walk_dispatch.cpp "pool-span markers" note was a
  mis-RE; also proves case188 + B704 load the PURE CAMERA at 0x1F8000F8 before projecting). Text =
  "Clear"+suffix (node+3==2, clut 0x7C7F) or string table 0x800A33C8[idx*12]+4 (clut 0x7DFF); code
  0x2D, tpage half 0x1F; packet pool-bumped 40B + OT-linked at otz-1.
- **port** (game/render/text_label.cpp): faithful orchestrator (GuestFrame<120,7> from abi_extract,
  register-faithful r16-r21, all callees substrate) + WqRec capture per surviving glyph (corners =
  template, transform = cmd+0x18 matrix factored via the shared wq_read_matrix/wq_factor_world
  helpers, render_internal.h). Replaces the RenderObserver wrap. **SBS caught a real draft bug**: the
  string-table read was mis-based at 0x800A33CC (+4 double-applied) → wrong string → one extra packet
  → pool-ptr diff at f190. Fixed → combat f6810 + watch_cut f25260 both 0-diff.
- **BbRec per-particle lerp REMOVED** (USER: "gems rendered at two different places between real and
  interpolated frames"): effect sub-lists reuse/walk particle addresses every frame, so a
  particle-addr-keyed lerp blends DIFFERENT sprites' positions — no stable cross-frame identity
  exists for these. Particles now draw at their own frame's state under the LERPED camera
  (world-glued, no ghosting; animation steps at logic rate — what the state says). WqRec lerp stays
  (stable node/seq keys — USER: "the weapon and the rope looks spectacular").
- **verify**: SBS-full 0-diff both legs; fps60 field gate PASS (preseqobj 0 flagged, bands 0/4).
- **refs**: text_label.cpp, render_internal.h (wq_read_matrix/wq_factor_world), quad_rtpt_submit.cpp
  (refactored to the shared helpers), perobj_billboard.cpp, render_observer.cpp (39F4C wrap removed),
  game_tomba2.cpp (text_label_install), render.h.

## BbRec capture sources = live CRs, not fixed scratch (C788-class rotation bug) (2026-07-16)

- FUN_8003C5F8/C788 RE'd (scratch/decomp/fx_leaves.c): both are billboardCompose VARIANTS ending in
  the owned billboardEmit — C5F8 composes into BUF+0x40, **C788 into BUF+0x20** — so the BbRec capture
  reading rotation from the fixed MAT_OUT (BUF+0x40) address grabbed the WRONG matrix for C788-class
  particles. Fixed: rotation = live GTE CR0-4, anchor = camᵀ·(CR5-7 − camT) (exact for every compose
  variant; billboardEmit's RTPT/RTPS never touch the CRs). C5F8/C788 particles are thereby fully
  display-pass covered WITHOUT owning the compose bodies (their func_8003C8F4 call dispatches to the
  native billboardEmit). 8011BE5C (render-mode-4 particle leaf) is overlay code not resident in the
  ram_sea dump — still unowned, invisible under pc_render if mode 4 ever fires (census remainder).
- gates: SBS-full 0-diff both legs (combat f8070, watch_cut f26910); fps60 field gate PASS.

## Widescreen: native 2D centered via ONE queue layout authority + emitter culls widened (2026-07-16, USER request)

- **centering**: native UI producers (Panel, Font/dialog, gauges, menus) pushed 4:3 coords and sat
  left-anchored in wide. The layout now lives in RenderQueue::emitOrQueue (the one funnel): for
  non-DEPTH prims under gpu_gpu_wide_engine, HUD/overlay shift +margin (centered, native size),
  dbg_node==0 RQ_BACKGROUND stretches to [0,ww). EXEMPT: the guest-OT walk (s_ot_2d_only —
  its sites still apply ws_2d_local_x with the #38/#52 fill rules) and backdropRender's REAL wide
  tiles (kBackdropDbgNode). 4:3 margin==0 → no-op. Verified: prompt panel + pause menu centered
  at aspect=1 (scratch/screenshots/wide_field/menu/cull.png).
- **cull widening** (bug #61 partial): billboardEmit / QuadRtptSubmit::submitQuad / textLabelEmit
  ran the stock all-corners-X>=320 drop, culling their content out of the RIGHT wide band (under the
  wide FOV OFX=nw/2, SX spans [0,nw)). Widened to the wide width under gpu_gpu_wide_engine — the
  submit.cpp submit_xmax precedent (later-119); a sanctioned wide-mode guest deviation (SBS legs
  force 4:3: combat f6360 + watch_cut f20760 both 0-diff). Object-level cull already has the
  engine margin (cull.cpp CULL_FAR_MULT / fov margin). Remaining #61 candidates: left band comes
  free (never 320-gated); guest-side per-list visibility byte for objects only partially re-included.

## fieldObjectsRender TYPE-CORRECT routing — pre-composed-matrix classes no longer double-camera'd (2026-07-16)

- **cause**: perObjFlush composed camera∘(cmd+0x18) for EVERY cmd-bearing node, but cmd+0x18's MEANING
  is class-specific: the perObjRenderDispatch family stores an OBJECT rotation; the F174 family
  (renderWalk table 0x80014DB8 — the render_walk_dispatch banner's 0x800104B8 was a stale wrong
  constant, corrected there long ago at :74 — types 1 and 4=text-label) stores a PRE-COMPOSED
  camera∘object MATRIX. perObjFlush applied the camera twice to those; other types (billboard
  composers, unowned overlay renderers) got a guessed-transform mesh draw.
- **tables RE'd from the live RAM dump** (scratch/decomp/census_ram.bin):
  0x800F2624/renderWalk@0x80014DB8: mesh {0,15} · pre-composed {1 (F174), 4 (39F4C label)} ·
  billboards {16..20} · overlay customs {3,5,6,7} · vtable {32} · no-op rest.
  0x800F2738/objListWalk4@0x80015000: mesh {0,15, 1(+B704 beams)} · billboard {16} · vtable {32}.
  0x800FB168: table not yet RE'd — flush-all behavior kept there.
- **fix**: fieldObjectsRender routes by (list, type): mesh types → perObjFlush; pre-composed types →
  new perObjFlushPreComposed (factor cmd+0x18 via wq_read_matrix/wq_factor_world, re-compose through
  projComposeObjectHost — camera exactly once, fps60-lerped via the sceneCam choke); all other types
  draw NOTHING natively (USER: "don't render any unowned things") instead of a wrong-transform mesh.
- **verify**: field shot identical (typeroute_field.png — nothing visible lost at seaside); SBS-full
  0-diff both legs (combat f6180, watch_cut f20040).

## Pre-existing render-port port_check FAILs (byte-divergence, deferred) [2026-07-17]

- **Status:** KNOWN, deferred (render path). Surfaced by `tools/port_check.py --all`.
- **FAILs (guest mem-store-width divergence vs gen body):** `Render::objListWalk1` (0x8003BB50),
  `objListWalk2` (0x8003BCF4), `objListWalk3` (0x8003BF00), `objListWalk4` (0x8003EEC0) in
  objlist_walk.cpp; `OverlayGroundGt3Gt4::gt3` (0x8013FB88); `Render::cmdListDispatch` (0x8003CDD8).
- **Triage needed (when owning render nodes):** decide per-port whether it is an EXECUTION-path port
  that MUST byte-match guest RAM (then the store-width mismatch is a REAL SBS divergence to fix) or a
  pc_render REBUILD that legitimately diverges from the GP0-packet gen body (then port_check FAIL is
  expected and the ORACLE marker should be dropped/relaxed for it). Do NOT mass-fix — assess intent first.
- **Not caused by** the 2026-07-17 abi_extract dead-code/sibling-latch fixes (those only affect post-
  return code; these FAIL mid-body).
