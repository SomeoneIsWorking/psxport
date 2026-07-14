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

## Verification protocol (standing, USER directive)

Any fps60 change: run the tree/windmill scene with `fps60dump`, and (when a user screencast is
supplied) ffmpeg-dump its frames; compare real-vs-interp pairs. A fix claim requires the paired
comparison, not only a log metric.
