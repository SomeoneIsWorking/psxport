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

### 2) RENDER subsystem (`game/render/`) — build AFTER assets, ON TOP of native assets.
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
  - **NEXT — extend the contiguity:** wire the owned prefix siblings the same way (e.g. pool-init
    `ov_8007B18C` in game/world/pool.cpp — expose + direct-call), then descend into the graphics-binding
    attach (model/geom set, render-record) on the live path, each verified via its A/B gate.
```
```
