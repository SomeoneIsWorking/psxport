# cmake/psxport.cmake — the PSX-generic native/dynarec framework as `psxport`.
#
# The library contains the game-agnostic PSX machine, native services, dynarec integration boundary,
# presentation owners, and vendored hardware backends. Title code and restricted game data remain in
# the consumer. Build-generated shader and identity headers belong to the consumer's build tree.

option(PSXPORT_BUILD_SMOKE "Build headless game-agnostic framework smoke (psxport_smoke)" OFF)

include("${CMAKE_CURRENT_LIST_DIR}/psxport_presentation_dependencies.cmake")
psxport_configure_presentation_dependencies()

if(TARGET psxport)
  return()  # already built (idempotent include from both the root and the game cmake)
endif()
set(PSXPORT_CMAKE_INCLUDED ON)
get_filename_component(PSXPORT_ROOT ${CMAKE_CURRENT_LIST_DIR} DIRECTORY)  # <psxport> repo root
set(RT runtime/psx)
set(MED ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen)

include("${PSXPORT_ROOT}/cmake/lightrec_dependency.cmake")
psxport_configure_lightrec_dependency()

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

# ---- framework source list (PSX-generic; no consumer title sources) ----------------------------
# All of runtime/psx/** + the vendored Beetle GTE/MDEC/SPU C backends + the RmlUi SDL backend.
set(PSXPORT_FRAMEWORK_SRC
  ${PSXPORT_ROOT}/runtime/psx/core/core.cpp
  ${PSXPORT_ROOT}/runtime/psx/game/game.cpp
  ${PSXPORT_ROOT}/runtime/psx/game/game_runtime.cpp
  ${PSXPORT_ROOT}/runtime/cpu/execution_exit.cpp
  ${PSXPORT_ROOT}/runtime/cpu/execution_ledger.cpp
  ${PSXPORT_ROOT}/runtime/cpu/execution_control.cpp
  ${PSXPORT_ROOT}/runtime/cpu/execution_services.cpp
  ${PSXPORT_ROOT}/runtime/cpu/function_reach.cpp # PSXPORT_REACH_REPORT: guest entry pcs reached, by image
  ${PSXPORT_ROOT}/runtime/cpu/host_dispatch_cache.cpp # per-address host-dispatch verdicts, flushed with their inputs
  ${PSXPORT_ROOT}/runtime/cpu/image_identity.cpp
  ${PSXPORT_ROOT}/runtime/cpu/guest_call.cpp # dispatchGuest{0..4}, callGuestNow — the finite single-turn call
  ${PSXPORT_ROOT}/runtime/cpu/guest_call_census.cpp # per-Core tally of calls that crossed a host turn
  ${PSXPORT_ROOT}/runtime/cpu/resumable_guest_call.cpp # one guest call across display fields: latch, cap, classify
  ${PSXPORT_ROOT}/runtime/cpu/invalidation.cpp
  ${PSXPORT_ROOT}/runtime/cpu/lightrec_executor.cpp
  ${PSXPORT_ROOT}/runtime/cpu/machine_snapshot.cpp # CPU/GTE/RAM capture and invalidating restore
  ${PSXPORT_ROOT}/runtime/cpu/native_dispatch.cpp
  ${PSXPORT_ROOT}/runtime/cpu/override_differential.cpp # per-function override differential (shadow mode)
  ${PSXPORT_ROOT}/runtime/cpu/override_differential_config.cpp
  ${PSXPORT_ROOT}/runtime/cpu/override_differential_contract.cpp
  ${PSXPORT_ROOT}/runtime/cpu/override_differential_report.cpp
  ${PSXPORT_ROOT}/runtime/cpu/segment_clock.cpp   # per-segment emulated-clock commitment ledger
  ${PSXPORT_ROOT}/runtime/cpu/side_effect_journal.cpp # device/host-service journal for the differential
  ${PSXPORT_ROOT}/runtime/cpu/store_observe.cpp # arms the dynarec store observer from PSXPORT_STORE_OBSERVE
  ${PSXPORT_ROOT}/runtime/psx/frame/frame_loop_shell.cpp
  ${PSXPORT_ROOT}/runtime/psx/frame/frame_presenter.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/guest_widescreen_projection.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/fs_util.cpp          # generic std::filesystem host-I/O wrapper (class Fs), no game types
  ${PSXPORT_ROOT}/runtime/psx/game/game_iface.cpp       # polymorphic GameRuntime install + bounded legacy adapter
  ${PSXPORT_ROOT}/runtime/psx/platform/coro.cpp
  ${PSXPORT_ROOT}/runtime/psx/config/cfg.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/memcensus.cpp      # --wrap=memcpy call-site attribution (PSXPORT_MEMCENSUS)
  ${PSXPORT_ROOT}/runtime/psx/platform/io_peripherals.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/sio_pad.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/dma_linked_list.cpp
  ${PSXPORT_ROOT}/runtime/psx/core/mem.cpp
  ${PSXPORT_ROOT}/runtime/psx/core/guest_memory.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/dma_callbacks.cpp # direct-runtime per-Game DMACallback registration state
  ${PSXPORT_ROOT}/runtime/psx/cpu/cpu_divide.cpp     # R3000 DIV/DIVU quotient/remainder semantics
  ${PSXPORT_ROOT}/runtime/psx/cpu/cop0.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/bios_interrupt.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/bios_pad_work_area.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/bios_libc_string.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/hle.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/hle_interrupt.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/syscall_exception.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/kernel_syscall.cpp
  ${PSXPORT_ROOT}/runtime/psx/boot/host_turn.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/threads.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_native.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/ordering_table.cpp # the guest DrawOTag chain: node header decoded by field name
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_native_raster.cpp # the software rasterizer: texel sample, blend, dither, triangle walk
  ${PSXPORT_ROOT}/runtime/psx/present/face_contest.cpp      # the face-contest rule: can the depth buffer be trusted to order this pair
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_native_scanout.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/gpu_primitive_dump.cpp # primitive-census CSV diagnostic owner
  ${PSXPORT_ROOT}/runtime/psx/debug/image_writer.cpp      # one checked RGB24 capture-file boundary
  ${PSXPORT_ROOT}/runtime/psx/debug/queue_row_probe.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/prim_color_census.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/gpu_debug.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/vram_xfer.cpp
  ${PSXPORT_ROOT}/runtime/psx/audio/spu_audio.cpp
  ${PSXPORT_ROOT}/runtime/psx/audio/audio_field_report.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/host_input.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/pad_input.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/pad_recording.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/pad_phase_replay.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/pad_record_replay.cpp
  ${PSXPORT_ROOT}/runtime/psx/core/snapshot.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/memcard.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/card_syscall_log.cpp
  ${PSXPORT_ROOT}/runtime/psx/movie/native_fmv.cpp
  ${PSXPORT_ROOT}/runtime/psx/movie/fmv_decode.cpp   # pure .STR decode (VLC/MDEC/XA), shared by the player + offline tools
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gte.c
  ${PSXPORT_ROOT}/runtime/psx/cpu/gte_beetle.cpp
  # The REAL PSX GPU, as an independent oracle for psx_render (runtime/psx/gpu/gpu_beetle.cpp explains
  # why our own rasterizer could not be one). gpu.c #includes gpu_polygon.c / gpu_sprite.c /
  # gpu_line.c, so those are NOT listed; gpu_polygon_sub.c and rhi/rhi_intf.c are separate units.
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gpu.c
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gpu_polygon_sub.c
  ${PSXPORT_ROOT}/vendor/beetle-psx/rhi/rhi_intf.c
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_beetle.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/native_projection.cpp
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/mdec.c
  ${PSXPORT_ROOT}/runtime/psx/movie/mdec_beetle.c
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/spu.c
  ${PSXPORT_ROOT}/runtime/psx/audio/spu_beetle.cpp
  # Beetle's own save-state chunk codec (MDFNSS_StateAction / MDFNSS_SaveSM / MDFNSS_LoadSM). It was
  # NOT compiled before, which is why runtime/psx/cpu/gte_vendor_hooks.cpp answered MDFNSS_StateAction
  # with a stub returning 1: the lifted SPU and MDEC call it, and a stub that never reads a byte
  # makes every device save state an empty section a load would accept. psx::state::BeetleDeviceState
  # (runtime/psx/state/beetle_device_state.*) now drives the fork's real SPU/MDEC state actions, and
  # defines the `StateAction` aggregate this unit calls.
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/state.c
  ${PSXPORT_ROOT}/runtime/psx/state/state_blob.cpp
  ${PSXPORT_ROOT}/runtime/psx/state/state_file.cpp
  ${PSXPORT_ROOT}/runtime/psx/state/beetle_device_state.cpp
  ${PSXPORT_ROOT}/runtime/psx/state/machine_state.cpp
  ${PSXPORT_ROOT}/runtime/psx/state/device_cpu.cpp
  ${PSXPORT_ROOT}/runtime/psx/state/device_gpu.cpp
  ${PSXPORT_ROOT}/runtime/psx/state/device_bus.cpp
  ${PSXPORT_ROOT}/runtime/psx/state/state_command.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/disc.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/disc_provision.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/cd_position.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/cd_override.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/cd_ready_delivery.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/cd_stock_read_completion.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/stock_cd_work_area.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/disc_toc.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/stock_cd_response.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/cd_drive_timing.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/cdc_command_phase.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/cdc_native.cpp
  ${PSXPORT_ROOT}/runtime/psx/audio/xa_stream.cpp
  ${PSXPORT_ROOT}/runtime/psx/frame/emulated_time.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/field_digest.cpp
  ${PSXPORT_ROOT}/runtime/psx/frame/frame_pacer.cpp
  ${PSXPORT_ROOT}/runtime/psx/frame/timing.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_device.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_window.cpp # the ONE window + swapchain for the whole product run
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_screen.cpp # present a host screen (title selector) as the whole picture
  ${PSXPORT_ROOT}/runtime/psx/present/pane_composite.cpp # ONE window frame from SEVERAL sessions' presented pictures
  ${PSXPORT_ROOT}/runtime/psx/present/picture_announce.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_fadewatch.cpp
  ${PSXPORT_ROOT}/runtime/psx/present/native_composite_capture.cpp # renderer-private completed-composite retention
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_depth.cpp # normalized 3D depth and per-primitive order policy
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_semi_order.cpp # world semi-transparent submission order
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_painter.cpp # painter target lifecycle + authored command staging
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_texture_coverage_selftest.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_modulation_selftest.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_wide_margin.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_semi_selftest.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_texture_phase_selftest.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_vk_untextured_selftest.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_perf.cpp
  ${PSXPORT_ROOT}/runtime/psx/frame/frame_time_histogram.cpp  # the frame-time distribution gpu_perf reports percentiles from
  ${PSXPORT_ROOT}/runtime/psx/debug/mods.cpp
  ${PSXPORT_ROOT}/runtime/psx/config/config.cpp   # layered CVar registry + the environment audit (docs/config.md)
  ${PSXPORT_ROOT}/runtime/psx/hle/platform_hle.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/scheduler.cpp
  ${PSXPORT_ROOT}/runtime/psx/boot/native_boot.cpp
  ${PSXPORT_ROOT}/runtime/psx/boot/machine.cpp # psx::Machine: the shared boot composition and the frame turn
  ${PSXPORT_ROOT}/runtime/psx/frame/field_turn.cpp # psx::FieldTurn: the per-field services a loop owes
  ${PSXPORT_ROOT}/runtime/psx/boot/crt0_boot.cpp # crt0_setup — the derived crt0 plan's shipping applier
  ${PSXPORT_ROOT}/runtime/psx/gpu/render_path.cpp   # render_path_install — the render-path tri-state, one parser for every boot spine
  ${PSXPORT_ROOT}/runtime/psx/gpu/proj_prim.cpp
  ${PSXPORT_ROOT}/runtime/psx/cpu/gte_vendor_hooks.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/proj_params.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/ot_attr.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/guest_packet_pool_windows.cpp
  ${PSXPORT_ROOT}/runtime/psx/core/guest_code_module.cpp
  ${PSXPORT_ROOT}/runtime/psx/boot/hw_bind.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/repl.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/dbg_server.cpp
  ${PSXPORT_ROOT}/runtime/psx/boot/native_stub.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/watchdog.cpp
  ${PSXPORT_ROOT}/runtime/psx/boot/boot.cpp
  ${PSXPORT_ROOT}/runtime/psx/boot/psx_exe_image.cpp
  ${PSXPORT_ROOT}/runtime/ui/rmlui_overlay.cpp  # RmlUi LIFETIME only — the UI itself is runtime/ui/
  ${PSXPORT_ROOT}/runtime/ui/rmlui_render_gpu.cpp
  ${PSXPORT_ROOT}/runtime/ui/rml_text.cpp       # DATA -> RML markup boundary (see rml_text.h)
  # ---- runtime/ui: the overlay's component tree. One file pair per responsibility.
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
  ${PSXPORT_ROOT}/runtime/ui/choice_navigator.cpp   # the rules of a choose-one list (no RmlUi)
  ${PSXPORT_ROOT}/runtime/ui/choice_screen.cpp      # the full-window choose-one screen over choice.rml
  ${PSXPORT_ROOT}/runtime/ui/menu_document.cpp      # the tree over assets/rml/menu.rml
  ${PSXPORT_ROOT}/runtime/psx/present/game_hooks_opt.cpp
  ${PSXPORT_ROOT}/runtime/psx/core/overlay_glue.cpp
  ${PSXPORT_ROOT}/runtime/psx/present/fade_interpolation.cpp # how two screen-fade endpoints resolve at t
  ${PSXPORT_ROOT}/runtime/psx/present/present_fade_state.cpp  # the fade a present composites, resolved for one Core
  ${PSXPORT_ROOT}/runtime/psx/present/fps60_game_hooks.cpp  # guarded callbacks used only by temporal presentation
  ${PSXPORT_ROOT}/runtime/psx/present/fps60_gpu_present.cpp # renderer pass used only by temporal presentation
  ${PSXPORT_ROOT}/runtime/psx/present/host_world_pass_strategy.cpp # explicit hook/capture adapter
  ${PSXPORT_ROOT}/runtime/psx/present/fps60.cpp            # interpolated-60fps lerp tier (framework render-infra; P1.7c)
  ${PSXPORT_ROOT}/runtime/psx/present/fps60_sequence_runs.cpp  # the captured-frame run grouping its diagnostic dump iterates
  ${PSXPORT_ROOT}/runtime/psx/present/projection_provenance.cpp # which scoped guest projection made each screen vertex
  ${PSXPORT_ROOT}/runtime/psx/gpu/guest_geometry_interpolation.cpp # in-between of the guest's own primitives by provenance
  ${PSXPORT_ROOT}/runtime/psx/gpu/guest_geometry_scene_source.cpp # InBetweenStrategy over the two above
  ${PSXPORT_ROOT}/runtime/psx/gpu/ot_lifo_depth.cpp    # PSX AddPrim head-insertion ties -> raster-distinct native depths
  ${PSXPORT_ROOT}/runtime/psx/gpu/render_queue.cpp     # engine-owned draw-ORDER authority (P1.7c)
  ${PSXPORT_ROOT}/runtime/psx/gpu/render_queue_attribution.cpp # attributes a FULL queue: runaway vs capacity
  ${PSXPORT_ROOT}/runtime/psx/gpu/render_submission.cpp
  ${PSXPORT_ROOT}/runtime/psx/present/wide_2d_layout.cpp # the 2D layout transform for a Core, by either widening mechanism
  ${PSXPORT_ROOT}/runtime/psx/gpu/painter_object_layer.cpp # atomic painter admission + authored command plan
  ${PSXPORT_ROOT}/runtime/psx/frame/pc_scheduler.cpp     # PC-native cooperative task scheduler; stage bodies via hooks (P1.7c)
  ${PSXPORT_ROOT}/runtime/psx/platform/synchronous_task_wait.cpp # one native synchronous FUN_80044BD4 owner
  ${PSXPORT_ROOT}/vendor/rmlui/Backends/RmlUi_Platform_SDL.cpp)

add_library(psxport STATIC ${PSXPORT_FRAMEWORK_SRC})
psxport_add_gpu_shaders(${PSXPORT_ROOT} psxport)

# ---- BUILD IDENTITY ---------------------------------------------------------------------------
# Stamp both framework and consumer identities during every build. The generator writes only when
# content changes, so provenance cannot go stale while unchanged translation units remain untouched.
# This rule lives here because consumers include this CMake module directly.
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

# Framework include dirs. The shader module separately attaches its exact build-owned include
# directory to psxport; it may differ from ${CMAKE_BINARY_DIR} when a consumer uses add_subdirectory.
target_include_directories(psxport PUBLIC
  # ${CMAKE_BINARY_DIR} is here for ONE header: the generated psxport_build_id.h above. It is PUBLIC
  # because a game's own translation units are entitled to the same identity the framework stamps.
  ${CMAKE_BINARY_DIR}
  ${PSXPORT_ROOT}/${RT} ${PSXPORT_ROOT}/runtime/cpu ${PSXPORT_ROOT}/runtime/ui
  # The subsystem directories of runtime/psx are ALL exported here, so every framework header and
  # every consumer's `#include "x.h"` resolves by basename after the step-2 split. One flat
  # directory with 320 files hid the boundaries; these lines make the boundaries the include path's
  # own statement of them.
  ${PSXPORT_ROOT}/${RT}/boot
  ${PSXPORT_ROOT}/${RT}/core
  ${PSXPORT_ROOT}/${RT}/cpu
  ${PSXPORT_ROOT}/${RT}/frame
  ${PSXPORT_ROOT}/${RT}/gpu
  ${PSXPORT_ROOT}/${RT}/present
  ${PSXPORT_ROOT}/${RT}/input
  ${PSXPORT_ROOT}/${RT}/cd
  ${PSXPORT_ROOT}/${RT}/audio
  ${PSXPORT_ROOT}/${RT}/movie
  ${PSXPORT_ROOT}/${RT}/debug
  ${PSXPORT_ROOT}/${RT}/config
  ${PSXPORT_ROOT}/${RT}/hle
  ${PSXPORT_ROOT}/${RT}/platform
  ${PSXPORT_ROOT}/${RT}/game
  # The save-state owner (runtime/psx/state/). Public so a title's own NativeStatePort implementation
  # can include machine_state.h and state_blob.h the way the framework's own sections do.
  ${PSXPORT_ROOT}/${RT}/state
  ${MED} ${MED}/psx
  ${PSXPORT_ROOT}/vendor/beetle-psx/libretro-common/include ${PSXPORT_ROOT}/vendor/beetle-psx
  ${PSXPORT_ROOT}/vendor/beetle-psx/deps/libchdr/include
  ${PSXPORT_ROOT}/vendor/rmlui/Include ${PSXPORT_ROOT}/vendor/rmlui/Backends)
# NO game include dirs: as of P1.7c the framework owns Fps60/RenderQueue/PcScheduler/VerifyHarness/FfSpan
# (their headers moved to runtime/psx/; the game reaches into them only through the GameHooks seam). The
# framework #includes no game/ header — the psxport_smoke link proves it (no game/** symbol resolves).

target_compile_definitions(psxport PUBLIC
  PSXPORT_SDL _XOPEN_SOURCE=700 RMLUI_STATIC_LIB RMLUI_SDL_VERSION_MAJOR=3)

target_compile_options(psxport PRIVATE -w -O2 -g)

# WHICH CALL SITES MOVE THE BYTES (runtime/psx/debug/memcensus.cpp). --wrap is a LINK-time rewrite, and
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
# runtime/psx/config/cfg.cpp is a thin shim over it, so every diagnostic in the port shares one output
# path. Vendored as a submodule (vendor/lucent) alongside beetle-psx/rmlui so the framework and its
# offline tools build hermetically — no configure-time network fetch. add_subdirectory installs the `lucent` target and
# its `lucent::lucent` alias; lucent's own tests are gated on its CMAKE_SOURCE_DIR and never build
# here.
#
# Lucent reads the public PSXPORT_DEBUG and PSXPORT_LOG_FILE names directly. They are build-time
# settings because the logger resolves them lazily at its own boundary; no separate initialization
# call may determine whether a diagnostic channel works.
set(LUCENT_CHANNEL_ENV  "PSXPORT_DEBUG")
set(LUCENT_LOG_FILE_ENV "PSXPORT_LOG_FILE")
if(ANDROID)
  set(LUCENT_FORMAT_BACKEND fmt CACHE STRING
    "Use the Android prefix's fmt package for Lucent's formatted logger" FORCE)
endif()
# EXPLICIT BINARY DIR, like rmlui's above (line 58). add_subdirectory with a source dir OUTSIDE the
# consuming project's tree is an error unless one is given — so without it the framework could only be
# built from a PSXPORT_ROOT nested under the consumer, and `-DPSXPORT_DIR=<a psxport clone elsewhere>`
# died here with "binary_dir must be specified". That is the whole point of the workspace's framework
# dev clone, so the path has to be honest about being relocatable.
add_subdirectory(${PSXPORT_ROOT}/vendor/lucent ${CMAKE_BINARY_DIR}/lucent_build)

target_link_libraries(psxport PUBLIC
  lucent::lucent
  lightrec rmlui_debugger rmlui_core chdr-static
  psxport_presentation_dependencies
  Threads::Threads m)

# ---- headless game-agnostic smoke (PSXPORT_BUILD_SMOKE=ON) ------------------------------------
# Links only psxport and inherited system dependencies. Any consumer-title symbol pulled into the
# Core-only client fails this link. See tools/smoke/psxport_smoke.cpp.
if(PSXPORT_BUILD_SMOKE)
  add_executable(psxport_smoke tools/smoke/psxport_smoke.cpp)
  set_target_properties(psxport_smoke PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/scratch/bin)
  # The smoke includes the public framework headers and inherits their build-owned include paths.
  target_link_libraries(psxport_smoke PRIVATE psxport)
endif()

# The vendored GPU sources use sscanf/fprintf/stderr without including <stdio.h>. Supply it on the
# compile line rather than editing the fork — psxport's rule is that beetle changes live in the
# committed fork, and this is a build-flag concern, not a source defect worth forking over.
set_source_files_properties(
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gpu.c
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gpu_polygon_sub.c
  PROPERTIES COMPILE_OPTIONS "-include;stdio.h")

# MEDNAFEN_VERSION_NUMERIC is written into the save-state stream header by the fork's MDFNSS_SaveSM,
# and Upstream's Makefile generates it into a settings header psxport does not build. It is never
# COMPARED on the way back in — MDFNSS_LoadSM checks the 16-byte magic and nothing else, because the
# per-field size checks inside each chunk are what actually version a state. So this supplies the
# symbol at the value the fork's own reader ignores, rather than forking a header to change it.
#
# HAVE_STRL tells the libretro compat header to use the C library's strlcpy/strlcat instead of the
# `strlcpy_retro__` shims, which live in libretro-common/strings.c — a unit psxport does not compile.
# state.c uses strlcpy exactly once, to copy a field LABEL into a 256-byte state buffer.
set_source_files_properties(
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/state.c
  PROPERTIES COMPILE_DEFINITIONS "MEDNAFEN_VERSION_NUMERIC=0;HAVE_STRL;_DEFAULT_SOURCE")
