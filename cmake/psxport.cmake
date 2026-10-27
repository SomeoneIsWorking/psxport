# cmake/psxport.cmake — the PSX-generic native/dynarec framework as `psxport`.
#
# The library contains the game-agnostic PSX machine, native services, dynarec integration boundary,
# presentation owners, and vendored hardware backends. Title code and restricted game data remain in
# the consumer. Build-generated shader and identity headers belong to the consumer's build tree.

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

# ---- RmlUi 6.3 (the overlay UI), static, Core + Debugger only --------------------------------
# vendor/rmlui is a submodule of upstream RmlUi pinned at the 6.3 release, the workspace's standard
# RmlUi version; nothing in it is modified.
set(BUILD_SHARED_LIBS OFF)
set(RMLUI_SAMPLES        OFF CACHE BOOL   "" FORCE)
set(RMLUI_LUA_BINDINGS   OFF CACHE BOOL   "" FORCE)
set(RMLUI_SVG_PLUGIN     OFF CACHE BOOL   "" FORCE)
set(RMLUI_LOTTIE_PLUGIN  OFF CACHE BOOL   "" FORCE)
set(RMLUI_FONT_ENGINE    freetype CACHE STRING "" FORCE)
add_subdirectory(${PSXPORT_ROOT}/vendor/rmlui ${CMAKE_BINARY_DIR}/rmlui_build EXCLUDE_FROM_ALL)

# ---- bug-report (the in-app bug report form) and setup-ui (the first-run screen), over that RmlUi --
include("${PSXPORT_ROOT}/cmake/shared_dependency.cmake")
psxport_add_shared_dependency(bug-report include/bug_report/draft.h)
psxport_add_shared_dependency(setup-ui include/setup_ui/setup_ui.h)

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
  ${PSXPORT_ROOT}/runtime/psx/game/title_debug_menu.cpp
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
  ${PSXPORT_ROOT}/runtime/psx/present/guest_widescreen_projection.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/fs_util.cpp          # generic std::filesystem host-I/O wrapper (class Fs), no game types
  ${PSXPORT_ROOT}/runtime/psx/platform/coro.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/memcensus.cpp      # --wrap=memcpy call-site attribution (PSXPORT_MEMCENSUS)
  ${PSXPORT_ROOT}/runtime/psx/platform/io_peripherals.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/sio_pad.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/dma_linked_list.cpp
  ${PSXPORT_ROOT}/runtime/psx/core/mem.cpp
  ${PSXPORT_ROOT}/runtime/psx/core/guest_memory.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/dma_callbacks.cpp # direct-runtime per-Game DMACallback registration state
  ${PSXPORT_ROOT}/runtime/psx/cpu/cpu_divide.cpp     # R3000 DIV/DIVU quotient/remainder semantics
  ${PSXPORT_ROOT}/runtime/psx/hle/bios_interrupt.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/bios_pad_work_area.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/bios_libc_string.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/hle_heap.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/bios_load_exec.cpp # A0:0x51 LoadAndExecute: load a CD executable and start it
  ${PSXPORT_ROOT}/runtime/psx/hle/hle.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/hle_interrupt.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/syscall_exception.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/kernel_syscall.cpp
  ${PSXPORT_ROOT}/runtime/cpu/host_turn.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/ordering_table.cpp # the guest DrawOTag chain and the tables a walk attributes to
  ${PSXPORT_ROOT}/runtime/psx/debug/image_writer.cpp      # one checked RGB24 capture-file boundary, stills and clips
  ${PSXPORT_ROOT}/runtime/psx/debug/gpu_debug.cpp
  ${PSXPORT_ROOT}/runtime/psx/audio/spu_audio.cpp
  ${PSXPORT_ROOT}/runtime/psx/audio/host_audio_output.cpp
  ${PSXPORT_ROOT}/runtime/psx/audio/wav_writer.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/host_input.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/input_injection.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/pad_input.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/stock_pad_buffers.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/pad_recording.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/pad_phase_replay.cpp
  ${PSXPORT_ROOT}/runtime/psx/input/pad_record_replay.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/memcard.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/card_syscall_log.cpp
  ${PSXPORT_ROOT}/runtime/psx/movie/native_fmv.cpp
  ${PSXPORT_ROOT}/runtime/psx/movie/movie_clock.cpp
  ${PSXPORT_ROOT}/runtime/psx/movie/fmv_decode.cpp
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gte.c
  ${PSXPORT_ROOT}/runtime/psx/cpu/beetle/gte_beetle.cpp
  # The PSX GPU device (runtime/psx/gpu/gpu_device.cpp). gpu.c #includes gpu_polygon.c / gpu_sprite.c /
  # gpu_line.c, so those are NOT listed; gpu_polygon_sub.c and rhi/rhi_intf.c are separate units.
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gpu.c
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/gpu_polygon_sub.c
  ${PSXPORT_ROOT}/vendor/beetle-psx/rhi/rhi_intf.c
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_device.cpp
  ${PSXPORT_ROOT}/runtime/psx/gpu/gp0_record_tap.cpp # executed GP0 commands into the FrameRecord
  ${PSXPORT_ROOT}/runtime/psx/gpu/texture_feedback.cpp # draws that sample VRAM gpu.c's texture cache may hold stale
  ${PSXPORT_ROOT}/runtime/psx/present/gp0_primitive_decode.cpp # a GP0 draw packet's words to a record primitive
  ${PSXPORT_ROOT}/runtime/psx/present/frame_record.cpp # one logic frame's GPU work in execution order
  ${PSXPORT_ROOT}/runtime/psx/present/emission_scope.cpp # producer scopes and the packet keys they bind
  ${PSXPORT_ROOT}/runtime/psx/present/owner_ledger.cpp # owner of every RAM word: scope instance or unowned writer
  ${PSXPORT_ROOT}/runtime/psx/present/ownership_verdict.cpp # unowned draws still or moving across an interval
  ${PSXPORT_ROOT}/runtime/psx/present/owner_journal.cpp # rows of unowned draws and held intervals
  ${PSXPORT_ROOT}/runtime/psx/present/frame_state.cpp # producer states by scope serial, collected per record
  ${PSXPORT_ROOT}/runtime/psx/present/state_producer.cpp # the registry of producer renders
  ${PSXPORT_ROOT}/runtime/psx/present/emit_memory.cpp # guest or host bytes a packet-emitting body runs over
  ${PSXPORT_ROOT}/runtime/psx/present/gte_control.cpp # the GTE control registers a render runs under
  ${PSXPORT_ROOT}/runtime/psx/present/packet_chains.cpp # packets a body linked into tag chains, as primitives
  ${PSXPORT_ROOT}/runtime/psx/present/frame_composer.cpp # the frame at t from a record and producer renders
  ${PSXPORT_ROOT}/runtime/psx/present/guest_redraw.cpp # a guest draw routine re-run for an in-between, then undone
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/mdec.c
  ${PSXPORT_ROOT}/runtime/psx/movie/mdec_device.cpp
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/psx/spu.c
  ${PSXPORT_ROOT}/runtime/psx/audio/spu_beetle.cpp
  # Beetle's state chunk codec; beetle_device_state.cpp defines the StateAction it calls.
  ${PSXPORT_ROOT}/vendor/beetle-psx/mednafen/state.c
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
  ${PSXPORT_ROOT}/runtime/psx/cd/cd_ready_delivery.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/cd_stock_read_completion.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/stock_cd_work_area.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/stock_libcd.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/disc_toc.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/hunk_reader.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/stock_cd_response.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/cdc_command_phase.cpp
  ${PSXPORT_ROOT}/runtime/psx/cd/cdc_native.cpp
  ${PSXPORT_ROOT}/runtime/psx/audio/xa_stream.cpp
  ${PSXPORT_ROOT}/runtime/psx/audio/xa_adpcm.cpp
  ${PSXPORT_ROOT}/runtime/psx/frame/emulated_time.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/field_digest.cpp
  ${PSXPORT_ROOT}/runtime/psx/frame/frame_pacer.cpp
  ${PSXPORT_ROOT}/runtime/psx/frame/timing.cpp
  ${PSXPORT_ROOT}/runtime/psx/render/render_api.cpp # the resolution plan
  ${PSXPORT_ROOT}/runtime/psx/render/render_readback.cpp # present shot, the pane hold probe and the VRAM captures
  ${PSXPORT_ROOT}/runtime/psx/render/render_device.cpp # the SDL GPU device: bring-up, samplers, pipelines, transfers
  ${PSXPORT_ROOT}/runtime/psx/render/render_submit.cpp # command-buffer submit and the persisted GPU fault latch
  ${PSXPORT_ROOT}/runtime/psx/render/render_present.cpp # a Game's present: build the present image, show it
  ${PSXPORT_ROOT}/runtime/psx/render/present_clip.cpp # the last second of window presents for a bug report clip
  ${PSXPORT_ROOT}/runtime/psx/gpu/gpu_ports.cpp # GP1 writes and DMA2 into the GPU device
  ${PSXPORT_ROOT}/runtime/psx/present/display_presenter.cpp # the guest display to the screen, once per present
  ${PSXPORT_ROOT}/runtime/psx/render/render_shader.cpp # SPIR-V shader creation
  ${PSXPORT_ROOT}/runtime/psx/gpu/record_geometry.cpp # write extents of record entries
  ${PSXPORT_ROOT}/runtime/psx/gpu/record_raster_setup.cpp # FrameRecord to record-raster quads and ops
  ${PSXPORT_ROOT}/runtime/psx/render/record_rasterizer.cpp # FrameRecord rasterized into one VRAM image
  ${PSXPORT_ROOT}/runtime/psx/render/record_present.cpp # RenderPath::Record present, shot and recordcheck
  ${PSXPORT_ROOT}/runtime/psx/render/render_window.cpp # the one window for the whole product run
  ${PSXPORT_ROOT}/runtime/psx/render/render_screen.cpp # a Game's present target: window or pane
  ${PSXPORT_ROOT}/runtime/psx/render/render_panes.cpp # a present image built from panes (the title selector)
  ${PSXPORT_ROOT}/runtime/psx/present/picture_announce.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/gpu_perf.cpp
  ${PSXPORT_ROOT}/runtime/psx/frame/frame_time_histogram.cpp  # the frame-time distribution gpu_perf reports percentiles from
  ${PSXPORT_ROOT}/runtime/psx/game/mods.cpp
  ${PSXPORT_ROOT}/runtime/psx/config/config.cpp
  ${PSXPORT_ROOT}/runtime/psx/config/number_text.cpp
  ${PSXPORT_ROOT}/runtime/psx/config/diagnostic_config.cpp # the opt-in diagnostic knobs
  ${PSXPORT_ROOT}/runtime/psx/config/diagnostic_run.cpp
  ${PSXPORT_ROOT}/runtime/psx/config/enhancement_gate.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/platform_hle.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/stock_dma_callbacks.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/stock_libgpu.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/stock_libgte.cpp
  ${PSXPORT_ROOT}/runtime/psx/hle/stock_vsync.cpp
  ${PSXPORT_ROOT}/runtime/psx/boot/run_end_report.cpp # the one report a finished run owes
  ${PSXPORT_ROOT}/runtime/psx/host/product_host.cpp # the multi-title product: picker, then the chosen title, then the picker
  ${PSXPORT_ROOT}/runtime/psx/host/title_session.cpp # one steppable boot-to-exit run of one catalog title
  ${PSXPORT_ROOT}/runtime/psx/catalog/single_title_catalog.cpp # the catalog of a one-title product
  ${PSXPORT_ROOT}/runtime/psx/host/run_budget.cpp # the run's cap in presentation-fence steps
  ${PSXPORT_ROOT}/runtime/psx/host/panel_sessions.cpp # one live session per available title and which of them runs
  ${PSXPORT_ROOT}/runtime/psx/host/panel_logo.cpp # a title's logo image and its lazy GPU texture
  ${PSXPORT_ROOT}/runtime/psx/host/picker_session.cpp
  ${PSXPORT_ROOT}/runtime/psx/host/picker_runtime.cpp
  ${PSXPORT_ROOT}/runtime/psx/host/picker_layout.cpp
  ${PSXPORT_ROOT}/runtime/psx/host/picker_panes.cpp
  ${PSXPORT_ROOT}/runtime/psx/host/picker_content.cpp
  ${PSXPORT_ROOT}/runtime/psx/host/executable_provisioner.cpp
  ${PSXPORT_ROOT}/runtime/psx/host/title_disc.cpp # a title's disc, keyed or one exact image
  ${PSXPORT_ROOT}/runtime/psx/host/saved_disc.cpp # the first-run disc choice in user data
  ${PSXPORT_ROOT}/runtime/psx/host/first_run_setup.cpp # the setup-ui screen that asks for the disc
  ${PSXPORT_ROOT}/runtime/psx/catalog/title_availability.cpp
  ${PSXPORT_ROOT}/runtime/psx/catalog/title_selection.cpp
  ${PSXPORT_ROOT}/runtime/psx/frame/field_turn.cpp # psx::FieldTurn: the per-field services a loop owes
  ${PSXPORT_ROOT}/runtime/psx/frame/logic_frame.cpp # psx::frame::LogicFrame: the host side of a title logic frame
  ${PSXPORT_ROOT}/runtime/psx/game/crt0_boot.cpp # Crt0Boot — the derived crt0 plan's shipping applier
  ${PSXPORT_ROOT}/runtime/psx/cpu/beetle/gte_vendor_hooks.cpp
  ${PSXPORT_ROOT}/runtime/psx/present/proj_params.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/ot_attr.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/producer_census.cpp
  ${PSXPORT_ROOT}/runtime/psx/game/guest_packet_pool_windows.cpp
  ${PSXPORT_ROOT}/runtime/psx/core/guest_code_module.cpp
  ${PSXPORT_ROOT}/runtime/psx/audio/spu_device.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/command_args.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/pad_frame_probes.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/snapshot.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/store_watch.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/repl.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/dbg_server.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/debug_commands.cpp
  ${PSXPORT_ROOT}/runtime/psx/debug/bug_report_session.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/spin_detector.cpp
  ${PSXPORT_ROOT}/runtime/psx/platform/watchdog.cpp
  ${PSXPORT_ROOT}/runtime/psx/game/psx_exe_image.cpp
  ${PSXPORT_ROOT}/runtime/psx/game/disc_executable.cpp # a boot stub's hand-off to an executable on its disc
  ${PSXPORT_ROOT}/runtime/ui/rmlui_library.cpp  # RmlUi process state on the render device
  ${PSXPORT_ROOT}/runtime/ui/rmlui_overlay.cpp  # a Game's RmlUi context and menu document
  ${PSXPORT_ROOT}/runtime/ui/rmlui_render_gpu.cpp
  ${PSXPORT_ROOT}/runtime/ui/rml_text.cpp       # DATA -> RML markup boundary (see rml_text.h)
  # ---- runtime/ui: the overlay's component tree. One file pair per responsibility.
  ${PSXPORT_ROOT}/runtime/ui/ui_event.cpp           # ScopedEventListener — registration IS lifetime
  ${PSXPORT_ROOT}/runtime/ui/ui_component.cpp       # Component base + the ONE data->DOM text boundary
  ${PSXPORT_ROOT}/runtime/ui/ui_assets.cpp          # asset resolution that REFUSES to report success
  ${PSXPORT_ROOT}/runtime/ui/mod_row_model.cpp      # what a Mods toggle row means
  ${PSXPORT_ROOT}/runtime/ui/title_debug_rows.cpp   # the Debug tab rows a title's debug menu declares
  ${PSXPORT_ROOT}/runtime/ui/menu_row.cpp           # one <select-button> + its binding
  ${PSXPORT_ROOT}/runtime/ui/menu_pane.cpp          # one tab's page of rows
  ${PSXPORT_ROOT}/runtime/ui/menu_tab_bar.cpp       # the <tab> row and which one is selected
  ${PSXPORT_ROOT}/runtime/ui/menu_readouts.cpp      # the live video, debug status and title readout lines
  ${PSXPORT_ROOT}/runtime/ui/menu_document.cpp      # the tree over assets/rml/menu.rml
  ${PSXPORT_ROOT}/runtime/psx/present/present_ledger.cpp # recorded vs composed producers of a t = 1 present
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
  ${PSXPORT_ROOT}/${RT}/core
  ${PSXPORT_ROOT}/${RT}/cpu
  ${PSXPORT_ROOT}/${RT}/frame
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
  ${PSXPORT_ROOT}/${RT}/catalog
  # Public: titles call the GPU device, its ports and the ordering-table walk.
  ${PSXPORT_ROOT}/${RT}/gpu
  ${PSXPORT_ROOT}/${RT}/render
  # Public: a title's NativeStatePort includes machine_state.h.
  ${PSXPORT_ROOT}/${RT}/state
  ${MED} ${MED}/psx
  ${PSXPORT_ROOT}/vendor/beetle-psx/libretro-common/include ${PSXPORT_ROOT}/vendor/beetle-psx
  ${PSXPORT_ROOT}/vendor/beetle-psx/deps/libchdr/include)
# Third-party headers: their own diagnostics are not ours.
target_include_directories(psxport SYSTEM PUBLIC ${PSXPORT_ROOT}/vendor/rmlui/Include ${PSXPORT_ROOT}/vendor/rmlui/Backends)
# NO game include dirs: the framework #includes no game/ header.

# Framework internals no title includes; framework tools link psxport_internal for them.
add_library(psxport_internal INTERFACE)
set(PSXPORT_INTERNAL_INCLUDES
  ${PSXPORT_ROOT}/${RT}/boot ${PSXPORT_ROOT}/${RT}/host ${PSXPORT_ROOT}/${RT}/cpu/beetle)
target_include_directories(psxport_internal INTERFACE ${PSXPORT_INTERNAL_INCLUDES})
target_include_directories(psxport PRIVATE ${PSXPORT_INTERNAL_INCLUDES})

target_compile_definitions(psxport PUBLIC
  PSXPORT_SDL _XOPEN_SOURCE=700 RMLUI_STATIC_LIB RMLUI_SDL_VERSION_MAJOR=3)

target_compile_options(psxport PRIVATE -w -O2 -g)

# WHICH CALL SITES MOVE THE BYTES (runtime/psx/debug/memcensus.cpp). --wrap is a LINK-time rewrite, and
# it must be on the final executable's link rather than this static library's, so it is INTERFACE:
# every consumer (the game exe) inherits it and every first-party memcpy/memmove call
# routes through __wrap_*. Always linked, never always counting — the wrapper is one relaxed load and
# a not-taken branch unless PSXPORT_MEMCENSUS=1, so an ordinary run is unaffected.
#
# ON by default because an instrument nobody can switch on without recompiling is an instrument
# nobody uses; set PSXPORT_WRAP_MEMCPY=OFF to link without it and prove the wrapper itself costs
# nothing (an A/B this option exists to make possible).
option(PSXPORT_WRAP_MEMCPY "Route first-party memcpy/memmove through the call-site census" ON)
if(PSXPORT_WRAP_MEMCPY)
  # --undefined pulls memcensus.o from the archive even when only a later library (zstd) calls memcpy.
  target_link_options(psxport INTERFACE -Wl,--wrap=memcpy -Wl,--wrap=memmove
    -Wl,--undefined=__wrap_memcpy -Wl,--undefined=__wrap_memmove)
endif()

# Link deps PUBLIC/INTERFACE so any consumer (the game exe, the smoke) inherits them.
# lucent — logging + configuration (https://github.com/SomeoneIsWorking/lucent). Vendored as a submodule (vendor/lucent) alongside beetle-psx/rmlui so the framework and its
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
  lightrec rmlui_debugger rmlui_core bug_report::bug_report chdr-static
  psxport_presentation_dependencies
  Threads::Threads m)
# The first-run screen is the host's own; titles never include it.
target_link_libraries(psxport PRIVATE setup_ui::setup_ui)
# The bug report's motion clip is an animated PNG.
find_package(ZLIB REQUIRED)
target_link_libraries(psxport PRIVATE ZLIB::ZLIB)

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

# ---- the product executable --------------------------------------------------------------------
# psxport_add_product(<target> CATALOG <factory> HEADER <header> [SOURCES <file>...] [LIBRARIES <lib>...])
# One executable whose main is runtime/psx/host/product_main.cpp. <factory> is the title's
# `std::unique_ptr<psx::host::TitleCatalog>()` function, declared in <header>.
function(psxport_add_product target)
  cmake_parse_arguments(PARSE_ARGV 1 PRODUCT "" "CATALOG;HEADER" "SOURCES;LIBRARIES")
  if(NOT PRODUCT_CATALOG OR NOT PRODUCT_HEADER OR PRODUCT_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "psxport_add_product(${target} CATALOG <factory> HEADER <header> [SOURCES ...] [LIBRARIES ...])")
  endif()
  set(product_main "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../runtime/psx/host/product_main.cpp")
  add_executable(${target} "${product_main}" ${PRODUCT_SOURCES})
  # The framework's own main sees the host internals; the title's sources do not.
  set_source_files_properties("${product_main}" TARGET_DIRECTORY ${target} PROPERTIES
    INCLUDE_DIRECTORIES "$<TARGET_PROPERTY:psxport_internal,INTERFACE_INCLUDE_DIRECTORIES>")
  target_compile_definitions(${target} PRIVATE
    "PSXPORT_PRODUCT_CATALOG=${PRODUCT_CATALOG}"
    "PSXPORT_PRODUCT_CATALOG_HEADER=\"${PRODUCT_HEADER}\"")
  target_link_libraries(${target} PRIVATE psxport ${PRODUCT_LIBRARIES})
endfunction()
