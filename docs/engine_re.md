# Tomba! 2 engine RE — the game's own engine (for the native reimplementation)

Living doc for the native-engine port (plan: reimplement Tomba2's engine in PC-native C). **Runtime is
INTERPRETER-ONLY (later-103):** un-owned code runs the real PSX binary on the flat interpreter; the static
recompiler is an offline analysis aid only. The **verification oracle is the Beetle emulator**
(`runtime/wide60rt`). Source for all line refs: `scratch/decomp/ram_f1000_all.c` (Ghidra decomp of
MAIN.EXE). Addresses are PSX RAM virtual addresses. **Verify against the oracle (or the original interpreted
path) before relying on any field — decomp is point-in-time.**

## Top-down PC-native engine port — FROM THE ENTRY POINT (later-159, the active spine)
Per the CLAUDE.md boundary, we REIMPLEMENT the engine PC-native starting at the game's entry point and
going down. The spine:
`crt0 FUN_800896E0` (BSS-zero, SP/gp/heap) `→ main FUN_80050b08` (override `ov_game_main`, native_boot.cpp)
`→ [init prefix]` `→ register task 0 = the stage sequencer FUN_800499e8 (START.BIN, stage-0 overlay load)`
`→ native frame loop`.

**Tool — `tools/disas.py <addr> [--mem]`:** MIPS-I disassembler for MAIN.EXE that resolves `lui+addiu/ori`
address builds and annotates every load/store with its absolute target + WIDTH (sb/sh/sw). USE THIS before
reimplementing any engine fn — Ghidra `DAT_*` hides widths and a wrong width silently corrupts the interface
state the PSX content reads (later-158). `--mem` = just the memory effects.

**Init prefix of `ov_game_main` — classified (PLATFORM = native platform owns it / keep dispatched;
ENGINE = reimplement PC-native):**
| fn | what | class |
|----|------|-------|
| 80089788 | one-shot guard (DAT_800abef0) | platform (no-op) |
| 80085b20 | intr.c lib callback | platform |
| 800898a0 | **CdInit** | platform (native CD) |
| 80080bf0(3) | **ResetGraph** | platform (native GPU) |
| 80080d64/80080ed4 | SetGraphDebug / **SetDispMask** | platform |
| 800865f0 | lib state setter (DAT_800abe20) | platform |
| **80050a0c** | **frame-state init** (vblank ctr, buffer parity DAT_1f800135, frame divisor DAT_1f800235, swap-mode DAT_1f80019c, …, DAT_80105ee8=0x45) | **ENGINE — DONE (engine_init.cpp `eng_init_framestate`)** |
| **800509b4** | **display + GTE projection**: InitGeom (80083ff8: ZSF3=0x155 ZSF4=0x100 H=1000 DQA/DQB), SetGeomOffset(160,120), H=350→DAT_801003f8, SetGeomScreen | **ENGINE — DONE (`eng_init_display`)**; FUN_80050738 (PSX draw/disp env structs) still dispatched (native single-env = next display step) |
| **80050a80** | **camera init**: identity matrix → scratch 0x1F8000F8 (the camera-rot the renderer reads) + 0x1F800118; cam fields (_1f8000ec=0x1000, _ee=H*-5, _d8=H*-0x50000) | **ENGINE — DONE (`eng_init_camera`)** |
| 80096a70/80099310/800991b0/800993a0 | SPU/sound + heap lib | platform (verify) |
| 80089bac(0xe,…) | CD/_bu command | platform |
| 80085900(3/1) | **VSync** | platform |
| 80075130 | **font/text system init** (FUN_800963a0/80091d70/…) | ENGINE (UI — later) |
| 8009c620(0) | subsystem init | TBD |
| 8001cc00 | DMA channel + pad init (FUN_80080830 0xf4000001…) | platform (DMA/pad hw) |
| 800520e0 | engine subsystem init (FUN_8007b328, DAT_800ecf4x, FUN_80088b00/80086620/80087a60) | ENGINE (later) |
| 80051e00 | **scheduler task-table init** (DAT_801fe000, "A0F.BIN", stride 0x70) | ENGINE (later) |
| 80051f14(0,FUN_800499e8) | **register task 0 = stage sequencer** (the level loader's entry) | ENGINE (the big next system) |
| 80085bb0(LAB_800506b4) | register VSyncCallback | platform |

**Order of attack / status:** init display+camera DONE (eng_init_*). **Level LOADER core DONE** —
FUN_800450bc overlay loader is PC-native (engine/engine_level.cpp `eng_load_stage`, later-162); its
scheduler-coupled orchestration (FUN_80052078 task-restart ending in ChangeTh/ov_switch longjmp,
FUN_800499e8 file-resolve) stays dispatched and calls the native loader — native-izing them needs the
cooperative-scheduler longjmp handshake first. Remaining named systems: **object/entity placement &
spawning**, the **main menu** (DEMO stage state machine @0x801062E4), font/text (80075130), engine
subsystem init (800520e0), and a real PC-native single display env (replace FUN_80050738). Each: `disas.py`
the fn, understand the data, reimplement PC-native in `engine/`, keep the PSX-content interface state exact.

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

## Deferred render pipeline — the render-command QUEUE + flush (later-130, the missing architecture)
Geometry submission is NOT inline in the entity handlers. It is a **deferred two-phase** system (proved by
`PSXPORT_DEBUG=geomblk`: all owned submits run with `g_current_object==0`, AFTER every per-object cull):
- **Phase 1 (entity walk, `FUN_8007a904`):** each handler runs gameplay AND, if its object is visible,
  ENQUEUES a **render command** into a per-frame render-command list (it does NOT project here).
- **Phase 2 (flush):** a loop drains the command list, loading each command's GTE transform and dispatching
  it to a per-mode renderer that does the actual projection + packet build (GT3/GT4 etc.).

**Render-command list + flush loop** (`gen_func_8003F174`, and a sibling at `gen_func_8003F0xx`; callers
0x8003d074/0x8003f138/0x8003f228): the list header has the command COUNT at `+8` (a second count/flag at
`+9`); the commands are reached via a **pointer array at `list+0xc0`** (`lw 0xc0(cursor)`, cursor += 4 per
iter). For each command struct `cmd`:
- `cmd+0x18 .. +0x2c` = the **per-object GTE transform** (rotation matrix + translation), loaded straight
  into the GTE control regs at flush via the `lwc2/ctc2` block — THIS is where the "96/54 ctc2 sites" live
  for queued objects (the transform is captured into the command at enqueue, replayed here).
- `cmd+0x40` = the **geomblk pointer** (the model's primitive-record list) → passed as `a0` to the dispatcher.
- flush calls dispatcher `gen_func_8003F698(a0=geomblk, a1=*0x800ED8C8 OTbase, a2=flag)`.

**Mode dispatcher `gen_func_8003F698`:** reads a render-mode byte `*0x800BF870` (`DAT_800bf870`, 0..0x15;
the engine_re cull note "if DAT_800bf870==4 force mode 2" is THIS global), indexes a **22-entry jump table at
0x80015268**, tail-calls the per-mode renderer. Early-outs to the generic GT3/GT4 path (mode→0x800803DC) when
`*0x1F800234 != 0`, `a2&1`, or mode≥0x16. Table (mode → renderer):
`0:0x80146478  1:0x80132dc0  2:0x8012555c  3:GT3/GT4(0x800803DC)  4:0x8013dafc  5:0x801362cc  6:0x8013d568
7:0x8012e1a0  8:0x8012a9dc  9..0x13:GT3/GT4  0x14:0x80116b14  0x15:0x8010b1b8`. The `0x8013xxxx` entries are
the per-scene OVERLAY submitter variants the margin handlers feed (NOT the natively-owned GT3/GT4 — that is
only mode 3 / ≥9). `*0x800BF870` is a GLOBAL set before/within a flush pass, so one pass renders in one mode.
- **Command struct fields (confirmed via `PSXPORT_DEBUG=rcmd`, later-131):** `+0x18` GTE transform (CR0-7),
  `+0x40` geomblk ptr. `a2`/`flag` passed to the dispatcher: bit0 forces the generic GT3/GT4 path. At the
  field, base/world commands carry flag=1, the widescreen-margin commands carry flag=0. The margin commands'
  geomblks are REAL (0x801exxxx model prim-lists) even though the node `+0x38` mdata is 0 — geometry is
  resolved at enqueue, not from `+0x38` (corrects later-129's "no model data").
- **Command struct ENQUEUE — `gen_func_80051B70`** (and a multi/instanced loop variant ~0x80051A60, and a
  layer-builder ~0x8003AE28): args `a0`=object node, `a1`=model group idx, `a2`=model sub idx. Allocates the
  command (`gen_func_8007AAE8`), stores the cmd ptr at **node+0xc0** (the object→command link), clears the cmd
  header (`+0/2/4/8/0xa/0xc`=0, `+6`=-1), sets scale `cmd+0x38/3a/3c`=0x1000, and resolves the geomblk via:
- **Geomblk resolution (data-driven, leaf `gen_func_80051B04`):** `geomblk = T + *(T + sub*4 + 4)`, where
  `T = *(0x800ECF58 + group*4)`. A **two-level model table at 0x800ECF58**: outer index = group, inner =
  sub-model. Fully deterministic from the handler's `(group,sub)` selectors → the native render-half computes
  the geomblk the same way (no guessing). Stored to `cmd+0x40`.
- **Transform build — `gen_func_80051C8C`:** builds the object's matrix into `node+0x98` (rotation from
  `node+0x54/56/58` via `gen_func_80084D10/80084EB0/80085050`) and translation `node+0xac/b0/b4` from position
  `node+0x2e/32/36`. This matrix is what the flush loads (cmd+0x18) into the GTE before projecting.
- **Commands are PERSISTENT, not per-frame (later-132, via `PSXPORT_WWATCH`):** `cmd+0x40` was written exactly
  ONCE across 2905 frames — the render command (geomblk + base transform) is built at object spawn / scene
  setup and kept at `node+0xc0`. Per frame only the transform translation/rotation updates + the flush runs.
  So the `+1` re-include's effect = the (already-built) cmd of a re-included object being drawn; the NATIVE
  margin plan reads `node+0xc0` (its persistent cmd) for a culled margin object and adds it to the flush list,
  WITHOUT poking `+1` (which also ticks gameplay). Probes: `PSXPORT_DEBUG=cmdenq`/`flush`/`enq`,
  `PSXPORT_WWATCH=lo,hi` (word-store PC tap; NB byte stores like `list+8` count are not caught).
- **NATIVE-OWNED (later-135) — `gen_func_8003CDD8` (the per-object flush), `gen_func_8003F698`
  (dispatcher generic path) and `gen_func_800803DC` are now reimplemented in C** (`engine/engine_submit.c`
  `submit_perobj_flush`/`native_dispatch`/`native_gt3gt4`, registered on `0x8003CDD8`). The world render
  submission runs with NO guest render code; **VRAM byte-identical** vs the recomp body (headless field
  f328, A/B `PSXPORT_PEROBJ_RECOMP=1`). Only the per-scene overlay submitter variants (mode-table
  `0x8013xxxx`) + the resident byte-packed `0x80027768` are still recomp (next RE). See journal later-135.
- **CORRECTION (later-133/134) — THE OBJECT NODE *IS* its render-command list, and rendering is per-object.**
  `node+0xc0` is the BASE of a cmd-pointer ARRAY (count at `node+8`), not a single ptr. Each visible object
  is rendered by **`gen_func_8003CCA4(node)`** (per-object render dispatch by `node+0xd` via jump table
  @0x80014ec8) → **`gen_func_8003CDD8(node, flag)`** (the MAJOR world flush; loop @0x8003ce40 reads
  `cmd = node[0xc0+i*4]`, `geomblk = cmd+0x40`, COMPOSES camera(`0x1f8000f8`)×object-matrix(`cmd+0x18`) into
  the GTE, translation from `cmd+0x2c/0x30/0x34`) → dispatcher `gen_func_8003F698`. The minor flush
  `0x8003F174` only drains the one static-decor list `0x800fb218` (8015ca04×24). The per-object transform
  (`cmd+0x18`, `node+0x98`) is built each frame by **`gen_func_80051C8C(node)`** in the handler's VISIBLE
  branch — a culled object's is stale/zero, so the native margin must call it before the render. Native margin
  (engine/margin_render.cpp): collect re-included type-`0x03` nodes in cull (no +1), then per node
  `gen_func_80051C8C` + `gen_func_8003CCA4` after the walk → +24 margin renders, base 100 byte-identical,
  gameplay 0-diff to render-cache. See journal later-133/later-134. RE REPL: `dbgclient.py ents/node/call/geomblk`.

## Geometry SUBMIT — `0x8007FDB0` (POLY_GT3 tri) + `0x8008007C` (POLY_GT4 quad) — NATIVE-OWNED (engine_submit.c)
These are the resident routines that turn a model's pre-built primitive-record list into GPU packets in
the OT. Both are now reimplemented natively in `engine/engine_submit.c` (`ov_submit_poly_gt3/4`),
**0-diff vs the recomp body on the field** (A/B: `PSXPORT_SUBMIT_RECOMP=1` keeps the recomp bodies).
This is the deletion of the reason the value-keyed "attach" depth-recovery hack existed — the engine now
computes the projection and can carry the real per-vertex view-Z straight to the renderer (Phase 2).
- **Caller** `gen_func_800803DC` (`0x800803DC`): `r16 = mem_r32(geomblk+0)` packs counts (low16 = tri
  count, high16 = quad count); `a0 = geomblk+16` (record array). Calls tri-submit `(a0, OTbase, triN)`,
  then quad-submit `(ret, OTbase, quadN)` — **tri-submit RETURNS a0 advanced past its records** (= quad
  array base), so the return value matters. Records are tri[] then quad[], contiguous.
- **Global** packet-pool write pointer `0x800BF544` (advanced past each committed packet; written back).
- **Triangle** (`0x8007FDB0`): 36-byte record `{+0 rgb0|code, +4 rgb1(rgb2=rgb1<<4), +8 uv0|clut,
  +12 uv1|tpage, +16 VXY0, +20 VZ0|VZ1, +24 VXY1, +28 VXY2, +32 VZ2|uv2}` → load V0..V2, **RTPT**, FLAG
  test, **NCLIP** backface (MAC0>0), frustum cull (drop iff all SX≥320 or all SY≥240 — right/bottom
  only), OT-z, → 40-byte **POLY_GT3** `{tag,rgb0,SXY0,uv0,rgb1,SXY1,uv1,rgb2,SXY2,uv2}`, OT tag len 9.
- **Quad** (`0x8008007C`): 44-byte record, project the lone 4th vertex (V3) via **RTPS** first, then the
  front tri (V0,V1,V2) via **RTPT**; 4-vertex frustum cull; → 52-byte **POLY_GT4**, OT tag len 12.
- **OT-bucket depth** (the `>>2` order-stat): record code byte `(code>>24)&3` selects **type 1 = farthest
  (max SZ)**, **type 2 = nearest (min SZ)**, **else = hardware AVSZ3/AVSZ4 average** (SZ FIFO: tri DR17-19,
  quad DR16-19). Then a log-compress `idx=(otz>>(sh&31))+(sh<<9), sh=otz>>10` and a **drawable clamp
  `idx∈[4,2047]`** (else the prim is dropped). Verified the min/max by exhaustively tracing both bodies.
- **Note:** the recomp bodies are slightly **nondeterministic** run-to-run (≈300px at one field frame —
  uninitialised packet padding the GPU re-reads); the native versions are deterministic and match a
  freshly-captured recomp run exactly.
- **Phase-2 depth — DONE for the owned prims.** Each owned submit records the vertex SZ (view-Z) keyed by
  the packet word address (`projprim_set_pz`); the renderer reads it (`projprim_lookup_pz` →
  `proj_pz_to_ord`) for true D32 occlusion under `PSXPORT_NATIVE_DEPTH`/`PSXPORT_SBS`. The value-keyed
  "attach" ring (gte_op capture + mem.c store hook) is **deleted**. Deterministic; fixed the water
  punch-through/flicker.
- **Overlay ownership via SCAN-ON-LOAD (later, this is the general mechanism).** The SAME GT3/GT4 submit
  library is also present in **runtime-loaded overlays** at per-scene-reused addresses (it ran interpreted
  → no depth). Owned generically: `rec_overlay_loaded(base,size)` (called by the CD loaders ov_cd_loadfile/
  ov_cd_async_read) clears prior scan-overrides and calls `engine_scan_overlay` (engine_submit.c), which
  scans the freshly-loaded bytes for the submit signature (packet-pool load `lui 0x800C`+`lw 0xF544`,
  RTPT, OT tag-len `lui 0x0900`=GT3 / `lui 0x0C00`=GT4) and registers the native impl via
  `rec_set_interp_override_auto` at each entry. Scan-on-LOAD (not per-call: classify-on-every-call was far
  too slow + thrashed). The flat interp now honours interp-overrides (coro_native_call → interp_override_for).
  VERIFIED 0-diff vs fully-interpreted (`PSXPORT_SUBMIT_RECOMP=1`) at f470/f560/f600. `PSXPORT_DEBUG=submit`
  logs each owned overlay submitter; `PSXPORT_NO_OVERLAY_OWN=1` is the A/B.
- **OPEN — full field depth coverage needs MORE submit VARIANTS.** Measured (`PSXPORT_DEBUG=ndepth` +
  the 2D-band op histogram + depth records-made/hit/miss counters): in the field only ~30% of gouraud-
  textured world polys get native depth; ~70% (op 0x3C GT4 + 0x34 GT3 + 0x2D flat-textured FT4) fall to the
  2D band because they come from submitters that record NO depth. These are NOT the GT3/GT4 library — they
  are **distinct submit variants**, e.g. `0x80027768` (recompiled MAIN): a GT4-packet builder with
  **byte-packed 8-bit vertex records** (`lbu`+`<<8`, verts built on the stack) and a **per-call X position
  offset** (`addu vx,vx,a1`) — a different record format/ABI, so the generic native GT3/GT4 impl CANNOT own
  it (owning it crashed: wrong return ptr → caller jumps into data). Other resident copies found by an
  op-fingerprint scan of the f560 RAM image: `0x8003B320`/`0x8003C8F4` (GT3), `0x8013CDD4`/`0x8013DD34`
  (overlay). Each distinct variant needs its own RE (record format, args, packet, cull, depth) + native
  port + 0-diff, then own resident copies via `rec_set_override` and overlay copies via the scan (extend
  classify_submit's signature per variant). NOTE the recompiled submitters are NOT covered by scan-on-load
  (that only sees CD-loaded overlays) — own them by fixed address.
- **`0x80027768` FULL decode (later-165, via `tools/disas.py 0x80027768`) — the next ownership target.**
  It is a LOOP over byte-packed GT4 quad records (NOT a single prim) and is the LIT/depth-cued sibling of
  the already-owned plain `submit_poly_gt4_bp` (engine/engine_submit.cpp:315 — mirror that native impl).
  Entry regs: `t3=a0` (record base), `t1=a0+0x18` (per-vertex byte block), `t2=*0x800bf544` (GP0 packet
  output cursor, written back to 0x800bf544 at the end, += 0x34/iter), `t0=t2+40`. Args: `a1<<22` = CLUT
  bank (same as gt4_bp); `a2` = OT/UV offset (`sra 16` at entry); `a3` = U offset (callers pass
  `a3 = a1+0` in shard_3/5/6, one site `a3 = (int32)a3>>16` shard_3:2570). Loop: control word
  `t5 = *(rec+4)` reloaded each iter at 0x8002779c; body packs `lbu`+`<<8` bytes from scattered rec
  offsets into stack words → GTE V0/V1/V2, runs RTPT 0x280030 + NCLIP/AVSZ + NCS color (GTE 0x0984000) +
  gouraud (0x0f8002a) + depth-cue, writes a **52-byte** packet, links into OT 0x800ed8c8 with prim code
  0x0c000000; advance rec t1/t3 += 0x24 (36, same stride as gt4_bp), out t0/t2 += 0x34 (52); `bgtz t5`
  continues. Per CLAUDE.md RENDER + later-96 (no dynamic GTE lighting, NC*/CC=0): reimplement as decode
  byte verts → `proj_native_xform` (float) → real depth → `gpu_draw_world_quad`, DROP the depth-cue fog
  bake (renderer owns fog), exactly like gt4_bp's native version. CONFIRM the NCS color is a passthrough
  via the VRAM A/B gate (build override-on vs a `*_RECOMP` super-call, headless dump at a frame where it
  fires, `cmp -l` byte-identical) before flipping it to default. Decode the exact V/UV/RGB byte offsets
  from the full disas when implementing — do NOT guess them.
- OPEN: 3D-projected overlay banners (hint signs) now sit behind nearer world geo (true depth vs the
  artist's OT-on-top) — overlay-vs-world depth semantics to resolve.

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
- **Higher res (native, not supersampling) — IMPLEMENTED (`PSXPORT_IRES=N`, default 1 = faithful):**
  the VK backend rasterizes the engine's submitted geometry into the scratch FB at N×(320|428)×240 by
  scaling the rasterization viewport (`gpu_vk.c` `s_ires`/`use_fb`/`push_wide`, shader already maps
  framebuffer-local×scale). Same engine output, sampled denser — crisp 3D edges; textures still sampled
  from PSX-res VRAM. Caps within the 1024-wide VRAM image: 4:3 → 3×, 16:9 → 2×. A dedicated >1024 render
  target would lift the cap (next). Distinct from the rejected supersample-and-downscale FB-cram trick.

## Water + sky — drawn via a NON-GTE (screen-space 2D) path; native-depth handling (later-98, later-104)
Observed during the lighting work (normal-viz): terrain/ground polys get a reconstructed per-face normal
(they go through RTPS/RTPT), but the **water surface does NOT** — it renders as a flat unlit layer. So the
water (and the sky backdrop) is **not GTE-projected 3D**; it's a separate screen-space / fixed-projection
layer (a scrolling textured surface and/or a framebuffer-reflection effect).

**Consequence for native depth (later-104, FIXED):** because the water/sky have no projection records,
the native-depth tee classifies them `is3d=0`. With the old two-band D32 model (3D world + a NEAR 2D
overlay band for HUD) the backdrops fell into the NEAR band and **occluded the entire 3D world** (water
over terrain). Fix = a **3-band depth model**: a FAR 2D-background band [0, 0.0625) for backdrops, the
3D world band [0.0625, 0.9375], and the NEAR 2D-overlay band (0.9375, 1] for HUD. Background vs HUD is
split by OT submission order (drawn before any 3D prim this frame = backdrop → far band; after = HUD →
near band; per-frame `s_seen3d` in gpu_native.c). The backdrops now sit behind the world correctly.

The water's *appearance* (the prior "green smeared streaks vs blue reflective water" report) is a
separate question about the reflection/MoveImage copy; to RE it, point `PSXPORT_PROVAT="x,y"` at a water
pixel for the prim op/texpage/CLUT + owning node, then read that handler in the decomp (compare to the
oracle at the SAME game-state, not by frame number).

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

**Per-frame loop** `FUN_80050b08` (0x80050b08) — function names CORRECTED from each libgpu fn's debug
string (earlier draft mislabeled DrawSync/DrawOTag/PutDrawEnv):
```
parity = DAT_1f800135 (0/1);  drawbuf base = 0x800bfe68 + parity*0x14000 (DAT_800bf544)
ctx = &DAT_800e80a8 + parity*0x81c              // per-buffer GPU context (DRAWENV+DISPENV+OT head)
FUN_80081458(ctx, 0x800)   = ClearOTagR(OT, 2048 entries)   // reset ordering table
FUN_800788ac()             = input + sub-tick
FUN_80051e60()             = task scheduler -> BUILDS the OT (entity handlers AddPrim into it)
FUN_80080f6c(0)            = DrawSync(0)         // WAIT for previous frame's draw to finish (not the draw!)
wait DAT_800e809c >= DAT_1f800235               // vblank gate: 1 = 60fps, 2 = 30fps  (<-- 60fps lever)
FUN_800506d0()
swap: FUN_8008179c(ctx+0x2000)=PutDispEnv (display area, GP1),
      FUN_800815d0(ctx+0x2014)=PutDrawEnv (draw-area CLIP+offset),
      FUN_80081560(ctx+0x1ffc)=DrawOTag(OT)     // <-- THE actual draw: kick the OT DMA to the GPU
      flip parity
```
libgpu (all via the 0x800A5998 table; names from debug strings): `FUN_80080f6c`=**DrawSync** (table+0x3c),
`FUN_80081560`=**DrawOTag** (table+0x18, the OT draw kick → our gpu_dma2_linked_list), `FUN_800815d0`=
**PutDrawEnv** (builds the 0x40-byte env packet at struct+0x1c via FUN_80081fb0, sends via table+8),
`FUN_8008179c`=**PutDispEnv** (GP1 display cmds via table+0x10), `FUN_80081690`=DrawOTagEnv,
`FUN_80081458`=ClearOTagR (table+0x2c), `FUN_800810f0`=ClearImage, `FUN_80081180`=ClearImage2 (fill bit
0x80000000), `FUN_80081218`=LoadImage (CPU→VRAM), `FUN_80081278`=StoreImage, `FUN_800812d8`=MoveImage.
- **60fps lever (native, real):** `DAT_1f800235` is the vblank-count target the loop waits for (1 vs 2).
  Our native loop owns this wait → drive at 60 by gating on 1 and interpolating object transforms (the
  entity-list snapshot, Phase-1). Not a renderer trick.
- **Widescreen lever (native, real):** PutDrawEnv (FUN_800815d0, ctx+0x2014 — draw-area clip+offset) +
  PutDispEnv (FUN_8008179c, ctx+0x2000 — display area) + the GTE OFX (now native, ov_set_geom_offset).
  Widen all three → genuine wider FOV. (Supersedes the rejected FB re-center.)
- **MoveImage (FUN_800812d8) = VRAM→VRAM copy** — the likely water-reflection / fade-buffer mechanism;
  prime suspect for the broken water (reflection copy) AND a place our GP0 emulator can drift. RE next.

### Render-buffer memory map — the packet pool + OT extent (later-125; the overflow threshold)
RE'd to find the fixed-buffer overflow threshold behind the widescreen corruption (later-124 mechanism #2).
All double-buffered by `parity = DAT_1f800135` (0/1), swapped each frame in native_boot.c. Addresses are
guest-RAM (`0x800…`); each value confirmed empirically by the `PSXPORT_DEBUG=pool` probe (OT roots = the
DrawOTag madr) at the field scene:
```
packet pool parity0   [0x800BFE68, 0x800D3E68)   0x14000 B   base = 0x800BFE68 (DAT_800bf544 reset)
packet pool parity1   [0x800D3E68, 0x800E7E68)   0x14000 B   stride 0x14000   <-- pool buffers END 0x800E7E68
gap / globals         [0x800E7E68, 0x800E80A8)   0x240 B     (incl. DAT_800e809c dwell counter @0x800E809C)
ctx parity0           [0x800E80A8, 0x800EA118)   0x2070 B    = &DAT_800e80a8 + 0*0x2070
  -> OT array         [0x800E80A8, 0x800EA0A8)   0x2000 B    2048 entries; DrawOTag root (head) @0x800EA0A4
  -> DISP/DRAW env    [0x800EA0A8, 0x800EA118)               +0x2000 DISPENV, +0x2014 DRAWENV, setup-OT @+0x2030
ctx parity1           [0x800EA118, 0x800EC188)   0x2070 B    OT head @0x800EC114, setup-OT node @0x800EC148
```
- The packet pool is a per-frame **bump allocator** (write ptr `DAT_800bf544`), shared by the engine's
  geometry submit AND the inline `AddPrim` 2D path. It holds the primitive packets (40 B GT3 / 52 B GT4)
  + the 1-word OT link tags. The OT array (2048 words) holds only bucket heads and does NOT grow per-prim.
- **Overflow threshold:** parity1's pool overflowing past `0x800E7E68` lands (after the 0x240 gap) in
  **ctx parity0's OT at 0x800E80A8** — i.e. a pool overflow corrupts the *alternate* frame's ordering
  table (classic double-buffer cross-corruption → garbage / flicker), not just its own packets.
- **Measured pressure (field scene):** 4:3 used ~31.5 KB/buffer (hi `0x000DB9B0`), 16:9 ~44 KB (the wide
  frustum's `ov_object_cull` re-include = +260 prims / +12.7 KB). 46% headroom at this scene, but a denser
  scene closes it. **later-125 fix:** native-DL owned prims now consume only their 1-word link tag (4 B,
  not 40/52 B) → 4:3 pool drops to ~9.8 KB (`0x000D649C`, −69%), removing the overflow pressure entirely
  while staying byte-identical. See native_dl.h.

### Ported to native C so far (faithful-first, oracle-gated)
- **GTE projection setters** (later-99): `ov_set_geom_offset` (0x800846D0 SetGeomOffset) + `ov_set_geom_screen`
  (0x800846F0 SetGeomScreen) in engine/game_tomba2.c. Native writes CR24=OFX<<16, CR25=OFY<<16, CR26=H.
  VERIFIED byte-identical: logs OFX=160/OFY=120/H=350, and the rendered frame is **0-pixel-diff** vs the
  recomp body (PSXPORT_GEOM_RECOMP=1 A/B). The engine's projection config is now PC-native — the widescreen
  FOV lever (widen OFX + draw-env clip) lives in our code. (Also owned earlier: LoadImage upload
  ov_upload_image 0x80081218; LZ/group asset codecs.)
- **DrawOTag** (later-99): `ov_draw_otag` (0x80081560) → native `gpu_dma2_linked_list` (walk OT, decode
  each primitive, rasterize). The engine's per-frame DRAW SUBMISSION now goes straight through our native
  renderer, not the DMA-register emulation. VERIFIED **0-pixel-diff** vs recomp (PSXPORT_OT_RECOMP=1) at
  f1500 (514-poly scene). Next: PutDrawEnv (FUN_800815d0) + PutDispEnv (FUN_8008179c) → full native screen
  geometry, then enable widescreen (wide OFX + wide clip + wide render target).
- **Native projection — RTPS/RTPT in native C** (later-100, plan atomic-riding-sparkle Phase 1): the GTE
  per-vertex projection (matrix·vertex → view-space IR1/2/3, depth SZ, integer screen SX/SY) is now
  reimplemented in native C in `runtime/recomp/gte_beetle.c` (`proj_native_vertex`, mirroring
  mednafen/psx/gte.c: MultiplyMatrixByVector_PT + UNR `Divide`/`CalcRecip`/DivTable + TransformXY clamps).
  It ALSO emits the FLOAT data the integer GP0 packet throws away — subpixel screen (precise_x/y) +
  view-space pos + depth — which the renderer needs and the OT-reordered GP0 stream can't carry. **VERIFIED
  0-diff** vs Beetle's GTE on real gameplay (the field, `PSXPORT_AUTO_GAMEPLAY=1`): over 1.28M verts/frame,
  IR=0-diff, SZ=0-diff, SX/SY=0-diff, and our float screen rounds to the integer projection 100%. Probe:
  `PSXPORT_PROJPROBE=1` (read-only verifier, gameplay untouched — the real GTE still runs; this only
  cross-checks). **Gotcha (RE'd here):** Beetle's XY_FIFO push writes the new vertex into BOTH slots 2 & 3
  (`XY_FIFO(2)=XY_FIFO(3)` runs after `(3)=new`), so after an RTPT the 3 verts read back at DR12/DR13/DR14
  (DR15==DR14), NOT DR13-15; the Z_FIFO shifts cleanly so its verts are at DR17/18/19. This replaces the
  value-keyed PGXP-lite cache as the depth/subpixel source.
- **Projection+submit call sites pinned** (later-100b, probe `PSXPORT_DEBUG=rtpcaller` = RA histogram at each RTP
  + jal-target decode). The RTP-containing project-and-submit functions, by call frequency at the field:
  `0x80109C80` (4689, overlay) and `0x801099B4` (2025, overlay) DOMINATE; resident `0x8007FDB0` (1504, the
  triangle fn) + `0x8008007C` (880, the quad fn — projects the 4th vertex via RTPS, then the triangle via
  RTPT, so execution order ≠ packet order); `0x80027768` (16). So there are 5+ distinct projection routines
  and the two dominant ones live in **runtime-loaded overlay code (not statically disassemblable)** → owning
  each function's packet layout is impractical. BUT all of them write the projected SXY into the **shared
  primitive pool via the global `DAT_800bf544`** (decomp `FUN_8007fdb0`/`FUN_8008007c`: `DAT_800bf544[2/5/8]
  = getCopReg(2,0xc/0xd/0xe)` = XY_FIFO, then OT-link `*DAT_800bf544 = old|0x9000000` and advance). That base
  IS the OT node, i.e. == `s_cur_node` (masked) when DrawOTag later decodes the packet (gpu_native.c:1220).
  **DESIGN for attach-at-submission (function-agnostic, no layout/RE of the overlay fns needed):** at the
  `gte_op` RTP chokepoint read `P = mem_r32(0x800bf544)` (the packet being built; stable across a packet's
  multiple RTPs, advances when the packet is committed) and append the projected vertex's (int sx,sy + float
  screen/viewpos/depth) to a per-frame list keyed by `P & 0x1FFFFC`. In `gp0_exec` poly decode, for the
  packet at `s_cur_node`, match each packet vertex to its float by `(sx,sy)` among that packet's ≤4
  candidates — globally collision-free (packet address) and intra-packet collision-free (only 3-4 verts),
  handling the quad reorder and culled-prim pollution. Retires the value-keyed `pgxp_lookup` cache. Verify:
  per-vertex hit rate ~100% for 3D polys (a miss = a 2D/screen-space prim, the free 2D/3D class), and
  rendering 0-diff vs the current PGXP path. Then Phase 2 = PC-native render targets + real depth (view-Z).
- **Attach infrastructure BUILT + measured; capture point is the open piece** (later-100c, `PSXPORT_ATTACH`).
  Implemented in gte_beetle.c: `projprim_push/lookup/reset/has_node` (per-frame float store hashed by packet
  node) + `projprim_capture` (at `gte_op`, reuses the 0-diff `proj_native_vertex`), and a renderer-side
  verifier in gpu_native.c `gp0_exec` (per-poly-vtx hit/miss by `(s_cur_node, sx, sy)`, reset each present).
  **Finding:** capturing the node as `mem_r32(0x800bf544)` covers only the RESIDENT project-and-submit fns →
  **~30% poly-vtx hit rate, and ~95% of misses are NODE-ABSENT.** The dominant overlay projection fns write
  into the SAME primitive-buffer region (resident packets at ~phys 0xD3xxx, overlay packets at ~0xD9xxx) but
  via a DIFFERENT running pool-pointer global — so a single hardcoded pool pointer is the wrong (fragile,
  non-function-agnostic) capture point. **SOLVED (later-100d): capture by the SXY word's guest ADDRESS.** The
  projection fns store the packed XY_FIFO word to packet memory (resident via recomp stores, overlay via
  interp stores — BOTH funnel through `mem_w32`). Implemented (keyed by ADDRESS now, not node): at `gte_op`
  push each vertex's packed SXY + native float into a small pending ring (`s_pr`, 32 deep); `attach_store_hook`
  (called from `mem_w32` for stores into the prim-pool region phys 0xB0000–0xF0000, gated) match-and-consumes
  the pending ring oldest-first and records the float keyed by the store ADDRESS (`projprim_set`, last-write-
  wins so a culled-then-reused slot resolves to the surviving prim); the render side tracks each GP0 word's
  source guest address (`s_fifo_addr[]`, set from `s_gp0_src` in the `gpu_dma2_linked_list` OT walk) and
  `gp0_exec` looks the float up by each vertex word's address (`projprim_lookup`). **VERIFIED (PSXPORT_ATTACH,
  field):** poly-vtx hit **100%** of GTE-projected (3D) polys (f400/f600 = 0 miss); the only misses are
  op-2F axis-aligned textured UI quads = 2D screen-space prims with no projection — the FREE 2D/3D class (a
  miss with a valid OT addr = 2D; addr==0 = direct/FMV). Fully function/pool-agnostic (covers the dominant
  overlay projection fns), no global value-collision (the value-match is bounded to the tiny pending window;
  the RESULT is address-exact). The dormant value-keyed `pgxp_lookup` is now superseded. **NEXT: Phase 2** —
  feed `projprim_lookup`'s view-space Z into a PC-native render target + real depth buffer; route 3D (hit) vs
  2D (miss) prims into separate passes; composite.

### Native ownership plan (reimplement libgpu, keep recomp body as oracle via rec_set_override)
1. **IN PROGRESS — native classified display list (later-123).** The geometry submit fns now build a
   PC-native `NativePrim` (engine/native_dl.{h,c}: would-be packet words + per-vertex view-Z) instead of
   writing the GPU packet to guest RAM; they link only a zero-length ordering node into the guest OT.
   `gpu_dma2_linked_list` renders owned nodes from the native arena via gp0_exec, taking depth from the
   carried view-Z (no address bridge). DONE for POLY_GT3/GT4 + the byte-packed GT4 field emitter (the bulk
   of 3D world geometry); VERIFIED 4:3 + 16:9 byte-identical (PSXPORT_DL_GUESTPKT A/B). **Remaining for
   full DrawOTag ownership:** the sprite/tile/flat/line builders + env packets (the field's 389 rects + 11
   env still write guest packets via libgpu / inline AddPrim) — once those emit NativePrims too, the OT
   walk becomes a pure render of a fully-classified native scene graph and NO render data lives in guest
   RAM. The recomp libgpu stays callable for A/B diff (PSXPORT_SUBMIT_RECOMP).
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

## Issue root-causes (understood via the scene classifier + frame-stepping, later-99)
- **Broken water** — ROOT CAUSE: the PGXP value-keyed vertex-smoothing TRICK was tearing the water's
  dense textured grid (collisions snapped grid verts to wrong subpixel positions). Water is ordinary
  textured geometry (scene classifier: no reflection copy). FIX: PGXP default-off (bf69890). Not a game bug.
- **Intro story-cutscene "flashes full visibility on fade-in"** — the cutscene is the post-NewGame prologue
  (stage 0x8010637C; "Tomba jumps into the sea"). Frame-stepped the fade-in in the port's SW path: the
  panel (drawn as ~3 sprites/rects, NO full-screen overlay, NO fill) brightens as a **smooth monotonic
  ramp** (mean 0.006→0.32 over ~30f) — i.e. the engine fades by **ramping the sprites' modulation color**
  0→full, and it does so CORRECTLY. So the flash is NOT in the game's fade logic — it is renderer-side:
  the **VK present path** (1-frame-behind/batched present, or stale frame at the title→black→fade
  transition) or the **FMV→engine handoff** (./run.sh has FMV on; drive.py SW path with NO_FMV is clean).
  NEXT: reproduce on the VK path (PSXPORT_VK_SHOT across the prologue entry) to pin the exact present-path
  frame, then fix our presentation — a renderer bug we own, distinct from the engine.
  - **later: reproduced on VK + characterised (live drive, PSXPORT_AUTO_NEWGAME=2 + debug-server step).**
    The glitch is a 1-2 frame brightness OUTLIER in the dark fade-in: a single FULL-BRIGHT frame on the
    otherwise near-black ramp (caught once at displayed-mean 1→57→2 between black neighbours; also a
    black-then-bright pair in the user's run). **It is INTERMITTENT** — two full step-through passes
    showed a clean smooth ramp with no flash. It was DETERMINISTIC before the buffer-flip hack was
    removed (commit b1f029a) and became intermittent after → the locus is the **flip/present timing**,
    not the engine fade (SW ramps correctly). Working hypothesis: VK renders frame N's tee'd geometry at
    frame N+1's present (1 frame behind SW, which rasterizes during DrawOTag), and with the now-
    unconditional double-buffer flip there's a race in which buffer/modulation the swapchain scans out
    during the fast ramp → an occasional stale full-bright (pre-fade) or just-cleared (black) buffer.
    NEXT: catch it in-situ (the step-drive flags displayed>>buf0/buf256) and confirm whether the present
    sampled a buffer/region the current frame didn't draw; then make the VK present frame-accurate
    (present the SAME frame's geometry, not 1-behind) so swap + render stay in lockstep.
  - **Tooling for this (committed):** PSXPORT_AUTO_NEWGAME=2 boots straight to the prologue and auto-
    pauses; debug-server `pause`/`step`/`play` (also keyboard P / '.'), `vkshot`/`vkvram` (reliable while
    paused — the loop re-presents each tick), `swvkcap` + tools/swvk_diff.py + tools/perceptual.py.
  - **later: PINNED the symptom precisely (it is DETERMINISTIC on the first pass through the prologue
    fade, NOT a race — the earlier "intermittent" reading was stepping a stale frame range).** At the
    flash the display origin is (0,256) and **framebuffer (0,256) holds the cutscene scene at FULL
    brightness (mean ~57)** while framebuffer (0,0) holds the dim fade (~3); the next frame the display
    origin flips to (0,0) (dim). So the two double-buffers diverge wildly DURING the fade — one carries a
    full-bright render of the scene (looks like the post-fade image: "Tomba is living peacefully…"), the
    other the early-fade dim render. Confirmed `disp=(x,y)` via the debug-server `frame` command + per-
    buffer `vkvram`. The user confirms the SW renderer fades smoothly, so this is **VK-specific: one
    double-buffer retains / receives a full-bright render of the scene that isn't re-dimmed in lockstep
    with the other.** RULED OUT (tested, no effect on the flash): the prims>0 flip hack; moving the
    present after DrawOTag (1-frame batch offset); a synchronous present (vertex-buffer reuse race).
    NEXT (decisive, not yet done): drive the SW renderer (PSXPORT_SW_GPU=1) to the same prologue frame
    and read framebuffer (0,256) — if SW has it dim there, the bug is VK redrawing/retaining that buffer
    full-bright (likely the persistent s_tex + how the cutscene's per-buffer redraw maps to VK's
    upload/tee path); compare the two renderers' per-buffer brightness across the fade.
  - **later2: SW renderer CONFIRMED smooth (PSXPORT_SW_GPU=1, same drive): no brightness spike, ramp
    0→62; VK spikes to ~57 at the same frame. So definitively VK-specific.** Causes RULED OUT by A/B
    (flash unchanged): flip hack; present-after-DrawOTag (no 1-frame batch offset); synchronous present
    (vertex-buffer reuse race); the OT-order DEPTH ordering (PSXPORT_VK_NODEPTH=1 — flash persists).
    Remaining mechanism: VK's persistent s_tex framebuffer (0,256) carries a FULL-BRIGHT render of the
    cutscene scene during the dim early fade, while SW's (0,256) is dim — i.e. VK populates/retains that
    buffer differently from SW for the same GP0 (the geometry is tee'd to VK and rendered into s_tex; the
    background/clear is uploaded from s_vram). NEXT: at the flash frame, dump SW s_vram (0,256) AND VK
    s_tex (0,256) for the SAME captured GP0 (extend swvkcap to also raw-dump s_vram) and diff which
    primitives/region differ; check whether the cutscene actually redraws (0,256) every frame or leaves
    it stale (then VK retains the old full-bright render where SW's last redraw was dim).
  - **SOLVED (2026-06-17): ROOT CAUSE = VK lost OT-order accumulation of OVERLAPPING semi-transparent
    prims.** The fade is NOT GPF/modulation — the cutscene scene is drawn at FULL vertex colour every
    frame and a full-screen SUBTRACTIVE semi tile (blend mode 2, `scene − fg`) darkens it; `fg` ramps
    248→123 so the scene fades IN from black. The flash is the ONE handoff frame between the panel
    fade-OUT and the scene fade-IN, where TWO stacked full-screen subtractive tiles are emitted
    (`PSXPORT_SEMIDUMP`: `blend=2 col=(255) fullscreen` then `blend=2 col=(8) fullscreen`). SW
    rasterizes them sequentially → `scene−255−8 = black`. VK drew ALL semi prims in ONE pass sampling a
    single PRE-semi framebuffer snapshot, so the two tiles did NOT accumulate — each subtracted from the
    same bright scene and the later (−8) tile OVERWROTE the (−255) tile → `scene−8 ≈ full bright` → a
    one-frame flash on the buffer it drew (displayed the next frame at disp=(0,256)). Diagnosed with two
    new env-gated, renderer-independent probes in gpu_native.c: **`PSXPORT_FADEDBG="a:b"`** (per-frame
    max prim colour, semi colour range, bigsemi count, disp/draw origin) and **`PSXPORT_SEMIDUMP=frame`**
    (each semi prim's blend mode + colour + bbox). The GP0 colour ramp is identical under SW and VK, so
    these settle "engine vs renderer" without VK. **FIX (gpu_vk.c + gpu_native.c):** partition the semi
    batch into overlap GROUPS (`gpu_vk_semi_group(bbox)` per semi prim; a prim that overlaps the current
    group's accumulated bbox starts a new group) and draw each group with its OWN fresh framebuffer
    snapshot, so a later group blends against earlier groups' results — exactly the sequential PSX/SW
    order. Non-overlapping semis (e.g. water tiles) stay ONE group → no extra cost. **VERIFIED:** VK fade
    now ramps smoothly f345→f400 (mean 0→12.7) tracking SW frame-for-frame, no spike (was ~57 at the
    flash frame); residual VK/SW drift ~10% from f382 on is the known dither/subpixel-rounding residual,
    not the flash.

## In-game pause / Options menu — state machine (RESOLVED later-112; replaced by our menu)
The in-game **pause menu** ("Options / Load data / Quit game") runs as a **task in the GAME overlay**
(0x80108xxx), which is why the main MAIN.EXE decomp (`ram_f1000_all.c`) has no caller for the draw
functions — they're reached only from overlay code + a jump table. Layout:
- **Task body / page dispatcher** `0x8010810C`: reads the **page byte `task+0x6B`** (task ptr =
  `*(u32)0x1F800138`, the scheduler's current-task pointer), bounds-checks `< 0xC`, then jumps through a
  **12-entry function-pointer table at `0x801062EC`** (`handler = table[page]`).
  | page | handler | role |
  |---|---|---|
  | 1 | `0x8010829C`→`FUN_8007eae4` | **main pause menu** draw "Options / Load data / Quit game" |
  | 2 | `0x801084A4` | close (resets `task+0x6B=0`, clears `DAT_1f800136`) |
  | 3 | `0x801082C0`→**`FUN_8007b45c`** | **the Options submenu controller** (the menu we replace) |
- **Main-menu handler** (page 1) reads button **edges `DAT_800e7e68`** (u16, active-high; Up=0x10,
  Down=0x40 move cursor `DAT_800bf808`; **Cross=0x4000** confirms). On Cross over "Options" (cursor 0)
  it sets `task+0x6B = cursor+3 = 3` → next frame the dispatcher enters the Options controller.
- **Options controller `FUN_8007b45c`** (0x8007b45c, NOT in the decomp dump — disasm via
  `tools/recomp/decode.py`): draws "Select Options" (`FUN_8007f104`: **Messages / Sound / Screen adjust /
  Controls** — the options the user deemed not worth keeping), dispatches its own sub-screens by
  `task+0x50` (table `0x80016E78`, 5 entries). Exits: **Triangle (0x1000)** → `task+0x6B=2` (close);
  **Circle (0x2000)** in the list → `task+0x6B=1` (back to the pause menu), resets cursor, sets
  `DAT_1f800136=1`. Menu SFX = `FUN_80074590(id, a1, 0)` (Circle: `0x14,0xFFF7`; Triangle/back: `0x11,0`).
- **Replacement** (later-112, `engine/game_tomba2.c` `ov_options_menu`, gated `PSXPORT_UI`):
  `rec_set_override(0x8007B45C, …)` shows our ImGui overlay instead of the game's options and owns the
  same Circle→page1 / Triangle→page2 back-nav + SFX. Faithful fallback: if the overlay isn't up
  (headless/window-less) it super-calls the real `FUN_8007b45c`. Verified the hook is reached
  (`PSXPORT_DEBUG=ui` + AUTO_GAMEPLAY → forced Cross at the auto-appearing menu logs the hit).
- **Other menus nearby** (same draw lib, separate flows): `FUN_8007ee74` "Continue / Load data / Quit
  game" (death/continue), `FUN_8007ed5c` Save prompt, `FUN_8007ef60` "OK to quit game?".

## Engine ownership status → `docs/engine-ownership-audit.md`
The actionable map of what the native engine OWNS vs. what still runs **on the interpreter** (the "black
box"), why widescreen corrupts (clip+cull+2D layers stay 4:3 while we fake the FB wide), and the prioritized,
oracle-gated port plan to own the render/camera/submit layer for REAL widescreen. **Read it before render/
camera/widescreen work.** (Gameplay logic stays interpreted — the Beetle emulator is the oracle, by design.)

## Open RE items (next, in order)
1. ~~The entity list + its walk~~ — **DONE** (above): lists `DAT_800fb168`/`DAT_800f2624`, walk
   `FUN_8007a904`, node layout. Handlers are per-object fn pointers @ +0x1c (not a type-indexed table).
2. Find `FUN_8007a904`'s caller (frame placement) — it has no static caller (called via task/overlay).
   Confirm at runtime (objlog / a tap) that it's the per-logic-frame object driver.
3. Object node full lifecycle: spawn/link (`:55976–56167`) and free; the pool base/extent.
4. Camera basis + GTE projection setup (for native render submission, Phase 3).
5. Whether handlers call the cull/submit path (`FUN_8007712c`) in field scenes, or a different render
   path (the journal noted the cull dispatcher didn't fire in the demo). `PSXPORT_DEBUG=obj` answers this.
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
