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

## RENDER subsystem — the first target (the current pain point)
The field must render ENTIRELY from scene data, no recomp render, no OT walk. Pipeline reference =
MAIN.EXE 0x8003f9a8 (orchestrator) → render walks → per-type drawers → 2D sprite band (0x80025d98) → backdrop
(0x8003df04→tilemap 0x80115598, DONE) → fades. Decompile each, map the scene-data model, reimplement under
`game/render/`. Status + the per-drawer RE map: `docs/render-rebuild.md` (next).

## Migration (non-destructive, keep it building every step)
Move existing `engine/*` files into their subsystem folders incrementally, updating the SRC lists in
`run.sh` + `tools/build_port.sh` and include paths in the SAME change; build after each move. Keep the recomp
fallback (`platform/`) intact. Retire the scenenative/g_ot_2d_only/bgonly/ot2dtest experiments as the native
render subsystem replaces them.
```
```
