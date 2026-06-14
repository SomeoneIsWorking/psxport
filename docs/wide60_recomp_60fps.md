# wide60 on the native recomp — interpolated 60 fps design

Status: DESIGN (2026-06-14). Owner of this doc: engine-RE/graphics. Implementers wire the
hooks; this doc does not modify runtime code. Prototype validating the core logic:
`scratch/wide60dev/lrate_proto.c` (builds + passes: rate detection at periods 2 and 3,
transform-lerp + reprojection midpoint strictly between endpoints).

## 0. Reframe: what carries over from the emulator-era wide60, what does NOT

The prior wide60 RE (journal entries "object-based 60fps", "wide60", the matcher, the RTP
reproject) was built for the **Beetle emulator runtime** — taps were inside Beetle's
interpreter/GPU (`gpu.cpp` `PushCommand`, `gte.cpp` ctc2/swc2 hooks, the
`psxport_set_*_hook` registry). **The port is now a native static recompilation.** We tap
OUR OWN code directly, which is strictly cleaner. Concretely:

**CARRIES OVER (the RE facts — proven, reusable):**
- The Tomba2 object pool: base `~0x800EF478`, **stride `0xC4`**, positions s16 at
  `obj+0x2e/0x32/0x36` (X/Y/Z), type at `+0x0c`, visible flag at `+0x01`. Pointer is the
  stable per-entity ID across frames; pool-slot reuse on scene change ⇒ snap.
- **The universal per-object chokepoint `0x8007712C`** (the cull/LOD dispatcher): `a0 =
  object*` once per logic frame for every live drawable. This is the object-identity tap.
- GTE RTPS/RTPT projection is **bit-reproducible** (tools/reproject.py: 6,348,755/
  6,348,755 = 100%). So geometry captured in object-local space can be re-projected at an
  interpolated (R,TR) faithfully. This is the whole basis of in-between synthesis.
- "The game projects one logic frame BEFORE it draws" → the join is **prev-segment
  projections to current draws**, segmented at the **display flip** (GP1 0x05). Same-frame
  join ~0-2%, prev-frame join 40-78%.
- Scope decision (memory `wide60-scope-decision`): interpolate ONLY GTE-projected geometry
  + camera. CPU-projected terrain and 2D UI/HUD stay at logic rate (snapped). **No flicker
  is the top priority**: unmatched geometry stays at frame-A, never half-interpolated.
- Tomba2 terrain is **CPU-projected** (no RTPS origin; 44% of drawn verts have no
  projection within ±4px). MVMVA-terrain hypothesis is FALSIFIED — do not look for a GTE
  op to tap for terrain. It snaps.

**DOES NOT CARRY OVER (emulator-specific — discard / re-point):**
- The Beetle `psxport_set_gpu_poly_hook` / `psxport_set_rtp_hook` / `psxport_on_gte_cr` /
  `psxport_capture_rtp` registry and `runtime/wide60.{h,cpp}`. Those live in the OLD
  `runtime/Makefile` build (libretro glue) which still defines `-DPSXPORT_HOOKS`. **The
  native build (`run.sh` / `tools/recomp/build.sh`) compiles `vendor/.../psx/gte.c` WITHOUT
  `-DPSXPORT_HOOKS`** — so those dormant taps inside Beetle's gte.c are compiled out. Do NOT
  resurrect them; tap our adapter `gte_beetle.c` instead. (Leave the `#ifdef PSXPORT_HOOKS`
  blocks in vendored gte.c alone — they're inert in this build.)
- The emulator's GP0 SRCADDR-comment trick and the difflib display-list aligner that keyed
  on DMA arena addresses. We have the GP0 stream directly in `gpu_native.c::gpu_gp0`, and we
  have a much stronger key: the object identity from the `0x8007712C` dispatcher. Arena-addr
  alignment becomes a fallback for prims with no object id, not the primary matcher.
- Beetle's GTE widescreen-scale hack (`widescreen_hack` in gte_beetle.c). For widescreen
  we own the projection/rasterizer; that's a separate tier, not this 60fps doc.
- Any savestate-based iteration (journal "savestates UNRELIABLE under HLE"): irrelevant now
  — boot is fast and deterministic; iterate from boot.

## 1. Logic-rate detection (measure, don't assume 30)

The port has a single, exact per-logic-frame chokepoint already wired:
**`ov_frame_update`** in `runtime/recomp/games_tomba2.c` = the override of **`FUN_800788AC`**
(the per-frame state update, sole caller is the StrPlayer main loop, runs once per logic
iteration immediately before the pace dwell). It already calls `gpu_present()` once per
logic frame. **One call to `ov_frame_update` == one logic frame.** That is the unit clock.

Crucially, in the native port we removed the VBlank busy-wait — there is no emulator vblank
counter to count. The logic rate is measured in *units of logic frames vs presented frames*
that WE will choose. So "detection" here measures the game's **content-change cadence**:
how many of the game's own logic iterations produce an identical transform/draw set (the
game internally double-/triple-buffering or holding) vs a fresh one. For Tomba2 every
`FUN_800788AC` call is expected to advance logic, but the detector must not assume that —
FMV, menus, and paused states hold.

**Method (prototype-proven, `lrate_proto.c`):**
1. Each logic frame, after the transform tap (§2) has captured the frame's object-transform
   set, compute a 64-bit FNV hash over the captured set (all live objects' R+TR, sorted by
   object id) plus the GP0 display-list fingerprint hash (§3).
2. Feed that hash to a `RateDet` (one per `ov_frame_update`). It counts how many consecutive
   ticks the hash was unchanged; on a change it records `period = ticks_held + 1` and votes
   into a small histogram (periods 1..8). The **modal period is the measured logic rate**.
   Period 1 ⇒ the game advances every iteration (so the present rate IS the logic rate, and
   we synthesize N-1 in-betweens to reach 60); period 2 ⇒ the iteration is 60Hz-paced but
   content changes every other one (true 30fps content), etc.
3. Detector output drives **how many in-between frames to synthesize** and **the present
   pace**. It adapts per-scene: a 20fps section re-votes toward period 3 (test2 PASS).

This replaces the dead "assume 30fps". It also gives the host present loop the real cadence
so we can present a fixed 60Hz wall-clock while the game runs at its own measured logic rate.

## 2. Transform tap — capture per-object camera+model transforms each logic frame

All GTE state flows through **our adapter `runtime/recomp/gte_beetle.c`** (thin shim over
Beetle's `gte.c`). Tap there — never in vendored gte.c.

Exact tap points (functions in gte_beetle.c, all OUR code):
- **`gte_write_ctrl(reg, v)`** — capture the camera/model transform fed to RTPS:
  - `reg 0..4` = rotation matrix **R** (3×3 signed 4.12, packed: CR0=(R11,R12),
    CR1=(R13,R21), CR2=(R22,R23), CR3=(R31,R32), CR4=R33).
  - `reg 5..7` = translation **TR** (TRX/TRY/TRZ, signed 32-bit).
  - `reg 24/25/26` = **OFX/OFY/H** screen-projection params (signed 16.16, 16.16, u16).
  These are exactly the `which < 8 || 24..26` set the old emulator tap used (Beetle gte.c
  line ~500), now captured in OUR adapter. Maintain a "current transform" shadow updated on
  each ctrl write.
- **`gte_op(c, insn)`** — on RTPS (op 0x01) / RTPT (op 0x30), the current R/TR/OF/H shadow
  is the transform that produced the vertices about to be drawn. At each RTP op, snapshot
  `{R[9], TR[3], OFX, OFY, H}` as the "active transform" and remember the **input local
  vertex(es)** (GTE data reg VXY0/VZ0 = DR0/DR1 for RTPS; VXY0/1/2 for RTPT) plus the
  **output screen XY** it just wrote to SXY-FIFO (DR12..14 after the op). That tuple —
  `(local V, transform, produced SXY)` — is what lets us re-project the same geometry at a
  lerped transform later. (reproject.py proves this is 100% faithful.)

**Associating a transform with object identity (the join):**
- Register a native override on **`0x8007712C`** (the cull/LOD dispatcher) via
  `rec_set_override(0x8007712Cu, ov_object_cull)` (recomp-overrides; super-call
  `gen_func_8007712C`). On entry `a0 (c->r[4]) = object*`. Set a thread-local
  `g_current_object = a0` for the duration of the super-call, then clear it.
- Every RTP op that fires *inside* that dispatcher's call tree is thereby tagged with
  `g_current_object`. So each captured `(local V, transform, SXY)` carries an object id =
  the stable pool pointer. This is the native equivalent of the emulator's swc2/mfc2 SXY
  hook, but exact (no value-keyed guessing): the object pointer is the ground-truth id.
- Object id = the raw `object*` pointer. Stable across frames = same entity ⇒ lerp.
  Pointer absent next frame (pool-slot reuse / scene change) ⇒ snap.

Data captured per logic frame into a double-buffered "transform frame" (A = prev, B = cur):
```
struct ObjXform { uint32_t obj_id; int16_t R[9]; int32_t TR[3]; int32_t OFX,OFY; uint16_t H; };
struct XformFrame { ObjXform xf[MAX_OBJ]; int n; uint64_t set_hash; };
```

## 3. Primitive capture + match across frames

Primitives are parsed in **`runtime/recomp/gpu_native.c`**. The capture taps:
- **`gpu_gp0(w)`** — the single funnel every primitive word passes through (direct GP0
  writes AND DMA2 linked-list/block, since `gpu_dma2_*` feed `gpu_gp0`). Tee a copy of each
  completed packet (we already know packet boundaries: `gp0_exec` runs once per complete
  primitive in `s_fifo[0..s_fcount)`). Capture, per primitive: the op byte, the vertices
  (x,y), uv, color, the current `s_tp_*` texpage + `s_clut_*` CLUT, and **draw order index**.
- **Frame boundary = the display flip.** `gpu_gp1` op `0x05` (display-area start) is the
  flip. But the present chokepoint `ov_frame_update`/`gpu_present()` is the natural frame
  fence and already fires once per logic frame — use the GP1 0x05 within the frame to mark
  the segment, and `gpu_present()` as the commit point. (Journal: draws join to the PRIOR
  segment's projections — keep frame A = previous committed list, B = current.)

**Matcher keying (strongest key first):**
1. **Object id** (from §2, via the `0x8007712C` tag joined to the primitive by produced
   SXY ≈ packet vertex XY, ±2px — the proven join). Primitives carrying the same object id
   across A and B are matched directly. This is the primary key and is object-based, not
   screen-space — the key requirement from `wide60-scope-decision`.
2. For primitives with an object id but ambiguous within an object, disambiguate by
   **(draw-order-within-object, prim type, texpage, CLUT-low, uv-low)** — note CLUT/texpage
   HIGH halves alternate with frame parity (the game double-buffers CLUTs); key on the LOW
   halves only (emulator-era finding, still true — it's the game's behavior, not Beetle's).
3. Primitives with NO object id (2D UI/HUD, CPU-projected terrain) are NOT matched →
   **snapped** (drawn from frame B as-is, no interpolation). This is the no-flicker rule.

A match produces a lerp pair. A mismatch (object id present in A, gone in B, or geometry
jumped beyond a displacement gate) ⇒ that primitive **snaps to frame A** (held), never
half-interpolated. The motion-coherence filter from the emulator era (snap pairs whose
displacement deviates from the local median) carries over as a guard against wrong pairs.

```
struct Prim { uint8_t op; Vtx v[4]; int nv; uint16_t tpage, clut; uint32_t obj_id; int order; };
struct PrimFrame { Prim p[MAX_PRIM]; int n; uint64_t fingerprint; };
```

## 4. Interpolation — synthesize the in-between frame

Given matched pairs and the measured logic period `P` (§1), to present at 60Hz we emit `P-1`
in-between frames between each pair of committed logic frames (P=2 ⇒ one in-between =
30→60fps). For in-between at parameter `t` in (0,1):

- **GTE-projected, matched primitives:** lerp the object's transform
  `M = lerp(A.xform, B.xform, t)` (R and TR component-wise; OFX/OFY/H snap — constant per
  scene), then **re-project the object's captured local vertices through M** using the same
  RTPS math gte.c runs (reproject.py-proven; prototype `reproject()` shows midpoint strictly
  between endpoints). Rasterize the reprojected primitive via the existing `gpu_native.c`
  path (build the GP0 packet with interpolated screen XY, original uv/color/tpage/clut, push
  through `gp0_exec`). Lerping the **transform** then reprojecting is correct (rigid-body),
  and far better than lerping screen-space XY (which warps under perspective).
  - If per-vertex local coords weren't captured for a matched prim, fall back to lerping the
    captured screen XY of the matched A/B vertices (the emulator-era path; acceptable, less
    accurate under strong perspective, gated by the displacement test).
- **Unmatched / snapped primitives** (2D UI, terrain, scene-change pop-ins): drawn at frame
  **A** unchanged on in-betweens (held), then frame B's version appears at the commit. No
  half-state ⇒ no flicker. Per scope, terrain/UI legitimately run at logic rate.
- **Present order:** A, lerp(A,B,1/P), … lerp(A,B,(P-1)/P), B, lerp(B,C,1/P), … — exactly
  the emulator-era cadence, now rendered by our own rasterizer.

Render path: build each synthesized frame's display list into the VRAM framebuffer via the
existing `gp0_exec`/`tri`/sprite path, then `present_window()`/PPM. The clear + draw-env
(E1..E6) from frame A/B must be re-applied per synthesized frame (the game's own clear is in
its list; replay it, but skip VRAM upload packets `0xA0`/`0x80`/`0xC0` on in-betweens —
re-uploading stale textures would clobber streamed VRAM, per the emulator-era finding).

## 5. Scope / quality (no-flicker is the top priority)

- Interpolate ONLY: GTE-projected geometry (object models) + camera (the global
  camera transform is just the R/TR shared by all objects in a scene; it lerps the same way).
- Snap (logic-rate, no lerp): CPU-projected terrain, 2D UI/HUD, FMV/MDEC output, any
  primitive with no object id, any matched pair whose displacement exceeds the gate, the
  whole frame on a scene change (pool-slot churn → all ids invalid → snap that frame).
- **No half-interpolation ever.** A primitive is either lerped (clean object match) or held
  at frame A. Unmatched geometry never blends.
- Disable synthesis entirely during: FMV (StrPlayer streaming — `gpu_present` still runs but
  there are ~no GTE ops; the rate detector will report period 1 with a static set), menus
  (hold), and the intro. The detector's "no GTE ops this frame" signal gates synthesis off
  (present each logic frame once, no in-betweens) — this is automatic, not a special case.
- Widescreen is a SEPARATE tier (own the projection scale in gte_beetle.c / the rasterizer).
  Keep it OFF while validating 60fps so coordinates stay native (journal: widescreen +
  internal-res both perturb the captured screen coords; validate 60fps at native 1x/4:3).

## 6. Concrete hook list (what the implementer touches)

All taps are in OUR files (gte_beetle.c, gpu_native.c, games_tomba2.c) + a new
`runtime/recomp/wide60.c` for the capture buffers / matcher / synthesizer. NOTHING in
vendored Beetle gte.c/gpu changes.

| # | File / function | Addr / reg | What |
|---|---|---|---|
| 1 | `games_tomba2.c` `ov_frame_update` (override of `FUN_800788AC`) | `0x800788AC` | Per-logic-frame fence. After super-call: commit B→A, run matcher, drive rate detector, then for `i in 1..P-1` synthesize+present in-between, then present B. (Currently it does one `gpu_present()`; wrap it.) |
| 2 | `games_tomba2.c` new `ov_object_cull` (override of cull dispatcher) | `0x8007712C` | `g_current_object = c->r[4]` (a0=object*); super-call `gen_func_8007712C`; clear. Tags every RTP op fired in its call tree with the object id. |
| 3 | `gte_beetle.c` `gte_write_ctrl` | CR `0..7`, `24..26` | Update the current-transform shadow (R, TR, OFX/OFY/H). |
| 4 | `gte_beetle.c` `gte_op` | RTPS `0x01`, RTPT `0x30` | Snapshot active transform + input local V (DR0/1 or VXY0/1/2) + produced SXY (DR12..14) tagged with `g_current_object`. |
| 5 | `gpu_native.c` `gpu_gp0` / `gp0_exec` | — | Tee each completed primitive packet (op, verts, uv, color, `s_tp_*`, `s_clut_*`, draw order) into the current PrimFrame. |
| 6 | `gpu_native.c` `gpu_gp1` | GP1 `0x05` | Mark display-flip segment boundary. |
| 7 | `gpu_native.c` (new entry) `gpu_replay_list(PrimFrame*)` | — | Render a synthesized display list through the existing `gp0_exec`/`tri`/sprite path (skip VRAM-upload ops `0xA0/0x80/0xC0` on in-betweens). |
| 8 | new `runtime/recomp/wide60.c` | — | Owns: `XformFrame A/B`, `PrimFrame A/B`, `RateDet`, the matcher (object-id key → token key → snap), the interpolator (lerp xform + reproject local V), `g_current_object`. Init from `games_tomba2_init`. |

Key data structures (see §2/§3 and `scratch/wide60dev/lrate_proto.c`):
- `ObjXform { obj_id, R[9], TR[3], OFX, OFY, H }`, `XformFrame { ObjXform[], n, set_hash }`.
- `Prim { op, Vtx v[4], nv, tpage, clut, obj_id, order }`, `PrimFrame { Prim[], n, fingerprint }`.
- `RateDet { last_hash, vblanks_since_change, period, votes[8] }` — modal period = logic rate.

Matcher keying recap: **object id (pool pointer) primary**; within an object,
(draw-order, prim type, texpage, **CLUT/uv LOW halves only**); no id ⇒ snap.

Reprojection: lerp R + TR (OFX/OFY/H snap), then `MultiplyMatrixByVector(R, V) + TR<<12`,
`Z`-divide by `H`, `OFX/OFY + IR*Q` — identical to gte.c RTPS (reproject.py: 100% faithful).

## Open items / risks
- The §2 join (RTP-produced SXY ≈ GP0 packet vertex XY, ±2px) must be re-validated on the
  native path — the emulator-era 79/79 / 97%-within-2px result was on Beetle. Same math, but
  confirm the native gpu_native.c vertex coords are in the same space as the SXY-FIFO output
  (no upscale/widescreen applied — keep both OFF during validation).
- Local-vertex capture from the GTE data regs at RTPS time needs the exact DR indexing
  (VXY0=DR0 low/high, VZ0=DR1). If capturing local V proves fiddly, the screen-XY lerp
  fallback (§4) still yields object-correct motion for the common rigid case.
- `MAX_OBJ`/`MAX_PRIM` sizing: journal measured 68-90 objects, ~1400 draws/frame for Tomba2.
  Size buffers ≥ 256 objects / ≥ 4096 prims.
