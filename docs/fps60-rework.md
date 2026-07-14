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
