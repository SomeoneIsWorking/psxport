# Codemap

Where each psxport concept lives. Feature chains (which owner calls which, hop by hop) are
`docs/features.md`; intent is `docs/project-goals.md`; capability state is `docs/project-state.md`;
atomic work is `docs/issues/`.

Dependency direction: title code → the title API (`runtime/psx/game/` and section 3.5 of
`docs/workspace/RESTRUCTURE.md`) → PSX CPU, device and platform owners → host libraries. The executor
calls services only through typed exits after synchronising guest state. `boot/`, `host/`, `gpu/`
and `cpu/beetle/` are PRIVATE to `psxport` (`PSXPORT_INTERNAL_INCLUDES`); framework tests and tools
reach them through `psxport_internal`, titles never do. Test oracles never link into the product.

## Owners

| owner | where | owns |
|---|---|---|
| Process top level | `runtime/psx/host/product_host.*` (`ProductHost::run`), `product_main.cpp` | The command line, provisioning, the selector and run budget; the only `main` |
| Title session | `runtime/psx/host/title_session.*` | The shared boot steps (`TitleSession::boot`) and one field (`TitleSession::step`) |
| Title host | `runtime/psx/host/` (`PickerSession`, `PanelSessions`, `RunBudget`, `PreRollBudget`, `PickerRuntime`) | One window and device for many titles, the in-window picker, quit scope |
| First-run disc setup | `runtime/psx/host/first_run_setup.*`, `saved_disc.*`, `title_disc.*` | The setup-ui screen, the saved disc choice, a title's disc for one read |
| Executable provisioning | `runtime/psx/host/executable_provisioner.*` | A missing boot executable extracted from the title's disc and authenticated |
| Title catalog | `runtime/psx/catalog/` | `TitleCatalog`, `SingleTitleCatalog`, `TitleIdentity`, selection, availability, logo image |
| Title interface | `runtime/psx/game/game_runtime.*` | `GameRuntime`, `FrameDriver`, `TitleSessionState`, `TitleAudio`, `TitleDevWarp`, `HostIdentity` |
| Game composition | `runtime/psx/game/game.*` | One title's `Core`, devices, presenters and session; `Game::wireRuntimeMembers` |
| Title commands | `runtime/psx/game/title_command.*` | One capture of a title command's reply for the REPL and the Debug tab |
| Crt0 replay | `runtime/psx/game/crt0_boot.*`, `runtime/psx/boot/crt0_verify.h` | `Crt0Boot::run` over a title's `Crt0Image`, the completeness refusal and the guest-bytes audit |
| Executable admission | `runtime/psx/game/psx_exe_image.*`, `disc_executable.h` | Mapping an authenticated PS-X EXE and its entry registers |
| Packet pool layouts | `runtime/psx/game/guest_packet_pool_windows.*` | The layouts a title answers `packetPool` with |
| Title settings | `runtime/psx/game/title_settings.h` | `TitleIntSetting` rows a title declares |
| Canonical PSX state | `runtime/psx/core/core.*` | Per-Core registers, RAM, scratchpad, device ownership |
| Guest memory | `runtime/psx/core/guest_memory.cpp`, `mem.cpp` | Loads, stores, write guards, MMIO dispatch and executable-write notification |
| Guest code modules | `runtime/psx/core/guest_code_module.*`, `overlay_glue.*` | A landed module's identity and generation |
| Guest ABI | `runtime/psx/core/guest_abi.h`, `guest_address_range.h` | The guest stack frame and address ranges |
| Machine snapshot tick | `runtime/psx/core/snapshot.*` | The periodic snapshot a title ticks |
| Architectural state | `runtime/psx/cpu/` (`r3000.h`, COP0, divide unit) | The register file the executor transfers |
| GTE | `runtime/psx/cpu/gte.h`, `gte_registers.h`, `gte_state.h` | `psx::cpu::Gte` per Core (`core.gte()`): reads, writes, `execute`, `storeScreenXy`, `saveRaw`/`restoreRaw` |
| Beetle GTE adapter | `runtime/psx/cpu/beetle/` (`beetle_gte.h`, `gte_beetle.cpp`, `gte_register_transfer.h`, `gte_vendor_hooks.cpp`) | Beetle's GTE binding, the executor's register transfer, disabled PGXP hooks |
| GTE guard | `runtime/psx/present/gte_control.*` | `GteGuard`: a host pass hands the GTE back as it found it |
| Lightrec executor | `runtime/cpu/lightrec_executor.*`, `fallback_policy.h` | Lightrec lifetime per Core, state sync, bounded execution, fallback telemetry |
| Typed exits | `runtime/cpu/execution_exit.*`, `execution_control.*`, `execution_services.*` | Exit reasons, budgets, `handleBreak`, exit requests |
| Image identity | `runtime/cpu/image_identity.*` | Authenticated resident and module identity and generation |
| Native dispatch | `runtime/cpu/native_dispatch.*`, `guest_call.*`, `guest_call_site.h` | Image-scoped overrides, `Producer{Arg}` registration, scoped original calls |
| Resumable guest call | `runtime/cpu/resumable_guest_call.*`, `guest_call_census.*` | A guest call across fields: boundary latch, turn cap, refusals; the run's call census |
| Host-dispatch cache | `runtime/cpu/host_dispatch_cache.*`, `guest_host_dispatch_kind.h` | What an address is, until an image, override or service registration changes |
| Invalidation | `runtime/cpu/invalidation.*`, `executable_write_source.h` | Executable writes revoking translated blocks and dispatch decisions |
| Override differential | `runtime/cpu/override_differential*`, `machine_snapshot.*`, `side_effect_journal.*` | Shadowing a native override against its original in a real run |
| Function reach | `runtime/cpu/function_reach.*` | Every dispatched guest pc per code image (`PSXPORT_REACH_REPORT`) |
| Store observer | `runtime/cpu/store_observe.*` | Armed guest-store watches |
| Host turn | `runtime/psx/boot/host_turn.cpp`, `runtime/cpu/host_turn.h` | The per-Core guest-time field clock and callback delivery |
| Lightrec dependency | `cmake/lightrec_dependency.cmake` | The pinned Lightrec revision; `tools/psxport_fetch.py --lightrec` makes its worktree |
| BIOS and kernel HLE | `runtime/psx/hle/` (`hle.cpp`, `hle_heap.cpp`, `hle_interrupt.cpp`, `bios_*`, `kernel_syscall.cpp`, `syscall_exception.*`) | BIOS/libc services, interrupts, kernel syscalls, `LoadExec` |
| Stock libraries | `runtime/psx/hle/stock_{libgpu,libgte,vsync,dma_callbacks}.*`, `runtime/psx/cd/stock_libcd.*`, `runtime/psx/input/stock_pad_buffers.*` | PSY-Q bodies a title binds at its addresses from `registerOverrides` |
| Platform HLE binding | `runtime/psx/hle/platform_hle.*` | `bindStock` and the per-Game binding table |
| CD drive | `runtime/psx/cd/cdc_native.cpp`, `cdc_state.h`, `cdc_command_phase.*`, `cd_drive_timing.h`, `cd_position.*` | The controller, its command phases and deterministic drive timing |
| Disc | `runtime/psx/cd/disc.*`, `disc_file.h`, `disc_provision.cpp`, `disc_toc.*` | CHD access, ISO9660 files, the TOC answers |
| Stock CD results | `runtime/psx/cd/stock_cd_command.h`, `stock_cd_response.*`, `stock_cd_work_area.*`, `cd_stock_read_completion.*`, `cd_ready_delivery.*` | Responses through CdSync, work-area bytes, the INT1 a synchronous read still owes |
| DMA and peripherals | `runtime/psx/platform/` (`dma_*`, `io_peripherals.*`, `irq_edge.h`) | DMA transfers, callbacks, IRQ edges |
| Guest continuations | `runtime/psx/platform/coro.*` | The `Coro` a title's task scheduler runs on |
| Memory card | `runtime/psx/platform/memcard.*`, `card_syscall_log.*` | The card device and its identity |
| Watchdog | `runtime/psx/platform/watchdog.*` | The frame watchdog `ProductHost` owns |
| Boot splash | `runtime/psx/platform/scea_splash.*`, `scea_asset.h` | Decoding the SCEA card picture |
| Field turn | `runtime/psx/frame/field_turn.*` | The services owed around a title's field body: pause, watchdog re-arm, RAM dump, one command |
| Frame loop | `runtime/psx/frame/frame_loop_shell.*`, `logic_frame.*` | Stepping a `FrameDriver`; the logic-frame start |
| Frame presentation | `runtime/psx/frame/frame_presenter.*` | `FramePresenter::commit`: seal, cut, compose, pace |
| Pacing and time | `runtime/psx/frame/frame_pacer.*`, `pace_plan.h`, `field_cadence.h`, `timing.*`, `emulated_time.*`, `field_rate.h` | Host pacing, field rate, emulated time |
| GPU device | `runtime/psx/gpu/gpu_device.*`, `gpu_display_mode.h` | Beetle `gpu.c`: the only VRAM and GP0 executor |
| Guest GPU ports | `runtime/psx/present/gpu_ports.*`, `vram_rect.h` | `gp0Write`, `gp1Write`, `gpuRead`, DMA2, `loadImage`, `vram`, `sealRecord` |
| Frame record | `runtime/psx/gpu/gp0_record_tap.*`, `texture_feedback.*`, `runtime/psx/present/frame_record.*` | The device's GP0 work recorded per logic frame |
| Record rasteriser | `runtime/psx/gpu/gpu_vk_record_raster.*`, `record_raster_setup.*`, `record_raster_regions.h`, `vram_bitmap.h` | Drawing a record on the device image |
| SDL3 GPU backend | `runtime/psx/gpu/gpu_vk_*.cpp`, `gpu_vk_window.*`, `gpu_vk_shader.*`, `runtime/psx/shaders_gpu/` | Window, device, present, submit, readback, shaders |
| Display presenter | `runtime/psx/present/display_presenter.*`, `present_ledger.*`, `present_plan.h`, `video_plan.h`, `pixel_size.h` | Presenting a record, an in-between or a host picture; the present ledger |
| Render mode | `runtime/psx/present/render_mode.h` | `RenderPath {Record, Device}`, `DisplayPassGuard` |
| Frame composer | `runtime/psx/present/frame_composer.*`, `state_producer.*`, `frame_state.*`, `emission_scope.*`, `emit_memory.*`, `blend.h` | In-betweens from `StateProducer` renders of saved state |
| GP0 decode | `runtime/psx/present/gp0_command.h`, `gp0_primitive_decode.*`, `state_bytes.h` | One decode of guest draw commands |
| Ordering table | `runtime/psx/present/ordering_table.*`, `host_ordering_table.*` | The guest OT walk and host OT emission |
| Widescreen | `runtime/psx/present/guest_widescreen_projection.*`, `viewport_layout.h`, `proj_params.*`, `picture_announce.*` | The title's aspect policy, canvas margins, projection publication |
| Panes | `runtime/psx/present/pane_composite.*`, `host_presentation.h`, `rgb24_picture.h` | Picker panes and host pictures |
| Host input | `runtime/psx/input/host_input.*` | The one SDL event drain, key state, `QuitScope` |
| Pad | `runtime/psx/input/pad_input.*`, `sio_pad.*`, `pad_delivery.h`, `input_phase.h` | The guest pad and its delivery |
| Pad record/replay | `runtime/psx/input/pad_recording.*`, `pad_phase_replay.*`, `pad_record_replay.*` | `.pad` files and phase-keyed replay |
| Audio | `runtime/psx/audio/` (`spu_audio.*`, `spu_device.*`, `spu_beetle.*`, `xa_stream.cpp`, `audio_*`) | The SPU, its host sink, XA streaming, audio policy |
| Movies | `runtime/psx/movie/native_fmv.*`, `fmv_decode.*`, `mdec_*` | FMV playback and the STR decode shared with `tools/fmv_export` |
| Save states | `runtime/psx/state/` | `MachineState`, `StateFile`, device sections, `NativeStatePort` |
| Configuration | `runtime/psx/config/config.*`, `config_var.h`, `config_vars.h`, `diagnostic_config.*`, `number_text.*` | The CVar ladder, typed accessors, `launch_setting`, number parsing |
| Launch config | `runtime/psx/config/typed_config.*` | The immutable `psx::config::Config` on `game.config` |
| Title enhancement knobs | `runtime/psx/config/enhancement_gate.h`, `stepped_range.h` | `pc_enh` gates and stepped menu rows |
| Control channel | `runtime/psx/debug/dbg_server.*`, `command_args.*` | The loopback automation endpoint |
| REPL | `runtime/psx/debug/repl.*` | Interactive inspection |
| Producer census | `runtime/psx/debug/producer_census.*`, `ot_attr.*` | Primitives per guest submitter, keyed and rendered |
| Diagnostics | `runtime/psx/debug/` (`gpu_debug.*`, `gpu_perf.*`, `memcensus.*`, `field_digest.*`, `image_writer.*`, `diagnostic_run.h`) | Dumps, counters and capture |
| Live mods | `runtime/psx/debug/mods.*` | Per-Game player toggles (`aspect`, `fps60`) and the settings file |
| Bug report | `runtime/psx/debug/bug_report_session.*`, `vendor/bug-report` | The in-app report and its reference frame |
| Player UI | `runtime/ui/` | The RmlUi overlay, menus, readouts, warp control |
| Logging | `vendor/lucent/` | All output |
| Framework tests | `tests/`, `tests/support/`, `tools/smoke/` | Hermetic tests, the stub runtime, the smoke product |
| CPU oracle | `tools/oracle/` | Mednafen snapshots, the full-console host, state-aligned comparison |
| Title tooling | `tools/port/` | Engines every title runs (`README.md` there) |
| Framework fetch | `tools/psxport_fetch.py` | A title's `external/psxport` and the Lightrec worktree; titles run it from psxport `main` |
| Workspace bootstrap | `scripts/bootstrap_workspace.py`, `scripts/submodule_state.py`, `scripts/sync_submodules.py` | Cloning the workspace and its submodules |
| Decompilation | `tools/decomp_pipeline.py`, `tools/decomp/` | Ghidra-backed decompilation and queries (`docs/workspace/GHIDRA.md`) |
| Binary and MIPS tools | `tools/formats/`, `tools/mips/`, `tools/disasm.py`, `tools/discdump.cpp`, `tools/crt0_extract.cpp` | Image formats, disassembly, disc reads |
| Policy | `tools/check_cpp_style.py`, `tools/policy/`, `tools/lint/`, `tools/repository_policy.py` | Format, tidy, size caps, re-harness `cpp_policy.py`, literal and repository checks |
| Build and CI | `CMakeLists.txt`, `cmake/`, `tools/build.py`, `tools/verify.py`, `.github/` | Composition, the gate, hosted CI |
| Vendored dependencies | `vendor/` (beetle-psx, lucent, rmlui, bug-report, setup-ui), `external/psycross` | Pinned forks and sources |

## Where new work goes

- A title-specific address, native body, frame or task policy: the title repo, through a `GameRuntime`
  virtual or a stock-library object it binds.
- A boot step every product shares: `TitleSession::boot`. A per-field service: `FieldTurn`.
- A new executor stop: `runtime/cpu/execution_exit.*`. Override or original-call semantics:
  `runtime/cpu/native_dispatch.*`. Code invalidation: `runtime/cpu/invalidation.*`.
- Lightrec cache or code-memory internals: the pinned Lightrec fork.
- PSX device behaviour: its `runtime/psx/<device>/` owner.
- Anything a picture shows: `docs/presentation.md` first.
- Environment or settings parsing: `runtime/psx/config/`.
- Interpreter semantics or state comparison: `tools/oracle/`, never the product.
- A decompilation becoming a build input: nowhere; that is a stop.
