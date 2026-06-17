# Tomba! 2 engine RE — the game's own engine (for the native reimplementation)

Living doc for the native-engine port (plan: reimplement Tomba2's engine in PC-native C, oracle =
the recompiled MIPS body). Source for all line refs: `scratch/decomp/ram_f1000_all.c` (Ghidra decomp of
MAIN.EXE). Addresses are PSX RAM virtual addresses. **Verify against the recomp body before relying on
any field — decomp is point-in-time.**

## Top-level control flow
- **Main loop** `FUN_80050b08` (`:31269`, override `ov_game_main`). After GPU/double-buffer + lib init,
  loops forever. Per frame, in order:
  1. clear OT / set buffer (`FUN_80081458`, buffer ptr `PTR_DAT_800ed8c8`, parity `DAT_1f800135`)
  2. `FUN_800788ac()` — per-frame fence (input + game sub-tick), override `ov_frame_update`
  3. `FUN_80051e60()` — **task scheduler** (runs the cooperative tasks; gameplay logic lives here)
  4. `FUN_80080f6c(0)` — flush/draw the OT to the GPU
  5. spin until vblank counter `DAT_800e809c` reaches `DAT_1f800235` (frame-rate gate)
  6. `FUN_800506d0()`, then swap buffers (`DAT_1f800135 = 1 - DAT_1f800135`)
- **Per-frame fence** `FUN_800788ac` (`:55345`, override `ov_frame_update`): reads pad, computes button
  **edges** — pressed = `DAT_800e7e68`, released = `DAT_800f23a4` (cur `DAT_800ecf54` & ~prev) — then
  calls `FUN_8005229c` (a CD/load sub-state-machine; NOT the object walk).

## Task scheduler (the engine is task-based, PSX-TCB style)
`FUN_80051e60` (`:?`) walks a **task table at `0x801fe000`**, stride **0x38 bytes**, until `0x801fe14f`
(~6 task slots). Per slot, field `+0` = state: `2` = ready→switch to it (`FUN_80080880`=change-thread),
`3` = needs-spawn (`FUN_80080860`=open-thread with args at +0x10/+0x18/+0x20, stores tid at +8). Thread
funcs are already native (`ov_open_thread`/`ov_change_thread`/`ov_switch`, `runtime/recomp/threads.c`).
**Gameplay (entity update + render submission) runs inside the main gameplay task** — its body is the
next RE target (see Open items).

## Object / entity model — the entity LIST + node (RESOLVED via RAM-dump search)
The active entities are a **doubly-linked list of pool nodes, stride 0xD0 (208 bytes)** (found by
searching gameplay RAM dumps `scratch/bin/{level_ram,ours_ram_gf}.bin` for the handler address bytes;
verified the prev/next chain). **Two lists** (objects spawn into one or the other; init `:55858/55860`,
insert `:55976–56167`):
- head **`DAT_800fb168`** (0x800fb168) and head **`DAT_800f2624`** (0x800f2624).

Node fields (offsets from the node = the handler's `param_1`):
| off | type   | meaning |
|-----|--------|---------|
| +1  | u8     | per-frame render flag (cleared by the walk + by cull `FUN_8007712c`) |
| +4  | u8     | per-type state/substate (e.g. `FUN_80040558` switches on `param_1[4]`) |
| +0xc| u8     | **entity type** (cull switch) |
| +0xe| u16    | model id (`& 0x3fff`), set by `FUN_80077b38` |
| +0x1c| ptr   | **handler fn pointer** (per-type update/render; called with node in a0 — gameplay code, stays PSX) |
| +0x20| ptr   | **prev** node |
| +0x24| ptr   | **next** node (list walk follows this) |
| +0x28| u16×2 | low = state (0x0002 = active?), high = per-object id |
| +0x2e| u16   | **position X** |
| +0x32| u16   | **position Y** |
| +0x36| u16   | **position Z** |
| +0x38| ptr   | model-data pointer (set by `FUN_80077b38` from a table) |

### The entity-list walk — `FUN_8007a904` (the engine's per-frame object driver)
```c
for (n = DAT_800fb168; n; n = *(n+0x24)) { *(n+1) = 0; (*(handler@n+0x1c))(n); }  // list 1
for (n = DAT_800f2624; n; n = *(n+0x24)) { *(n+1) = 0; (*(handler@n+0x1c))(n); }  // list 2
```
Clears the render flag, then calls each node's handler (the PSX gameplay/render routine). A second
walk of `DAT_800f2624` exists at `:18660` (likely a separate pass). **This is the native entity
manager's target (Phase 1):** reimplement the walk in native C, call each handler via `rec_dispatch`
(gameplay stays PSX), and snapshot node transforms across frames for correct object interpolation.

## Per-object cull / LOD + submit
- **Cull/LOD** `FUN_8007712c` (`:54440`, override `ov_object_cull`): args = (obj*, dx, dy, dz) where
  (dx,dy,dz) = object pos − camera pos, sign-extended from the u16s. Computes `dist² = dx²+dy²+dz²`
  (`FUN_80077fb0` ≈ isqrt), and **depth along camera forward** = `dx*camFwd.x + dy*camFwd.y + dz*camFwd.z`
  (`_DAT_1f8000e8/ea/ec` = camera forward vector). Branches on entity type `+0xc` and global mode
  `_DAT_1f800084` to decide cull (return 0) vs LOD tier. Special: if `DAT_800bf870 == 4`, force mode 2.
- **Submit wrappers** (each sets render mode then calls the cull dispatcher), called from the per-type
  entity handlers:
  | fn | `_DAT_1f800080` | `_DAT_1f800084` | extra pos offset |
  |----|----|----|----|
  | `FUN_8007778c` `:54685` | 0 | 0 | none |
  | `FUN_800777fc` `:54702` | 0 | 2 | none |
  | `FUN_80077870` `:54719` | 0 | 1 | none |
  | `FUN_800778e4` `:54736` | 0 | 0 | +Y(param2) |
  | `FUN_80077958` `:54754` | 0 | 0 | +X,+Y |
  | `FUN_800779d0` `:54772` | 0 | 0 | +X,+Y,+Z |
  | `FUN_80077a4c` `:54791` | 1 | 0 | +X,+Y,+Z |
  | `FUN_80077acc` `:54810` | 1 | 4 | absolute pos args |
- Per-type handlers (call the wrappers with their obj*): e.g. `:21274 :27367 :28376` → `FUN_8007778c`;
  `:45944 :45991 :46120` → `FUN_80077a4c`. These are the entity update/render routines (Phase 2 targets).

## Camera
- Position (u16): `_DAT_1f8000d2` (X), `_DAT_1f8000d6` (Y), `_DAT_1f8000da` (Z).
- Forward vector (s16): `_DAT_1f8000e8/ea/ec` (used in the cull depth dot product).
- Full basis (right/up): per-object rotation matrix is loaded to GTE CR0-4 + translation CR5-7 right
  before each RTPS/RTPT (96 / 54 static ctc2 sites) — the per-object transform, the Phase-3 native target.

## Projection setup — `gen_func_800509B4` (0x800509B4) = the NATIVE WIDESCREEN lever (RESOLVED later-98)
Found by histogramming `gte_write_ctrl(reg,…)` in `generated/`: OFX/OFY/H (CR24/25/26) are written at
exactly 2 sites — libgte `InitGeom` defaults + this one real config. The engine's projection config:
```
gen_func_800509B4 (0x800509B4):
  InitGeom (gen_func_80083FF8 / 0x80083FF8): ZSF3(cr29)=341, ZSF4(cr30)=256, H(cr26)=1000,
      DQA(cr27)=-4194, DQB(cr28)=320<<16, OFX/OFY=0  (libgte reset; H+depth-cue overwritten next)
  SetGeomOffset(160,120)  (gen_func_800846D0 / 0x800846D0): OFX(cr24)=160<<16, OFY(cr25)=120<<16
  SetGeomScreen(350)      (gen_func_800846F0 / 0x800846F0): H(cr26)=350; also caches H=350 @0x801003F8
```
So the GTE projection is: **screen center (OFX,OFY)=(160,120), focal length H=350**, screen 320×240.
Screen X = OFX + IR1·H/Sz, screen Y = OFY + IR2·H/Sz.
- **NATIVE widescreen (no squish, no renderer trick):** override `gen_func_800509B4` to set **OFX=214**
  (=428/2 for 16:9) and **widen the draw-environment + clip rect to 428** (keep OFY=120, H=350 → identical
  vertical FOV and per-unit scale). The GTE then projects vertices across the wider screen → genuinely
  wider horizontal FOV, computed by the engine's own projection. 2D HUD (drawn in screen space, bypasses
  GTE) is repositioned separately. **TODO:** find the draw-environment / clip-rect (screen width) setup —
  near the double-buffer/OT setup `FUN_80081458` / disp-env; needed to widen the clip to match OFX.
- **Higher res (native, not supersampling):** our native renderer rasterizes the engine's submitted
  geometry; render the FB at NxN of 320×240 (or 428×240) by scaling the rasterization viewport — the
  geometry is the same engine output, just sampled denser. (Distinct from the rejected supersample-and-
  downscale FB-cram trick.)

## Water — drawn via a NON-GTE path (RE in progress, later-98)
Observed during the lighting work (normal-viz): terrain/ground polys get a reconstructed per-face normal
(they go through RTPS/RTPT), but the **water surface does NOT** — it renders as a flat unlit layer. So the
water is **not GTE-projected 3D**; it's a separate screen-space / fixed-projection layer (likely a scrolling
textured surface and/or a framebuffer-reflection effect). The user reports it rendering wrong (green smeared
streaks instead of blue reflective water). **NEXT RE:** identify the water's draw — point the provenance
tool (`PSXPORT_PROVAT="x,y"`, gpu_native.c) at a water pixel to get the prim op/texpage/CLUT + owning node
+ handler addr, then read that handler in the decomp. Compare to the oracle at the SAME game-state (dual-
core harness, below), NOT by frame number.

## Lighting / shading model (RESOLVED — later-96, via GTE op histogram + control-reg snapshot)
Tooling: `PSXPORT_GTEPROBE=<frame>` (gte_beetle.c) dumps the GTE ops that ACTUALLY execute + a
lighting/fog control-register snapshot. Static-callsite histogram across `generated/shard_*.c` agrees.

**There is NO dynamic GTE lighting.** The hardware per-vertex lighting ops `NCDS/NCDT/NCCS/NCCT/NCS/
NCT/CC/CDP` execute **zero** times (and have zero call-sites). So no normal·light-matrix shading, no
light sources. What the GTE actually runs (f1500/f3000 counts): `RTPS/RTPT` (projection), `MVMVA`
(transforms), `NCLIP` (backface sign), `AVSZ3/4` (OT depth), `GPF` (interpolate IR·IR0), and
`DPCS/DPCT` (depth-cue). Implications for the shading model:
- **Vertex colors are BAKED** into the model data (artist-painted per-vertex RGB), not computed.
- **`GPF` (highest count after RTP) scales the baked color by a scalar IR0** — the per-object/global
  brightness + fade-in/out factor. (GPF: MAC = IR0·IR → color FIFO.)
- **Atmosphere = GTE depth-cue FOG (`DPCS`/`DPCT`)**: final vertex color is interpolated toward the
  **FarColor (CR21-23)** by a depth factor `IR0 = DQB + DQA·(H/Sz)` (`DQA=cr27`, `DQB=cr28`). The
  FarColor is **scene-tinted**: f1500 (water/dusk) = (0,0,0) fade-to-black; f3000 (lava) = (1280,0,0)
  red glow. DQA=6, DQB=0 in both. This depth-cue IS "the game's lighting/atmosphere".

**Where final color enters the GPU (the interception point):** the computed per-vertex RGB lands in the
**GP0 gouraud-polygon packets** → captured in `gpu_native.c` gp0_exec polygon tee as `rs/gs/bs`
(op 0x20-0x3F). That is the universal sink regardless of how the game derived the color.

**What a native lighting engine needs that the PSX stream doesn't directly give: per-vertex normals /
3D position.** Now available: PGXP (later-95) caches per-vertex screen (x,y) + **precise_z** (view-space
Z). Unproject (screen + z via H/OFX/OFY, CR24-26) → view-space position per vertex → per-FACE normal by
cross product of triangle edges. That unlocks PC-native directional/point lighting, normal-based
shading, SSAO, and a replacement per-pixel fog (read the scene FarColor from CR21-23 for the tint),
all replacing/augmenting the baked color + GTE depth-cue. (Camera basis: see Camera section / CR24-31.)

## Graphics pipeline — the REAL draw path (libgpu), the ownership target (later-99)
Today the recompiled game runs **Sony libgpu** → writes GP0/GP1 → our GPU emulator (gpu_native/gpu_vk)
just rasterizes the resulting byte stream. "Owning the graphics" = reimplementing the libgpu layer in
native C (game calls into OUR DrawOTag/PutDrawEnv/primitive code), so every draw is understood, not
black-boxed. The layer is small and standard (libgpu), dispatched via a jump-table at **0x800A5998**
(the libgpu "GPU sys" struct; entries are fn-ptrs at +0x08 DMA-send, +0x14 DrawOTagEnv, +0x2c ClearOTagR,
+0x3c DrawOTag/DrawSync).

**Per-frame loop** `FUN_80050b08` (0x80050b08):
```
parity = DAT_1f800135 (0/1);  drawbuf base = 0x800bfe68 + parity*0x14000 (DAT_800bf544)
ctx = &DAT_800e80a8 + parity*0x81c              // per-buffer GPU context (DRAWENV+DISPENV+OT head)
FUN_80081458(ctx, 0x800)   = ClearOTagR(OT, 2048 entries)   // reset ordering table
FUN_800788ac()             = input + sub-tick
FUN_80051e60()             = task scheduler -> BUILDS the OT (entity handlers AddPrim into it)
FUN_80080f6c(0)            = DrawOTag(OT)        // DMA the built OT to the GPU
wait DAT_800e809c >= DAT_1f800235               // vblank gate: 1 = 60fps, 2 = 30fps  (<-- 60fps lever)
FUN_800506d0()
swap: FUN_80081560(ctx+0x1ffc)=PutDrawEnv (draw-area CLIP+offset),
      FUN_800815d0(ctx+0x2014)=PutDispEnv (display area),  flip parity
```
libgpu primitives (all via the 0x800A5998 table): `FUN_80080f6c`=DrawOTag, `FUN_80081458`=ClearOTagR,
`FUN_800810f0`=ClearImage, `FUN_80081180`=ClearImage2 (GP0 fill bit 0x80000000), `FUN_80081218`=LoadImage
(CPU→VRAM), `FUN_80081278`=StoreImage (VRAM→CPU), `FUN_800812d8`=MoveImage (VRAM→VRAM copy),
`FUN_80081560`=PutDrawEnv, `FUN_800815d0`=PutDispEnv, `FUN_80081504`=DrawSync+DrawOTagEnv.
- **60fps lever (native, real):** `DAT_1f800235` is the vblank-count target the loop waits for (1 vs 2).
  Our native loop owns this wait → drive at 60 by gating on 1 and interpolating object transforms (the
  entity-list snapshot, Phase-1). Not a renderer trick.
- **Widescreen lever (native, real):** PutDrawEnv (ctx+0x1ffc, draw-area clip) + PutDispEnv (ctx+0x2014)
  + the GTE OFX (FUN_800509B4). Widen all three → genuine wider FOV. (Supersede the rejected FB re-center.)
- **MoveImage (FUN_800812d8) = VRAM→VRAM copy** — the likely water-reflection / fade-buffer mechanism;
  prime suspect for the broken water (reflection copy) AND a place our GP0 emulator can drift. RE next.

### Native ownership plan (reimplement libgpu, keep recomp body as oracle via rec_set_override)
1. Own **DrawOTag** (FUN_80080f6c): walk the ordering table in native C, decode each primitive packet by
   type (POLY_F/FT/G/GT, SPRT, TILE, LINE, the env/copy packets), submit to our renderer WITH semantics
   (so we know "this is the fade tile", "this is water", "this is HUD"). The recomp libgpu stays callable
   for A/B diff. This is the single highest-leverage step — it converts the GP0 black box into an
   understood scene graph.
2. Own **PutDrawEnv/PutDispEnv** + the GTE projection (FUN_800509B4) → native widescreen.
3. Own **MoveImage/LoadImage/StoreImage** (VRAM↔VRAM/CPU) → fixes reflection/copy effects (water, fades).
4. Own the **OT clear/build** + the entity-list walk (Phase-1) → native 60fps interpolation.

## Fades (intro cutscene "flashes full visibility on fade-in") — RE target
Symptom: during a story-cutscene fade-IN, one frame shows the scene at FULL brightness before the fade
takes over. Tomba2 has no GPU global brightness; a fade is either (a) the GTE color scalar (GPF, IR0 —
later-96: GPF scales baked vertex colors by a per-frame brightness/fade factor), or (b) a full-screen
semi-transparent TILE drawn over the scene. The flash = the fade factor/overlay is one frame late or
mis-initialized in our path. **To fix it properly we must own DrawOTag** (see plan #1): then we see the
fade primitive/var directly each frame and can compare its first-frame value vs the oracle. Tracked as
the concrete proof that we now understand the draw path.

## Scene accounting — native classified display list (OWNERSHIP step 1, later-99)
`PSXPORT_SCENEDUMP=N` (gpu_native.c `gpu_scene_dump`): a read-only walk of the same OT DrawOTag DMAs,
classifying every primitive (poly/rect/line/fill/VRAM-copy/upload/env) + tracking draw-area/offset, and
logging the categories where effects live (VRAM→VRAM copies, fills, large/semi overlays = fade tiles).
This is the port ACCOUNTING for every draw instead of blind GP0 rasterization. Findings (f1500 water/demo):
- **TWO DrawOTag passes per frame:** (1) a tiny setup OT — `FILL (0,0,0) 320x240 @ (0,256)` = back-buffer
  clear (y=256 is the parity-1 framebuffer); (2) the main OT = **514 polys + 389 rects + 11 env**, ~1 tiny
  (2x1) VRAM copy.
- **Water is ordinary TEXTURED GEOMETRY, not a framebuffer-reflection copy** (the only VRAM→VRAM copy is
  degenerate 2x1). REFUTES the earlier "reflection effect" hypothesis. The broken-water artifact the user
  saw was almost certainly the PGXP value-keyed vertex-smoothing trick tearing the water's dense textured
  grid — now disabled by default (bf69890). The water polys are among the 514; next attribute them by
  screen region + texpage/CLUT (extend SCENEDUMP with a region filter) to confirm vs the oracle.

## Open RE items (next, in order)
1. ~~The entity list + its walk~~ — **DONE** (above): lists `DAT_800fb168`/`DAT_800f2624`, walk
   `FUN_8007a904`, node layout. Handlers are per-object fn pointers @ +0x1c (not a type-indexed table).
2. Find `FUN_8007a904`'s caller (frame placement) — it has no static caller (called via task/overlay).
   Confirm at runtime (objlog / a tap) that it's the per-logic-frame object driver.
3. Object node full lifecycle: spawn/link (`:55976–56167`) and free; the pool base/extent.
4. Camera basis + GTE projection setup (for native render submission, Phase 3).
5. Whether handlers call the cull/submit path (`FUN_8007712c`) in field scenes, or a different render
   path (the journal noted the cull dispatcher didn't fire in the demo). `PSXPORT_OBJLOG=1` answers this.
6. ~~Projection setup~~ — **DONE** (later-98): `gen_func_800509B4`, OFX/OFY/H = 160/120/350. Widescreen
   lever identified. Remaining: the draw-environment/clip-rect width setup (to widen the clip with OFX).
7. **Water draw path** (broken in port): NOT GTE-projected (separate layer). Identify via provenance +
   dual-core state-synced diff. The immediate user-visible regression.

## Dual-core differential harness (planned tooling — sync on GAME STATE, not frame number)
Per user direction: run the port and the Beetle oracle (`runtime/wide60rt`) and compare at the SAME game
state (e.g. attract/demo start), because their boot timings differ so frame numbers don't align. Sync via
an observable guest-RAM latch — e.g. the stage var `native_boot` prints as `stage=DEMO 0x801062E4`, or the
documented scene latch `0x800BE258==2`. Gate both cores to that latch, then diff GP0 stream / VRAM /
framebuffer there. Extends the existing `tools/drive.py` + `docs/diff-driver.md`.
