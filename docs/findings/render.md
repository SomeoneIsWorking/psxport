# Findings — render / engine submit

## Opening-cutscene narration renders nothing on the native FIELD path
- **symptom:** New Game → the opening story cutscene ("Tomba is living peacefully in the country when Zippo finds a mysterious...") draws NOTHING; the prior menu's stale VRAM shows through (looks like the menu "overstays"/garbles).
- **status:** fixed 10a07e0
- **cause:** the cutscene runs in the FIELD/GAME stage (0x8010637C). ov_draw_otag's field branch (game_tomba2.cpp) runs ov_scene_native (PC-native 3D world) and SKIPPED the PSX OT walk entirely — so ALL guest 2D (the narration glyph SPRITES the field submits to the OT) was dropped.
- **fix:** game_tomba2.cpp field branch now runs ov_scene_native THEN a 2D-only OT walk (g_ot_2d_only=1; gpu_dma2_linked_list; =0), queuing leftover 2D HUD sprites as RQ_HUD on top of the native world. gpu_native.cpp g_ot_2d_only mode DROPS all guest-OT polys (the field 3D world is native-owned; is3d is unreliable here — projprim is empty on the field path, so is3d==0 for every poly → keeping them re-emits the whole world as flat HUD = render-queue overflow + the free-roam crash) and keeps only 2D HUD sprites.
- **refs:** journal later-252; game_tomba2.cpp ov_draw_otag; gpu_native.cpp g_ot_2d_only

## 2D-poly overlays and world-billboard sprites on the field 2D-only walk (open frontier)
- **symptom:** in-field gradient/fade PANELS (polys) or world-billboard sprites may be missing or flat in the 2D-only field overlay pass.
- **status:** known-issue (frontier)
- **cause:** on the native field path projprim/obj_depth provenance is empty, so 2D polys can't be told from 3D-world polys (all is3d==0). g_ot_2d_only drops all polys to avoid re-emitting the world; world-billboard sprites aren't discriminated.
- **fix:** NOT yet solved. The cutscene text + common HUD are sprites and work. Don't "fix" by keeping field polys in the 2D walk — that re-introduces the render-queue overflow/crash (see above).
- **refs:** journal later-252; gpu_native.cpp g_ot_2d_only comment

## native field path renders ONLY the sky/sea backdrop — the 3D world (grass/house/tree/Tomba) is occluded
- **symptom:** in the GAME free-roam seaside field, the native render path (`ov_scene_native`, field-default in `ov_draw_otag`) shows the cyan sea + sky backdrop + 2D HUD but NO 3D world; `PSXPORT_RENDER_PSX=1` (PSX OT walk) renders the full correct scene (grass/house/tree/fence/crane/Tomba). Same on SDL_GPU and the old VK renderer (USER-confirmed).
- **status:** known-issue (the later-235..245 "sea on top" / render-ordering blocker; OPEN #1)
- **cause:** ENGINE-SIDE render ordering, NOT a renderer regression — the SDL_GPU port reproduced the same depth-band behavior the VK path had. The decoupled native scene (`ov_scene_native`: backdrop tilemap RQ_BACKGROUND + terrain + entity lists at real depth) draws the backdrop but the world geometry does not land in front of it on this path. The PSX OT walk is correct because it uses PSX OT order. The render-queue band math in gpu_gpu.cpp is internally consistent (backdrop set_order_2d_bg≈0 far, world ord3d∈[0.0625,0.9375], HUD≈1 near; GREATER_OR_EQUAL + clear 0) — so the gap is in what `ov_scene_native` actually QUEUES for the world, not the renderer.
- **fix:** NOT a gpu_gpu.cpp fix — do not re-diff renderers (gpu_vk.cpp is deleted; it had the SAME bug). A/B method: headless `PSXPORT_AUTO_SKIP=1 PSXPORT_DEBUG_SERVER=1`, `shot` native vs relaunch with `PSXPORT_RENDER_PSX=1` `shot` (scratch/screenshots/autoskip_freeroam.png vs autoskip_psx.png). Next: instrument `ov_scene_native` (engine_submit.cpp) to confirm whether the world walks/entity-lists emit RQ_WORLD prims at the seaside at all (vs empty), since the backdrop alone draws — i.e. is the terrain/entity walk producing geometry on THIS code path.
- **refs:** journal later-235..245, engine_submit.cpp ov_scene_native, game_tomba2.cpp:219 (field routing), gpu_native.cpp rq_emit_or_queue, gpu_gpu.cpp ord3d/set_order_2d_bg
