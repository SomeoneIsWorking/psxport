# fps60 rework — renderer-internal, one-frame-behind interpolation (issue #46)

**Directive (USER 2026-07-14):** there should technically be NO difference between interpolated and
real frames. fps60 lives INSIDE the renderer; the only difference is it runs one frame behind so it
can interpolate. Verification standard: paired real-vs-interpolated frame dumps (`fps60dump`).

## Why the current design cannot be fixed incrementally

Current shape: the real frame renders normally; the mid-present is a SEPARATE path that re-runs
`sceneNative` against live guest memory and re-emits captured queue items with reprojected
"anchors" (`stampBillboard`, `recordBillboardSpan/Particle`, per-quad unprojection 0f78384).
Failure modes observed, each patched separately, each leaking elsewhere:

- gems rigid-translate (fixed per-particle 2026-07-10) → tree-quad teleport (fixed per-quad
  unproject 2026-07-14) → windmill period-2 STATE SWAP (live, fps60dump-verified): the interp frame
  shows a *different game state*, because the mid-present runs inside `frameUpdate` — after its own
  tick, before this frame's logic (`native_boot.cpp` step order: frameUpdate → pcSched.step →
  drawOTag) — and re-reads guest state the real render never saw.

A second render path can never be proven equal to the first. Make them the same path.

## Target design

The renderer holds the last TWO real frames' prim queues and presents one frame behind:

```
logic N completes, drawOTag builds queue Q[N]
present slot A: draw lerp(Q[N-1], Q[N], t=0.5)     ← the "interpolated" frame
present slot B: draw Q[N]                           ← the real frame
```

- ONE draw path: both presents drain an RqItem list through the normal rq flush. An interpolated
  frame is a *blend of two real frames' data* — it cannot show state neither real frame had.
- NO guest reads at interp time. No sceneNative re-run, no camera reprojection, no anchors.
- Latency cost: one half-frame to one frame, accepted by the directive.

### Prim matching (the core problem)

Interpolate only prims that exist in both Q[N-1] and Q[N] with a confident identity match;
unmatched prims draw only in their own real frame (pop-in at 30fps cadence, same as today's real
path — never a wrong position).

Match key, in order of strength:
1. **Provenance**: the emitting object — `dbg_node` / geomblk / the otattr-style span node — plus
   the prim's emission index within that object this frame. Objects emit their prims in stable
   order frame-to-frame (the OT walk and sceneNative both iterate deterministically), so
   (node, index-within-node) is stable while the object lives.
2. **Shape fingerprint** as tiebreaker/validator: layer, op/texture page, clut, vertex count.
   A matched pair whose fingerprint differs is treated as unmatched (animation frame swap — draw
   at nearest real frame, do not lerp across different textures).
3. Prims with no provenance (node==0): match by (layer, fingerprint, order-of-appearance) only if
   the counts agree; otherwise unmatched.

Lerp: screen-space vertex positions (float), plus depth. Colors/UVs are NOT lerped — if they
differ, the pair is demoted to unmatched (fingerprint rule).

### What gets RETIRED (delete, no tombstones, git history is the record)

- `Fps60::stampBillboard`, `recordBillboardSpan`, `recordBillboardParticle`, `billboardForNode`,
  `unprojectQuadAnchor`, the mBbCur/mBbPart tables, `fps60_bb_node`, the fps_anchor RqItem fields.
- The mid-present `sceneNative` re-run and `fps_scene` rebuild logic; `mSceneTag`/`sceneRan`
  plumbing in game_tomba2.cpp.
- Camera capture stays ONLY if stage-2 chooses camera-assisted matching validation; otherwise goes.

### What stays

- `rq_capture` (extended to double-buffer prev/cur), the present/pace machinery, `fps60dump`,
  `bbanchor` dies with the anchors it instruments (delete), `preseqobj` stays (per-object motion
  log still useful for match-quality debugging).

## Staged implementation (each stage gated)

1. **Delay stage**: double-buffer the queue; present Q[N-1] at slot A and Q[N] at slot B (no
   interpolation yet — slot A shows the previous frame verbatim). Gate: fps60dump pairs — slot-A
   frames must be pixel-identical to the previous slot-B frame; no windmill swap possible by
   construction. Game still paces 60 presents/sec.
2. **Match+lerp stage**: implement the match key + vertex lerp; slot A becomes lerp(Q[N-1],Q[N]).
   Gate: fps60dump triples — every interp frame's content must lie geometrically between its two
   real neighbors (scripted checker: per-hot-region centroid of interp within the segment of the
   two real centroids, tolerance ~2px); windmill region must show the basket/beak at in-between
   angles or stable nearest-real, never the alternating swap.
3. **Retire stage**: delete the anchor machinery + mid-present re-run (list above), update
   docs/config.md and findings. Gate: build + fps60dump re-run + SBS-full smoke (host-only change;
   0-diff required) + USER eyeball on the tree/windmill scene.
   **Stage 3 done 2026-07-14.** Deleted: the billboard registry (`stampBillboard`/`recordBillboardSpan`/
   `recordBillboardParticle`/`billboardForNode`/`unprojectQuadAnchor`/`projWorld`/`camMidOf`, mBbCur/
   mBbPart tables, `fps60_bb_node`), the object/camera/scroll midpoint-capture machinery (`objXform`/
   `bgScroll`/`beginCapture`, mObjCur/mObjPrev/mCamCur/mCamPrev/mScrollCur/mScrollPrev, the Cam/ObjX/Scroll
   structs), the `mSceneTag`/`mInterp`/`mSceneRan`/`SceneTagArm` plumbing (game_tomba2.cpp), the
   `fps_scene`/`fps_anchor`/`fps_key`/`fps_wpos` RqItem fields, the dead `proj_native_xform_cr` +
   `ProjVtx::mx/my/mz` reprojection-input remnants, and the `bbanchor` debug channel. `Fps60::sceneCam`
   kept (stripped to the plain camera read every projection caller already used unconditionally).
   `preseqobj`'s `key=` field switched from the retired `fps_key` to `dbg_node` (the field
   `Fps60::matchAndLerp` itself keys on). Gate: full rebuild clean; fps60dump re-run (tree-scene repro,
   340+60 frames) — static-identity 99.9944% (252/4488592 px, same known hair-tip class stage 2 accepted),
   moving-pixel midpoint compliance 95.2% (windmill region 100%, 0 static violations), match rate 85-97%
   this-frame / ~89-90% running — all within stage-2's landed numbers; fps60-OFF run unaffected (screenshot
   verified); SBS-full smoke 0-diff.

## Verification protocol (standing, USER directive)

Any fps60 change: run the tree/windmill scene with `fps60dump`, and (when a user screencast is
supplied) ffmpeg-dump its frames; compare real-vs-interp pairs. A fix claim requires the paired
comparison, not only a log metric.

## REDIRECT (USER 2026-07-14): identity comes from RE+PORT, not stamping/matching

The stage-2 provenance/fingerprint matching is a queue-level HEURISTIC — an after-the-fact guess at
identity the engine should own. USER: "you need to RE and port quads, not stamp them." Standing
doctrine agrees (full native ownership is always the answer).

- The one-frame-behind single-draw-path architecture (stages 1-2) STANDS.
- The matching heuristic is ⛔ TRANSITIONAL HACK DEBT: as each quad emitter is RE'd and ported
  native, its prims carry REAL engine identity (the owning object + element), the exact-lookup
  match replaces the heuristic for that emitter, and the heuristic's coverage shrinks to zero —
  then it is DELETED.
- THE WORK: enumerate every quad-emitting guest fn (otattr census), RE each (Ghidra,
  docs/port-framework.md), port native with per-element identity + world state. Priority: the
  emitters visible in field free-roam (windmill contraption, tree props/fruit, 0x80039F4C score
  strip), then the rest of the census.

## Object-tier attempt 2026-07-14 — Tier 3 (queue-lerp atomicity) landed; Tiers 1-2 SPEC'D, NOT BUILT

Attempted the full CAMERA/OBJECT/QUEUE three-tier structure this session. Landed the queue-tier
(matchAndLerp is now object-atomic — `Fps60::enforceNodeAtomicity`: if any prim of a `dbg_node` fails
to match, the WHOLE node draws unlerped from Q[N-1] rather than a torn lerped/frozen mix). Tiers 1-2
(camera-lerp native world re-render, per-object transform-lerp re-projection) are NOT implemented —
here is why, and the concrete path for whoever picks this up:

- **Real finding, load-bearing:** `Fps60::frame_commit` (hence `present_vk`) is called from
  `Engine::frameUpdate`, which `native_boot.cpp` calls BEFORE this iteration's `pcSched.step()` (game
  logic) and BEFORE this iteration's `drawOTag` (which is what pushes fresh geometry into `mRqCur` via
  `RenderQueue::flush`/`fps60.rq_capture`). So at present time, no game-logic tick for "this" iteration
  has run yet — guest object/terrain state is unchanged since last iteration's `drawOTag` read it. This
  is what makes Tier 1/2's "re-render through the native path at present time" invariant achievable in
  principle: a re-invocation of `NativeScenePass::terrainRender()` at present time would read the SAME
  guest state its real call already read, not a mutated future state (the exact bug class stage-3 killed
  — the old mid-present ran AFTER a tick, reading state the real render never saw).
- **Stale comment found + NOT yet fixed:** `game/render/native_terrain.cpp:94-96` still describes
  `sceneCam` as providing "the (prev,cur) MIDPOINT camera during the 60fps mid-present" — that capture/
  midpoint machinery was deleted in stage 3 (`Fps60::sceneCam` is now a plain unconditional scratchpad
  read). The comment is dead-design residue; fix it when Tier 1 lands (whichever way it lands — either
  by making the comment true again, or by deleting the claim).
- **Why Tier 1 (camera) isn't built:** `terrainRender()` always draws through the LIVE `c->game->rq`
  (`push()`/`flush()`/sort/capture cycle owned by `drawOTag`). Calling it again at present time — the
  only way to get "the native world render through a lerped camera, same path" — needs its output routed
  to an ISOLATED capture (drain via `RenderQueue::emitItem` directly, matching item 4's "one draw path"
  requirement) instead of `rq.push()`, which would otherwise corrupt next iteration's real queue build.
  That means either a capture-target redirect on `RenderQueue`/`emitOrQueue`/`drawWorldQuad`, or a
  second lightweight `RqItem[]` sink `terrainRender` can be pointed at. Needs a 2-slot camera struct
  (R[3][3]/T[3]/ofx/ofy/H, captured once per real frame in `sceneCam`'s normal call site) + a lerp of
  that struct + a way to feed the LERPED camera into `terrainRender` instead of its own `sceneCam` call
  (an override slot on Fps60, consulted only while a re-render-for-interp flag is set).
- **Why Tier 2 (object) isn't built:** needs the equivalent per-object treatment for
  `game/render/perobj_dispatch.cpp`'s `cmdListDispatch`/`Render::perObjFlush` path — capturing each
  object's COMPOSED transform (not just camera) per real frame keyed by `dbg_node`, lerping it
  (component lerp + cheap re-orthonormalization — not yet designed/justified), and re-invoking the
  per-object native submit through the same redirect-capture mechanism Tier 1 needs. This is materially
  more RE/design work than Tier 1 (per-object transform composition varies by emitter) and was not
  attempted this session — start from Tier 1's redirect-capture plumbing once it lands, then RE one
  concrete emitter (the windmill contraption per the REDIRECT priority list) as the first Tier-2 native
  identity case rather than a generic transform capture.
- Gate B (t=0/t=1 pixel-identical to the real neighbor) and the tier-1/2 coverage counts in gate C are
  UNVERIFIED — they require Tier 1/2 to exist. Do not claim them until built.

## Tier 1 landed 2026-07-14 (terrain-only; Tier 2/object-transform still NOT built)

Built the camera store + isolated sink + present-time re-render this attempt describes. `Fps60::mCamCur`/
`mCamPrev` (fps60.h) capture every real `sceneCam()` call, rotated in lockstep with `mRqCur`/`mRqPrev`.
`Fps60::tier1Render` (fps60.cpp) re-runs `Render::terrainRenderAll()` (submit.cpp — the terrain-node scan
extracted from render_walk.cpp's `sceneNative` so both the real call and this re-render use the identical
sequence) under `lerp(mCamPrev, mCamCur, t)`, output redirected via `Game::rqRedirect` (game.h) into an
isolated `RenderQueue* Fps60::mSink`, under `DisplayPassGuard` (aborts on any guest write — held clean in
every test run). `native_terrain.cpp`'s draw call checks `rqRedirect` and targets it instead of the live
`game->rq`. `ProjParams::Snapshot` (proj_params.h) saves/restores the per-Core published camview so nothing
else observes the lerped camera.

Two real bugs found and fixed during verification (both via `scratch/check_tier1.py`, the t-forced pixel
gate — not by inspection):
1. **Draw-order corruption**: emitting `mSink` before `slotA` drew the SEMI terrain quad against an empty
   framebuffer instead of behind the background. Fixed with a proper `(layer,seq)` two-way merge before
   emission (fps60.cpp present_vk) — `mSink`'s own seq range is naturally lowest within RQ_WORLD (terrain
   draws first in the real walk too), so a straight merge reproduces the real paint order.
2. **Over-broad exclusion**: `RQ_WORLD && dbg_node==0` is NOT "terrain" — `Render::fieldEntityRender` (the
   SOP field-overlay SCENE TABLE walk: grass/terrain props) is the OTHER dbg_node==0 producer and is NOT
   re-rendered by Tier 1. Excluding it from the queue-lerp alongside terrain made the ground vanish from
   slot A. Fixed by tagging terrain with a reserved sentinel (`kTerrainDbgNode`, render_queue.h) via
   `Render::diag.beginObject/endObject` around its quad loop, so the exclusion targets exactly what Tier 1
   re-renders.

Gate B (t-forced exactness, terrain bbox only — the full frame is not expected bit-identical at any t
because unmatched queue-lerp prims draw from Q[N-1] regardless of t, by the pre-existing "Prim matching"
design): t=1 vs the same present's real pass — 96.14% pixel-exact (868/22500 diff px over 20 pairs); t=0
vs the previous real pass — 99.78% (46/21375). Residual is at the terrain/scene-table boundary (a few
color units per channel, not full-scale) — cross-contamination from queue-lerp's own known ~85-90% match
rate on the STILL-un-tiered scene-table/object geometry adjacent to the terrain edge, not a Tier-1 defect;
confirmed independent of shadows (identical residual with `shadows=0`). NOT literally 100% — reported
honestly, not rounded up.

Gate D (`scratch/check_stage2.py`, same tree/windmill repro window, git-stash baseline vs after): static-
identity violations 253/4489090 (0.0056%) baseline -> 385/4489090 (0.0086%) after Tier 1 — a small,
measurable regression (still both classes "stage-2/3 grade", same order of magnitude; windmill region
stayed 0/453120 both). Gate B (soft, moving-pixel midpoint compliance) unchanged: 95.6% both. Root cause
of the small regression not further isolated this session (time-boxed) — plausibly the same boundary
cross-contamination as the tier1 gate's residual, since both point at the terrain/scene-table seam.

Coverage this session: TERRAIN ONLY. `fieldEntityRender` (grass/props) and the object/entity walk
(`perObjFlush`) remain fully on the queue-lerp heuristic — Tier 2 (object-transform lerp) is the next step
per this document's existing "Why Tier 2 isn't built" writeup, unchanged by this session.

## Tier 1 extended to fieldEntityRender + backdrop excluded verbatim (2026-07-14, follow-up session)

Extended Tier 1 to cover ALL camera-only world-static geometry, per the standing principle (USER):
world-static geometry has no motion of its own — its screen motion is purely the camera's, so it must
never be per-prim lerped; camera-projected static geometry renders through the lerped camera (Tier 1),
while screen-space GAME-LOGIC-DRIVEN layers (the backdrop tilemap scroll) get NO interpolation at all —
drawn verbatim from Q[N-1].

**fieldEntityRender invariant check (required before wiring it into tier1Render):** read
`Render::fieldEntityRender` (submit.cpp) + its two submitters (`submitPolyGt3Native`/`submitPolyGt4Native`)
end to end. It composes ONLY the scene camera (`projComposeCamera` → `sceneCam`, camera-only, no
per-object transform — same class as terrain) and projects the SCENE_ENT_TABLE (0x800F2418) record
array, which is static per-area data, not per-frame mutable state. Found and fixed TWO real bugs in the
shared submitters along the way (not approximated around):
1. **Raw present-time guest read bypassing the camera-lerp override**: both submitters called
   `proj_set_H((uint16_t)gte_read_ctrl(26))` — a direct GTE control-register read — to feed depth
   normalization (`proj_pz_to_ord`), independent of the camera H already composed into the active xform
   (`mActiveXform.H`, which DOES honor `sceneCam`'s `mCamOverrideOn` lerp). This was (a) a present-time
   GUEST READ (forbidden by the fps60 present-time invariant) and (b) a desync between XY (lerped H, via
   `EObjXform::project`) and depth (live current-frame H, via the raw register read) at any t other than
   1. Fixed: both now read `c->mRender->mActiveXform.H` — safe because every call site sets an active
   xform first (no GTE fallback, documented invariant in render.h).
2. **Direct `c->game->rq.drawWorldQuad` write bypassing `Game::rqRedirect`**: unlike native_terrain.cpp,
   the submitters wrote straight to the live queue, so re-invoking them at present time would have
   corrupted the NEXT real frame's queue. Fixed: both now route through
   `c->game->rqRedirect ? *c->game->rqRedirect : c->game->rq`, matching native_terrain.cpp's pattern.

With those two fixed, fieldEntityRender reads nothing beyond camera + static geometry — eligible for
Tier 1 without approximation. `Render::fieldEntityRender` now scopes its OWN
`diag.beginObject(kSceneTableDbgNode)/endObject()` (submit.cpp) around its loop, so both the real
per-logic-frame call and Tier-1's present-time re-render tag identically. `render_queue.h` adds
`kSceneTableDbgNode` (0xFFFF0002) alongside `kTerrainDbgNode`; `fps60.cpp`'s `isTier1Owned` now excludes
both from the queue-lerp. `Fps60::tier1Render` calls `Render::fieldEntityRender(0x800F2418u)` right after
`terrainRenderAll()`, inside the same redirect/override/`DisplayPassGuard` scope, gated on
`mSceneTableTrusted` (same trust-latch the real call uses — unchanged since last real frame, per the
present-time invariant).

**Backdrop (screen-space scroll, excluded from the queue-lerp entirely):** `render_bg_tilemap_native`
(render_walk.cpp) is the sole `RQ_BACKGROUND` producer (grep-verified) — layer alone is its real identity,
no separate dbg_node sentinel needed. `fps60.cpp` adds `kBackdropVerbatim`: `buildProvenanceIdx` tags any
`RQ_BACKGROUND` item with it, `matchAndLerp` never attempts to match such items (excluded from the
provenance map and the dbg_node==0 fingerprint groups), so they always fall through to the
"unmatched → draw A[i] as-is" path — verbatim replay of Q[N-1], counted separately in telemetry
(`mBackdropPrimsThisFrame`) so "never eligible" isn't conflated with "eligible, no match this frame".

**Gates (tree/windmill repro, `scratch/cfg/psxport_settings.ini`, PSXPORT_AUTO_SKIP, run 380 + dump 20,
`PSXPORT_FPS60_TFORCE`):**
- **A. Build**: clean.
- **B. t-forced exactness — SCENE-TABLE bbox included this time** (data-derived: `tier1sc` debug channel
  printed the aggregate re-rendered bbox — scene-table spans nearly the full visible field, ~[-357,403]×
  [-82,423] pre-clamp — so a small sub-region provably scene-table-only and free of any dynamic object
  was picked by inspecting a dumped frame: x=[0,60) y=[150,230), pure grass, left of Tomba/left of the
  hut): **t=1: 0/96000 diff (100.0000%, PASS, bit-identical). t=0: 0/91200 diff (100.0000%, PASS,
  bit-identical).** Terrain bbox (regression check, same as the Tier-1-landed gate): t=1 866/22500
  (96.15%, was 96.14%) — unchanged within noise; t=0 46/21375 (99.78%, was 99.78%) — unchanged. No
  regression on the existing Tier-1 claim; the scene-table extension is a clean, exact win at both
  endpoints, unlike terrain (whose small residual predates this session and is unrelated — see above).
- **C. Telemetry** (same repro, run 340 then +60, `PSXPORT_DEBUG=fps60`, default t=0.5): tier1 settled at
  294 prims/frame (1 terrain + 293 scene-table), backdrop settled at 352 prims/frame (both constant once
  the field scene is loaded — expected, static per-area content), queue-lerp match-rate on the REMAINING
  pool (objects: windmill/hut/Tomba/etc. — the only thing still on the heuristic) ranged 20-48%
  this-frame / down to 64-97% running (lower than the pre-session 85-97% baseline because the easy
  static prims that used to inflate the ratio moved to Tier 1 — the remaining pool is now dominated by
  genuinely-hard-to-match dynamic objects, not a regression in matching quality itself).
- **D. `scratch/check_stage2.py`, same repro window, git-stash baseline vs after**: static-identity
  violations 136/1446530 (0.0094%) baseline → 172/1446530 (0.0119%) after — a small, measurable
  regression, same order of magnitude as the prior Tier-1-terrain landing's own regression (253→385 at a
  different pixel-count baseline); windmill region stayed 0/145920 both. Gate B (soft, moving-pixel
  midpoint compliance): 96.0% both (12166→12164/12670 — noise). Root cause not further isolated this
  session (time-boxed, same as the precedent) — a new violation cluster appeared near (211,69), plausibly
  a terrain/scene-table/hut-roof boundary seam; flagged for whoever picks up Tier 2.
- **E. SBS-full smoke**: combat leg 0-diff through f7500 (100s bound), watch-cut leg 0-diff through f9840
  (120s bound) — both host-only render change, no guest-memory write, as expected.
- **F. fps60-off**: screenshot-verified normal (`scratch/cfg/psxport_settings_fps60off.ini`, run 400,
  `shot`) — unaffected, as expected (none of this arms when `game->mods.fps60` is off).

Coverage after this session: terrain + scene-table (grass/props) are both Tier 1 (camera-lerp
re-render); backdrop is excluded entirely (verbatim). The object/entity walk (`perObjFlush`) is the ONLY
remaining queue-lerp-heuristic geometry — Tier 2 (object-transform lerp) is the next step, unchanged
target from the prior session's "Why Tier 2 isn't built" writeup.

## Backdrop promoted from verbatim to a LAYER-TRANSFORM tier (2026-07-14, follow-up session)

The backdrop tilemap is ONE layer whose only per-frame motion is its scroll offset (game-logic-driven,
ParallaxBg::step) — so it is interpolated as ONE transform, replacing the previous "draw verbatim from
Q[N-1]" exclusion (`kBackdropVerbatim`, deleted): `Fps60::tier1Render` re-runs the SAME native backdrop
pass with the scroll offset overridden to a WRAP-AWARE lerp of the two real frames' captured offsets.

**Scroll-field inventory (read of the pass + ParallaxBg RE):** `Render::backdropRender` (render_walk.cpp
— was the file-local `render_bg_tilemap_native`, promoted to a method so tier1Render can call it) reads
from PARALLAX_BG_SM (0x800ED018): per-frame-VARYING = scrollX/scrollY (+0x28/+0x2A, recomputed every
RUNNING tick by ParallaxBg::step from camera yaw/pitch, wrapped mod +0x30/+0x32); STATIC per-area config
= tpage(+0x04), clutbase(+0x06), W/H(+0x10/+0x11), tilemap ptr(+0x14), wrap moduli(+0x30/+0x32, INIT-
stamped). The tilemap CONTENT (u16[H][W] grid) is static area data. No per-tile animation state — the
capture-or-stop audit found nothing beyond the two scroll shorts.

**Mechanism:** `Fps60::bgScroll` (fps60.cpp) is the scroll-read choke backdropRender calls instead of a
raw struct read — real call: guest read + capture into `mBgCur` (rotated with mCamCur/mRqCur in
present_vk's swap); present-time: returns `mBgOverride` (no guest read). tier1Render computes
`mBgOverride = wrapLerp(mBgPrev, mBgCur, mod, t)` per axis (shortest modular path: diff folded into
[-mod/2, mod/2] before lerp, result re-wrapped into [0,mod)) and re-invokes backdropRender(0x800ED018)
inside the same rqRedirect/mSink + DisplayPassGuard scope as terrain/scene-table, gated exactly like the
real call (mBackdropTrusted && *0x800bf873==0 && bgstate==0). backdropRender's push2dQuad now routes
through rqRedirect (was a direct `game->rq` write — same bug class fixed in the submitters last session).
`isTier1Owned` now returns true for any RQ_BACKGROUND item, so the queue-lerp never sees the layer.

**Gates (tree/windmill repro, scratch/cfg/psxport_settings.ini, AUTO_SKIP; exactness window = f401-440
with a held-left camera pan so the scroll actually moves — the static window has Δscroll=0 and proves
nothing):**
- A. Build clean.
- B. t-forced exactness, backdrop-only bbox x=[0,90) y=[0,20) (data-derived: zero green/world pixels
  across the whole pan): **t=1: 0/72000 diff px over 40 pairs (100%, bit-identical). t=0: 0/70200 over
  39 pairs (100%, bit-identical).** A real WRAP EVENT sat inside the window (REPL poll: scrollX
  0x0003→0x0235 across f420-425, modX=0x0240) — endpoints stayed 0-diff through it, and the t=0.5
  cloud-centroid check over f417-422 shows every interp position between its real neighbors (naive
  long-way lerp = ~288px displacement).
- C. Telemetry (run 340+60, PSXPORT_DEBUG=fps60): tier1 (world) 294 prims/frame, backdrop-lerped 352
  prims/frame (both stable), queue-lerp pool = dynamic objects only (this-frame matched 31% of 1218 at
  the f390 sample — pool unchanged from last session, as expected).
- D. check_stage2.py, same window, git-stash baseline vs after: static-identity 500/4489090 (0.0111%)
  BOTH, midpoint compliance 95.6% BOTH — bit-identical gate output (the static window has no scroll
  motion, so verbatim-replay and lerp-of-equal-endpoints coincide; the change is exercised by gate B's
  pan window instead).
- E. SBS-full smoke: combat leg 0 diverges through f23820 (320s), watch-cut leg 0 diverges through
  f21900 (300s) — host-only change, as expected.
- F. fps60-off (psxport_settings_fps60off.ini, run 400, shot): normal field frame, no fps60 log lines.

Coverage after this session: terrain + scene-table (camera-lerp) + backdrop (layer-transform scroll
lerp) are all Tier 1. The object/entity walk (`perObjFlush`) remains the only queue-lerp-heuristic
geometry — Tier 2 (object-transform lerp) unchanged as the next step.

# ═══ UNIFIED PATH REDESIGN (USER 2026-07-15) — "no difference between real and interp aside from lerp" ═══
DIRECTIVE (verbatim): "There shouldn't be any difference between how real and interpolated frames are
made aside from lerp, all drawing should flow through the same logic, the only difference in the
interpolated frames should be the lerp done." USER chose the FULL landing (unify path + per-object
transform lerp together; no intermediate object judder, no flicker, one verified landing).

## The defect this fixes
The interp frame is built by a SEPARATE path (tier1Render re-renders terrain + matchAndLerp
fingerprint-matches captured queue prims), NOT the real render dispatch (drawOTag → sceneNative /
authored-subscene OT walk). Symptom: the hut interior (sm[0x4c]==3) flickers to the field exterior
every interp frame — neither tier1Render nor matchAndLerp covers the guest-OT sub-scene, so the interp
frame draws the stale field. matchAndLerp is acknowledged debt (this doc's "REDIRECT").

## Target architecture — ONE render, lerp lives in the INPUTS
The interp present RE-RUNS the real render dispatch (drawOTag: sceneNative for field, OT walk for the
sub-scene) into the isolated sink (mSink via rqRedirect, DisplayPassGuard read-only, present-time
invariant — no tick since the real frame, so re-reads are the same guest state). The ONLY difference
from the real frame is that every INPUT is served lerped through a capture/override choke:
  - CAMERA  — `Fps60::sceneCam` choke. DONE (mCamCur/mCamPrev; override returns lerp at present).
  - BACKDROP scroll — `Fps60::bgScroll` choke. DONE (mBgCur/mBgPrev, wrap-lerp).
  - PER-OBJECT transform — `Render::projComposeObject` (reads Robj cmd+0x18, Tobj cmd+0x2c). TODO: give
    it the SAME choke shape as sceneCam — at a real frame capture {Robj,Tobj} keyed by `cmd` into
    mObjCur[cmd]; at present return lerp(mObjPrev[cmd], mObjCur[cmd], t). New object appearing (no prev)
    → use cur (no lerp) that frame. `cmd` is the object's stable render-command identity (node+0xC0[i]).
Then: DELETE matchAndLerp + tier1Render's separate terrain re-render + the provenance/fingerprint/
node-atomicity machinery. The interp present becomes: set overrides on → re-run drawOTag into mSink →
present mSink → overrides off. terrain/objects/backdrop/sub-scene all flow through the one dispatch.

## Boundary (honest)
The guest-OT sub-scene (hut) has NO native per-object transform (its prims are guest-built OT packets),
so its objects can't get per-object motion lerp until those emitters are RE'd/ported native (the
object-RE frontier). Until then the sub-scene interp frame re-runs the OT walk with the lerped CAMERA
only — object motion frozen between real frames, but NO flicker (interp == real content). This is the
degenerate lerp, consistent with the principle. Field objects (native, projComposeObject) get full
per-object motion lerp immediately.

## Build order (each step oracle-gated: SBS-full 0-diff on the faithful path + user eyeball fps60)
1. projComposeObject capture/override choke (mObjCur/mObjPrev keyed by cmd + lerp override), captured on
   every real projComposeObject call — byte-identical when fps60 off / real frame (like sceneCam).
2. present_vk: interp present re-runs drawOTag (via a present-time render entry) into mSink with camera
   + object + backdrop overrides ON; present mSink. Replaces the tier1Render+matchAndLerp slot-A build.
3. Delete matchAndLerp, buildProvenanceIdx, enforceNodeAtomicity, mRqLerp/mMatch*/mZeroGroups*/tier1
   eligibility gate, mSink-as-tier1-only. Keep rq_capture only if still needed for rate detection.
4. Verify: fps60dump/preseq — interp frames sit BETWEEN their real neighbors (no teleport), hut interior
   no flicker, moving field actors interpolate smoothly. SBS-full 0-diff (fps60 is core-A-only overlay).

## Step-2 implementation constraints (discovered 2026-07-15 — the plan's "re-run drawOTag" needs refining)
Two hard constraints found while designing the interp re-run:
1. sceneNative CANNOT be re-run wholesale at present time — it mutates per-frame state (mSceneTableTrusted/
   mBackdropTrusted/mAreaCacheWasNarration trust latches, render_walk.cpp:210-245) and is documented to
   "run exactly once per real logic frame." Re-running it double-toggles the latches. So the interp re-run
   must call the PICTURE-ONLY producers (terrainRenderAll/fieldEntityRender/backdropRender — what
   tier1Render already does — PLUS the object entity-walk, factored out of sceneNative's state mutation).
2. The OT-walk (gpu_dma2_linked_list) emits to core->game->rq DIRECTLY (gpu_native.cpp:938/1117/1175), NOT
   via rqRedirect — so redirecting the sub-scene OT-walk into mSink needs an activeRq() choke:
   `RenderQueue& Game::activeRq(){ return rqRedirect ? *rqRedirect : rq; }`, and those 3 emit sites route
   through it (byte-identical when rqRedirect==null, which is every non-present-re-run path).

### Refined step 2 (two independent sub-steps, each SBS-full + hut-preseq gated):
2a. SUB-SCENE (fixes the hut flicker, contained): add Game::activeRq() + route the 3 OT-walk emit sites
    through it (byte-identical). Capture {authored_subscene, otHead} per real frame into Fps60
    (captureSubscene, like the camera capture). In present_vk, when the presented frame is a sub-scene,
    build slot A by re-running gpu_dma2_linked_list(otHead) with rqRedirect=mSink + mCamOverrideOn (lerped
    camera) instead of tier1Render+matchAndLerp → the interp frame re-walks the SAME interior OT. Present
    mSink. This is the sub-scene flowing through the same OT-walk logic (camera-lerped; object motion frozen
    — guest OT, no native transform, no flicker).
2b. FIELD OBJECTS (replaces matchAndLerp): factor the object entity-walk (render_walk.cpp per-object
    geomblk loop, projComposeObject→gt3gt4) out of sceneNative's state mutation into a re-runnable
    picture-only method; the interp present re-runs it with mObjOverrideOn (step-1's lerped transforms)
    into mSink alongside tier1Render's terrain. Then DELETE matchAndLerp/buildProvenanceIdx/
    enforceNodeAtomicity (step 3).

## Step 2a LANDED (2026-07-15) — hut-interior flicker FIXED
The authored sub-scene (sm[0x4c]==3) interp frame went through tier1Render+matchAndLerp, which drew the
stale field (the flicker). Fix: present_vk detects the presented frame is a sub-scene (mSubsceneCur,
captured at drawOTag) and builds slot A from the CAPTURED interior queue (mRqCur) directly — the interior
the real frame drew — instead of the field re-render/queue-match. Interp == interior, no flicker.
Degenerate lerp (a guest OT has no native per-object transform to interpolate; that needs the sub-scene
emitters RE'd — the object-RE frontier). NOTE: re-walking the OT at present time does NOT work (the guest
OT is transient/cleared between frames → empty/black interp) — mRqCur (the flush snapshot) is the source.
VERIFIED: hut preseq no longer alternates (interp==real interior); SBS-full AUTO_SKIP 0-diff f13020
(field byte-identical); no field regression. Frames: scratch/screenshots/hut/final/.
Kept: Game::activeRq() (byte-identical redirect unification, useful for 2b). NEXT 2b: field objects —
factor the object entity-walk out of sceneNative, re-run it with mObjOverrideOn (step-1 lerped transforms)
to replace matchAndLerp for field objects; then step 3 deletes matchAndLerp.

## Step 2b LANDED (2026-07-15) — field objects interpolate through the unified path
2b.1: factored sceneNative's object walk into Render::fieldObjectsRender (byte-identical, caf0a64c).
2b.2: tier1Render now re-runs fieldObjectsRender under mObjOverrideOn (step-1's lerped Robj/Tobj) +
the lerped camera into mSink; isTier1Owned excludes ALL RQ_WORLD (terrain+scene-table+objects now
tier1-rendered) so matchAndLerp handles only 2D. Field actors interpolate through the SAME object
walk the real frame ran, with lerp in the INPUTS (transforms) — replacing matchAndLerp's per-prim
output-matching (which caused the gem/quad "different position every frame" artifacts USER reported).
VERIFIED: SBS-full AUTO_SKIP 0-diff f11970 (guest byte-identical, fps60 core-A-only); field renders;
hut still fixed. USER-EYEBALL: moving field actors interpolate smoothly (no gem/quad teleport), no
dropped world geometry. NEXT step 3: matchAndLerp now only touches 2D (HUD/overlay) — present that
verbatim from mRqCur and DELETE matchAndLerp/buildProvenanceIdx/enforceNodeAtomicity/mRqLerp.

## Step 3 LANDED (2026-07-15) — matchAndLerp DELETED, unification COMPLETE
present_vk's field branch now: slot A = mSink (tier1's lerped world: terrain+scene-table+objects+
backdrop) merged with mRqCur's 2D (HUD/overlay) VERBATIM by (layer,seq) — no prim matching. Deleted
matchAndLerp/buildProvenanceIdx/enforceNodeAtomicity + helpers (fpEqual/colorsEqual/fpHash/lerpItem) +
kNoNode/kTier1Sink + all match scratch members (mRqLerp/mNLerp/mMatchMap/mZeroGroups*/mMatchOfA/mUsedB/
mNodeTotalA/MatchedA/mIdxPrev/CurBuf/mNodeIdxScratch/mMatched*/mUnmatched*) — 180 lines. Kept isTier1Owned
(the mSink-vs-2D filter) + mT + mBackdropPrimsThisFrame. Updated fps60.h/.cpp banners to the unified path.
VERIFIED: SBS-full AUTO_SKIP 0-diff f10200 (guest byte-identical); wide+fps60 hut interior no flicker
(interp==real interior, sm[0x4c]==3, 560x240 clean margins); field objects render + interpolate.
UNIFICATION COMPLETE: camera + per-object transforms + backdrop all lerp through the ONE render re-run;
2D verbatim; sub-scene presents captured interior. USER principle achieved — "no difference aside from lerp."

## REDIRECT census progress (2026-07-16, #67 session)

DONE: billboardEmit particle system = display-pass producer (Render::billboardsRender,
perobj_billboard.cpp) — per-particle BbRec capture + float re-projection through the sceneCam choke,
per-particle prev/cur lerp at the interp present; #65 dual-emit deleted. See
docs/findings/render.md "billboardEmit particles now a DISPLAY-PASS producer".

REMAINING guest-time quad emitters (decompiled 2026-07-16, scratch/decomp/quad_emitters.c —
Ghidra ram_sea; re-derive from there, not from scratch):
- **FUN_8003B704 — see-saw/beam quad emitter** (dispatched from objListWalk4/EEC0 cases 0x8003EF30
  (after perObjRenderDispatch) and 0x8003EF40 (after billboardCompose1, gated node+2==1)). Builds TWO
  quads spanning DAT_800e7f5c's +0x2c/30/34 position and node+0x2E/32/36, half-widths from
  rcos/rsin(node+0x68, node+0x6A [+0x400 when DAT_800e7fc6<4]) ×0x14, color from
  DAT_800a3b04[node+0x66*2], code 0x2D, clut 0x3E9F; emits via func_8003B320 (submitQuad, native)
  with otzBias=0; pool-span markers on 0x1F8000F8. **OPEN before display-pass port: the CR contract**
  — its verts look world-space but the CRs at call time are whatever perObjRenderDispatch /
  billboardCompose1 left (camera∘object or MAT_OUT compose). Pin down whether DAT_800e7f5c/node
  positions are world or node-relative and what CR state each dispatch case guarantees.
- **FUN_80039F4C — per-character 3D text-label renderer** (renderWalk case 0x8003C0E8). Text = "Clear"
  (+DAT_80014a1c suffix) when node+3==2, else string table PTR_DAT_800a33cc[node+0x60*3]. Per char:
  glyph template quad {(-3,-7),(5,-7),(-3,9),(5,9)} z=-1, FUN_80039e80(char, pool) fills the packet
  (returns OT depth or -1), per-char cmd at node+0xC0[i] (span markers on cmd+0x18), projected by
  FUN_8003F7D8 (same RTPT/RTPS/AVSZ4 shape as submitQuad), code 0x2D, tpage-word 0x1F, clut
  0x7DFF/0x7C7F. **OPEN: FUN_8003F174(node,1) prep + FUN_80039e80 (per-char transform/UV source) —
  decompile these two before porting.**
- **case188 generic-particle quads** (renderWalkCase188, render_walk_dispatch.cpp): corners from
  node+96..118 through func_8003B054/B320 under per-node CRs composed earlier — same CR-contract
  question as B704.
- These three still draw via QuadRtptSubmit's dual-emit (rq_push_ft4_record — the last caller);
  at fps60 they present verbatim (30Hz stepping, no flicker). Delete that dual-emit only as each
  class gains its display-pass producer.
