# Tomba!2 — PC GAME architecture (the real rebuild)

**Mandate (user, 2026-06-24):** stop hacking the recompiled render path with flags. **Build a real PC game.**
RE each game SUBSYSTEM and reimplement it natively in its OWN folder with sub-subsystem files. The recompiled
MAIN.EXE stays ONLY as the live FALLBACK for code not yet ported, so the game keeps running while we port
top-down. See memory [[make-a-pc-game-subsystem-rebuild]].

## The two layers
- **`platform/`** — the PSX→PC FRAMEWORK (generic; future `psxport` submodule). The R3000 interpreter, memory,
  boot, HLE/BIOS, CD/disc, pad, memcard, the VK device + present, and the low-level GPU/GTE/SPU/MDEC backends.
  This is NOT the game; it runs un-ported guest code and provides host services. Currently `runtime/recomp/*`.
- **`game/`** — the Tomba!2 PC GAME, organized by SUBSYSTEM (below). Each subsystem is RE'd from the recomp
  reference and reimplemented native-from-data. The recomp body of an un-ported function is the live fallback
  (called via `rec_dispatch`) AND the behavioral reference — never the thing we bolt onto.

## Game subsystems (folder = subsystem, files = sub-subsystems)
```
game/
  app/         game loop, frame step, present, pacing, 60fps      (ov_frame_update, native_step_frame, fps60, wide60)
  render/      THE RENDERER — draws the frame from SCENE DATA, native, real depth, engine-owned order
    scene/       draw-list / layers / depth-sort / the render queue            (render_queue)
    mesh/        3D model/mesh draw from geomblk + node transform              (engine_submit GT3/GT4, engine_project)
    terrain/     terrain mesh build + draw                                     (native_terrain)
    backdrop/    parallax tilemap backdrop                                     (ov_bg_tilemap_native)
    sprite/      2D sprites & world BILLBOARDS (pickups, weapon, decals)       (RE NEEDED)
    hud/         HUD / 2D overlay / font / UI rects                            (hud, engine_font, engine_ui_rect)
    transition/  screen fades / area transitions                              (RE NEEDED)
    lighting/    per-area lights, shading                                      (lighting)
  world/       scene & world management
    area/        area/level loading, the field stage machine                  (engine_level, engine_stage)
    entity/      entity/object model + the active object lists                 (entity, object_init)
    spawn/       object placement & spawning                                   (entity_spawn)
    transform/   scene-node transforms / hierarchy
  actors/      characters & behavior (content; port where hot/needed)
    player/      Tomba controller (move/jump/attack)                           (engine_player)
    npc/ enemy/  per-object state machines / AI                               (objbeh_*, actor_sm_*)
  camera/      camera system                                                   (engine_camera)
  physics/     collision grid + response, hitboxes, cull/LOD                   (collision, hitbox, grid_offset, cull)
  assets/      resource load & decode (archives, textures, models, anim, palettes) (asset, engine_bav)
  audio/       SPU mixer, sequencer, SFX, the native music director            (audio/, sound, sound_voice, sop)
  input/       controller / input mapping                                      (input)
  save/        save/load, inventory                                            (save, inventory)
  ui/          front-end menus / title / demo / in-game menu                   (menu, engine_demo)
  math/        math + PRNG + LUTs                                              (mathlib)
```
(Parenthesized = the existing `engine/*` file(s) that become that subsystem's starting point.)

## Method per subsystem (the loop — do this, don't flag-hack)
1. **DECOMPILE** the subsystem's real functions into readable C. Overlay code: `dumpram` + `scratch/dual/
   mdis.py <addr> [n]`; MAIN.EXE: `tools/disas.py <addr> [--mem]`. Cache decomps under `scratch/decomp/<subsys>/`.
2. **Understand the DATA**: the structs it reads/writes, the guest globals/scratchpad it shares with still-PSX
   content (the content-interface — those writes must stay correct).
3. **REIMPLEMENT native-from-data** in the subsystem folder: read the game's own data, do the work in PC C,
   draw via the native renderer (float transforms, real depth, engine-owned 2D layers). NO `rec_dispatch` of
   this subsystem's render/logic, NO PSX-OT replay, NO GP0-packet emit/tag, NO A/B `debug`/env flag.
4. **WIRE top-down**: a native function runs because its already-native PARENT calls it directly. Un-ported
   leaves `rec_dispatch` to the recomp (live fallback) until ported.
5. **VERIFY**: the engine builds the right world/picture; the guest state still-PSX content reads is correct.
   Render correctness = USER eyeballs `./run.sh` (agent builds + sends shots). No oracle.

## BUILD ORDER — ASSETS FIRST, THEN RENDER (user, 2026-06-24)
**Do NOT build the renderer before porting ASSET HANDLING.** The renderer must consume NATIVELY-OWNED assets,
not PSX VRAM. So the asset subsystem is the FIRST target, and the renderer is built on top of it.

### 0) OFFLINE ASSET EXPORT — DONE (proves the formats before any renderer; user-directed first step)
`tools/export_model.py` parses the GEOMBLK 3D-model format straight from a RAM dump and decodes each prim's
texture from a VRAM dump → standard OBJ + MTL + PNG. `tools/preview_model.py` is a textured offline
rasterizer (verification only). VALIDATED on the seaside field: exported the see-saw (12-part node, 200
faces), a flower prop, and a fence post — correct geometry, UVs, and REAL decoded textures (stone/foliage/
wood/thatch). This confirms the asset format RE; the in-engine native asset loader (below) reuses the exact
same parse. Capture inputs via REPL: `ents` (model/geomblk addrs), `dumpram <f>`, `vramraw <f>`.
- **GEOMBLK** (RE'd, engine/engine_submit.cpp native GT3/GT4 + tools/export_model.py docstring): header
  +0=u32 counts (lo16 #GT3 tris, hi16 #GT4 quads); records @+16 (GT3 36B, then GT4 44B); model-space s16
  verts + u8 UVs + RGB + PSX tpage/clut per prim. An OBJECT's full model = all its render-cmd geomblks
  (cmd-ptr array @node+0xC0, count @node+8, geomblk @cmd+0x40). Characters (Tomba, ri=0x10) are 2.5D SPRITES,
  not geomblk meshes — separate sprite-atlas format (RE next).
- **TEXTURE/VRAM** (RE'd): PSX tpage → (tp_x=(tp&0xF)*64, tp_y=((tp>>4)&1)*256, mode=(tp>>7)&3); 4bpp/8bpp
  index → CLUT @ (clut&0x3F)*16, (clut>>6)&0x1FF; 15bpp direct; BGR555 (0x0000 = transparent).

### 1) ASSET subsystem (`game/assets/`) — port FIRST. Own these as NATIVE data (not PSX VRAM/4bpp+CLUT):
- **textures** — decode the game's 4bpp/8bpp+CLUT images into native RGBA8 textures in a texture cache, keyed
  by the game's own id (so the renderer binds a native texture, not a VRAM (tp_x,tp_y)+CLUT).
- **the texture ATLAS** — the scene/character atlas (256x256… texpages): decode to native atlas texture(s) +
  a UV map from the game's atlas layout.
- **3D models** — parse the geomblk format (prim-lists: positions/UVs/colors per GT3/GT4 tri/quad; node+0xC0
  command lists) into native vertex/index buffers.
- **sprites** — the 2D sprite/billboard cells (pickups, weapon, HUD glyphs): decode to native sprite records
  (atlas texture + UV rect + size).
- **terrain** — the terrain/heightfield + tile data into a native terrain mesh + its texture.
- Existing starting point: `engine/asset.cpp` (ov_lz_decompress/ov_unpack_group/ov_load_texgroup/
  ov_upload_image) currently LZ-decodes + uploads to PSX VRAM. KEEP the LZ/archive parsing; REPLACE the
  "upload to VRAM" endpoint with "decode to native texture/mesh/sprite". RE the formats from the recomp
  reference (tools/disas.py / mdis.py) + memory ([[object-pipeline-and-depth-regression]] geomblk,
  [[tomba2-native-display-list]], the tilemap atlas in later-244). Cache decomps in scratch/decomp/assets/.

### 2) RENDER subsystem (`game/render/`) — FOUNDATION STARTED (2026-06-24), decoupled native path.
First cut of the decoupled NATIVE render path (NO GTE / NO OT / NO GP0). Tree: `game/render/scene/`
(`scene_data.h` = `SceneObject`/`SceneCamera`/`RenderScene`; `scene_build.cpp` `render_scene_collect` walks
the 3 entity lists, selects 3D-mesh nodes, builds float model→view from node euler+pos+scale + the
scratchpad camera) + `game/render/mesh/` (`mesh_draw.cpp` parses the geomblk GT3/GT4 records, transforms
verts in FLOAT, projects, draws textured tris/quads via the engine's real-depth `gpu_draw_world_quad`) +
`render_native.cpp` (`render_scene_native`). Invoked additively in `ov_draw_otag` behind the `rendernative`
DIAGNOSTIC channel (default-off — the PSX-vanilla path is untouched and remains the default). Produces real
geometry live (seaside: ~47 objects / ~425 prims/frame). KNOWN first-cut limits (follow-ups): flat
unmodulated vertex color, VRAM-sampled textures (native RGBA cache is the asset step), opaque-only, and
static props that store orientation in cmd+0x18 (not node euler) may be axis-aligned until euler-vs-matrix
is reconciled. Render correctness is USER-eyeballed, not self-verified.

### 2b) RENDER subsystem (`game/render/`) — build AFTER assets, ON TOP of native assets.
Field renders ENTIRELY from scene data + native assets — no recomp render, no OT walk, no GP0 emit/tag, no
flags. Reference pipeline = MAIN.EXE 0x8003f9a8 (orchestrator) → render walks → per-type drawers → 2D sprite
band (0x80025d98) → backdrop (0x8003df04→tilemap 0x80115598) → fades. Decompile each, map the scene-data
model, reimplement under `game/render/` drawing native meshes/sprites with native textures. Per-drawer RE map:
`docs/render-rebuild.md`.

## Migration (non-destructive, keep it building every step)
Move existing `engine/*` files into their subsystem folders incrementally, updating the SRC lists in
`run.sh` + `tools/build_port.sh` and include paths in the SAME change; build after each move. Keep the recomp
fallback (`platform/`) intact. Retire the scenenative/g_ot_2d_only/bgonly/ot2dtest experiments as the native
render subsystem replaces them.

### Migration status
- **`game/world/` — the OBJECT subsystem — MIGRATED (2026-06-24).** The PC-native object-CREATION +
  GRAPHICS-BINDING pipeline (previously orphan grab-bag code in `engine/entity_spawn.cpp` / `object_init.cpp`
  / `entity.cpp`) now lives in clean sub-subsystem files: `game/world/{spawn,placement,graphics_bind,
  verify_gate,pool,entity}.cpp` (+ headers, `world_pool.h`). Verbatim relocation — no logic change; builds
  clean. `graphics_bind.cpp` carries the forward `SceneObject` native scene-data struct (geometry ref +
  float transform + texture id) the decoupled native renderer will consume. `ov_build_xform` (FUN_80051C8C)
  stays in `engine/engine_submit.cpp` for now (depends on a submit static; moves with the render subsystem).
  - **PLACEMENT now LIVE top-down (2026-06-24, no overrides):** `ov_field_run` case-0 (engine/engine_stage.cpp
    ~300) now calls `ov_place_objects(c)` DIRECTLY (replacing `rec_dispatch(0x80072a78)`). Native object
    placement drives the live seaside field. Verified headless (`PSXPORT_AUTO_SKIP=1`): gate ON
    (`debug placeverify`) = both per-load calls byte-exact (full RAM+scratchpad) vs the PSX reference;
    gate OFF (native actually drives) = field reached, 155 nodes placed identical to the reference set, no
    bad opcode. This is the first native function on the LIVE object-creation path.
  - **SPAWN path now fully native (2026-06-24):** `spawn_dispatch` (and `replace_dispatch`) no longer
    `rec_dispatch` the per-type spawn variants — they call the 5 owned native bodies directly via
    `spawn_variant_native(c, cls)` (entity_spawn / spawn_pool2 / pool_spawn×3). So the live object-creation
    chain — placement → spawn dispatch → spawn variant → pool pop + list link + identity stamp — is ALL
    PC-native. `placeverify` stays match #1/#2 (byte-exact end-to-end vs PSX), gate-off native-driven reaches
    the field clean (151 nodes, 0 bad opcode). The per-type spawn VARIANTS' per-class init beyond the
    pool/list primitive is already inside these native bodies; only the deeper content handlers (node+0x1c
    behaviors) remain PSX.
  - **GRAPHICS-BIND now native+live (2026-06-24):** the native entity walk (engine_tomba2.cpp
    `dispatch_native_behavior`) already routes the seaside-resident handlers (739ac/73cd8/741dc) to native;
    their one graphics-binding call (per-object render-record init FUN_80051B70 → alloc record, resolve
    geomblk from the two-level model table, store into node+0xC0) now calls native `ov_obj_record_init`
    instead of rec_dispatch. So "assigning graphics" runs PC-native live for these handlers. Verified:
    handler gates 0 mismatch (obj739ac 1150 / obj73cd8 2300 / obj741dc 550), gate-off native-driven binds
    geomblks into node+0xC0 (ents gb0=…), field clean.
  - **Behavior handler FUN_80071A3C owned native+live (2026-06-24):** added `engine/objbeh_80071a3c.cpp`
    and routed it through `dispatch_native_behavior`. A resident state machine on node[4] (state 0 runs
    FUN_800716B4 + a global-flag-gated overlay event dispatch on 0x800BFAE1/0x800BFAE6 vs area 0x800BF870;
    state 1 FUN_80071768 + conditional FUN_800518FC; state 3 FUN_8007A624) — control flow owned native, the
    sub-behavior leaves stay PSX via rec_dispatch. Verified live on seaside: `obj80071a3cverify` 550+
    matches, 0 mismatch; gate-off native-driven reaches the field clean (no bad opcode).
  - **Object-pool init FUN_8007B18C owned native+live (2026-06-24):** the field case-0 prefix
    (engine_stage.cpp ov_field_run) now calls `ov_pool_init_run` (game/world/pool.cpp + pool.h) directly
    instead of rec_dispatch(0x8007b18c). It builds the 520-slot object pool / free-list and runs the eight
    sub-inits (leaves stay PSX via rec_dispatch). Inline A/B gate `poolinitverify` match #1/#2 (byte-exact
    full RAM+scratchpad+v0 vs rec_super_call); gate-off native-driven reaches the field clean (151 nodes).
  - **Control-block reset FUN_800796DC owned native+live (2026-06-24):** the next case-0 prefix leaf now
    calls `ov_796dc_run` (game/world/pool.cpp) instead of rec_dispatch(0x800796dc) — zeroes the 104-byte
    control block at 0x800BF808, clears ~30 scratchpad fields, runs its sub-inits (leaves stay PSX). Shared
    once-per-load gate `world_init_gate`; `init796dcverify` match #1/#2 byte-exact (incidental v0 mirrored:
    recomp epilogue leaves the 0x800c0000 store base). 151 nodes gate-off.
  - **Area record-seed FUN_800263E8 owned native+live (2026-06-24):** next case-0 prefix leaf → `ov_263e8_run`
    (game/world/pool.cpp). Walks a per-area byte sequence (table 0x8009D414[area]) to a 0xFF terminator,
    allocating a record per byte via 0x8007AD98 (PSX leaf) and stamping record[0]=1/record[2]=byte. Gate
    `init263e8verify` match #1/#2 byte-exact (incidental v0=0xFF terminator mirrored). 151 nodes gate-off.
  - **Clamp/control-block reset FUN_80075240 owned native+live (2026-06-24):** next case-0 prefix leaf →
    `ov_75240_run` (game/world/pool.cpp). Resets the control block at 0x800BE1F8 (clamp limits 0x7FFF/0x1FFF,
    counters) around three PSX leaf calls (0x80075D58/0x80075824/0x80099490). Gate `init75240verify` match
    #1/#2 byte-exact. With this, the FIRST FOUR case-0 prefix steps + placement are all native; only
    0x800783dc / 0x80078610 remain PSX in the prefix. 151 nodes gate-off.
  - **Per-area view/scroll setup FUN_800783DC owned native+live (2026-06-25):** next case-0 prefix leaf →
    `ov_783dc_run` (game/world/pool.cpp). Builds the view control block at 0x800E7E80 from the area control
    block 0x800BF870 (mode-3 path allocs a record via 0x80072DDC; other modes read a per-mode geometry table
    0x800A54A8 or the saved-camera scratchpad fields), then publishes four fields into the scratchpad camera
    (0x1F800207/0160/0162/0164). Two callees (0x80048D3C, 0x80072DDC) stay PSX leaves. Gate `init783dcverify`
    match #1/#2 byte-exact (incidental v0=0x1F800000 mirrored). 151 nodes gate-off. Only 0x80078610 now
    remains PSX in the case-0 prefix.
  - **Final per-area view init FUN_80078610 owned native+live (2026-06-25):** the LAST case-0 prefix leaf →
    `ov_78610_run` (game/world/pool.cpp). Zeroes the scratchpad (0x1F8000D0) + main (0x800E8008) view
    control blocks, seeds fixed view params, copies the three view vectors from the 0x800E7E80 block (built
    by ov_800783DC) into both (scratchpad copy biased by 0xFEC00000/0xF92A0000), runs the camera matrix
    builder 0x8006D02C, then sets the area draw-range half-word 0x801003F8 (233 if area mode==3 else 350) and
    runs 0x800846F0. Four callees (0x80051794×2, 0x8006D02C, 0x800846F0) stay PSX leaves. Gate
    `init78610verify` match #1/#2 byte-exact (incidental v0=0x80100000 mirrored — the `lui v0,0x8010` value
    survives to return). 151 nodes gate-off. **The ENTIRE field case-0 prefix is now PC-native** (pool init →
    796dc → 263e8 → placement → 75240 → 783dc → 78610); no rec_dispatch leaves remain in it.
  - **Case-0 tail state-index select FUN_80074F24 owned native+live (2026-06-25):** the field case-0 tail
    leaf → `ov_74f24_run` (game/world/pool.cpp), replacing `d1(0x80074f24, area)`. Early-outs on scratchpad
    0x1F800137==1 / area==21; else selects a per-area state index s0 (42, 10, or a bit-masked table lookup of
    0x800BFE56 → 0x800A4F68/0x800A4F50), calls 0x800750D8(s0,1), publishes s0 to scratchpad 0x1F80023B,
    clears 0x800BE22B. Callee 0x800750D8 stays a PSX leaf. Gate `init74f24verify` match #1/#2 byte-exact
    (incidental v0 mirrored for both early-outs and the work path). 151 nodes gate-off. **The whole field
    stage case-0 is now PC-native** (only the area==8 conditional 0x80114b90 leaf remains, never hit on
    seaside).
  - **Behavior handler FUN_8004CE14 owned native+live (2026-06-25):** added `engine/objbeh_8004ce14.cpp`
    and routed it through `dispatch_native_behavior`. A resident per-object state machine on node[4]: state 0
    seeds node fields + a record-list ptr from table 0x800A3F00[node[3]] then falls through; states 0/1 walk
    a 16-byte record list (node[0x6c]) to a 0xFF terminator, running a short-circuit visibility test per
    record (flag bit7 → FUN_8004D7EC vs FUN_8004D868, mask node[0x74]) and OR-ing an act-bit into node[0x70]
    when FUN_80077ACC / FUN_80111CCC returns nonzero; at the terminator stamps node[0x6a]=count, node[11]=31,
    FUN_80077EFC, node[1]=1. Control flow + all writes owned native; the 7 sub-behavior leaves stay PSX via
    rec_dispatch. Verified live on seaside: `obj8004ce14verify` 950+ matches, 0 mismatch (full RAM+scratchpad
    A/B); gate-off native-driven reaches the field clean (151 nodes, 0 bad opcode). One more resident handler
    (0x8006F2D0, ~450 instr) + the overlay handlers remain; 0x8004C238 still has its 40-mismatch bug.
  - **Behavior handler FUN_8006F2D0 owned native+live (2026-06-25):** added `engine/objbeh_8006f2d0.cpp`
    and routed it through `dispatch_native_behavior`. THE hottest still-PSX resident handler (~x777/field-frame,
    ~450 instr) — a state machine on node[4]: state 0 allocates a node[8]-long record list (FUN_8007AAE8 per
    record) seeded from table 0x800A4BA8 + base *(u32*)0x800ECF5C; state 1 reads the input/area block at
    0x800E7E80, branches on pad 0x1F80018E / 0x1F8001A8 to set node[1] + the rec[0xC0]/[0xC4] anim words
    (FUN_8004766C / FUN_80047B5C), runs FUN_8006F138 then spawns/despawns the linked node[0x14]/node[0x10]
    children (FUN_8006EFF4 / FUN_8007E038 / FUN_8006F02C) gated by area flags at 0x800BF840; states 2/3 trivial;
    EVERY exit clears byte 0x800BF840. Transcribed 1:1 as a register machine (goto labels = guest addresses) so
    delay-slot clobbers are exact (e.g. the f570 `v1=512` that deads the f574/f5b8 sub-branches). Control flow +
    all writes owned native; the 9 sub-behavior leaves stay PSX via rec_dispatch. Verified live on seaside:
    `obj8006f2d0verify` 750+ matches, 0 mismatch (full RAM+scratchpad A/B); gate-off native-driven reaches the
    field clean (151 nodes, 0 bad opcode). Overlay handlers remain; 0x8004C238 still has its 40-mismatch bug.
  - **Behavior handler FUN_8013C538 owned native+live (2026-06-25):** added `engine/objbeh_8013c538.cpp`
    and routed it through `dispatch_native_behavior`. THE hottest still-PSX OVERLAY handler (~x6091/field-frame
    on seaside; ~110 instr) — an area-overlay routine NOT in MAIN.EXE, disassembled from the field RAM dump
    (`scratch/ram/field_seaside.bin`). State machine on node[4]: state 0 reads area byte 0x800BF9E0, picks a
    scatter count (node[0x4e]=7,n=7 if <6 else 1,n=1) and seeds n stride-8 records at node[0x50] via
    FUN_80032A44(a0,a1) random offsets, sets node[4]=1 and FALLS INTO state 1; state 1 reads area block
    0x800E7E80 (byte +363/+42 early-outs), copies node[0x38]→node[0x34], then dithers the node[0x50] recs with
    node[0x4e] iterations of 3x FUN_8009A450() (two jitter variants gated on 0x800BF9E0<6: &3 vs &7 masks),
    then FUN_8002B278 / FUN_80031780; states 2/3 → FUN_8007A624; state ≥4 trivial. Transcribed 1:1 as a
    register machine (goto labels = guest addresses); a0/a1 written into c->r only where the guest writes them
    so the no-arg FUN_8009A450 leaves inherit the recomp's a0/a1. Control flow + all node writes owned native;
    the 5 sub-behavior leaves stay PSX via rec_dispatch. Verified live on seaside: `obj8013c538verify` 6100+
    matches, 0 mismatch (full RAM+scratchpad A/B); gate-off native-driven reaches the field clean (151 nodes,
    0 bad opcode). Disassemble further overlay handlers from the RAM dump (`tools/disas.py --ram …`).
  - **NEXT — extend the contiguity:** own the remaining per-object behavior handlers
    (e.g. the overlay-resident 0x801xxxxx handlers; the model-attach sites FUN_80077B38 +
    other per-object render-record callers) so the full graphics-bind set runs native; own the remaining
    case-0 prefix leaves (0x800796dc / 0x800263e8 / …). Each verified via its A/B gate. (Render itself =
    the `game/render/` decoupled native path, in progress separately.)
```
```
