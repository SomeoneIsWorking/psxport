# Tomba! 2 (SCUS-94454) — object/entity system RE

Goal: drive object-based 60fps interpolation from the game's own entity state
(per the user: RE how the game tracks objects, override that tracks them, the
interpolator reads from there + a custom renderer).

## The per-object cull/submit chokepoint — `0x8007712C`
RE'd 2026-06-13 from a frame-12000 RAM dump (`scratch/raw/t2_f12000.bin`,
disassembled with `tools/disasm.py`). The engine has no single flat object
array — each object *type* has its own update/draw handler. But **every live
drawable object funnels through one function**, the cull/LOD dispatcher:

    0x8007712C  enqueue/cull(a0 = object*, a1 = dx, a2 = dy, a3 = dz)
      - first insn: `sll v0,a1,0x10` (entry signature 0x00051400)
      - squares+sums a1/a2/a3 -> distance² -> jal 0x80077fb0 (sqrt) -> v0 = dist
      - `sb zero, 1(a0)`  : clears the object's visible flag (obj+0x01, byte)
      - `lbu v1, 0xc(a0)` : object type/category byte (obj+0x0c), switched on
        (cases 2/4/5/9) to pick near/mid/far cull-cone + distance LOD
      - the six cull-cone `slti v0,v1,THRESH` sites (see cull-widen.md) live in
        this function (0x800772D4..0x800775D0); v1 = cos-scaled dot/distance

Callers are ~9 wrapper variants (0x800777E4, 858, 8CC, 940, 9B8, A34, AB4, B20,
F98), one per object representation, each computing the camera-relative vector
then calling 0x8007712C. The main wrapper `0x8007778C` (entry sig 0x27BDFFE8)
alone has ~85 callers (every object-type handler). Hooking the cull function
0x8007712C captures ALL objects regardless of wrapper.

## Object struct (0xC4 bytes) — verified by full A/B dump diff (frame 14130/14132)
    obj + 0x00 : flags/state bytes (byte +0x01 = visible flag, set by cull pass)
    obj + 0x0c : type/category (u32; common values 2/3/4/5)
    obj + 0x1c : handler / update function pointer (code ptr, e.g. 0x800739AC)
    obj + 0x24 : next-object pointer (intrusive linked list through the pool)
    obj + 0x2c : world X position, 32-bit 16.16 fixed (int part = the s16 @+0x2e)
    obj + 0x30 : world Y position, 16.16 fixed (int @+0x32)
    obj + 0x34 : world Z position, 16.16 fixed (int @+0x36)
    obj + 0x98 : 3x3 rotation matrix, 9 x s16, 1.0 = 0x1000 (identity for static
                 props: [1000,0,0, 0,1000,0, 0,0,1000])
    obj + 0xac : world position as 3 x s32 integers (the GTE TR vector copy)
    obj + 0xc0 : model/placement-data pointer (per instance; steps 0x44 across
                 the static-prop pool: 800F2BC4, 800F2C08, 800F2C4C, ...)

This layout is shared across the static-prop pool (type 4) and appears common to
types 2/3/5 (all have valid code handlers @+0x1c). NB: only the static-prop pool
is the contiguous 0xC4-stride array; other entities (player/NPCs) are separate
pools at other addresses — enumerate them via the 0x8007712C chokepoint (by
pointer), do NOT index the array past its end.

Camera world position lives in the scratchpad:
    0x1F8000D2 : camera X (u16)   0x1F8000D6 : camera Y   0x1F8000DA : camera Z
(wrapper does obj.pos - cam.pos to get the camera-relative vector it culls on.)

## Object array (verified in-runtime via the enumeration hook)
Hooking 0x8007712C (`PSXPORT_T2_OBJLOG=1`) and reading obj+0x2e/32/36 confirms a
**contiguous object pool**, observed live from frame ~7037 (68-90 objects):

    base   ~0x800EF478 (slot 0 of the active pool in that scene)
    stride  0xC4 (196 bytes) per object
    e.g. 800EF478, 800EF53C, 800EF600, 800EF6C4, ... (+0xC4 each)

Type-4 objects with real world positions, e.g. 800EF478 = (4200,-900,4268).

## Object identity for interpolation — pointer is the ID
Verified: the same object* recurs frame-to-frame at the same address with
consistent position while the entity lives (pointer stability holds). When a
scene changes, a pool slot is reused for a different entity (position jumps
discontinuously) — the interpolator must treat a large per-slot jump as a
re-bind (snap, don't lerp), same principle as the old primitive matcher.
A struct-internal unique-id field (if any) is still TBD; the pointer suffices.

## Motion is camera-dominated (key design fact)
In the captured 3D scene (frames 7035-15922) all 89 live objects are static
across consecutive logic frames — they are placed world props. The on-screen
motion comes from the CAMERA. So the interpolator must interpolate the camera
view (and the few independently-moving entities) and reproject, NOT lerp object
world transforms alone. (This is also why DuckStation's screen-space prim matcher
is the wrong tool: it can't reconstruct camera parallax.)

The wrapper reads the camera from scratchpad 0x1F8000D2/D6/DA (pos); the
authoritative camera transform (position + 3x3 rotation matrix, = the GTE
rotation matrix + TR loaded before RTPS) is the next RE target.

## Reprojecting renderer — projection core PROVEN (chosen approach: from-scratch)
RTP vertex tap (psxport_set_rtp_hook, via RTPS/RTPT in gte.c) reports each
projected vertex as (input local V) + (game output screen SX,SY); the transform
in effect comes from the GTE CR tap. PSXPORT_T2_RTPDUMP=<csv> dumps full tuples
(R, TR, OFX, OFY, H, sf, V, SXY). tools/reproject.py faithfully reimplements the
GTE RTPS (exact DivTable/CalcRecip reciprocal, the dist/Z divide, Lm_B IR sat,
Lm_G screen sat) and **reproduces the game's SX/SY on 6,348,755/6,348,755 = 100%
of captured vertices** (frame 14130-ish scene, all rotations + edge cases). So we
can re-project the same geometry at an interpolated (R,TR) faithfully — the
projection half of the custom renderer is done. Remaining: cross-frame transform
matching, transform interpolation (TR linear, R nlerp/slerp), textured
rasterization (VRAM as texsrc), present the extra frame.

## Renderer capture layer (runtime/wide60.cpp) + the project->draw latency
wide60 captures, per frame: the GTE transforms (CR tap), the projected vertices
(RTP tap: SXY -> local vertex + transform), and the GP0 polygons (GPU poly tap:
verts/uv/color/clut/tpage). It joins each polygon vertex to its transform by
screen-space SXY (poly packet xy == GTE SXY, both pre-draw-offset).

KEY FINDING (2026-06-13): the game **projects geometry one logic frame before it
draws it.** Joining polys against the *same* frame's projections gives ~0-2%;
joining against the *previous* frame's projections gives 40-78%. So the capture
must be segmented by the GPU display flip (GP1 0x05) and draws joined to the
PRIOR flip-segment's projections. The <100% remainder is 2D geometry (UI / text /
2D backgrounds) that never goes through RTPS — those correctly snap, not lerp.
NEXT: hook the flip for proper frame boundaries, then match/interp/rasterize.

UPDATE: flip boundaries done (GP1 0x05 tap). Flip-segmented join = ~56-65% of
drawn vertices (78% peak). The unjoined ~35-44% splits into:
 (a) ~120-140 polys/flip that DECODE to out-of-range coords (>11-bit) — GP0
     polygon-variant decode edge cases in wide60 OnGpuPoly still to fix; and
 (b) genuinely CPU-projected geometry (Tomba 2 computes terrain/background
     screen coords on the CPU, not via GTE/RTPS — confirmed earlier), which a
     pure-RTPS reprojection renderer cannot reach by construction.
IMPLICATION: a pure-RTPS reprojection renderer covers only the GTE-projected
subset for Tomba 2. User direction: RE + tap the CPU projection path (full
fidelity).

## How the terrain is transformed: MVMVA + CPU perspective divide
GTE op histogram (PSXPORT_WIDE60_LOG, ~3500 flips): RTPT(0x30)=19063,
RTPS(0x01)=6166, **MVMVA(0x12)=13190**, NCLIP(0x06)=16689, AVSZ3/4=1842/3005.
The heavy MVMVA use is the terrain: MVMVA does R*V+TR (into IR1/2/3 = view
space) but NOT the perspective divide / screen mapping — the game does that on
the CPU. That's why terrain verts never appear in the RTPS SXY capture.
FIRST HYPOTHESIS (terrain = MVMVA + CPU divide) — **FALSIFIED 2026-06-13.**
Tapped MVMVA (capture input vert + MAC result), reprojected with the verified
RTPS divide, added to the join map: coverage did NOT move. Dumping the
predictions showed why — Tomba 2's MVMVA ops are **lighting/normal transforms,
not vertex projection**: inputs like V=(0,..), results with mac1==0 (so sx==OFX
center) and off-screen Y (367/523/1023). So the terrain is projected by **pure-
CPU MIPS math** (transform AND perspective divide on the CPU), using no GTE op
we can reproject. The MVMVA tap is kept for counting only; it does not feed the
join. (DEAD END — do not retry the MVMVA-reprojection route.)

OPEN: covering terrain now requires either (a) locating & reimplementing the
game's pure-CPU terrain projection routine (deep, game-specific MIPS RE), or
(b) a screen-space fallback for non-GTE polys (the option not taken earlier).
The GTE character/object geometry (~56%) IS fully reprojectable (RTPS verified
100% bit-exact) — that part of the renderer can proceed regardless.

## Status / next
- [x] cull/submit chokepoint (0x8007712C) — enumerates all live objects by ptr
- [x] full 0xC4 object struct mapped (pos 16.16 @+0x2c, rot matrix @+0x98,
      handler @+0x1c, model ptr @+0xc0, linked-list @+0x24)
- [x] pointer stability verified; pool-slot reuse on scene change = snap
- [x] faithful reprojection RTPS verified 100% bit-exact (tools/reproject.py)
- [x] camera/object transforms captured via GTE tap (psxport_set_gte_cr_hook):
      CR0-4 = 3x3 rotation matrix (s16, 1.0=0x1000), CR5-7 = translation TR.
      KEY: TR is CAMERA-RELATIVE — it changes every frame as the camera moves
      even for static objects, so the GTE stream already encodes on-screen motion
      (incl. parallax). 41-58 transforms/frame. PSXPORT_T2_GTELOG=1 to dump.
      This is the stream the interpolator lerps and the renderer reprojects.
- [ ] a moving scene (drive to overworld gameplay via REPL) to verify motion
- [ ] per-frame snapshot override (camera + objects, prev/cur, snap on rebind)
- [ ] custom interpolating renderer (reproject at lerped camera+object state)
