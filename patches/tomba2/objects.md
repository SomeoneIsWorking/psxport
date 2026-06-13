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

## Object struct fields (from wrapper 0x8007778C)
    obj + 0x01 : visible flag (byte; set per frame by the cull pass)
    obj + 0x0c : type/category byte
    obj + 0x2e : world X position (s16)
    obj + 0x32 : world Y position (s16)
    obj + 0x36 : world Z position (s16)
(More fields — rotation, model pointer — TBD.)

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

## Status / next
- [x] cull/submit chokepoint located (0x8007712C) — enumerates all live objects
- [x] position fields located (obj+0x2e/0x32/0x36, s16)
- [x] chokepoint fires + pointer stability verified in a live scene (frame 7037+)
- [x] object pool: contiguous, stride 0xC4, base ~0x800EF478
- [ ] rotation field(s) + model-data pointer in the struct
- [ ] a moving-object scene to develop/verify motion interpolation
- [ ] per-frame object snapshot override (prev/cur keyed by pointer, snap on jump)
- [ ] custom interpolating renderer driven by snapshots
