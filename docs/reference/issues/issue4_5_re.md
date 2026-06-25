# Issues #4 & #5 — render order / depth family — RE + PC-native fix spec

READ-ONLY RE (no source modified). Methodology per `docs/gfx-debug.md`: the engine OWNS its render
PC-native (real D32 depth + its own 2D layer/sort); there is NO oracle. Diagnosed LIVE via headless
REPL + `debug ndepth`/`objz` + `PSXPORT_PRIMDUMP`.

- **#4** "Flames render over foliage" — torch/brazier semi-transparent flame billboards draw on top of
  thatch/leaves that should occlude them. Same family as ropes-over-terrain, barrel-face.
- **#5** "Barrel/container faceted faces flicker red/blue" — z-fighting between near-coplanar faces.

Screenshots read: `issue4_flames.png` (flame in front of green thatch leaves), `issue4_ropes.png`
(ropes drawn over a wall/grass), `issue5_barrel.png` (barrel: blue band on top + red band on bottom,
the captured snapshot of the per-frame red/blue flip), `issue5_wall_decals.png` (horseshoe decal /
slat wall).

---

## 1. The depth-assignment code path for semi / 2D / effect prims

There are TWO depth sources in the renderer, and **only the second is alive for OT-walked prims today**:

### (a) Per-vertex real depth — for ENGINE-OWNED 3D geometry only
The owned submitters (POLY_GT3 `submit_poly_gt3_native` `engine/engine_submit.cpp:242`, POLY_GT4
`submit_poly_gt4_native` :287, byte-packed GT4, and native terrain) project model verts in float
(`proj_native_xform` `runtime/recomp/gte_beetle.cpp:244`) and tee the quad **directly** to the rasterizer
via `gpu_draw_world_quad` (`runtime/recomp/gpu_native.cpp:579`) with a per-vertex `depth[k] =
proj_pz_to_ord(p[k].pz)` (`gte_beetle.cpp:201`). This bypasses the guest OT entirely → real D32 per-pixel
occlusion. It carries the **sub-integer view-Z** fix (`gte_beetle.cpp:277-290`): `out->pz` is built from
`tmp2_unshifted/4096.0` (12-bit fraction intact) instead of the GP0 integer SZ.

NOTE: the OLD per-vertex attach for *OT-walked* prims is DEAD. `projprim_set_pz`
(`gte_beetle.cpp:432`) has **zero callers** — confirmed live (`projprim(vtx) records=0` every frame).
So in `gp0_exec` the per-vertex lookup `projprim_lookup_pz(vaddr[i],&pz)` (`gpu_native.cpp:786`) NEVER
succeeds. Every prim that goes through the guest OT walk gets is3d=0 from this branch.

### (b) Whole-object world-position depth — the ONLY depth OT-walked 2D/effect prims can get
For a prim drawn through the guest OT (`gpu_dma2_linked_list` → `gp0_exec`), depth is assigned by
**provenance span lookup**, keyed by the OT node address:

- During an object's render, `g_pkt_track=1` is set and every guest store into the packet pool
  `[0x800BFE68, 0x800E7E68)` extends `[g_pkt_lo, g_pkt_hi)` (`runtime/recomp/mem.cpp:226-229`).
- After the object renders, `gpu_obj_depth_add(lo, hi, ord)` records that span + the object's
  world-position depth `ord = proj_pz_to_ord(object_world_view_depth(...))`
  (`gpu_native.cpp:82`, span table; `engine_submit.cpp:576` world depth = camera-forward·(obj−cam)>>12).
  A **+1/512 camera-ward bias** is added (`gpu_native.cpp:90`) so a decal sits just in front of its host
  surface.
- At the OT walk, each node sets `s_cur_node = 0x80000000|addr` (`gpu_native.cpp:1629`). For a 2D prim
  (poly: `gpu_native.cpp:801-802`; sprite: `:982`) `obj_depth_lookup(s_cur_node,&od)`
  (`gpu_native.cpp:93`) checks `s_cur_node ∈ [lo,hi)`. On a HIT it stamps `dep[i]=od`, sets `is3d=1`,
  and the prim joins the 3D world band with real depth. On a MISS it falls to the flat 2D band.

The owned render walks that publish these spans: `ov_render_walk_snapshot` (`engine_submit.cpp:730`),
the per-object dispatch `ov_render_cmd` (`:123`, the universal chokepoint at `0x8003F698`),
`ov_collectable_quad` (`:670`), and — added for #4 — the auxiliary walks
`ov_rwalk_aux_bcf4`/`_bf00`/`_eec0` (`:837/:899/:952`, addresses `0x8003BCF4/0x8003BF00/0x8003EEC0`,
registered `game_tomba2.cpp:1392-1397`).

### Band layout (`runtime/recomp/gpu_gpu.cpp:184-201`)
Single D32 buffer, nearer = larger ord, clear 0.0, compare GREATER_OR_EQUAL:
- 2D **BACKGROUND** band `[0, 0.0625)` — `set_order_2d_bg`, for non-projected backdrops (sky/sea/**painted
  tilemap**), ordered by OT index.
- 3D **WORLD** band `[0.0625, 0.9375]` — `ord3d(d)` `:187`, real per-vertex depth OR obj_depth.
- 2D **OVERLAY/HUD** band `(0.9375, 1]` — `set_order_2d`, HUD/UI/banners over the world.

---

## 2. #4 root cause (flames over foliage) — EVIDENCE + mechanism

**The headless-reachable field/menu scene cannot show the flame hut** (the flame "Burning House" village
is past free-roam, which AUTO/skip does not reach — `docs/driving-the-game.md §5`, port-progress L482:
"hut door blocked by a barrel"). But the LIVE diagnostics on the reachable field pin the mechanism:

`debug ndepth` at the first 3D field (f600):
```
projprim(vtx) records=0  lookups hit=0 miss=34      <- per-vertex OT depth is DEAD (source (a) off)
obj_depth spans=4  2D-prim lookups hit=2 miss=59     <- only 4 object spans; 59/61 2D prims MISS
[ndepth f600] real-depth(3D) prims=2  OT-band(2D) prims=32  3D%=5.9
```
`PSXPORT_PRIMDUMP=600` (413 prims): **2** is3d=1, **352** bg=1 (backdrop), **59** is3d=0/bg=0 (HUD band),
379 of them SPRITES. The "world" in a Tomba2 field is overwhelmingly a **2D tilemap of small op-0x65
sprite tiles painted as the backdrop**, not real 3D geometry.

There are TWO distinct failure modes producing "flame over foliage", and the issue's hypothesis only
covers the first:

**(i) Effect prim misses its obj_depth span → flat 2D HUD band (the issue's stated hypothesis).**
A flame/rope billboard whose GP0 packet is NOT written into the tracked pool `[0x800BFE68,0x800E7E68)`
during a tagged walk (or whose owning walk wasn't owned) gets `g_pkt_hi==g_pkt_lo`, no `gpu_obj_depth_add`,
so `obj_depth_lookup` MISSES → the prim lands in the OVERLAY band `(0.9375,1]` → draws over EVERYTHING.
The live `2D-prim lookups hit=2 miss=59` shows the obj_depth coverage is tiny: the vast majority of 2D
prims currently miss. The aux-walk fix (port-progress L322-326) plugged two of these queues but is marked
"FLAME occlusion itself awaits the flame-hut scene + USER eyeball (not headless-reachable yet)" — i.e. it
is UNVERIFIED for the actual flame, and other effect submitters may still miss.

**(ii) The foliage that should occlude has NO depth — it is the painted backdrop (the deeper cause).**
Even when (i) is fixed and the flame correctly receives its real obj_depth in the 3D band `[0.0625,0.9375]`,
**the thatch leaves are part of the 2D tilemap backdrop** (`bg=1`, far band `[0,0.0625)` via
`set_order_2d_bg`, classified by `node_is_bg`/`bg_2d` `gpu_native.cpp:798,959`). The far band is
unconditionally BEHIND the entire 3D band. So any flame with real world depth is ALWAYS in front of the
leaves — because the leaves carry no occluding depth at all; they are a flat picture. The depth buffer has
nothing to occlude the flame with. The screenshot (`issue4_flames.png`) is exactly this: a leafy tilemap
roof (backdrop) with the flame billboard composited in front.

This is the structural root cause of the whole family: **objects that should occlude (foliage canopy,
the wall in `issue4_ropes.png`) are 2D backdrop tiles with no per-pixel depth, so the depth buffer can
never occlude a foreground effect with them.** The PSX hid this because it painted strictly in OT order;
our engine owns depth, and a real depth test exposes that the occluders have no depth.

---

## 3. #5 root cause (barrel red/blue z-fight) — EVIDENCE + mechanism

The barrel is a **faceted 3D mesh** (octagonal prism, stacked horizontal bands — see `issue5_barrel.png`:
a blue/frost band and a red/flame band stacked, with stone rims between). Two competing depth-assignment
realities cause the flicker:

**Most likely: the barrel renders through the OT walk and its faces share ONE whole-object obj_depth.**
If the barrel object renders via a guest per-type renderer (not the owned GT3/GT4 float path), all its
emitted face packets fall inside ONE `[g_pkt_lo,g_pkt_hi)` span tagged with a SINGLE
`object_world_view_depth` (`engine_submit.cpp:576`, `ord = proj_pz_to_ord(... >>12)`). Every face then
gets the **identical** `od` depth value (`gpu_native.cpp:802`: `for(i) dep[i]=od`). With every face at the
exact same D32 depth and a GREATER_OR_EQUAL test, which face "wins" a shared pixel is decided purely by
**submission/draw order within the frame**, and that order is not stable frame-to-frame (queue swap
double-buffering, list re-enumeration) → the red band and the blue band trade the contested pixels each
frame → the red/blue flicker. This is the classic "unstable obj-depth assignment across an object's own
faces" the issue names. The whole-object depth is also COARSE: `object_world_view_depth` returns
`dot>>12` (integer view-Z, sub-12-bit fraction discarded `:581`), so even distinct faces a few units apart
collapse to the same `proj_pz_to_ord` value.

**If instead the barrel is owned GT3/GT4** (float path, per-vertex `proj_pz_to_ord(p.pz)` with the
sub-integer pz fix `gte_beetle.cpp:281`), faces at genuinely different Z get distinct depth and DON'T
fight. But two TRULY coplanar layers (e.g. a decal slat face laid on the barrel body face) project to
the same `pz` → same `ord` → z-fight regardless, since neither has any bias to separate them
(`gpu_draw_world_quad` applies no per-face offset, unlike the obj_depth path which adds +1/512). The
`proj_pz_to_ord` mapping is affine in `1/pz`, so far/large pz compresses many distinct depths into a
narrow ord range, shrinking the separation between near-coplanar faces below D32 resolvable steps.

**Which path the barrel takes must be confirmed live in the barrel scene** (not headless-reachable);
the static evidence supports the shared-obj_depth path as primary, with coplanar-decal z-fight as the
secondary contributor (also explains `issue5_wall_decals.png`: the horseshoe decal coplanar with the
slat wall — the +1/512 bias `gpu_native.cpp:90` was added specifically for wall decals but a too-small
bias still fights when the wall itself has real depth).

---

## 4. PC-native FIX PLAN (give the engine the correct depth RULE; never re-sync with PSX OT)

### #4 — flames over foliage
Two fixes, in order; (B) is the real one.

**(A) Close the obj_depth coverage gap so EVERY effect billboard inherits its object's world depth.**
- Confirm in the live flame-hut scene which render walk / submitter emits the flame and rope billboards,
  and whether their packets land in the tracked pool `[0x800BFE68,0x800E7E68)`. Use `debug objz`
  (`engine_submit.cpp:139` `[rcmddep]`, `:805/:960` node dumps) + `provat x y` on a flame pixel to read
  the writing node + its band, and `debug ndepth` to watch `hit/miss`.
- For any effect submitter whose span isn't currently tracked, own its walk PC-native exactly like the
  aux walks (`engine_submit.cpp:837+`) and wrap it with `g_pkt_track`/`gpu_obj_depth_add(world-pos)`.
  This is the issue's stated fix and the path the aux-walk work already started; it makes the flame
  occlude/be-occluded by anything that has REAL depth.

**(B) The occluders (foliage canopy, walls) must HAVE depth — promote the world tilemap out of the flat
backdrop band into real 3D world depth.** The thatch/leaves and the rope wall are currently 2D backdrop
tiles (`bg=1`, far band), so nothing can occlude in front of them. The PC-native rule: a tile that is part
of the *playfield surface the player and effects move through* is not a sky backdrop — it should carry a
world depth (its tilemap cell's world Z) and live in the 3D band, so the depth buffer occludes effects
correctly. Concretely: the engine-owned tilemap/background drawer (`ov_bg_tilemap`, the `bg_range_add`
provenance) should split "true backdrop" (sky/sea, no depth, far band) from "playfield foliage/wall tiles"
(real world depth, 3D band) using SCENE data — its own classification, NOT the PSX OT order. This is the
larger render-ownership work; (A) alone will NOT make the flame go behind the leaves because the leaves
have no depth to occlude with.

USER must eyeball: drive to the flame-hut village, confirm (i) flames now sit BEHIND the foreground
leaves/thatch that overhang them and IN FRONT of the wall behind, and (ii) no regression to backdrop sky/sea
ordering elsewhere (the far-band split is delicate — see the journal sea-on-top history).

### #5 — barrel red/blue z-fight
Goal: a STABLE per-face depth so contested pixels resolve identically every frame.

1. **Diagnose live** (barrel scene): `provat x y` on a fighting pixel across consecutive frames — does the
   writing node/face flip? Read its band/ord. Determine whether the barrel is OT-walked (shared obj_depth)
   or owned-GT4 (per-vertex). `debug objz` `[rcmddep] ... ord=` shows whether all its faces share one ord.

2. **If shared obj_depth (primary hypothesis): give each face its OWN depth, finely.** The barrel must not
   collapse all faces to one `object_world_view_depth>>12`. Either (a) route the barrel through the owned
   GT3/GT4 float submit so each face carries per-vertex `proj_pz_to_ord(p.pz)` (sub-integer pz, distinct
   per face) — the proper PC-native fix, faces then occlude by true geometry; or (b) if it must stay
   OT-walked, compute per-PRIM depth (project the prim's own verts) instead of one object-center depth,
   and keep the 12-bit fraction (use the sub-integer view-Z, not `dot>>12`). A single shared coarse depth
   is the bug; remove the sharing.

3. **For genuinely coplanar faces/decals (secondary; also `issue5_wall_decals.png`): add a STABLE,
   deterministic per-face depth bias** so the decal/front-band always wins by a fixed tiny epsilon — not
   a frame-dependent draw-order tiebreak. The existing +1/512 obj_depth bias (`gpu_native.cpp:90`) is the
   right idea; for the barrel apply a per-face ordinal bias (face index × a sub-LSB epsilon, e.g.
   `face_i / 65536` within the object) so stacked coplanar layers get a fixed, repeatable ordering. The
   epsilon must be smaller than the gap between distinct world objects (so it never pops a face in front of
   genuinely nearer geometry) yet larger than the per-frame reprojection jitter (so it doesn't flip). This
   gives the engine a stable ordering RULE for its own coplanar faces — it does NOT consult the PSX OT.

USER must eyeball: drive to the barrel, confirm the red/blue flicker is gone (faces stable, correct band
visible), and confirm no new z-fighting or wrong-face-on-top on other faceted objects (catapult, crates —
journal L4487 lists faceted objects to spot-check) and on the wall decals.

---

## 5. Reachability note / live repro the USER must run

Neither scene is reachable headless: free-roam gameplay is not reached by `newgame`/`skip`/`AUTO_*`
(`docs/driving-the-game.md §5`; port-progress L482 "seaside exit isn't a plain walk/jump — hut door
blocked by a barrel"). The static + partial-live evidence above is conclusive on MECHANISM; the per-scene
confirmation and the visual gate are USER-driven.

Live repro recipe (windowed, with the live debug server):
```
PSXPORT_GPU_WINDOW=1 PSXPORT_DEBUG_SERVER=1 scratch/bin/tomba2_port "<disc.chd>"
# play to the flame-hut village (#4) / beside the red barrel (#5), then:
tools/dbgclient.py debug ndepth objz scene     # obj_depth hit/miss, per-node band, classified DL
tools/dbgclient.py provat <x> <y>              # which node/prim wrote a flame/leaf/barrel-face pixel + its band
tools/dbgclient.py provat <x> <y>              # repeat across frames for #5 to catch the face flip
tools/dbgclient.py shot scratch/screenshots/issueN.ppm
```
For #4 read whether the flame node hits an obj_depth span (band = 3D vs OVERLAY) and whether the leaf
node is `bg`/far-band. For #5 read whether the two fighting faces share one ord and whether the writing
node flips frame-to-frame.
