# cmake/tomba2_port.cmake — build the native PC port binary `tomba2_port` (SDL3 / SDL_GPU).
#
# THE build for the port. Produces scratch/bin/tomba2_port (where the REPL/driver tooling + docs
# expect it). As of the P1.7 framework-split gate (2026-07-17) this target is now just the GAME:
#   * ALL PSX-generic framework code lives in the STATIC library `psxport` (cmake/psxport.cmake) —
#     runtime/recomp/** + the vendored Beetle GTE/MDEC/SPU backends + the RmlUi SDL backend + the
#     generated SDL_GPU shader header.
#   * This target compiles ONLY game/* + generated/* (the substrate shards) and links libpsxport.a.
#
#   cmake -S . -B build && cmake --build build --target tomba2_port
#   ./scratch/bin/tomba2_port scratch/bin/tomba2/MAIN.EXE      # (after run.sh has extracted MAIN.EXE)
#
# The renderer is SDL_GPU (gpu_gpu.cpp, framework side) — SDL3 owns window+input+audio+GPU.

option(PSXPORT_BUILD_PORT "Build the Tomba!2 native port binary (tomba2_port)" ON)

# The framework static library (psxport) + its option(PSXPORT_BUILD_SMOKE) + the standalone smoke.
# Always included so `psxport` / `psxport_smoke` are buildable even when the game target is off.
include(${CMAKE_SOURCE_DIR}/cmake/psxport.cmake)

if(NOT PSXPORT_BUILD_PORT)
  return()
endif()

# ---- game source list (game/* only — the framework moved to cmake/psxport.cmake) --------------
set(GAME_SRC
  game/game_tomba2.cpp
  game/cd/libcd_native.cpp
  game/core/asset.cpp
  game/core/game_config.cpp
  game/core/game_ctx.cpp
  game/core/game_hooks.cpp
  game/core/repl_commands.cpp
  game/core/register_overrides.cpp
  game/core/str.cpp
  game/core/pc_scheduler.cpp
  game/core/verify_harness.cpp
  game/math/mathlib.cpp
  game/math/rng.cpp
  game/math/mtx.cpp
  game/math/trig.cpp
  game/render/cull.cpp
  game/player/collision.cpp
  game/player/hitbox.cpp
  game/player/grid_offset.cpp
  game/world/spawn.cpp
  game/scene/scene_events.cpp
  game/scene/script_interp.cpp
  game/audio/sfx.cpp
  game/audio/audio_dispatch.cpp
  game/audio/sequencer.cpp
  game/world/area_slots.cpp
  game/scene/mode_state_arm.cpp
  game/world/placement.cpp
  game/world/graphics_bind.cpp
  game/world/pool.cpp
  game/world/entity.cpp
  game/render/render_native.cpp
  game/render/scene_build.cpp
  game/render/mesh_draw.cpp
  game/object/actor_sm_24448.cpp
  game/object/actor_sm_reward.cpp
  game/object/cube_text_ledger.cpp
  game/ai/beh_scene_ui_trigger.cpp
  game/ai/beh_typed_init_scene_trigger.cpp
  game/ai/beh_pickup_collect_trigger.cpp
  game/ai/beh_substate_edge_orchestrator.cpp
  game/ai/beh_jumptable_release_trigger.cpp
  game/ai/release_trigger_motion.cpp
  game/ai/beh_typed_table_seed_gate.cpp
  game/ai/beh_typed_jumptable_pair.cpp
  game/ai/beh_cull_substate_orchestrator.cpp
  game/ai/beh_id_compare_motion_dispatch.cpp
  game/ai/actor_zoned_attacker.cpp
  game/ai/attack_orbit_substate.cpp
  game/ai/actor_melee_engage.cpp
  game/ai/beh_actor_tomba_proximity_combat.cpp
  game/ai/melee_proximity.cpp
  game/ai/beh_jumptable_flag_gate.cpp
  game/ai/beh_cull_tick_render.cpp
  game/ai/beh_sibling_angle_track.cpp
  game/ai/beh_visibility_gate_dispatch.cpp
  game/ai/beh_record_list_scanner.cpp
  game/ai/beh_area_event_dispatch.cpp
  game/ai/beh_pad_child_linker.cpp
  game/ai/beh_scatter_record_dither.cpp
  game/ai/beh_area_threshold_ptr_swap.cpp
  game/ai/beh_scatter_ramp_machine.cpp
  game/ai/beh_pure_inner_dispatch.cpp
  game/ai/beh_anim_trigger_gates.cpp
  game/ai/beh_box_seed_phase_gate.cpp
  game/ai/beh_typed_anim_spawn.cpp
  game/ai/beh_id_routed_dispatch.cpp
  game/ai/beh_pure_substate_dispatch.cpp
  game/ai/beh_linked_advance_branch.cpp
  game/ai/beh_typed_init_exit_poker.cpp
  game/ai/beh_child_trig_motion.cpp
  game/ai/beh_prng_velocity_machine.cpp
  game/ai/beh_quad_record_table_seed.cpp
  game/ai/beh_flagbit_timer_machine.cpp
  game/ai/beh_two_child_steer.cpp
  game/ai/beh_single_child_cull.cpp
  game/ai/beh_twin_record_steer.cpp
  game/ai/beh_multi_record_phase_machine.cpp
  game/ai/beh_sine_motion_sfx.cpp
  game/ai/beh_box_rearm_sub.cpp
  game/ai/beh_node3_router.cpp
  game/ai/beh_actor_move_sm.cpp
  game/ai/beh_variant_actor_sm.cpp
  game/ai/beh_lift_platform.cpp
  game/ai/beh_event_record_machine.cpp
  game/ai/beh_typed_variant_router.cpp
  game/ai/beh_camera_target_follow.cpp
  game/ai/beh_cube_text_spawn.cpp
  game/ai/beh_area_transition_machine.cpp
  game/ai/beh_rand_phase_cull.cpp
  game/ai/beh_pos_history_trail.cpp
  game/ai/beh_variant_overlay_lifecycle.cpp
  game/ai/beh_a06_multi_actor.cpp
  game/ai/beh_a06_scripted_actor.cpp
  game/ai/beh_a06_script_fades.cpp
  game/ai/beh_a08_scene_actor.cpp
  game/ai/beh_toy_spawn_family.cpp
  game/ai/beh_sop_intro_pilot.cpp
  game/ai/beh_sop_intro_lifted.cpp
  game/ai/beh_sop_intro_narration.cpp
  game/ai/sop_overlay_shadow.cpp
  game/ai/sop_intro_events.cpp
  game/ai/beh_seaside_prox_substate.cpp
  game/ai/area_seaside_perframe.cpp
  game/ai/beh_substate_edge_leaves.cpp
  game/ai/beh_cull_substate_leaves.cpp
  game/player/actor_tomba.cpp
  game/player/actor_tomba_actions.cpp
  game/player/actor_tomba_action_8005accc.cpp
  game/player/actor_tomba_action_8005aee4.cpp
  game/player/actor_tomba_action_8005f1b0.cpp
  game/player/actor_tomba_action_800588bc.cpp
  game/player/actor_tomba_action_800531dc.cpp
  game/player/actor_tomba_action_800660ac.cpp
  game/player/actor_tomba_action_8005ef48.cpp
  game/core/engine_field_transition.cpp
  game/scene/bg_scene_transition_sm.cpp
  game/scene/parallax_bg.cpp
  game/scene/scene_transition.cpp
  game/scene/transition_state3.cpp
  game/object/object_list.cpp
  game/object/array8_dispatch.cpp
  game/world/object_table.cpp
  game/core/demo_leaf_a.cpp     # port_gen.py validation draft (0x8001CE90), UNWIRED dead code
  game/core/demo_leaf_b.cpp     # port_gen.py validation draft (0x8002311C), UNWIRED dead code
  game/object/script_vm.cpp
  game/object/animation.cpp
  game/input/input.cpp
  game/input/pad_edge_fence.cpp
  game/ui/menu.cpp
  game/ui/dialog_text_stream.cpp
  game/items/inventory.cpp
  game/render/lighting.cpp
  game/ui/bav_loader.cpp
  game/ui/save_menu.cpp
  game/audio/music_coord.cpp
  game/scene/startup.cpp
  game/ui/font.cpp
  game/ui/panel.cpp
  game/scene/level_load.cpp
  game/render/fps60.cpp
  game/object/behavior_dispatch.cpp
  game/render/submit.cpp
  game/render/node_xform.cpp
  game/render/projection.cpp
  game/render/render_frame.cpp
  game/render/cine_bars.cpp
  game/render/narration_swirl.cpp
  game/render/render_walk.cpp
  game/core/engine.cpp
  game/core/field_owned_leaves.cpp
  game/core/field_seq_scheduler.cpp
  game/core/announcer_cue_push.cpp
  game/core/spawn_type6_node.cpp
  game/core/field_target_cursor.cpp
  game/scene/sop.cpp
  game/scene/demo.cpp
  game/camera/cutscene_camera.cpp
  game/camera/cutscene_camera_selftest.cpp
  game/math/gte_math.cpp
  game/math/wide_re_gte_transform3.cpp
  game/render/wide_re_libgpu_leaves.cpp
  game/render/wide_re_gpu_dma_queue.cpp
  game/render/wide_re_gpu_loadimage_streamer.cpp
  game/render/wide_re_gpu_putdrawenv.cpp
  game/render/native_terrain.cpp
  game/render/render_queue.cpp
  game/render/screen_fade.cpp
  game/render/margin_render.cpp
  game/render/ffspan.cpp
  game/render/quad_rtpt_submit.cpp
  game/audio/native_audio.c
  game/audio/native_music.cpp
  game/audio/music_list.cpp
  game/render/render_observer.cpp
  game/render/overlay_gt3gt4.cpp
  game/render/overlay_ground_gt3gt4.cpp
  game/render/tile_grid_layer.cpp
  game/render/widescreen_margin_quad.cpp
  game/render/hud_gauge_emitter.cpp
  game/render/perobj_dispatch.cpp
  game/render/perobj_billboard.cpp
  game/render/text_label.cpp
  game/render/render_walk_dispatch.cpp
  game/render/overlay_type_dispatch.cpp
  game/render/objlist_walk.cpp)

# The recompiler substrate: the statically-recompiled shards (C++ content in .c files) = the game
# binary MAIN.EXE + each OVERLAY module. emit.py writes the exact TU list to
# generated/rec_sources.cmake (GEN_REC_SRCS, basenames). Compiled as C++.
#
# -foptimize-sibling-calls IS REQUIRED, NOT an optimization nicety: a guest TAIL JUMP (a computed `jr`
# routed to rec_dispatch, or a `j`/branch to a framed sibling) is emitted as `dispatch(c,x); return;` /
# `func_<addr>(c); return;` in tail position. The guest uses such tail jumps for LOOPS (e.g. the
# register-based jump-table state machine at 0x8007E2F8) that iterate indefinitely. Without sibling-call
# optimization each iteration is a real C call -> the stack grows per loop -> SIGSEGV. With it, the whole
# rec_dispatch -> main_dispatch -> func_<addr> -> gen_func_<addr> tail chain collapses to a jump, so the
# loop runs in O(1) stack. -O2 enables it; we set it explicitly atop -O1 so the dependency is documented
# and survives an -O level change.
include(${CMAKE_SOURCE_DIR}/generated/rec_sources.cmake)
list(TRANSFORM GEN_REC_SRCS PREPEND generated/)
set_source_files_properties(${GEN_REC_SRCS}
  PROPERTIES LANGUAGE CXX
  COMPILE_OPTIONS "-O1;-foptimize-sibling-calls;-fno-strict-aliasing;-fwrapv")

add_executable(tomba2_port ${GAME_SRC} ${GEN_REC_SRCS})
# The framework's shader header is generated by the psxport library's custom target; the game exe
# (via gpu_gpu.cpp in libpsxport) transitively needs it present before its own compile ordering.
add_dependencies(tomba2_port gen_gpu_shaders)

# C++17 for this target (engine), matching the framework library.
set_target_properties(tomba2_port PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
  ENABLE_EXPORTS ON                                   # -rdynamic: watchdog backtrace symbol names
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/scratch/bin)

# GAME include dirs. The framework include dirs (RT, generated, vendored backends, SDL/freetype) are
# inherited PUBLICly from the psxport link below — only the game/* subfolders are added here.
target_include_directories(tomba2_port PRIVATE
  game  game/ai  game/audio  game/camera  game/cd  game/core  game/input  game/items  game/math  game/object  game/player  game/render  game/scene  game/ui
  game/world)

target_compile_options(tomba2_port PRIVATE -w -O2 -g
  ${SDL3_CFLAGS_OTHER} ${FREETYPE_CFLAGS_OTHER})

# The framework library carries all system/vendored link deps + compile defs as PUBLIC, so linking it
# is all the game exe needs.
target_link_libraries(tomba2_port PRIVATE psxport)
