# cmake/tomba2_port.cmake — build the native PC port binary `tomba2_port`.
#
# This mirrors tools/build_port.sh / run.sh step 4 (the authoritative shell build): same source list,
# include dirs, flags, and link libraries. It produces scratch/bin/tomba2_port (where the REPL/driver
# tooling and docs expect it), exactly like the shell scripts. The shell build remains the canonical
# path (run.sh also extracts MAIN.EXE from the disc and launches); CMake is the IDE/clangd-friendly
# alternative for compiling the port. KEEP THE SOURCE LIST BELOW IN SYNC with build_port.sh + run.sh.
#
# Enable with -DPSXPORT_BUILD_PORT=ON (default ON). If SDL2/Vulkan/FreeType (pkg-config) are missing,
# the target is skipped with a warning so the lightweight discdump-only configure (run.sh) still works.
#
#   cmake -S . -B build && cmake --build build --target tomba2_port
#   ./scratch/bin/tomba2_port scratch/bin/tomba2/MAIN.EXE      # (after run.sh has extracted MAIN.EXE)

option(PSXPORT_BUILD_PORT "Build the Tomba!2 native port binary (tomba2_port)" ON)
option(PSXPORT_SUBSTRATE  "Link recompiled shards (generated/) instead of interpreter-only" OFF)

if(NOT PSXPORT_BUILD_PORT)
  return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 sdl2)
pkg_check_modules(VULKAN vulkan)
pkg_check_modules(FREETYPE freetype2)
if(NOT (SDL2_FOUND AND VULKAN_FOUND AND FREETYPE_FOUND))
  message(WARNING "tomba2_port skipped: needs pkg-config sdl2 + vulkan + freetype2 "
                  "(found sdl2=${SDL2_FOUND} vulkan=${VULKAN_FOUND} freetype2=${FREETYPE_FOUND})")
  return()
endif()

set(RT runtime/recomp)
set(ENG engine)
set(MED vendor/beetle-psx/mednafen)

# ---- vendored RmlUi (HTML/CSS mod overlay), static, Core + Debugger only ----------------------
set(BUILD_SHARED_LIBS OFF)
set(RMLUI_SAMPLES        OFF CACHE BOOL   "" FORCE)
set(RMLUI_LUA_BINDINGS   OFF CACHE BOOL   "" FORCE)
set(RMLUI_SVG_PLUGIN     OFF CACHE BOOL   "" FORCE)
set(RMLUI_LOTTIE_PLUGIN  OFF CACHE BOOL   "" FORCE)
set(RMLUI_FONT_ENGINE    freetype CACHE STRING "" FORCE)
add_subdirectory(vendor/rmlui EXCLUDE_FROM_ALL)

# ---- generated Vulkan SPIR-V header (runtime/recomp/gpu_vk_shaders.h) --------------------------
# tools/gen_vk_shaders.sh compiles shaders_vk/*.{vert,frag} and embeds the SPIR-V; it writes into the
# source tree (as the shell build does). Re-run it when a shader source changes.
file(GLOB SHADER_SRCS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/${RT}/shaders_vk/*.vert ${CMAKE_SOURCE_DIR}/${RT}/shaders_vk/*.frag)
set(SHADERS_H ${CMAKE_SOURCE_DIR}/${RT}/gpu_vk_shaders.h)
add_custom_command(OUTPUT ${SHADERS_H}
  COMMAND bash ${CMAKE_SOURCE_DIR}/tools/gen_vk_shaders.sh
  DEPENDS ${SHADER_SRCS} ${CMAKE_SOURCE_DIR}/tools/gen_vk_shaders.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Generating Vulkan SPIR-V header (gpu_vk_shaders.h)"
  VERBATIM)
add_custom_target(gen_vk_shaders DEPENDS ${SHADERS_H})

# ---- source list (KEEP IN SYNC with tools/build_port.sh and run.sh) ---------------------------
set(PORT_SRC
  ${RT}/dispatch.cpp ${RT}/cfg.c ${RT}/mem.cpp ${RT}/stubs.cpp ${RT}/hle.cpp ${RT}/threads.cpp
  ${RT}/interp.cpp ${RT}/gpu_native.cpp ${RT}/gpu_debug.cpp ${RT}/vram_xfer.cpp ${RT}/spu_audio.c
  ${RT}/pad_input.cpp ${RT}/memcard.cpp ${RT}/native_fmv.cpp
  ${MED}/psx/gte.c ${RT}/gte_beetle.cpp ${MED}/psx/mdec.c ${RT}/mdec_beetle.c ${MED}/psx/spu.c ${RT}/spu_beetle.c
  ${RT}/disc.c ${RT}/cd_override.cpp ${RT}/cdc_native.c ${RT}/xa_stream.c ${RT}/timing.cpp
  ${RT}/gpu_vk.cpp ${RT}/gpu_perf.cpp ${RT}/mods.c
  ${ENG}/game_tomba2.cpp ${ENG}/asset.cpp ${ENG}/mathlib.cpp ${ENG}/cull.cpp ${ENG}/collision.cpp
  ${ENG}/hitbox.cpp ${ENG}/grid_offset.cpp ${ENG}/entity.cpp ${ENG}/entity_spawn.cpp
  ${ENG}/actor_sm_24448.cpp ${ENG}/objbeh_739ac.cpp ${ENG}/objbeh_73cd8.cpp ${ENG}/objbeh_741dc.cpp
  ${ENG}/script.cpp ${ENG}/animation.cpp ${ENG}/input.cpp ${ENG}/menu.cpp ${ENG}/inventory.cpp
  ${ENG}/hud.cpp ${ENG}/lighting.cpp ${ENG}/engine_bav.cpp ${ENG}/save.cpp ${ENG}/sound.cpp
  ${ENG}/engine_init.cpp ${ENG}/engine_font.cpp ${ENG}/engine_level.cpp ${ENG}/fps60.cpp
  ${ENG}/engine_tomba2.cpp ${ENG}/engine_submit.cpp ${ENG}/engine_stage.cpp ${ENG}/engine_demo.cpp
  ${ENG}/engine_camera.cpp ${ENG}/engine_math.cpp ${ENG}/engine_player.cpp ${ENG}/native_terrain.cpp
  ${ENG}/render_queue.cpp ${ENG}/clib.cpp ${ENG}/gte.cpp ${ENG}/gpu_lib.cpp ${ENG}/sound_voice.cpp
  ${ENG}/object_init.cpp ${ENG}/native_misc.cpp ${RT}/peripheral_misc.cpp ${ENG}/margin_render.cpp
  ${RT}/sync_overrides.cpp ${RT}/native_boot.cpp ${RT}/dbg_server.cpp ${RT}/native_stub.cpp
  ${RT}/watchdog.c ${RT}/boot.cpp
  ${RT}/imgui_overlay.cpp ${RT}/overlay_glue.cpp ${RT}/rmlui_render_vk.cpp
  vendor/rmlui/Backends/RmlUi_Platform_SDL.cpp)

# PSXPORT_SUBSTRATE: link the statically-recompiled shards (C++ content in .c files) instead of the
# interpreter-only default. Compile them as C++ (the shell build uses `$CXX -x c++`).
if(PSXPORT_SUBSTRATE)
  list(APPEND PORT_SRC generated/shard_disp.c generated/shard_0.c generated/shard_1.c generated/shard_2.c
                       generated/shard_3.c generated/shard_4.c generated/shard_5.c generated/shard_6.c
                       generated/shard_7.c)
  set_source_files_properties(
    generated/shard_disp.c generated/shard_0.c generated/shard_1.c generated/shard_2.c
    generated/shard_3.c generated/shard_4.c generated/shard_5.c generated/shard_6.c generated/shard_7.c
    PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-O1")
endif()

add_executable(tomba2_port ${PORT_SRC} ${SHADERS_H})
add_dependencies(tomba2_port gen_vk_shaders)

# C++17 + -fpermissive for this target (mednafen/engine), overriding the project-wide C++20.
set_target_properties(tomba2_port PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
  ENABLE_EXPORTS ON                                   # -rdynamic: watchdog backtrace symbol names
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/scratch/bin)

target_include_directories(tomba2_port PRIVATE
  ${RT} ${ENG} ${MED} ${MED}/psx
  vendor/beetle-psx/libretro-common/include vendor/beetle-psx
  vendor/beetle-psx/deps/libchdr/include
  vendor/rmlui/Include vendor/rmlui/Backends
  ${SDL2_INCLUDE_DIRS} ${VULKAN_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS})

target_compile_definitions(tomba2_port PRIVATE
  PSXPORT_SDL _XOPEN_SOURCE=700 RMLUI_STATIC_LIB RMLUI_SDL_VERSION_MAJOR=2
  $<$<BOOL:${PSXPORT_SUBSTRATE}>:PSXPORT_SUBSTRATE>)

# -w (warnings off) and -O2 -g, matching the shell build; -fpermissive only for C++.
# clang (macOS) has no -fpermissive and makes C++11 braced-init narrowing a hard ERROR (e.g. the
# provably-safe int16_t->float in engine_project.cpp matrix inits, and the generated shards which
# narrow and can't be hand-edited). -Wno-c++11-narrowing is clang's diagnostic; -Wno-narrowing is GCC's;
# each is harmlessly ignored by the other. Keeps the CMake (macOS) build in step with build_port.sh/run.sh.
target_compile_options(tomba2_port PRIVATE -w -O2 -g
  $<$<COMPILE_LANGUAGE:CXX>:-fpermissive -Wno-c++11-narrowing -Wno-narrowing>
  ${SDL2_CFLAGS_OTHER} ${VULKAN_CFLAGS_OTHER} ${FREETYPE_CFLAGS_OTHER})

target_link_libraries(tomba2_port PRIVATE
  rmlui_debugger rmlui_core chdr-static
  ${SDL2_LIBRARIES} ${VULKAN_LIBRARIES} ${FREETYPE_LIBRARIES}
  Threads::Threads m)
target_link_directories(tomba2_port PRIVATE
  ${SDL2_LIBRARY_DIRS} ${VULKAN_LIBRARY_DIRS} ${FREETYPE_LIBRARY_DIRS})
