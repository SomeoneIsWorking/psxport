#!/usr/bin/env bash
# Incremental build of the native port (scratch/bin/tomba2_port).
#
# The native port is NOT built by any Makefile; it is built by run.sh (full
# build-extract-run) or by this script (incremental compile + relink, no run).
# Use this during a fix loop so you don't re-extract/re-recompile the whole game core.
#
#   tools/build_port.sh                      # recompile only sources newer than their .o, relink
#   tools/build_port.sh runtime/recomp/cdc_native.c   # force-recompile these file(s), relink
#   tools/build_port.sh all                  # recompile every source, relink
#
# Object naming mirrors run.sh: scratch/obj/<path-with-/.-as-_>.o
# Then run scratch/bin/tomba2_port directly (REPL via PSXPORT_REPL=1, or windowed).
set -eu
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
CXX="${CXX:-c++}"   # RmlUi overlay (rmlui_overlay.cpp + rmlui_render_vk.cpp + RmlUi SDL backend) is C++
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
RT=runtime/recomp       # common PSX->PC platform (future psxport submodule)
ENG=engine              # Tomba2Engine — the game-specific native engine + RE
MED=vendor/beetle-psx/mednafen
OBJ=scratch/obj
mkdir -p "$OBJ"

# No -Igenerated: the interpreter-only runtime links no recompiled shards (later-101).
INC="-I$RT -I$ENG -Igame/world -Igame/render -Igame/render/scene -Igame/render/mesh -I$MED -I$MED/psx -Ivendor/beetle-psx/libretro-common/include -Ivendor/beetle-psx -Ivendor/beetle-psx/deps/libchdr/include"
CFLAGS="-O2 -g -w -D_XOPEN_SOURCE=700 $INC $(pkg-config --cflags sdl3 2>/dev/null) -DPSXPORT_SDL"
# The RmlUi overlay + raylib SBS presenter are DROPPED in the SDL_GPU build (their SDL2/Vulkan/OpenGL
# backends don't port to SDL3/SDL_GPU this pass — see docs/render-backend-port.md). Their symbols are
# satisfied by rmlui_overlay_stub.cpp / sbs_raylib_stub.cpp, so no static libs are built or linked.
# -fpermissive (GCC) downgrades C++11 braced-init narrowing to a warning; clang has no -fpermissive and makes
# it a hard ERROR, so a portable build needs the explicit -Wno for clang (e.g. the provably-safe int16_t->float
# in engine_project.cpp's matrix inits, and the recompiler-generated shards which narrow and can't be hand-edited).
# -Wno-c++11-narrowing is clang's exact diagnostic; -Wno-narrowing covers GCC; each is harmlessly ignored by the other.
CXXFLAGS="-O2 -g -w -fpermissive -Wno-c++11-narrowing -Wno-narrowing -std=c++17 $INC $(pkg-config --cflags sdl3 2>/dev/null) -DPSXPORT_SDL"

# PSXPORT_SUBSTRATE=1: link the statically-recompiled bodies (generated/shard_*.c + shard_disp.c) as the
# no-interpreter substrate (top-down native port). The shards are C++ content in .c files (call Core
# methods c->mem_*), so compile_one routes generated/*.c through $CXX -x c++. Default (unset) = the
# interpreter-only build (no shards linked; dispatch.cpp's rec_func_index() is always -1, so the
# interp's recompiled-routing is a no-op). See docs/native-port-plan.md.
SHARDS=""
if [ -n "${PSXPORT_SUBSTRATE:-}" ]; then
  CFLAGS="$CFLAGS -DPSXPORT_SUBSTRATE -Igenerated"
  CXXFLAGS="$CXXFLAGS -DPSXPORT_SUBSTRATE -Igenerated"
  SHARDS="generated/shard_disp.c generated/shard_0.c generated/shard_1.c generated/shard_2.c generated/shard_3.c generated/shard_4.c generated/shard_5.c generated/shard_6.c generated/shard_7.c"
  OBJ=scratch/obj_sub   # SEPARATE object cache: substrate TUs carry -DPSXPORT_SUBSTRATE; sharing the
  mkdir -p "$OBJ"       # interp-only OBJ dir would relink stale/wrong-flag objects (dup rec_dispatch).
fi

tools/gen_gpu_shaders.sh   # compile+embed the SDL_GPU present shaders (gpu_gpu_shaders.h) before gpu_gpu.cpp

# Same source list as run.sh step 4 (keep in sync). C++ TUs (.cpp) = RmlUi overlay; compiled with $CXX.
# OOP refactor: the runtime is now C++ (state lives in `class Core`, core.h). Most TUs are .cpp;
# the remaining .c are leaf C subsystems (cfg/disc/cdc/xa/spu/mdec adapters/mods/watchdog) + vendored
# mednafen, reached across the boundary via extern "C".
SRC="$RT/dispatch.cpp \
  $RT/cfg.c $RT/mem.cpp $RT/stubs.cpp $RT/hle.cpp $RT/threads.cpp $RT/interp.cpp $RT/gpu_native.cpp $RT/gpu_debug.cpp $RT/vram_xfer.cpp $RT/spu_audio.c $RT/pad_input.cpp $RT/memcard.cpp $RT/native_fmv.cpp \
  $MED/psx/gte.c $RT/gte_beetle.cpp $MED/psx/mdec.c $RT/mdec_beetle.c $MED/psx/spu.c $RT/spu_beetle.c \
  $RT/disc.c $RT/cd_override.cpp $RT/cdc_native.c $RT/xa_stream.c $RT/timing.cpp $RT/gpu_gpu.cpp $RT/gpu_perf.cpp $RT/mods.c $ENG/game_tomba2.cpp $ENG/asset.cpp $ENG/mathlib.cpp $ENG/cull.cpp $ENG/collision.cpp $ENG/hitbox.cpp $ENG/grid_offset.cpp game/world/spawn.cpp game/world/placement.cpp game/world/graphics_bind.cpp game/world/verify_gate.cpp game/world/pool.cpp game/world/entity.cpp game/render/render_native.cpp game/render/scene/scene_build.cpp game/render/mesh/mesh_draw.cpp $ENG/actor_sm_24448.cpp $ENG/beh_scene_ui_trigger.cpp $ENG/beh_typed_init_scene_trigger.cpp $ENG/beh_pickup_collect_trigger.cpp $ENG/beh_substate_edge_orchestrator.cpp $ENG/beh_jumptable_release_trigger.cpp $ENG/beh_typed_table_seed_gate.cpp $ENG/beh_typed_jumptable_pair.cpp $ENG/beh_cull_substate_orchestrator.cpp $ENG/beh_id_compare_motion_dispatch.cpp $ENG/beh_jumptable_flag_gate.cpp $ENG/beh_cull_tick_render.cpp $ENG/beh_sibling_angle_track.cpp $ENG/beh_visibility_gate_dispatch.cpp $ENG/beh_record_list_scanner.cpp $ENG/beh_area_event_dispatch.cpp $ENG/beh_pad_child_linker.cpp $ENG/beh_scatter_record_dither.cpp $ENG/beh_area_threshold_ptr_swap.cpp $ENG/beh_scatter_ramp_machine.cpp $ENG/beh_pure_inner_dispatch.cpp $ENG/beh_anim_trigger_gates.cpp $ENG/beh_box_seed_phase_gate.cpp $ENG/beh_typed_anim_spawn.cpp $ENG/beh_id_routed_dispatch.cpp $ENG/beh_pure_substate_dispatch.cpp $ENG/beh_linked_advance_branch.cpp $ENG/beh_typed_init_exit_poker.cpp $ENG/beh_child_trig_motion.cpp $ENG/beh_prng_velocity_machine.cpp $ENG/beh_quad_record_table_seed.cpp $ENG/beh_flagbit_timer_machine.cpp $ENG/beh_two_child_steer.cpp $ENG/beh_single_child_cull.cpp $ENG/beh_twin_record_steer.cpp $ENG/beh_multi_record_phase_machine.cpp $ENG/beh_sine_motion_sfx.cpp $ENG/beh_box_rearm_sub.cpp $ENG/beh_node3_router.cpp $ENG/beh_actor_move_sm.cpp $ENG/beh_variant_actor_sm.cpp $ENG/beh_lift_platform.cpp $ENG/beh_event_record_machine.cpp $ENG/beh_typed_variant_router.cpp $ENG/beh_camera_target_follow.cpp $ENG/beh_cube_text_spawn.cpp $ENG/beh_area_transition_machine.cpp $ENG/bg_scene_transition_sm.cpp $ENG/script.cpp $ENG/animation.cpp $ENG/input.cpp $ENG/menu.cpp $ENG/inventory.cpp $ENG/hud.cpp $ENG/lighting.cpp $ENG/engine_bav.cpp $ENG/save.cpp $ENG/sound.cpp $ENG/engine_init.cpp $ENG/engine_font.cpp $ENG/engine_level.cpp $ENG/fps60.cpp $ENG/engine_tomba2.cpp $ENG/engine_submit.cpp $ENG/engine_project.cpp $ENG/engine_render.cpp $ENG/engine_stage.cpp $ENG/sop.cpp $ENG/engine_demo.cpp $ENG/engine_camera.cpp $ENG/engine_math.cpp $ENG/engine_player.cpp $ENG/native_terrain.cpp $ENG/render_queue.cpp $ENG/clib.cpp $ENG/gte.cpp $ENG/gpu_lib.cpp $ENG/sound_voice.cpp $ENG/native_misc.cpp $RT/peripheral_misc.cpp $ENG/margin_render.cpp $ENG/audio/native_audio.c $ENG/audio/native_music.c $ENG/audio/music_list.c $RT/sync_overrides.cpp $RT/native_boot.cpp $RT/dbg_server.cpp $RT/native_stub.cpp $RT/watchdog.c $RT/dualcore.cpp $RT/sbs.cpp $RT/sbs_raylib_stub.cpp $RT/boot.cpp \
  $RT/rmlui_overlay_stub.cpp \
  $SHARDS"

objof() { echo "$OBJ/$(echo "$1" | tr '/.' '__').o"; }

FORCE=""
[ "${1:-}" = "all" ] && FORCE="all" && shift || true
for f in "$@"; do FORCE="$FORCE $f"; done

compile_one() { o="$(objof "$1")";
  case "$1" in
    generated/*.c) $CXX $CXXFLAGS -x c++ -O1 -c "$1" -o "$o" ;;  # recompiled shards: C++ content, lower opt (6MB)
    *.cpp|*.cc)    $CXX $CXXFLAGS -c "$1" -o "$o" ;;
    *)             $CC  $CFLAGS  -c "$1" -o "$o" ;;
  esac || { echo "FAILED: $1" >&2; exit 1; }; }
export -f compile_one objof; export CC CXX CFLAGS CXXFLAGS OBJ

TODO=""
for s in $SRC; do
  o="$(objof "$s")"
  # gpu_vk.c embeds the generated SPIR-V header — recompile it when that header changes too, else a
  # shader-only edit relinks a STALE object with old bytecode (cost a long debug session once).
  hdr_dep=""; case "$s" in *gpu_gpu.cpp) hdr_dep="$RT/gpu_gpu_shaders.h";; esac
  if [ "$FORCE" = "all" ] || [ ! -f "$o" ] || [ "$s" -nt "$o" ] || \
     { [ -n "$hdr_dep" ] && [ "$hdr_dep" -nt "$o" ]; } || printf '%s' " $FORCE " | grep -qF " $s "; then
    TODO="$TODO $s"
  fi
done
if [ -n "$TODO" ]; then
  echo "[build] compiling:$TODO"
  printf '%s\n' $TODO | xargs -P"$JOBS" -I{} bash -c 'compile_one "$@"' _ {}
else
  echo "[build] nothing to recompile"
fi

CHDR_DIR=build/vendor/beetle-psx/deps/libchdr
find_a() { find "$CHDR_DIR" -name "$1" 2>/dev/null | head -1; }
CHD_LIBS="$(find_a 'libchdr-static.a') $(find_a 'libchdr-lzma.a') $(find_a 'libminiz.a')"
ZSTD_A="$(find_a 'libzstd.a')"; [ -n "$ZSTD_A" ] && CHD_LIBS="$CHD_LIBS $ZSTD_A" || CHD_LIBS="$CHD_LIBS $(pkg-config --libs libzstd 2>/dev/null || echo -lzstd)"

OBJS=""; for s in $SRC; do OBJS="$OBJS $(objof "$s")"; done
# shellcheck disable=SC2086
# Link with $CXX so libstdc++ is pulled in.
$CXX -rdynamic $OBJS $CHD_LIBS $(pkg-config --libs sdl3) -lpthread -lm -o scratch/bin/tomba2_port || { echo "[build] link failed" >&2; exit 1; }
echo "[build] linked scratch/bin/tomba2_port"
