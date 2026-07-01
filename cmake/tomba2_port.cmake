# cmake/tomba2_port.cmake — build the native PC port binary `tomba2_port` (SDL3 / SDL_GPU).
#
# THE build for the port (the old hand-rolled run.sh g++ link is the fallback). Produces
# scratch/bin/tomba2_port (where the REPL/driver tooling + docs expect it). The source list below is the
# same set run.sh compiles — KEEP IN SYNC with run.sh / tools/build_port.sh.
#
#   cmake -S . -B build && cmake --build build --target tomba2_port
#   ./scratch/bin/tomba2_port scratch/bin/tomba2/MAIN.EXE      # (after run.sh has extracted MAIN.EXE)
#
# The renderer is SDL_GPU (gpu_gpu.cpp) — SDL3 owns window+input+audio+GPU; no Vulkan/SDL2 dev needed.
# The RmlUi mod/debug overlay (ESC) links the vendored static librmlui(_debugger) + system freetype.

option(PSXPORT_BUILD_PORT "Build the Tomba!2 native port binary (tomba2_port)" ON)

if(NOT PSXPORT_BUILD_PORT)
  return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 sdl3)
pkg_check_modules(FREETYPE freetype2)
if(NOT (SDL3_FOUND AND FREETYPE_FOUND))
  message(WARNING "tomba2_port skipped: needs pkg-config sdl3 + freetype2 "
                  "(found sdl3=${SDL3_FOUND} freetype2=${FREETYPE_FOUND})")
  return()
endif()

set(RT runtime/recomp)
set(MED vendor/beetle-psx/mednafen)

# ---- vendored RmlUi (HTML/CSS mod overlay), static, Core + Debugger only ----------------------
set(BUILD_SHARED_LIBS OFF)
set(RMLUI_SAMPLES        OFF CACHE BOOL   "" FORCE)
set(RMLUI_LUA_BINDINGS   OFF CACHE BOOL   "" FORCE)
set(RMLUI_SVG_PLUGIN     OFF CACHE BOOL   "" FORCE)
set(RMLUI_LOTTIE_PLUGIN  OFF CACHE BOOL   "" FORCE)
set(RMLUI_FONT_ENGINE    freetype CACHE STRING "" FORCE)
add_subdirectory(vendor/rmlui EXCLUDE_FROM_ALL)

# ---- generated SDL_GPU SPIR-V header (runtime/recomp/gpu_gpu_shaders.h) ------------------------
# tools/gen_gpu_shaders.sh compiles shaders_gpu/*.{vert,frag} (incl. the RmlUi overlay shaders) and
# embeds the SPIR-V into the source tree. Re-run when a shader source changes.
file(GLOB SHADER_SRCS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/${RT}/shaders_gpu/*.vert ${CMAKE_SOURCE_DIR}/${RT}/shaders_gpu/*.frag)
set(SHADERS_H ${CMAKE_SOURCE_DIR}/${RT}/gpu_gpu_shaders.h)
add_custom_command(OUTPUT ${SHADERS_H}
  COMMAND bash ${CMAKE_SOURCE_DIR}/tools/gen_gpu_shaders.sh
  DEPENDS ${SHADER_SRCS} ${CMAKE_SOURCE_DIR}/tools/gen_gpu_shaders.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Generating SDL_GPU SPIR-V header (gpu_gpu_shaders.h)"
  VERBATIM)
add_custom_target(gen_gpu_shaders DEPENDS ${SHADERS_H})

# ---- source list (KEEP IN SYNC with run.sh / tools/build_port.sh) -----------------------------
set(PORT_SRC
  runtime/recomp/dispatch.cpp
  runtime/recomp/interp.cpp          # ORACLE engine (later-278): pure-MIPS interpreter for the oracle Core
  runtime/recomp/coro.cpp
  runtime/recomp/overlay_router.cpp
  runtime/recomp/cfg.c
  runtime/recomp/mem.cpp
  runtime/recomp/stubs.cpp
  runtime/recomp/hle.cpp
  runtime/recomp/threads.cpp
  runtime/recomp/gpu_native.cpp
  runtime/recomp/gpu_debug.cpp
  runtime/recomp/vram_xfer.cpp
  runtime/recomp/spu_audio.c
  runtime/recomp/pad_input.cpp
  runtime/recomp/memcard.cpp
  runtime/recomp/native_fmv.cpp
  vendor/beetle-psx/mednafen/psx/gte.c
  runtime/recomp/gte_beetle.cpp
  vendor/beetle-psx/mednafen/psx/mdec.c
  runtime/recomp/mdec_beetle.c
  vendor/beetle-psx/mednafen/psx/spu.c
  runtime/recomp/spu_beetle.c
  runtime/recomp/disc.c
  runtime/recomp/cd_override.cpp
  runtime/recomp/cdc_native.c
  runtime/recomp/xa_stream.c
  runtime/recomp/timing.cpp
  runtime/recomp/gpu_gpu.cpp
  runtime/recomp/gpu_perf.cpp
  runtime/recomp/mods.c
  game/game_tomba2.cpp
  game/core/asset.cpp
  game/math/mathlib.cpp
  game/render/cull.cpp
  game/player/collision.cpp
  game/player/hitbox.cpp
  game/player/grid_offset.cpp
  game/world/spawn.cpp
  game/world/placement.cpp
  game/world/graphics_bind.cpp
  game/world/verify_gate.cpp
  game/world/pool.cpp
  game/world/entity.cpp
  game/render/render_native.cpp
  game/render/scene/scene_build.cpp
  game/render/mesh/mesh_draw.cpp
  game/object/actor_sm_24448.cpp
  game/ai/beh_scene_ui_trigger.cpp
  game/ai/beh_typed_init_scene_trigger.cpp
  game/ai/beh_pickup_collect_trigger.cpp
  game/ai/beh_substate_edge_orchestrator.cpp
  game/ai/beh_jumptable_release_trigger.cpp
  game/ai/beh_typed_table_seed_gate.cpp
  game/ai/beh_typed_jumptable_pair.cpp
  game/ai/beh_cull_substate_orchestrator.cpp
  game/ai/beh_id_compare_motion_dispatch.cpp
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
  game/scene/bg_scene_transition_sm.cpp
  game/items/script.cpp
  game/object/animation.cpp
  game/input/input.cpp
  game/ui/menu.cpp
  game/items/inventory.cpp
  game/ui/hud.cpp
  game/render/lighting.cpp
  game/ui/engine_bav.cpp
  game/items/save.cpp
  game/audio/sound.cpp
  game/scene/engine_init.cpp
  game/ui/engine_font.cpp
  game/scene/engine_level.cpp
  game/render/fps60.cpp
  game/object/engine_tomba2.cpp
  game/render/engine_submit.cpp
  game/render/engine_project.cpp
  game/render/engine_render.cpp
  game/render/engine_render_walk.cpp
  game/scene/engine_stage.cpp
  game/scene/sop.cpp
  game/scene/engine_demo.cpp
  game/camera/cutscene_camera.cpp
  game/math/engine_math.cpp
  game/player/engine_player.cpp
  game/render/native_terrain.cpp
  game/render/render_queue.cpp
  game/core/clib.cpp
  game/math/gte.cpp
  game/render/gpu_lib.cpp
  game/audio/sound_voice.cpp
  game/render/margin_render.cpp
  game/audio/native_audio.c
  game/audio/native_music.c
  game/audio/music_list.c
  runtime/recomp/sync_overrides.cpp
  runtime/recomp/native_boot.cpp
  runtime/recomp/repl.cpp
  runtime/recomp/dbg_server.cpp
  runtime/recomp/native_stub.cpp
  runtime/recomp/watchdog.c
  runtime/recomp/dualcore.cpp
  runtime/recomp/sbs.cpp
  runtime/recomp/sbs_present_sdl.cpp
  runtime/recomp/selftest.cpp
  runtime/recomp/boot.cpp
  runtime/recomp/rmlui_overlay.cpp
  runtime/recomp/rmlui_render_gpu.cpp
  runtime/recomp/overlay_glue.cpp
  vendor/rmlui/Backends/RmlUi_Platform_SDL.cpp)

# The recompiler substrate: link the statically-recompiled shards (C++ content in .c files). The
# interpreter was removed (2026-06-30) — these shards ARE the execution substrate for every
# non-native guest function. The MAIN module + each OVERLAY module (overlapping \BIN\*.BIN stage
# overlays) emit a dynamic set of TUs, so emit.py writes the exact list to generated/rec_sources.cmake
# (GEN_REC_SRCS, basenames). Compiled as C++.
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
list(APPEND PORT_SRC ${GEN_REC_SRCS})
set_source_files_properties(${GEN_REC_SRCS}
  PROPERTIES LANGUAGE CXX
  COMPILE_OPTIONS "-O1;-foptimize-sibling-calls;-fno-strict-aliasing;-fwrapv")

add_executable(tomba2_port ${PORT_SRC} ${SHADERS_H})
add_dependencies(tomba2_port gen_gpu_shaders)

# C++17 + -fpermissive for this target (mednafen/engine), overriding the project-wide C++20.
set_target_properties(tomba2_port PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
  ENABLE_EXPORTS ON                                   # -rdynamic: watchdog backtrace symbol names
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/scratch/bin)

target_include_directories(tomba2_port PRIVATE
  ${RT} ${CMAKE_SOURCE_DIR}/generated
  game  game/ai  game/audio  game/camera  game/core  game/input  game/items  game/math  game/object  game/player  game/render  game/scene  game/ui
  game/world game/render game/render/scene game/render/mesh
  ${MED} ${MED}/psx
  vendor/beetle-psx/libretro-common/include vendor/beetle-psx
  vendor/beetle-psx/deps/libchdr/include
  vendor/rmlui/Include vendor/rmlui/Backends
  ${SDL3_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS})

target_compile_definitions(tomba2_port PRIVATE
  PSXPORT_SDL _XOPEN_SOURCE=700 RMLUI_STATIC_LIB RMLUI_SDL_VERSION_MAJOR=3)

# -w (warnings off) and -O2 -g, matching the shell build; -fpermissive + narrowing-suppression only for
# C++ (clang has no -fpermissive and treats braced-init narrowing as a hard error). Keeps the CMake
# (macOS) build in step with run.sh / build_port.sh.
target_compile_options(tomba2_port PRIVATE -w -O2 -g
  $<$<COMPILE_LANGUAGE:CXX>:-fpermissive -Wno-c++11-narrowing -Wno-narrowing>
  ${SDL3_CFLAGS_OTHER} ${FREETYPE_CFLAGS_OTHER})

target_link_libraries(tomba2_port PRIVATE
  rmlui_debugger rmlui_core chdr-static
  ${SDL3_LIBRARIES} ${FREETYPE_LIBRARIES}
  Threads::Threads m)
target_link_directories(tomba2_port PRIVATE
  ${SDL3_LIBRARY_DIRS} ${FREETYPE_LIBRARY_DIRS})
