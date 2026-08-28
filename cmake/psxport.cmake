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

if(TARGET psxport)
  return()  # already built (idempotent include from both the root and the game cmake)
endif()
set(PSXPORT_CMAKE_INCLUDED ON)
get_filename_component(PSXPORT_ROOT ${CMAKE_CURRENT_LIST_DIR} DIRECTORY)  # <psxport> repo root
set(RT runtime/recomp)
set(MED ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen)

# ---- vendored RmlUi (HTML/CSS mod overlay), static, Core + Debugger only ----------------------
set(BUILD_SHARED_LIBS OFF)
set(RMLUI_SAMPLES        OFF CACHE BOOL   "" FORCE)
set(RMLUI_LUA_BINDINGS   OFF CACHE BOOL   "" FORCE)
set(RMLUI_SVG_PLUGIN     OFF CACHE BOOL   "" FORCE)
set(RMLUI_LOTTIE_PLUGIN  OFF CACHE BOOL   "" FORCE)
set(RMLUI_FONT_ENGINE    freetype CACHE STRING "" FORCE)
add_subdirectory(${PSXPORT_ROOT}/vendor/rmlui ${CMAKE_BINARY_DIR}/rmlui_build EXCLUDE_FROM_ALL)

# ---- generated SDL_GPU SPIR-V header ----------------------------------------------------------
# tools/gen_gpu_shaders.py compiles shaders_gpu/*.{vert,frag} (incl. the RmlUi overlay shaders) and
# embeds the SPIR-V into this consumer's build tree. Shared *.glsl includes are dependencies, not
# entry points. A source-tree output is incorrect: independent consumer builds would all register the
# same ignored file as their BYPRODUCT, so cleaning one build could delete another build's input.
include(${PSXPORT_ROOT}/cmake/gpu_shaders.cmake)

# ---- framework source list (PSX-generic; NO game/*, NO generated/*) ---------------------------
# All of runtime/recomp/** + the vendored Beetle GTE/MDEC/SPU C backends + the RmlUi SDL backend.
set(PSXPORT_FRAMEWORK_SRC
  ${PSXPORT_ROOT}/runtime/recomp/core.cpp
  ${PSXPORT_ROOT}/runtime/recomp/game.cpp
  ${PSXPORT_ROOT}/runtime/recomp/game_runtime.cpp
  ${PSXPORT_ROOT}/runtime/recomp/frame_loop_shell.cpp
  ${PSXPORT_ROOT}/runtime/recomp/frame_presenter.cpp
  ${PSXPORT_ROOT}/runtime/recomp/guest_widescreen_projection.cpp
  ${PSXPORT_ROOT}/runtime/recomp/fs_util.cpp          # generic std::filesystem host-I/O wrapper (class Fs), no game types
  ${PSXPORT_ROOT}/runtime/recomp/game_iface.cpp       # polymorphic GameRuntime install + bounded legacy adapter
  ${PSXPORT_ROOT}/runtime/recomp/recomp_iface.cpp     # framework↔generated seam storage (RecompRegistry install)
  ${PSXPORT_ROOT}/runtime/recomp/dispatch.cpp
  ${PSXPORT_ROOT}/runtime/recomp/interp.cpp           # ORACLE engine: pure-MIPS interpreter for the oracle Core
  ${PSXPORT_ROOT}/runtime/recomp/interp_diagnostics.cpp # interpreter trace and native-call diagnostics
  ${PSXPORT_ROOT}/runtime/recomp/coro.cpp
  ${PSXPORT_ROOT}/runtime/recomp/overlay_router.cpp
  ${PSXPORT_ROOT}/runtime/recomp/cfg.cpp
  ${PSXPORT_ROOT}/runtime/recomp/memcensus.cpp      # --wrap=memcpy call-site attribution (PSXPORT_MEMCENSUS)
  ${PSXPORT_ROOT}/runtime/recomp/mem.cpp
  ${PSXPORT_ROOT}/runtime/recomp/dma_callbacks.cpp # direct-runtime per-Game DMACallback registration state
  ${PSXPORT_ROOT}/runtime/recomp/cpu_divide.cpp     # R3000 DIV/DIVU quotient/remainder semantics
  ${PSXPORT_ROOT}/runtime/recomp/stubs.cpp
  ${PSXPORT_ROOT}/runtime/recomp/bios_interrupt.cpp
  ${PSXPORT_ROOT}/runtime/recomp/bios_libc_string.cpp
  ${PSXPORT_ROOT}/runtime/recomp/hle.cpp
  ${PSXPORT_ROOT}/runtime/recomp/syscall_exception.cpp
  ${PSXPORT_ROOT}/runtime/recomp/host_turn.cpp
  ${PSXPORT_ROOT}/runtime/recomp/threads.cpp
  ${PSXPORT_ROOT}/runtime/recomp/gpu_native.cpp
  ${PSXPORT_ROOT}/runtime/recomp/gpu_primitive_dump.cpp # primitive-census CSV diagnostic owner
  ${PSXPORT_ROOT}/runtime/recomp/image_writer.cpp      # one checked RGB24 capture-file boundary
  ${PSXPORT_ROOT}/runtime/recomp/gpu_debug.cpp
  ${PSXPORT_ROOT}/runtime/recomp/vram_xfer.cpp
  ${PSXPORT_ROOT}/runtime/recomp/spu_audio.cpp
  ${PSXPORT_ROOT}/runtime/recomp/audio_field_report.cpp
  ${PSXPORT_ROOT}/runtime/recomp/sbs_audio_compare.cpp
  ${PSXPORT_ROOT}/runtime/recomp/pad_input.cpp
  ${PSXPORT_ROOT}/runtime/recomp/snapshot.cpp
  ${PSXPORT_ROOT}/runtime/recomp/native_diff.cpp
  ${PSXPORT_ROOT}/runtime/recomp/hostprof.cpp
  ${PSXPORT_ROOT}/runtime/recomp/fntrace.cpp
  ${PSXPORT_ROOT}/runtime/recomp/memcard.cpp
  ${PSXPORT_ROOT}/runtime/recomp/native_fmv.cpp
  ${PSXPORT_ROOT}/runtime/recomp/fmv_decode.cpp   # pure .STR decode (VLC/MDEC/XA), shared by the player + offline tools
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gte.c
  ${PSXPORT_ROOT}/runtime/recomp/gte_beetle.cpp
  # The REAL PSX GPU, as an independent oracle for psx_render (runtime/recomp/gpu_beetle.cpp explains
  # why our own rasterizer could not be one). gpu.c #includes gpu_polygon.c / gpu_sprite.c /
  # gpu_line.c, so those are NOT listed; gpu_polygon_sub.c and rhi/rhi_intf.c are separate units.
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gpu.c
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gpu_polygon_sub.c
  ${PSXPORT_ROOT}/vendor/beetle-psx/rhi/rhi_intf.c
  ${PSXPORT_ROOT}/runtime/recomp/gpu_beetle.cpp
  ${PSXPORT_ROOT}/runtime/recomp/native_projection.cpp
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/mdec.c
  ${PSXPORT_ROOT}/runtime/recomp/mdec_beetle.c
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/spu.c
  ${PSXPORT_ROOT}/runtime/recomp/spu_beetle.cpp
  ${PSXPORT_ROOT}/runtime/recomp/disc.cpp
  ${PSXPORT_ROOT}/runtime/recomp/disc_provision.cpp
  ${PSXPORT_ROOT}/runtime/recomp/cd_override.cpp
  ${PSXPORT_ROOT}/runtime/recomp/cd_drive_timing.cpp
  ${PSXPORT_ROOT}/runtime/recomp/cdc_command_phase.cpp
  ${PSXPORT_ROOT}/runtime/recomp/cdc_native.cpp
  ${PSXPORT_ROOT}/runtime/recomp/xa_stream.cpp
  ${PSXPORT_ROOT}/runtime/recomp/emulated_time.cpp
  ${PSXPORT_ROOT}/runtime/recomp/frame_pacer.cpp
  ${PSXPORT_ROOT}/runtime/recomp/timing.cpp
  ${PSXPORT_ROOT}/runtime/recomp/gpu_vk.cpp
  ${PSXPORT_ROOT}/runtime/recomp/gpu_vk_depth.cpp # normalized 3D depth and per-primitive order policy
  ${PSXPORT_ROOT}/runtime/recomp/gpu_painter.cpp # painter target lifecycle + authored command staging
  ${PSXPORT_ROOT}/runtime/recomp/gpu_vk_semi_selftest.cpp
  ${PSXPORT_ROOT}/runtime/recomp/gpu_vk_texture_phase_selftest.cpp
  ${PSXPORT_ROOT}/runtime/recomp/gpu_perf.cpp
  ${PSXPORT_ROOT}/runtime/recomp/mods.cpp
  ${PSXPORT_ROOT}/runtime/recomp/config.cpp   # layered CVar registry + the environment audit (docs/config.md)
  ${PSXPORT_ROOT}/runtime/recomp/native_gate.cpp
  ${PSXPORT_ROOT}/runtime/recomp/sync_overrides.cpp
  ${PSXPORT_ROOT}/runtime/recomp/override_registry.cpp
  ${PSXPORT_ROOT}/runtime/recomp/scheduler.cpp
  ${PSXPORT_ROOT}/runtime/recomp/native_boot.cpp
  ${PSXPORT_ROOT}/runtime/recomp/render_path.cpp   # render_path_install — the render-path tri-state, one parser for every boot spine
  ${PSXPORT_ROOT}/runtime/recomp/dualview_snapshot.cpp
  ${PSXPORT_ROOT}/runtime/recomp/proj_prim.cpp
  ${PSXPORT_ROOT}/runtime/recomp/pgxp.cpp
  ${PSXPORT_ROOT}/runtime/recomp/proj_params.cpp
  ${PSXPORT_ROOT}/runtime/recomp/ot_attr.cpp
  ${PSXPORT_ROOT}/runtime/recomp/producer_db.cpp
  ${PSXPORT_ROOT}/runtime/recomp/hw_bind.cpp
  ${PSXPORT_ROOT}/runtime/recomp/repl.cpp
  ${PSXPORT_ROOT}/runtime/recomp/repl_service.cpp   # "is anybody going to READ this stdin?" — the refusal guard for loops with no REPL pump
  ${PSXPORT_ROOT}/runtime/recomp/dbg_server.cpp
  ${PSXPORT_ROOT}/runtime/recomp/native_stub.cpp
  ${PSXPORT_ROOT}/runtime/recomp/watchdog.cpp
  ${PSXPORT_ROOT}/runtime/recomp/dualcore.cpp
  ${PSXPORT_ROOT}/runtime/recomp/sbs.cpp
  ${PSXPORT_ROOT}/runtime/recomp/sbs_present_sdl.cpp
  ${PSXPORT_ROOT}/runtime/recomp/selftest.cpp
  ${PSXPORT_ROOT}/runtime/recomp/boot.cpp
  ${PSXPORT_ROOT}/runtime/recomp/rmlui_overlay.cpp  # RmlUi LIFETIME only — the UI itself is runtime/ui/
  ${PSXPORT_ROOT}/runtime/recomp/rmlui_render_gpu.cpp
  ${PSXPORT_ROOT}/runtime/recomp/rml_text.cpp       # DATA -> RML markup boundary (see rml_text.h)
  # ---- runtime/ui: the overlay's COMPONENT tree (Dusklight src/dusk/ui/ shape, CC0). One file
  # pair per component; ui_component.h documents what was taken and where ours deliberately differs.
  ${PSXPORT_ROOT}/runtime/ui/ui_event.cpp           # ScopedEventListener — registration IS lifetime
  ${PSXPORT_ROOT}/runtime/ui/ui_component.cpp       # Component base + the ONE data->DOM text boundary
  ${PSXPORT_ROOT}/runtime/ui/ui_assets.cpp          # asset resolution that REFUSES to report success
  ${PSXPORT_ROOT}/runtime/ui/mod_row_model.cpp      # what a menu row MEANS (Mods toggle/adjust tables)
  ${PSXPORT_ROOT}/runtime/ui/warp_control.cpp       # the Debug tab's dev area warp
  ${PSXPORT_ROOT}/runtime/ui/render_path_control.cpp # the Display tab's live renderer selector
  ${PSXPORT_ROOT}/runtime/ui/menu_row.cpp           # one <select-button> + its binding
  ${PSXPORT_ROOT}/runtime/ui/menu_pane.cpp          # one tab's page of rows
  ${PSXPORT_ROOT}/runtime/ui/menu_tab_bar.cpp       # the <tab> row and which one is selected
  ${PSXPORT_ROOT}/runtime/ui/menu_readouts.cpp      # the live video/world/music/warp status lines
  ${PSXPORT_ROOT}/runtime/ui/menu_document.cpp      # the tree over assets/rml/menu.rml
  ${PSXPORT_ROOT}/runtime/recomp/game_hooks_opt.cpp
  ${PSXPORT_ROOT}/runtime/recomp/overlay_glue.cpp
  ${PSXPORT_ROOT}/runtime/recomp/fps60_game_hooks.cpp  # guarded callbacks used only by temporal presentation
  ${PSXPORT_ROOT}/runtime/recomp/fps60_gpu_present.cpp # renderer pass used only by temporal presentation
  ${PSXPORT_ROOT}/runtime/recomp/fps60.cpp            # interpolated-60fps lerp tier (framework render-infra; P1.7c)
  ${PSXPORT_ROOT}/runtime/recomp/ot_lifo_depth.cpp    # PSX AddPrim head-insertion ties -> raster-distinct native depths
  ${PSXPORT_ROOT}/runtime/recomp/render_queue.cpp     # engine-owned draw-ORDER authority (P1.7c)
  ${PSXPORT_ROOT}/runtime/recomp/painter_object_layer.cpp # atomic painter admission + authored command plan
  ${PSXPORT_ROOT}/runtime/recomp/pc_scheduler.cpp     # PC-native cooperative task scheduler; stage bodies via hooks (P1.7c)
  ${PSXPORT_ROOT}/runtime/recomp/synchronous_task_wait.cpp # one native synchronous FUN_80044BD4 owner
  ${PSXPORT_ROOT}/runtime/recomp/verify_harness.cpp   # A/B verify scaffold (skip/observable half split to game verify_skip.cpp) (P1.7c)
  ${PSXPORT_ROOT}/vendor/rmlui/Backends/RmlUi_Platform_SDL.cpp)

add_library(psxport STATIC ${PSXPORT_FRAMEWORK_SRC})
psxport_add_gpu_shaders(${PSXPORT_ROOT} psxport)

# ---- BUILD IDENTITY (kanban #91 step 2) -------------------------------------------------------
# `git describe --always --dirty` for the framework AND for the consuming project, written into
# ${CMAKE_BINARY_DIR}/psxport_build_id.h. This is the framework's ONLY build stamp; nothing else here
# emitted GIT_*/BUILD_ID/__DATE__, which is why the producer claim file could not say which build
# earned a claim.
#
# A CUSTOM TARGET IN `ALL`, NOT A CONFIGURE-TIME VARIABLE, and the difference is the whole point: a
# configure-time id goes stale the moment you commit and rebuild without re-running cmake, and a stale
# provenance stamp files new work under the old sha — the exact false negative kanban #91 is about. The
# generator writes only when the content CHANGES, so the cost is one `git describe` per build and no
# spurious recompilation. It lives in cmake/psxport.cmake rather than the root CMakeLists because a
# consuming GAME includes this file directly and never processes psxport's own CMakeLists — put in the
# root, every game build would have had no build id at all.
set(PSXPORT_BUILD_ID_H ${CMAKE_BINARY_DIR}/psxport_build_id.h)
add_custom_target(psxport_build_id ALL
  COMMAND ${CMAKE_COMMAND}
          -DPSXPORT_ROOT=${PSXPORT_ROOT}
          -DAPP_ROOT=${CMAKE_SOURCE_DIR}
          -DOUT=${PSXPORT_BUILD_ID_H}
          -P ${PSXPORT_ROOT}/cmake/build_id.cmake
  BYPRODUCTS ${PSXPORT_BUILD_ID_H}
  COMMENT "psxport: stamping build identity (git describe --always --dirty)"
  VERBATIM)
add_dependencies(psxport psxport_build_id)

# C++17 for the framework (mednafen backends), overriding the project-wide C++20.
set_target_properties(psxport PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Framework include dirs. RT + generated + the vendored backends are PUBLIC so consumers inherit them.
# generated/ is PUBLIC because framework sources include game-generated headers (overlay_router.cpp ->
# overlay_table.h). The shader module separately attaches its exact build-owned include directory to
# psxport; it may differ from ${CMAKE_BINARY_DIR} when a consumer uses add_subdirectory. The symbols
# those generated headers declare (main_dispatch, g_rec_overlays, rec_func_index) are DEFINED in
# generated/ sources (the game substrate) — so the archive carries them as UNDEFINED, resolved only at
# the final game-exe link. That is expected for a static archive.
target_include_directories(psxport PUBLIC
  # ${CMAKE_BINARY_DIR} is here for ONE header: the generated psxport_build_id.h above. It is PUBLIC
  # because a game's own translation units are entitled to the same identity the framework stamps.
  ${CMAKE_BINARY_DIR}
  ${PSXPORT_ROOT}/${RT} ${PSXPORT_ROOT}/runtime/ui ${CMAKE_SOURCE_DIR}/generated
  ${MED} ${MED}/psx
  ${PSXPORT_ROOT}/vendor/beetle-psx/libretro-common/include ${PSXPORT_ROOT}/vendor/beetle-psx
  ${PSXPORT_ROOT}/vendor/beetle-psx/deps/libchdr/include
  ${PSXPORT_ROOT}/vendor/rmlui/Include ${PSXPORT_ROOT}/vendor/rmlui/Backends
  ${SDL3_INCLUDE_DIRS} ${SDL3_IMAGE_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS})
# NO game include dirs: as of P1.7c the framework owns Fps60/RenderQueue/PcScheduler/VerifyHarness/FfSpan
# (their headers moved to runtime/recomp/; the game reaches into them only through the GameHooks seam). The
# framework #includes no game/ header — the psxport_smoke link proves it (no game/** symbol resolves).

target_compile_definitions(psxport PUBLIC
  PSXPORT_SDL _XOPEN_SOURCE=700 RMLUI_STATIC_LIB RMLUI_SDL_VERSION_MAJOR=3)

target_compile_options(psxport PRIVATE -w -O2 -g
  ${SDL3_CFLAGS_OTHER} ${FREETYPE_CFLAGS_OTHER})

# WHICH CALL SITES MOVE THE BYTES (runtime/recomp/memcensus.cpp). --wrap is a LINK-time rewrite, and
# it must be on the final executable's link rather than this static library's, so it is INTERFACE:
# every consumer (the game exe, psxport_smoke) inherits it and every first-party memcpy/memmove call
# routes through __wrap_*. Always linked, never always counting — the wrapper is one relaxed load and
# a not-taken branch unless PSXPORT_MEMCENSUS=1, so an ordinary run is unaffected.
#
# ON by default because an instrument nobody can switch on without recompiling is an instrument
# nobody uses; set PSXPORT_WRAP_MEMCPY=OFF to link without it and prove the wrapper itself costs
# nothing (an A/B this option exists to make possible).
option(PSXPORT_WRAP_MEMCPY "Route first-party memcpy/memmove through the call-site census" ON)
if(PSXPORT_WRAP_MEMCPY)
  target_link_options(psxport INTERFACE -Wl,--wrap=memcpy -Wl,--wrap=memmove)
endif()

# Link deps PUBLIC/INTERFACE so any consumer (the game exe, the smoke) inherits them.
# lucent — logging + configuration (https://github.com/SomeoneIsWorking/lucent). The cfg_* API in
# runtime/recomp/cfg.cpp is a thin shim over it, so every diagnostic in the port shares one output
# path. Vendored as a submodule (vendor/lucent) alongside beetle-psx/rmlui so the framework and its
# offline tools build hermetically — no configure-time network fetch, and tools can compile lucent's
# sources directly (see tools/fmv_export/build.sh). add_subdirectory installs the `lucent` target and
# its `lucent::lucent` alias; lucent's own tests are gated on its CMAKE_SOURCE_DIR and never build
# here.
#
# THE TWO NAMES OUR USERS ACTUALLY TYPE. lucent's own variables are LUCENT_DEBUG / LUCENT_LOG_FILE;
# every script, doc, .env and agent brief across the four repos says PSXPORT_DEBUG / PSXPORT_LOG_FILE,
# so lucent is BUILT to read those. It is a build-time setting and not an init call on purpose:
# lucent resolves both lazily on the first log call, so with the name baked in there is nothing to
# initialise and therefore no initialisation that can fail to run.
#
# That last point is not theoretical. These two variables used to be loaded by bootstrap_once() in
# runtime/recomp/cfg.cpp, reachable ONLY from a cfg_* entry point — so a plain lucent::debug() did not
# load them, and the whole thing worked purely because ~700 legacy cfg_log* sites fire during boot.
# Finishing the cfg_* retirement would have switched every diagnostic channel in four repos off,
# silently. tests/test_lucent_channel_env.cpp is the gate that keeps it defused.
set(LUCENT_CHANNEL_ENV  "PSXPORT_DEBUG")
set(LUCENT_LOG_FILE_ENV "PSXPORT_LOG_FILE")
# EXPLICIT BINARY DIR, like rmlui's above (line 58). add_subdirectory with a source dir OUTSIDE the
# consuming project's tree is an error unless one is given — so without it the framework could only be
# built from a PSXPORT_ROOT nested under the consumer, and `-DPSXPORT_DIR=<a psxport clone elsewhere>`
# died here with "binary_dir must be specified". That is the whole point of the workspace's framework
# dev clone, so the path has to be honest about being relocatable.
add_subdirectory(${PSXPORT_ROOT}/vendor/lucent ${CMAKE_BINARY_DIR}/lucent_build)

target_link_libraries(psxport PUBLIC
  lucent::lucent
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

# The vendored GPU sources use sscanf/fprintf/stderr without including <stdio.h>. Supply it on the
# compile line rather than editing the fork — psxport's rule is that beetle changes live in the
# committed fork, and this is a build-flag concern, not a source defect worth forking over.
set_source_files_properties(
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gpu.c
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gpu_polygon_sub.c
  PROPERTIES COMPILE_OPTIONS "-include;stdio.h")
