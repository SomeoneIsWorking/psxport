# cmake/psxport.cmake — the PSX-generic framework as a STANDALONE STATIC library `psxport`.
#
# This is the P1.7 "prove the framework is game-agnostic" gate + groundwork for the eventual repo
# split. It factors ALL of runtime/recomp/** (+ the vendored Beetle GTE/MDEC/SPU backends + the RmlUi
# SDL backend + the generated SDL_GPU shader header) into ONE static archive that contains NO game/
# and NO generated/ source. The game binary (tomba2_port) then links it (see tomba2_port.cmake).
#
# HONEST CAVEAT (found while building this gate, 2026-07-17): the framework is NOT yet header-clean.
# The framework class `Game` (runtime/recomp/game.h) embeds FIVE game classes BY VALUE —
#   Fps60 (game/render), RenderQueue (game/render), PcScheduler (game/core),
#   VerifyHarness (game/core), FfSpan (game/render)
# and #includes their game headers (game.h lines 30-42). Because ~30 framework .cpp include game.h
# (e.g. mem.cpp), the framework CANNOT COMPILE without game/core + game/render on its include path.
# Those two dirs are therefore added below as PRIVATE with this note; the "zero game headers" premise
# of the gate is FALSE at the header level. The deeper question the smoke answers is whether the
# archive nonetheless LINKS standalone for a Core-only client (link-level agnosticism). See the smoke
# target + docs; do NOT paper over the header leak — the real fix is to move those 5 classes (or their
# framework-facing base interfaces) framework-side / behind the GameHooks seam.

option(PSXPORT_BUILD_SMOKE "Build headless game-agnostic framework smoke (psxport_smoke)" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 sdl3)
pkg_check_modules(SDL3_IMAGE sdl3-image)
pkg_check_modules(FREETYPE freetype2)
if(NOT (SDL3_FOUND AND SDL3_IMAGE_FOUND AND FREETYPE_FOUND))
  # Hard stop with the fix, not a skip: a skipped target surfaces later as make's baffling
  # "No rule to make target" instead of naming the missing library.
  set(_missing "")
  if(NOT SDL3_FOUND)
    string(APPEND _missing "  sdl3        — Fedora: SDL3-devel | Debian/Ubuntu: libsdl3-dev | macOS: brew install sdl3\n")
  endif()
  if(NOT SDL3_IMAGE_FOUND)
    string(APPEND _missing "  sdl3-image  — Fedora: SDL3_image-devel | Debian/Ubuntu: libsdl3-image-dev | macOS: brew install sdl3_image\n")
  endif()
  if(NOT FREETYPE_FOUND)
    string(APPEND _missing "  freetype2   — Fedora: freetype-devel | Debian/Ubuntu: libfreetype-dev | macOS: brew install freetype\n")
  endif()
  message(FATAL_ERROR "psxport: missing pkg-config dependencies:\n${_missing}"
                      "Install the package(s) above and re-run ./run.sh")
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

# ---- framework source list (PSX-generic; NO game/*, NO generated/*) ---------------------------
# All of runtime/recomp/** + the vendored Beetle GTE/MDEC/SPU C backends + the RmlUi SDL backend.
set(PSXPORT_FRAMEWORK_SRC
  runtime/recomp/core.cpp
  runtime/recomp/game_iface.cpp       # framework↔game seam storage (GameConfig/GameHooks install)
  runtime/recomp/dispatch.cpp
  runtime/recomp/interp.cpp           # ORACLE engine: pure-MIPS interpreter for the oracle Core
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
  runtime/recomp/spu_audio.cpp
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
  runtime/recomp/disc_provision.cpp
  runtime/recomp/cd_override.cpp
  runtime/recomp/cdc_native.c
  runtime/recomp/xa_stream.c
  runtime/recomp/timing.cpp
  runtime/recomp/gpu_gpu.cpp
  runtime/recomp/gpu_perf.cpp
  runtime/recomp/mods.cpp
  runtime/recomp/native_gate.cpp
  runtime/recomp/sync_overrides.cpp
  runtime/recomp/override_registry.cpp
  runtime/recomp/scheduler.cpp
  runtime/recomp/native_boot.cpp
  runtime/recomp/dualview_snapshot.cpp
  runtime/recomp/proj_prim.cpp
  runtime/recomp/pgxp.cpp
  runtime/recomp/proj_params.cpp
  runtime/recomp/pkt_span.cpp
  runtime/recomp/ot_attr.cpp
  runtime/recomp/hw_bind.cpp
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

add_library(psxport STATIC ${PSXPORT_FRAMEWORK_SRC} ${SHADERS_H})
add_dependencies(psxport gen_gpu_shaders)

# C++17 for the framework (mednafen backends), overriding the project-wide C++20.
set_target_properties(psxport PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Framework include dirs. RT + generated + the vendored backends are PUBLIC so consumers inherit them.
# generated/ is PUBLIC because framework sources include generated headers (overlay_router.cpp ->
# overlay_table.h; gpu_gpu.cpp/rmlui_render_gpu.cpp -> gpu_gpu_shaders.h which is in RT). The symbols
# those generated headers declare (main_dispatch, g_rec_overlays, rec_func_index) are DEFINED in
# generated/ sources (the game substrate) — so the archive carries them as UNDEFINED, resolved only at
# the final game-exe link. That is expected for a static archive.
target_include_directories(psxport PUBLIC
  ${RT} ${CMAKE_SOURCE_DIR}/generated
  ${MED} ${MED}/psx
  vendor/beetle-psx/libretro-common/include vendor/beetle-psx
  vendor/beetle-psx/deps/libchdr/include
  vendor/rmlui/Include vendor/rmlui/Backends
  ${SDL3_INCLUDE_DIRS} ${SDL3_IMAGE_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS})
# PRIVATE game include dirs — FORCED by the header leak documented at the top of this file (game.h
# embeds Fps60/RenderQueue/PcScheduler/VerifyHarness/FfSpan by value). ONLY these two dirs, and ONLY
# because the framework does not yet own those five classes. Removing these two lines is the litmus for
# "framework is header-clean"; today it fails to compile without them.
target_include_directories(psxport PRIVATE game/core game/render)

target_compile_definitions(psxport PUBLIC
  PSXPORT_SDL _XOPEN_SOURCE=700 RMLUI_STATIC_LIB RMLUI_SDL_VERSION_MAJOR=3)

target_compile_options(psxport PRIVATE -w -O2 -g
  ${SDL3_CFLAGS_OTHER} ${FREETYPE_CFLAGS_OTHER})

# Link deps PUBLIC/INTERFACE so any consumer (the game exe, the smoke) inherits them.
target_link_libraries(psxport PUBLIC
  rmlui_debugger rmlui_core chdr-static
  ${SDL3_LIBRARIES} ${SDL3_IMAGE_LIBRARIES} ${FREETYPE_LIBRARIES}
  Threads::Threads m)
target_link_directories(psxport PUBLIC
  ${SDL3_LIBRARY_DIRS} ${SDL3_IMAGE_LIBRARY_DIRS} ${FREETYPE_LIBRARY_DIRS})

# ---- headless game-agnostic smoke (PSXPORT_BUILD_SMOKE=ON) ------------------------------------
# Links ONLY libpsxport.a (+ its inherited system deps) — NO game/, NO generated/. Any undefined
# game/generated symbol pulled in by the Core-only client fails THIS link, which is exactly the proof
# (or disproof) of link-level agnosticism. See tools/smoke/psxport_smoke.cpp.
if(PSXPORT_BUILD_SMOKE)
  add_executable(psxport_smoke tools/smoke/psxport_smoke.cpp)
  set_target_properties(psxport_smoke PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/scratch/bin)
  # The smoke includes framework headers (core.h etc.) — inherits RT/generated via the PUBLIC iface.
  target_link_libraries(psxport_smoke PRIVATE psxport)
endif()
