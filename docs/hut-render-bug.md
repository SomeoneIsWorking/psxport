# Hut-entry render bug — diagnosis (2026-06-25)

**Symptom (user, live + deterministic replay):** walk Tomba into the hut near the start area — gameplay
transitions into the hut interior, BUT the native render keeps showing the village exterior, and the
screen fade is missing. "As if the transition didn't fully happen."

## Deterministic repro (the input recorder makes this exact + repeatable)
- Recording: `scratch/bin/hut_entry.pad` (a hand-played session; recording is now always-on by default to
  `scratch/bin/pad_session.pad`, so the user can play on macOS and send the `.pad`).
- Replay: `PSXPORT_PAD_REPLAY=scratch/bin/hut_entry.pad` (deterministic — the engine has no clock/RNG).
- Diagnostics (all in `runtime/recomp/pad_input.cpp`, replay-frame indexed):
  - `PSXPORT_PAD_SHOT_AT=f0,f1,...` → `scratch/screenshots/padshot_<f>.ppm`
  - `PSXPORT_PAD_TRACE=lo-hi` → per-frame markers (bf839, bf80f, 1f800236, sm[0x4a/4c/4e], scene, bf870, Tomba X/Z)
  - `PSXPORT_PAD_DUMP_AT=f0,...` → 2 MB RAM (+.spad) + scene classification at those replay frames
- The transition window: f284–296 Tomba walks INTO the hut (Z 3968→4160 — "up" is depth, not sideways);
  f298 `bf80f` 0→2 (seamless transition direction byte); f362 `sm[0x4c]` 2→3 (mid-transition field handler
  `0x801070b4` = `ov_field_run_x` → `ov_field_frame_x` → `ov_render_frame_x`); **f394 Tomba Z swaps 4160→4608
  = the interior sub-scene**; exits ~f408–459. `scene-active` (0x800BE258) STAYS 2 and `bf870` STAYS 0 —
  it is a SUB-SCENE swap within the same area, not an area-id change (so `warp <id>` cannot reproduce it).

## ROOT CAUSE — strictly a RENDER bug (mandated A/B confirms)
Same native gameplay, render layer swapped (`PSXPORT_RENDER_PSX=1` = PSX render, default = native render):
- **PSX render @ f394: draws the hut INTERIOR** (Tomba inside, wooden floor, walls, NPC, hanging items) — correct.
- **Native render @ f394: draws the village EXTERIOR** — the bug.
Gameplay swaps the sub-scene correctly (PSX render proves the interior geometry is in guest RAM); the
NATIVE engine render does not follow it.

## What it is NOT (ruled out)
- **Not terrain.** `terrain geomblk 0x8009FAE8` is byte-identical village↔interior (RAM dump diff); both PSX
  `FUN_8002ab5c`→`FUN_80027768(&DAT_8009fae8,…)` and native `terrain_render_pc` hardcode that village buffer.
  Skipping the native terrain pass (`debug noterr`) at f394 → NO change (village still drawn).
- **Not the BG node.** `bg ptr 0x800e8008` unchanged; skipping `ov_bg_render` (`debug nobg`) → NO change.
- **Not `ecf58` model groups** (unchanged) and **not the `sm[0x4a]==5` fade-transition** (`FUN_80108a60`,
  ported in engine_stage.cpp but a DIFFERENT door type = area-id warps; the hut uses the bf80f seamless path).

## What it IS — the object/entity render draws the wrong (village) object set
With terrain+bg skipped the exterior STILL draws, so the stale village comes from the **object/entity render**
(the per-object renderers + entity walk). The interior is entity/scene-overlay driven: the RAM blocks that
change village→interior are `0x800E0000` (scene tables), `0x800F0000` (entity nodes — hut actors loaded),
`0x80100000`, `0x80140000` (scene overlay — interior objects + handlers). The native render is rendering the
VILLAGE objects in the interior while the PSX render renders the INTERIOR objects — same entity list, same
gameplay. Strong suspect: the per-object RENDER SNAPSHOT QUEUE (scratchpad cursor `0x1F800140` / count
`0x1F800146`, drained by the gen_func_8003BB50 driver) — notably the SAME `0x1F800140..15F` region the SBS
divergence detector flagged. Either the native render walks a stale snapshot / wrong object set, or the
sub-scene swap's object-list update isn't reflected in what the native render drains.

## The FADE half
The seamless transition's fade is `FUN_8007e9c8` called from a still-recomp content handler via the OT path
(NOT one of the 3 `engine_fade_set` native call-sites). The native renderer no longer draws the OT fade rect,
so the fade is invisible. Fix: route this caller's fade through `engine_fade_set` (own the transition's fade
delivery natively) — or restore a path so the seamless transition fade reaches the engine fade.

## NEXT STEPS (the fix)
1. Pin the exact object-render divergence at f394: instrument the snapshot-queue driver (gen_func_8003BB50 /
   the entity walk that fills `0x1F800140`) — log the node set the native render drains vs what PSX drains.
   (VK-level attribution: the OT scene-dump is blind to native VK geometry; use the ffspan per-pass tags /
   vkstats / a per-object-render node log.) Find why native draws village objects, PSX draws interior.
2. Make the native object render follow the swapped sub-scene object set (the engine owns object selection
   from the current scene, not a stale snapshot/camera).
3. Route the transition fade (`FUN_8007e9c8` from the seamless-transition content handler) through
   `engine_fade_set`.
4. This is a CLASS of bug ("native render doesn't follow scene/sub-scene state"). After fixing, sweep for
   siblings: other sub-scene swaps, in-area scene changes, anything where the native render reads a fixed/stale
   source instead of the current scene.

Diagnostic probes added: `engine_submit.cpp` `cfg_dbg("noterr")`/`cfg_dbg("nobg")` skip the native terrain/bg
passes in `ov_render_walk` (set via the debug server `debug noterr,nobg`).
