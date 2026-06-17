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
- Full basis (right/up) + projection consts (GTE OFX/OFY/H): TODO — read the GTE setup at frame start.

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

## Open RE items (next, in order)
1. ~~The entity list + its walk~~ — **DONE** (above): lists `DAT_800fb168`/`DAT_800f2624`, walk
   `FUN_8007a904`, node layout. Handlers are per-object fn pointers @ +0x1c (not a type-indexed table).
2. Find `FUN_8007a904`'s caller (frame placement) — it has no static caller (called via task/overlay).
   Confirm at runtime (objlog / a tap) that it's the per-logic-frame object driver.
3. Object node full lifecycle: spawn/link (`:55976–56167`) and free; the pool base/extent.
4. Camera basis + GTE projection setup (for native render submission, Phase 3).
5. Whether handlers call the cull/submit path (`FUN_8007712c`) in field scenes, or a different render
   path (the journal noted the cull dispatcher didn't fire in the demo). `PSXPORT_OBJLOG=1` answers this.
