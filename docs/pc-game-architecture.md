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
    mesh/        3D model/mesh draw from geomblk + node transform              (submit GT3/GT4, projection)
    terrain/     terrain mesh build + draw                                     (native_terrain)
    backdrop/    parallax tilemap backdrop                                     (ov_bg_tilemap_native)
    sprite/      2D sprites & world BILLBOARDS (pickups, weapon, decals)       (RE NEEDED)
    hud/         HUD / 2D overlay / font / UI rects                            (font)
    transition/  screen fades / area transitions                              (RE NEEDED)
    lighting/    per-area lights, shading                                      (lighting)
  world/       scene & world management
    area/        area/level loading, the field stage machine                  (level_load, core/engine)
    entity/      entity/object model + the active object lists                 (entity, object_init)
    spawn/       object placement & spawning                                   (entity_spawn)
    transform/   scene-node transforms / hierarchy
  actors/      characters & behavior (content; port where hot/needed)
    player/      Tomba controller (move/jump/attack)                           (actor_tomba)
    npc/ enemy/  per-object state machines / AI                               (beh_*, actor_sm_*)
  camera/      camera system                                                   (cutscene_camera)
  physics/     collision grid + response, hitboxes, cull/LOD                   (collision, hitbox, grid_offset, cull)
  assets/      resource load & decode (archives, textures, models, anim, palettes) (asset, bav_loader)
  audio/       SPU mixer, sequencer, SFX, the native music director            (audio/, sfx, native_music, sop)
  input/       controller / input mapping                                      (input)
  save/        save/load, inventory                                            (save_menu, inventory)
  ui/          front-end menus / title / demo / in-game menu                   (menu, demo)
  math/        math + PRNG + LUTs                                              (mathlib)
```
(Parenthesized = the existing `game/*` file(s) that become that subsystem's starting point.)

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
  - **Behavior handler FUN_80071A3C owned native+live (2026-06-24):** added `engine/beh_area_event_dispatch.cpp`
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
  - **Behavior handler FUN_8004CE14 owned native+live (2026-06-25):** added `engine/beh_record_list_scanner.cpp`
    and routed it through `dispatch_native_behavior`. A resident per-object state machine on node[4]: state 0
    seeds node fields + a record-list ptr from table 0x800A3F00[node[3]] then falls through; states 0/1 walk
    a 16-byte record list (node[0x6c]) to a 0xFF terminator, running a short-circuit visibility test per
    record (flag bit7 → FUN_8004D7EC vs FUN_8004D868, mask node[0x74]) and OR-ing an act-bit into node[0x70]
    when FUN_80077ACC / FUN_80111CCC returns nonzero; at the terminator stamps node[0x6a]=count, node[11]=31,
    FUN_80077EFC, node[1]=1. Control flow + all writes owned native; the 7 sub-behavior leaves stay PSX via
    rec_dispatch. Verified live on seaside: `obj8004ce14verify` 950+ matches, 0 mismatch (full RAM+scratchpad
    A/B); gate-off native-driven reaches the field clean (151 nodes, 0 bad opcode). One more resident handler
    (0x8006F2D0, ~450 instr) + the overlay handlers remain.
  - **Behavior handler FUN_8006F2D0 owned native+live (2026-06-25):** added `engine/beh_pad_child_linker.cpp`
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
    field clean (151 nodes, 0 bad opcode). Overlay handlers remain.
  - **Behavior handler FUN_8013C538 owned native+live (2026-06-25):** added `engine/beh_scatter_record_dither.cpp`
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
  - **Behavior handler FUN_8013C3F4 owned native+live (2026-06-25):** added `engine/beh_area_threshold_ptr_swap.cpp`
    (2nd-hottest overlay handler ~x4632/field-frame on seaside, ~80 instr; disassembled from the field RAM
    dump). State machine on node[4]: state 0 gates on area byte 0x800BF9E0 (>=28 -> node[4]=3) then node[4]=1
    and FALLS INTO state 1, which sets node[0x34] from node[0x38] (or overlay data ptrs 0x8014AC18/0x8014AF20
    selected via node[3] vs 0x800BF9E0 thresholds), then FUN_8002B278 / FUN_80031780; states 2/3 -> FUN_8007A624.
    Transcribed 1:1 (e.g. the c454 16-bit state store, the c494 branch-delay sltiu read at L4a8). Control flow +
    all node writes owned native; the 3 sub-behavior leaves stay PSX via rec_dispatch. Verified live:
    `obj8013c3f4verify` 4600+ matches, 0 mismatch (full RAM+scratchpad A/B); gate-off field clean (151 nodes,
    0 bad opcode).
  - **Behavior handler FUN_8013C9C0 owned native+live (2026-06-25):** added `engine/beh_scatter_ramp_machine.cpp`
    (3rd-hottest overlay handler ~x4302/field-frame on seaside, ~190 instr). A TWO-LEVEL state machine
    disassembled from the field RAM dump incl. its two in-overlay jump tables (jt0 @0x8010A000 [11 entries,
    node[5]] + jt1 @0x8010A030 [10 entries, node[5]-1]). Outer state node[4]: state 0 seeds node fields from
    word/stride-6 overlay tables @0x80109FC4/0x80109FD6 (by node[3]) then falls into state 1; state 1
    early-outs on area bytes (0x800E7FEB / 0x800BF9E0>=20 / 0x800BF816 / 0x800E7EAA>=17), runs a node[6]/[7]
    countdown, then dispatches jt0[node[5]] — an 11-way inner sub-state that ramps a node[0x56] counter and
    node[0x54] timers, toggles area bytes 0x800BF9E0/0x80109FC0, and twice calls FUN_80074590; tail dispatches
    jt1 to FUN_8002B278 for some (node[3],node[5]) combos. Transcribed 1:1; the two guest jump tables become
    switch->goto; signed-byte timer tests (sll v0,24;bgez) preserved. Control flow + all node/global/overlay
    writes owned native; the 3 leaves stay PSX via rec_dispatch. Verified live: `obj8013c9c0verify` 4300+
    matches, 0 mismatch (full RAM+scratchpad A/B); gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80136D9C owned native+live (2026-06-25):** added `engine/beh_pure_inner_dispatch.cpp`
    (~x2334/field-frame on seaside, ~90 instr). A pure CONTROL-FLOW dispatcher — owns no node writes; all
    effects are in its 8 sub-behavior leaves. Two-level dispatch (outer node[4], inner node[5]): state 0 ->
    FUN_80136F08; state 3 -> FUN_8007A624; state 1 conditionally calls FUN_8007778C then routes node[5] to
    FUN_80138A64/FUN_8018CDC4/FUN_80137198/FUN_8018CA1C and finally FUN_801389C8 (gated on node[1]/node[3] and
    area bytes 0x800BF89C/0x800E7EAA/0x800BF809). Transcribed 1:1; all leaves stay PSX via rec_dispatch.
    Verified live: `obj80136d9cverify` 2300+ matches, 0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80129C00 owned native+live (2026-06-25):** added `engine/beh_anim_trigger_gates.cpp`
    (~x2334/field-frame on seaside, ~130 instr). Two-level state machine with an in-overlay jump table
    (jt @0x80109C5C, 5 cases on node[3]). State 0 routes node[3] to FUN_801296E0/FUN_8012982C/FUN_80129984;
    state 1 dispatches jt: cases 0/1 are animation-triggers (gate on area bytes 0x800E7EAA/0x800E7FC7, set
    node[1]=1, bump node[0xC0][+12] += 16, FUN_800517F8); cases 2/3 -> FUN_80129160/FUN_801292E4; case 4
    early-outs on 0x800BF816/0x800BF8BC then bumps node[8] records' field[+8] by +16 when < -127 and calls
    FUN_80051C8C. Transcribed 1:1; jump table -> switch->goto; signed hword tests preserved. Control flow +
    direct node/record writes owned native; leaves stay PSX via rec_dispatch. Verified live:
    `obj80129c00verify` 2300+ matches, 0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_8012A0B8 owned native+live (2026-06-25):** added `engine/beh_box_seed_phase_gate.cpp`
    (~x2334/field-frame on seaside, ~135 instr). Outer state machine on node[4]: states 2/3 -> FUN_8007A624;
    state 0 is the INIT path (node[11]=32, node[4]=1, node[8]=node[9]=0, node[0x18]=0x8013EA64), routes node[3]
    to FUN_801360F4 + FUN_80139838 x2 (node[3]<2) or FUN_8013AC34 (>=2), then copies a per-node[3] record from
    the resident overlay table @0x80149EC4 (stride 10) into node fields 0x80/0x82/0x84/0x86 and computes two
    round-toward-zero midpoints (node[0x2E]/0x4E, node[0x36]) before FUN_80129E8C; state 1 reads scratchpad byte
    0x1F800207 and, when 25<=b<32, calls FUN_80129E8C and sets node[1]=1. Table READ live from overlay RAM (not
    embedded). Transcribed 1:1; signed (lh/sra) vs unsigned (lhu/lbu) preserved; control flow + direct node
    writes owned native, leaves stay PSX via rec_dispatch. Verified live: `obj8012a0b8verify` 2300+ matches,
    0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_8012DA04 owned native+live (2026-06-25):** added `engine/beh_typed_anim_spawn.cpp`
    (~x2331/field-frame on seaside, ~200 instr). Outer state machine on node[4] with TWO in-overlay jump
    tables (jt0 @0x80109DAC INIT, jt1 @0x80109DCC the per-node[3] animation/spawn sub-states), both indexed
    by node[3]<8. State 0 resets the node block or calls FUN_80051B70/FUN_800517F8; state 1's jt1 cases drive
    a node[5] sub-state, call FUN_80077B38 (model-attach) on entry then FUN_8004BD64 (5-arg, arg5 = &node[0x60]
    passed on the STACK), copy node[1] from node[0x10][1], and gate on area bytes 0x800BF9DD/0x800BF9B5 to run
    the FUN_8004D4C4+FUN_8004B0D8 tail / advance node[4]=3; state 2 runs that tail when node[3]==2; state 3
    -> FUN_8007A624. Mirrors the guest prologue (sp-=40) so the leaf reads arg5 from the right frame slot.
    Transcribed 1:1; both jump tables READ live from overlay RAM; control flow + direct node writes owned
    native, leaves stay PSX via rec_dispatch. Verified live: `obj8012da04verify` 2300+ matches, 0 mismatch;
    gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80121978 owned native+live (2026-06-25):** added `engine/beh_id_routed_dispatch.cpp`
    (hottest still-PSX overlay handler ~x1592/field-frame on seaside, ~115 instr). Outer state machine on
    node[4]: state 0 INIT (FUN_800519E0 + FUN_80077C40, seeds node[0x80..0x86]=140/280/128/256, node[0x44]=384,
    node[4]+=1); state 1 routes node[3] to a per-id sub-behavior leaf (0/1/95/96/97/98/99 ->
    FUN_801225BC/80122D58/801220FC/80121B44/80121CF8/80122CA4/8018BF08, all other ids none) then ALWAYS
    FUN_80122BF4(node)+node[0x2B]=0; state 3 -> FUN_8007A624. No leaf takes a stack arg. Transcribed 1:1;
    control flow + direct node writes owned native, leaves stay PSX via rec_dispatch. Verified live:
    `obj80121978verify` 1550+ matches, 0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80125E0C owned native+live (2026-06-25):** added `engine/beh_pure_substate_dispatch.cpp`
    (~x1556/field-frame on seaside, ~80 instr). PURE control-flow dispatcher (no direct node writes of its
    own). State 0 -> FUN_801253E8; state 1 -> FUN_8007778C then node[5] 0/1/2 -> FUN_80125FE0/801255CC/80125800;
    state 2 -> FUN_8007778C then FUN_801261FC when node[5] in {2,3}; states 1/2 share a tail (FUN_800518FC when
    node[1]!=0); state 3 -> FUN_8007A624. Transcribed 1:1; control flow owned native, all sub-behavior leaves
    stay PSX via rec_dispatch. Verified live: `obj80125e0cverify` 1550+ matches, 0 mismatch; gate-off field
    clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80128760 owned native+live (2026-06-25):** added `engine/beh_linked_advance_branch.cpp`
    (~x1556/field-frame on seaside, ~95 instr). Outer state machine on node[4]: state 0 -> FUN_80128308;
    state 1 splits on node[3] (0 -> branch A, 1 -> branch B), both gate an "advance node[5]" reset
    (node[11]=0, node[0x10]=0, node[5]+=1) on the linked object node[0x10][0x5E]; branch A -> FUN_801281B8,
    branch B drops to a scratchpad[0x207]<6 gate that runs FUN_801281B8+FUN_801285EC; state 3 -> FUN_8007A624.
    Transcribed 1:1; control flow + direct node writes owned native, leaves stay PSX via rec_dispatch.
    Verified live: `obj80128760verify` 1550+ matches, 0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80118240 owned native+live (2026-06-25):** added `engine/beh_typed_init_exit_poker.cpp`
    (~x1556/field-frame on seaside, ~370 instr — the biggest of the hot set). Outer state machine on node[4],
    each state sub-dispatching on node[3] (+node[5]/node[0x5E] inner sub-states). State 0 INIT per node[3]
    (FUN_80077B38 model-attach + node[0x80..0x86] sizes, node[3] 0/3 also FUN_8004B354, then node[4]+=1);
    state 1 is the heavy one (FUN_80077EFC, FUN_8004BD64 5-arg/stack arg, a shared block calling FUN_80051D90
    to fill scratchpad 0x1F8000C0 then copy node[0x2E]/0x32/0x36 + FUN_8004B374); state 2 runs FUN_8004D4C4+
    FUN_8004B0D8 exit tails, node[4]=3, and pokes area-flag bytes (0x800BF9DF|=0x20, 0x800BF9EA clears bits at
    node[0x60]&31 / (node[0x60]+4)&31, 0x800BF9EE|=2); state 3 -> FUN_8007A624. Mirrors the guest prologue
    (sp-=48) for the stack arg. Transcribed 1:1; control flow + direct node/area-flag writes owned native,
    leaves stay PSX via rec_dispatch. Verified live: `obj80118240verify` 1550+ matches, 0 mismatch; gate-off
    field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_8013A900 owned native+live (2026-06-25):** added `engine/beh_child_trig_motion.cpp`
    (~x1554/field-frame on seaside, ~205 instr). Outer state machine on node[4]: state 0 INIT gates on global
    0x800ED098, seeds node fields + sizes, allocates node[8] child records (FUN_8007AAE8) filled from the
    per-node[3] source table @0x8014AAB0 (8 bytes/rec) + FUN_80051B04, then per node[3] (0 -> FUN_801252C0
    x2 gated on 0x800BF8F7; 2 -> motion-field seed); state 1 gates scratchpad[0x207] in [23,31] + FUN_8007778C,
    then node[3]==0 runs a trig block (FUN_80083E80/F50 of the 0x800E7ED8 angle + FUN_80085690 x2, writing
    node[0xC4] record fields), 1/2 -> FUN_80135414, then FUN_800517F8; state 3 -> FUN_8007A624. The math helpers
    are ordinary rec_dispatch leaves (return via v0, not gte_op); a0 fidelity in the record loop kept by not
    clobbering c->r[4] between FUN_80051B04 and the next FUN_8007AAE8. Transcribed 1:1; control flow + direct
    node/record writes owned native. Verified live: `obj8013a900verify` 1550+ matches, 0 mismatch; gate-off
    field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80117658 owned native+live (2026-06-25):** added `engine/beh_prng_velocity_machine.cpp`
    (~x1552/field-frame on seaside, ~430 instr — the biggest overlay handler owned so far). Two-level state
    machine on node[4] with s2 = node[0x10] (a guest pointer the handler writes velocity/timer fields through,
    s2[14/20/22/24/26]): state 0 INIT seeds node fields per node[3] (0/1) + calls FUN_80077B38/FUN_80051B70,
    then FUN_8004B354 and node[4]++; state 1 is a per node[5]/node[94]/node[3] sub-machine driving the s2 fields
    (3 PRNG draws via FUN_8009A450) that converges on a shared node[3] dispatch (node[3]==0 -> FUN_8007778C +
    FUN_80077B5C + FUN_8004B374; node[3]==1 -> scratchpad[0x207]<5 gate + node[0xC0] struct -> pos update via
    FUN_800517F8); state 2 fires sound/effect leaves (FUN_8004D4C4/4F4, FUN_8004ED94, FUN_8004B0D8,
    FUN_8004BD04/BEA8, FUN_80042354, FUN_80040CDC, FUN_8005308C) + area-flag poke 0x800BF9DC|=1; state 3 ->
    FUN_8007A624. Transcribed 1:1 as a register machine (goto labels = guest addresses); delay-slot stores
    before a jal (node[0x3c]/0x5e/0x4/0x2b) mirrored to execute before the callee; lh/sra signed vs lhu/lbu
    unsigned preserved. PRNG draws return via c->r[2] and advance the shared RNG (gate rolls RAM back so both
    sides draw the same sequence). Control flow + direct node/s2/global writes owned native. Verified live:
    `obj80117658verify` 1550+ matches, 0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80135D64 owned native+live (2026-06-25):** added `engine/beh_quad_record_table_seed.cpp`
    (~x1552/field-frame on seaside, ~230 instr). Same record-alloc shape as FUN_8013A900. State 0: if
    node[3]<2 and global 0x800ED098 >= 4, allocate node[8]=4 child records (FUN_8007AAE8) filled from the
    per-node[3] source table @0x8014A780 (8 bytes/rec) + FUN_80051B04; then seed the common block (node[4]=
    node[0]=1, sizes 30/60/50/100, node[0x2E/0x32/0x36/0x56] from tbl2 @0x8014A7A0[node[3]], node[0x48]=512,
    node[0x4A]=50, node[0x60]=node[0x32]; node[3]==0 clamp of node[0x2E] vs scratch[0x160]) then FUN_80135414
    + FUN_800517F8. State 1: compute a gate from mem[0x800E7E84]/[0x800E7EAA]+scratch, optionally reload the
    tbl2 fields, then gate on scratchpad[0x207] (==23 needs scratch[0xDA]>=11000) + mem[0x800E7EAA]<32 and run
    FUN_8007778C/FUN_80135414/FUN_800517F8. State 3 -> FUN_8007A624. a0 fidelity in the record loop kept by
    not clobbering c->r[4] between FUN_80051B04 and the next FUN_8007AAE8; the transient node[4]=3 on the
    0x800ED098<4 path is overwritten by node[4]=1 at the tail (END RAM identical). Transcribed 1:1; control
    flow + direct node/record/global writes owned native. Verified live: `obj80135d64verify` 1550+ matches,
    0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_8013B2E4 owned native+live (2026-06-25):** added `engine/beh_flagbit_timer_machine.cpp`
    (~x778/field-frame on seaside, ~150 instr). State 0 INIT: FUN_800519E0(node,3,mem[0x800ECFD4],0x8015AABC)
    gate, node[0x3C]=mem[0x800ECFD8], global mem[0x800BF873] gate (-> node[4]=3), bit node[3] of mem[0x800BFA13]
    selects FUN_8013AF18(node,1,31) vs node[0x5E]=0, then FUN_80077C40(node,0x8001B7B0,a2); seeds node fields +
    node[4]++ (+ node[5]=4 when node[0x5E]==0). State 1: FUN_8007778C + FUN_80076D68, per node[6] sub-machine
    (timer node[0x40] countdown, node[0x2B]==3 block firing FUN_8004ED94(97|98,65) + global 0x800BF809 toggle),
    then FUN_8013B024(node,31) + FUN_800518FC; tail clears node[0x29]=node[0x2B]=0. State 3 -> FUN_8007A624.
    srav masked to (node[3]&31). Transcribed 1:1; control flow + direct node/global writes owned native.
    Verified live: `obj8013b2e4verify` 750+ matches, 0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80131D08 owned native+live (2026-06-25):** added `engine/beh_two_child_steer.cpp`
    (~x778/field-frame on seaside, ~135 instr). State 0 INIT: global mem[0x800ED098]<2 gate (-> node[4]=3),
    else seed node fields + node[4]++ and allocate 2 child records (FUN_8007AAE8) into node[0xC0]/node[0xC4]
    (each zeroed, rec[6]=-1), then FUN_80051B04(rec0,12,2) + FUN_80051B04(rec1,12,3), rec1[0]=6/rec1[2]=-1400,
    seed node[0x80..0x86]/node[0x40..0x4C]. State 1: per node[5] (==1 -> FUN_80131840; ==0 -> node[5] set from
    mem[0x800BF8B8]==255), then a1=sign16(scratch[0x162]-node[0x32]), FUN_800778E4(node,a1); if !=0 ->
    rec1[10]=(rec1[10]-32)&0xFFF + FUN_800517F8. State 3 -> FUN_8007A624. a0 fidelity in the record loop:
    guest a0 at first FUN_8007AAE8 is the original state byte (set c->r[4]=orig, don't touch across loop).
    Transcribed 1:1; control flow + direct node/record/global writes owned native. Verified live:
    `obj80131d08verify` 750+ matches, 0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80132400 owned native+live (2026-06-25):** added `engine/beh_single_child_cull.cpp`
    (~x778/field-frame on seaside, ~80 instr). Outer state machine on node[4]. State 0 INIT:
    v0=FUN_80051B70(node,12,37); bail if v0!=0, else seed node[0x80..0x86]=30/60/50/100,
    node[0x60]=-2350/node[0x62]=-1630/node[0x50]=1920, node[0]=1, node[0x29]=0, node[0x5E]=0, node[4]++,
    node[0x32]+=128, node[3]=0, then node[0x10]=FUN_8013A730(node). State 1: if mem[0x800BF89C]==2 OR
    mem[0x800E7EAA]!=node[4] -> v0=FUN_8007778C(node); if v0!=0 -> FUN_80132020 + FUN_800517F8; ALWAYS
    node[0x2B]=0 at the tail. State 3 -> FUN_8007A624. Dead `addiu v1,v0,-1936` (0x800BF870, never
    stored/read) dropped. Transcribed 1:1; control flow + direct node/global writes owned native. Verified
    live: `obj80132400verify` 750+ matches, 0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80133D6C owned native+live (2026-06-25):** added `engine/beh_twin_record_steer.cpp`
    (~x778/field-frame on seaside, ~180 instr). Outer state machine on node[4]. State 0 INIT (a0 clobbered
    to 2 by the beq-delay → node[8]=node[9]=2): mem[0x800ED098]<2 gate (→node[4]=3), else seed
    node[0x80/0x82]=140/node[0x84]=10/node[0x86]=70 + allocate 2 child records (FUN_8007AAE8) into
    node[0xC0]/node[0xC4] (rec0{[6]=-1,[0]=0}, rec1{[6]=0,[0]=-140}) + FUN_80051B04(rec,12,iter). State 1:
    steer rec0[0x0C] ±5 toward a1 / rec1[0x0C] ±10 toward a3 (snap-on-overshoot), targets keyed on
    node[0x29] (and, when ==1, on FUN_800781E0 dist of scratch[0x160/0x164]-rec0[0x2C/0x34]); then if
    mem[0x800E7EAA]<22 and FUN_8007778C(node)!=0 → FUN_800517F8(node). State 3 → FUN_8007A624. a0 fidelity
    in the record loop: first FUN_8007AAE8 a0=2, FUN_80051B04 leaves a0=rec for the 2nd. The step-clamp
    snaps to the FULL 32-bit target truncated to 16 bits (sh), not its sign-extension. Transcribed 1:1.
    Verified live: `obj80133d6cverify` 750+ matches, 0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80134FD8 owned native+live (2026-06-25):** added `engine/beh_multi_record_phase_machine.cpp`
    (~x778/field-frame on seaside, ~260 instr — TWO-level state machine, outer node[4] / inner node[5]).
    Outer state 0 INIT: mem[0x800ED098]<10 gate (→node[4]=3); else node[8]=node[9]=10 + allocate node[8]
    child records (FUN_8007AAE8/FUN_80051B04) into node[0xC0+4*i] with rec[8]=tbl@0x8014A758[i], then
    node[8]=9 + seed node[0x60..0x6E]. Inner node[5]==0: mem[0x800BF9DD]>=15 init (tbl2@0x8014A76C loop)
    else node[5]=1; ALWAYS node[8]++/FUN_800517F8/node[8]-- + 6x FUN_801252C0 record cascade
    ({[20]=node,[16]=node[0xC8/0xD8/0xC4/0xCC/0xD0]}) + FUN_8004CC64(node[0xD4],12). Inner node[5]==1:
    FUN_801344AC + node[6]/mem[0x800E7EAA]-gated FUN_80077EBC vs FUN_800779D0(node,0,-400,600). Inner
    node[5]==2: mem[0x800E7EAA]==37/range-gated FUN_80077EBC vs FUN_8007778C, then FUN_801347E4. Common
    tail node[8]++/FUN_800517F8/node[8]--. a0 fidelity in the record loop (first FUN_8007AAE8 a0=node,
    FUN_80051B04 carries a0=rec); FUN_801252C0 cascade rec[20]/rec[16] mirrored before the next call.
    Transcribed 1:1. Verified live: `obj80134fd8verify` 750+ matches, 0 mismatch; gate-off field clean
    (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_80136158 owned native+live (2026-06-25):** added `engine/beh_sine_motion_sfx.cpp`
    (~x778/field-frame on seaside, ~290 instr — a movement/steering handler with a sin step). State 0 INIT:
    FUN_80051B70 gate + node[0x80]=576/node[0x82]=1152 + FUN_8004766C/FUN_80048750 + seed node[0x90..0x94]
    from node[0x2E/0x32/0x36], falls through to State 1. State 1 inner node[5] machine: a should-run gate
    (scratch[0x207]>=24 AND mem[0x800BF816]!=1), then the MATH block (gated mem[0x800BF809]==0) clamps
    node[0x44] to a ±window, derives an angular step (rounding divides), sin(s2)·832>>12 into node[0x36]
    (+ mem[0x800E7EB6] accumulate when node[0x29]==1), stores rec[0x0C]=s2 / node[0x44]=s1; post-math runs
    the mem[0x800E7EAA]-keyed FUN_8007703C/8007778C + velocity-magnitude SFX (FUN_80074AF0/80074590).
    Transcribed 1:1 with `goto L<hex>` labels (delay-slot-faithful: branch reads pre-delay reg, delay-slot
    writes still execute). FUN_80083E80 = owned sin leaf (no GTE). Verified live: `obj80136158verify` 750+
    matches, 0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **TOOLING SHIFT (user, 2026-06-25): handlers now get REAL descriptive names + are ported from GHIDRA
    decomp, not hand-disassembly.** All 36 prior `objbeh_<addr>.cpp` renamed to `beh_<descriptive>.cpp`
    (guest addr kept in the header comment + BEH_FN + the dispatch case). Decompile any overlay handler to
    readable C with `<local-notes>/skills/decomp-port/DecompDump.py` against the Ghidra project
    `scratch/ghidra/proj/tomba2_field` (program **field_seaside.bin**, base 0x80000000, MIPS:LE:32 — NOT the
    stale field_ram.bin which is zeros in the overlay). `DECOMP_TARGETS=<file> DECOMP_OUT=scratch/decomp/field2
    analyzeHeadless … -process field_seaside.bin -noanalysis -postScript DecompDump.py`. DecompDump now
    creates a function on demand when the analyzer never reached the overlay address. Don't write "NO GTE" in
    comments (GTE paths aren't forbidden; the PC renderer just owns its own rendering).
  - **Behavior handler FUN_8013ADBC = `beh_box_rearm_sub` owned native+live (2026-06-25, first via the Ghidra
    flow):** 4-state machine — state 0 seeds size/box params node[0x80..0x86] + node[0x40]=30; state 1 gates
    FUN_8013AC98 on FUN_8007778C; state 2 re-arms node[5]=node[0x5E] then FUN_8013AC98; state 3 FUN_8007A624.
    Verified live: `box_rearm_subverify` 750+ matches, 0 mismatch; gate-off field clean (151 nodes).
  - **Behavior handler FUN_8011CBD0 = `beh_node3_router` owned native+live (2026-06-25, Ghidra flow):**
    4-state machine — state 0 (gated mem[0x800BF89C]>3) seeds box params + node[0x3C]=mem32[0x800ECFA4]
    (shared context ptr); state 1 routes on node[3] to FUN_8011C674/FUN_8011CA04 + node[1]-gated
    FUN_800518FC + tail FUN_8011CD14; state 2 sets node[4]=3; state 3 FUN_8007A624. Verified live:
    `node3_routerverify` 750+ matches, 0 mismatch; gate-off field clean (151 nodes).
  - **Behavior handler FUN_8011D988 = `beh_actor_move_sm` owned native+live (2026-06-25, Ghidra flow):** a
    rich movement actor — outer node[4] phases (1 vs 2), each a switch on the movement sub-state node[5]
    dispatching a large FUN_8011/8012xxxx movement-leaf family, gated on mem[0x800BF809]/scratch[0x137]/
    node[3]. Preserves the state-2 case-0xB fallthrough (->2/7/8) and the state-1 cross-case goto (cases
    0/1 -> case 4's FUN_8012185C). State 3 clears node[0x1B]&0x40 or despawns. Verified live:
    `actor_move_smverify` 750+ matches, 0 mismatch; gate-off field clean (151 nodes).
  - **Behavior handler FUN_8011D578 = `beh_variant_actor_sm` owned native+live (2026-06-25, Ghidra flow +
    raw cross-check):** state 0 INIT gated on FUN_800519E0 seeds a behavior table @node[0x7C]=0x8014DE54 +
    node[0x2C/0x30/0x34] 32-bit consts + a node[3]-variant (node[0x56]/node[0x7B]); state 1 routes node[3]
    (0->FUN_8011D108) / node[5] (0: scratch/FUN_8007778C-gated FUN_80042354/80040D68 advance; 1: FUN_80077E7C
    + node[0x70] retreat) + FUN_80041098/8004190C; state 3 despawn; tail FUN_800518FC/8011D82C. GOTCHA fixed
    vs Ghidra: several `lbu == 0xFF` it typed as `== -1` (mem[0x800BF8BC], node[0x70]); scratch[0x160] is a
    SIGNED lh. Verified live: `variant_actor_smverify` 750+ matches, 0 mismatch; gate-off field clean (151).
  - **Behavior handler FUN_8013A330 = `beh_lift_platform` owned native+live (2026-06-25, Ghidra+raw, biggest
    this session):** a 13-segment vertical mover that follows a parent's direction; registers itself as the
    singleton mem32[0x800BF854]. State 0 allocates 13 child records + FUN_80118974/8013A184/8013989C init;
    state 1 mirrors parent[0x5E] into node[0x5E] to drive node[0x30] up/down by node[0x50]*±0x100, clamps to
    node[0x60]/node[0x62] (toggling mem[0x800BF9EE] + node[0xBF]), plays SFX 0x8D (FUN_80074590), runs a
    node[5] sub-machine (FUN_80139E64/80139C2C/8013A008) + FUN_800517F8; tail writes node[1] into each
    record[0x3F] + FUN_80139A70. KEY GOTCHA: Ghidra's `unaff_s0` else-path = the INCOMING guest s0 (c->r[16],
    callee-saved → preserved across rec_dispatch) — reading it reproduces the recomp's register flow exactly;
    `mem[0x800BF8B9] == -1` is `== 255`; mem[0x800ED098] is a signed lh. Verified live: `lift_platformverify`
    750+ matches, 0 mismatch; gate-off field clean (151 nodes).
  - **Behavior handler FUN_80136954 = `beh_event_record_machine` owned native+live (2026-06-25, Ghidra+raw):**
    a 4-record object that fires an event/cutscene via a node[5] jump-table machine. State 0 allocates 4
    records from the 5-short table @0x8014A7E4 + seeds node[0x60..0x6E]; state 1 (FUN_8007778C-gated) runs
    node[5] cases: 0 = mem[0x800BF8B9]==255 gate, 2 = mem[0x800BF9DD]==12 advance, 3 = FUN_8013681C, 4 = a
    node[6] sub-phase that fires the event (DAT_800E7E84/85 + FUN_80042354/80042310/8006FF10 spawn +
    FUN_800440E4); tail FUN_800517F8 + node[0x2B]=0. GOTCHAs: `== -1` is `== 255` (lbu); `DAT_800bf80c._2_1_`
    = mem8(0x800BF80E); case-4 FUN_800440E4 reached only via the n6==1/n6==2-success fallthrough. Verified
    live: `event_record_machineverify` 750+ matches, 0 mismatch; gate-off field clean (151 nodes).
  - **Behavior handler FUN_8011C164 = `beh_typed_variant_router` owned native+live (2026-06-25, Ghidra+raw,
    biggest handler ~1196 B):** a TYPE-routed actor. node[3] = variant, node[4] = outer state. State 0 INIT
    seeds node[0x3c]=*PTR(0x800ecf80), box node[0x80..0x86], FUN_80077b38(.,0x8014c808,8), and for type<2
    copies the 8-byte entry @0x80148914+type*8 → node[0x2e]/[0x32]/[0x36](16b)+node[0x2a](8b); type==3 sets
    node[4]=3 when mem8(0x800bf8bc)==255. State 1 ACTIVE = `switch(node[3])` 0..0x14 → per-type sub-behaviors
    (FUN_8011bc3c/b324/b738/ada8/c090/bf04); cases 0 & 1 are identical PRNG (FUN_8009a450) substate machines
    on node[6] with a node[0x40] countdown; every state-1 exit sets node[0x29]=0. State 2 ENTER, state 3
    EXIT (FUN_8007a624). GOTCHAs: `mem8(0x800e7eaa) < 0x1a` is unsigned `sltiu`; `== -1` is `== 255` (lbu);
    case-2's FUN_8004bd64 takes a 5th *stacked* arg → mirror the recomp frame (sp-0x30) into guest stack
    below entry sp + dispatch with that frame sp. Verified live: `typed_variant_routerverify` 750+ matches,
    0 mismatch; gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_8004C238 = `beh_visibility_gate_dispatch` now LIVE (2026-06-25, later-232c fix):**
    the long-written resident handler that had a 40-mismatch gate bug is now wired and verified. Root cause:
    STATE-1 cases 6-14 each clear `node[0x29]=0` in the DELAY SLOT of their `j 0x8004c750` (0x8004c634/c64c/
    c65c/c66c/c690/c6a8/c6c0/c6d8/c6f0/c700/c710/c720/c730/c740) — the original transcription dropped those
    delay-slot stores and just fell through to the c750 tail (which only cleared node[0x2b]). Fix: clear
    node[0x29]=0 at the shared c750 tail (every recomp predecessor of c750 has it 0, so exact). Verified live
    on seaside: `visibility_gate_dispatchverify` 3100+ matches, 0 mismatch; gate-off field clean (151 nodes).
  - **Behavior handler FUN_80059ED8 = `beh_camera_target_follow` owned native+live (2026-06-25, resident,
    THE hottest still-PSX resident handler ~x741):** a camera/view tracker. node[0x10] = the tracked target
    object; the handler derives view extents (node[0x40]/[0x42] by sign of target[0x17e]), the look-at
    (node[0x48]/[0x4a]/[0x4c] + smooth params [0x56]/[0x58]) via three branches (target[0x29]==0||[0x78]!=0
    → A: target[0x16b]==8 immediate else FUN_800489e4 + scratchpad 0x1f8001a0..a6 gating; else → B:
    target[0x6a] nibble!=2 copy), scroll-clamps node[0x4e]/[0x50] to [0,0x80]/[0,0x100], then dispatches a
    per-area camera routine (switch on 0x800BF870: 0/4/6→FUN_8010c5a8/80115afc/80114294 gated mem8(0x800bf816)
    ==0, 8→FUN_8011332c, 0xb→FUN_8010bc10, 0xe→FUN_8010b238) gated on 0x800BF873==0 && mem8(0x800bf80d)==0 &&
    target[0x158]==0. States 2&3 → FUN_8007a624. GOTCHAs: target[0x17e]/[0x44]/[0x32]/[0x4a] signed `lh`;
    mem16(0x1f80017c) is `lhu`; 0x1f8001a6 nibble=((int16)v>>8)&0xf, signbit=(int16)v&0x8000; scroll math
    arithmetic >>2 on a signed delta. Verified live: `camera_target_followverify` 700+ matches, 0 mismatch;
    gate-off field clean (151 nodes, 0 bad opcode).
  - **Behavior handler FUN_8003AD48 = `beh_cube_text_spawn` owned native+live (2026-06-25, resident):** the
    "cube letters" text actor (~x142). State 0 measures a string (node[3]==2 -> "Clear" @0x800a3a8c, else
    table mem32(0x800a33cc + node[0x60]*12)) via FUN_80073750, stores the length in node[8] (+1 for Clear),
    handles overflow (>=33 chars -> two FUN_8009a730 logs + node[4]=2), gates on mem16(0x800ed098), then
    spawns node[8] glyph records (FUN_8007aae8 + FUN_80051b04(rec,1,uVar7) per glyph) and seeds the layout
    fields. State 1 routes node[3] to FUN_8003a790/a9a0/abe4 then node[1]=1 + FUN_800517f8; state 2 sets
    node[4]=3 and decrements globals 0x800bf849/0x800ed06c; state 3 FUN_8007a624. GOTCHA: the record-alloc
    loop relies on a0/a1 LEFTOVER — FUN_8007aae8 carries the a0 left by the prior rec_dispatch (FUN_80073750
    first iter, FUN_80051b04 after, which leaves a0=rec), so c->r[4] is NOT written before FUN_8007aae8.
    Verified live: `cube_text_spawnverify` 100 matches, 0 mismatch; gate-off field clean (151 nodes).
  - **Behavior handler FUN_80127798 = `beh_area_transition_machine` owned native+live (2026-06-25, overlay):**
    THE area-transition / cutscene-fade driver (~x774). node[4] state machine; STATE 2's node[5]==3 path runs
    a node[6] PHASE MACHINE that fades out (DAT_800bf9b5=3 + FUN_80042354/FUN_80040b48), FIRES the next-area
    load (FUN_80054198 + FUN_80054d14), animates the transition camera (FUN_80085690 + `<<8`/`/64` deltas into
    node[0x4e/50/52] + the DAT_800e80xx block + FUN_80074590), integrates the deltas into DAT_800e7eac/eb0/eb4,
    and re-seeds. Tail re-projects (FUN_8004bd64, 5th arg STACKED) + commits the camera (FUN_8006cba8). Owning
    this is what lets the cutscene FADE be driven PC-native and enables a debug area-switch (node[5]==3 IS the
    switch mechanism). GOTCHAs: scratchpad[0x207] gate is unsigned `(v-29)<3`; Ghidra FALSELY flagged
    FUN_80054d14 "noreturn" (it returns — case-2's camera setup after it is real); case-4 writes are CONCAT22
    (store the HI halfword); camera div is signed `/64` (truncate toward 0); `<<16>>8` == (int16)<<8. VERIFIED
    headless: hot paths (state 0/1/2 sub<3) `area_transition_machineverify` 750+ matches, 0 mismatch; AND the
    full node[5]==3 transition machine — phases 0/1/2/3/4 + the FUN_8004bd64/FUN_8006cba8 tail — gate-verified
    0 mismatch by forcing a live object through each phase via the REPL (the A/B compare is valid from any
    forced snapshot; even phase-2's real area-load re-ran deterministically). Gate-off field clean (151 nodes).
  - **MILESTONE — per-object behavior-handler frontier CLEARED (2026-06-25).** Of the 46 distinct per-object
    handlers exercised in headless seaside (`debug behhist`), **45 are owned native**. The only one left is
    **0x8013C1DC** — ~x4/run, too rare to gate meaningfully.
  - **NEXT — beyond the behavior handlers:** advance the spine to the next still-PSX layer — the model-attach
    sites (FUN_80077B38 + the per-object render-record callers) so the full graphics-bind set runs native,
    the remaining case-0 prefix leaves (0x800796dc / 0x800263e8 / …), and the `game/render/` decoupled native
    render path. Each verified via its A/B gate or (for render) the live-eyeball gate. See docs/port-progress.md.
```
```
