# Codemap

This map owns subsystem placement and dependency direction only. Product intent is
`docs/project-goals.md`; factual capability state is `docs/project-state.md`; migration ordering and
acceptance gates are `docs/migration.md`; atomic work is `docs/issues/`.

## Architecture layers

The intended dependency direction is:

`consumer title policy -> psxport composition/API -> PSX CPU and platform owners -> host libraries`

The CPU executor calls platform/HLE/native services only through narrow callbacks after synchronizing
guest-visible state. Platform, renderer, audio, UI, and diagnostic owners never choose a guest CPU
engine. Test-oracle targets may depend on the production CPU state and memory seams; the gameplay
library never depends on a test oracle.

## Ownership map

| Subsystem | Responsibility | Current location | New work belongs | Entry point / deep doc |
| --- | --- | --- | --- | --- |
| Product composition | Construct title-neutral runtime owners and run the host loop | `runtime/psx/native_boot.cpp` | Keep orchestration thin; CPU implementation moves to runtime/cpu/ | `runtime/psx/game_runtime.h` |
| Canonical PSX state | Per-`Core` registers, RAM/scratchpad, devices, and game association | `runtime/psx/core.h`, `runtime/psx/core.cpp` | `Core` owns the composed CPU executor but not its implementation | `runtime/psx/r3000.h` |
| Lightrec lifetime | One dynarec-default Lightrec state and callback context per `Core` | `runtime/cpu/lightrec_executor.*`, `runtime/cpu/fallback_policy.h` | Keep backend lifetime, fallback admission policy, and complete bounded-fallback telemetry per `Core`; maintained-fork work belongs in the pinned dependency | `docs/migration.md` |
| Architectural-state bridge | Synchronize GPR, HI/LO, PC/delay state, CP0, GTE, interrupt, and cycle state at every host boundary | `runtime/cpu/lightrec_executor.*`, `runtime/psx/core.*` | The executor bridge, never title wrappers | `docs/migration.md` |
| GTE register transfer | Import/export raw GTE banks at executor and COP2-operation boundaries, materialize SXYP alias, and mirror published FLAG without CPU-port side effects | `runtime/psx/gte_register_transfer.h` | One bulk transfer owner; CPU MTC2/MFC2 semantics stay in the backend and native register ports | `tests/test_lightrec_gte_transfer.cpp` |
| Guest task lifetime | Coroutine continuation across JIT budgets, authored cooperative yield/cancellation, and task-slot state | `runtime/psx/scheduler.cpp`, `runtime/psx/pc_scheduler.cpp` | Keep task lifetime in the scheduler; unsupported typed exits require an explicit host-loop delivery contract | `tests/test_guest_task_lifecycle.cpp` |
| Bounded execution results | Typed budget, override, HLE/device, interrupt/exception, frame, thread, and fault exits | `runtime/cpu/execution_exit.*`, `runtime/cpu/execution_control.*` | Add a typed reason/result here | `docs/migration.md` |
| Executable admission | Validate and map an already authenticated PS-X EXE buffer, publish executable writes, and establish entry registers; bounded file startup composes this mapper | `runtime/psx/psx_exe_image.*`, `runtime/psx/boot.cpp` | Title owns exact revision authentication of the same buffer; image catalog and invalidation retain residency/cache ownership | `runtime/psx/psx_exe_image.h` |
| Code identity | Authenticate resident/module bytes, assign load generations, and resolve complete physical resource spans | `runtime/cpu/image_identity.*`, `runtime/psx/overlay_glue.cpp` | The image catalog and title-owned identity policy | `runtime/cpu/image_identity.h` |
| Native/original dispatch | Resolve image-generation-plus-address overrides and scoped original calls | `runtime/cpu/native_dispatch.*`, `runtime/cpu/guest_call.*` | The per-Core dispatcher; title policy remains in the consumer | `runtime/cpu/native_dispatch.h` |
| Host-turn delivery | Guest-time field clock (one field owed per field period of `Timing::emulatedCpuTicks` since the last delivered field) and guest-thread callback delivery with IRQ, native-dispatch, redirect, and reentrancy deferral | `runtime/psx/host_turn.cpp`, `runtime/cpu/execution_services.cpp` (raises the request from instruction accounting) | Preserve pending requests until an eligible dispatch boundary; host wall time never decides how many fields a guest update observes | `runtime/cpu/host_turn.h`, `tests/test_host_turn_guest_clock.cpp` |
| Executable invalidation | Normalize executable writes and revoke translated blocks plus captured dispatch decisions | `runtime/cpu/invalidation.*` | Central invalidation owner | `docs/issues/0050-centralize-lightrec-code-invalidation.md` |
| Lightrec cache/code memory | Translated-block cache, chaining, executable-memory allocation/publication, and teardown | Exact pinned checkout selected by `PSXPORT_LIGHTREC_DIR` | The maintained Lightrec fork; psxport only owns its adapter | `docs/migration.md` |
| Independent CPU oracle | Mednafen execution, exact CPU snapshots, and explicitly normalized architectural windows | `tools/oracle/oracle_snapshot.*`, `tools/oracle/oracle_boundary.*`, `tools/oracle/oracle_window.cpp` | Separate diagnostic process; never product linkage or an unmodeled full-device oracle | `tools/oracle/README.md` |
| Full-console oracle host | Pinned software-core build, explicit firmware admission, finite input-driven stepping, bounded PC records, and canonical RAM/frame/audio hash windows | `tools/oracle/console*.py`, `console_observer.py`; optional fork `mednafen/psx/pc_observer.*` | `console.py`; separate diagnostic process with its own complete console lifecycle and no product linkage | `tools/oracle/CONSOLE.md` |
| State-aligned comparison driver | Drive a product REPL session and the full-console host to title-declared checkpoints with identical per-frame pad delivery, compare declared main-RAM ranges, seed a selftest divergence, and write the report | `tools/oracle/compare.py`, `tools/oracle/compare_cores.py` | `compare.run(title, product, args, out_dir)`; titles own predicates, exclusions, barrier, lookahead, schedule and launch | `docs/oracle.md` |
| Guest memory access | Shared native/CPU RAM and scratchpad loads/stores, write guards, watchpoints, and executable-write notification | `runtime/psx/guest_memory.cpp` | `Core::mem_r*`, `Core::mem_w*`; every mapped write notifies the central invalidation owner after bytes become visible | `runtime/psx/core.h` |
| Hardware bus | MMIO dispatch, DMA transfers, and peripheral routing | `runtime/psx/mem.cpp`, `runtime/psx/io_peripherals.cpp` | Small cohesive modules under `runtime/psx/` until a responsibility-driven platform move | `runtime/psx/io_peripherals.h` |
| BIOS/SDK HLE | Sony BIOS/libc/work-area/interrupt and measured SDK service boundaries, including the BIOS pad work-area flag callbacks | `runtime/psx/hle.cpp`, `runtime/psx/bios_pad_work_area.cpp`, `runtime/psx/hle_interrupt.cpp`, `runtime/psx/bios_interrupt.cpp`, `runtime/psx/bios_libc_string.cpp`, `runtime/psx/platform_hle.h` | The smallest existing HLE owner; new guest execution enters through bounded exits | `tests/test_bios_pad_work_area.cpp` |
| Stock libcd TOC completion | GetTN/GetTD from parsed disc tracks and one per-Game command response retained through CdSync | `runtime/psx/stock_cd_response.*`, state in `runtime/psx/cd.h`; called by `runtime/psx/cd_override.cpp` | The stock CD result owner; keep unrelated command effects and reads in `cd_override.cpp` | `tests/test_cd_stock_toc.cpp` |
| Stock libcd command work area | Setloc's four position bytes and Setmode's mode byte retained in guest RAM for later CdLastPos/reader use | `runtime/psx/stock_cd_work_area.*` resolves direct-plan or legacy addresses and publishes guest bytes; `runtime/psx/cd_override.cpp` invokes it | Direct titles declare measured guest addresses in `PlatformHlePlan`; this module keeps the stock bookkeeping rule single-source | `tests/test_cd_stock_workarea.cpp` |
| Continuous CD callback delivery | Route a current INT1 through either a host-owned ready callback that consumes the controller response or the guest libcd ISR that consumes it before invoking that callback | Delivery owner in `runtime/psx/guest_cd_stream_callback_layout.h`; drive service in `runtime/psx/cd_override.cpp`; guest IRQ delivery in `runtime/psx/hle_interrupt.cpp` | Direct titles declare the measured owner; legacy consumers retain host dispatch | `tests/test_cd_stream_callback.cpp` |
| CD, DMA, timers, input | Disc/CDC/XA, stock CD command/read ABIs, DMA callbacks/IRQs, emulated time, root counters, and SIO pad | `runtime/psx/cd_override.cpp`, `runtime/psx/cdc_native.cpp`, `runtime/psx/dma_callbacks.cpp`, `runtime/psx/emulated_time.cpp`, `runtime/psx/sio_pad.cpp` | The relevant device module, never the CPU executor | `docs/project-state.md` |
| GTE, GPU, MDEC, SPU | PSX coprocessor and media-device state/operations | `runtime/psx/gte_beetle.cpp`, `runtime/psx/gpu_beetle.cpp`, `runtime/psx/mdec_beetle.c`, `runtime/psx/spu_beetle.cpp` | Existing device owner or maintained Beetle fork | `vendor/beetle-psx/` |
| GTE vendor hooks | Required Beetle C ABI for disabled PGXP/NCLIP and unused savestate integration | `runtime/psx/gte_vendor_hooks.cpp` | Stateless vendor hooks; explicit projection provenance remains in `ProjPrim`, never a value-keyed PGXP cache | `runtime/psx/proj_prim.h` |
| Native projection math | Exact integer PSX affine/projection outputs and fractional native screen/depth projection | `runtime/psx/native_projection.*` | `project` owns fixed endpoint semantics; `project_view` shares fractional projection across endpoints and title-owned temporal sampling | `docs/presentation-contract.md` |
| Primitive submission | Construct resolved items through one emitter and select live census/unscoped observation or isolated admission at queue construction | `runtime/psx/render_submission.cpp`, `runtime/psx/render_queue.h` | Queue-local observation policy; title admission owners retain and reset their isolated queue | `RenderQueue::emitOrQueue` |
| Native renderer | Render queue, PSX/native draw paths, Vulkan/SDL_GPU presentation, and image output | `runtime/psx/render_queue.cpp`, `runtime/psx/gpu_native.cpp`, `runtime/psx/gpu_vk.cpp`, `runtime/psx/frame_presenter.cpp` | The narrow rendering owner under `runtime/psx/` | `docs/one-renderer.md` |
| Display time and pacing | Rational simulated fields/devices/IRQ, title-neutral host field count, and per-instance host deadlines with runtime-selected delivery ownership | `runtime/psx/timing.*`, `emulated_time.*`, `frame_pacer.*`, `game_runtime.*`; dispatch in `frame_presenter.cpp` | Timing advances simulated time and host field count without writing title RAM; title frame owners mirror measured guest counters; FramePacer owns host deadline; GameRuntime chooses combined delivery or waiting for already-delivered fields | `docs/presentation-contract.md` |
| Presentation enhancements | Widescreen projection, interpolation, composite policy, and title capabilities | `runtime/psx/guest_widescreen_projection.cpp`, `runtime/psx/fps60.cpp`, `runtime/psx/temporal_scene_source.h`, `runtime/psx/fps60_legacy_scene_source.cpp`, `runtime/psx/render_capabilities.h` | Fps60 owns isolated reconstruction/merge and cadence; title-owned TemporalSceneSource owns endpoints and exact producer membership; explicit GameHooks adapter owns existing capture chokes; never CPU selection | `docs/presentation-contract.md` |
| Audio output | SPU sample production, queue policy, and host playback | `runtime/psx/spu_audio.cpp`, `runtime/psx/audio_queue_policy.h` | Existing audio owner | `runtime/psx/spu_audio.h` |
| Input | Host events, controller mapping, and guest pad transport | `runtime/psx/pad_input.cpp`, `runtime/psx/sio_pad.cpp` | Existing input/device owner | `runtime/psx/pad_input.h` |
| Player UI | RmlUi lifetime, event routing, and componentized player controls | `runtime/ui/`, `runtime/psx/rmlui_overlay.cpp` | One component per responsibility under `runtime/ui/`; overlay keeps lifetime only | `docs/ui-architecture.md` |
| Configuration | CLI/environment/settings/runtime-diagnostic precedence, typed compare-run roles, enhancement suppression, and fallback-limit validation | `runtime/psx/config.cpp`, `runtime/psx/config_var.h`, `runtime/psx/config_vars.h`, `runtime/psx/diagnostic_run.h` | The configuration owner; no product CPU-engine setting and no title-local comparison parser | `docs/config.md` |
| Logging | Product diagnostic sink, formatting, filtering, and channels through Lucent | `vendor/lucent/`, call sites under `runtime/psx/` | Lucent for reusable behavior; call sites remain one line in the owning module | `vendor/lucent/README.md` |
| Game/framework seam | Title identity, immutable facts, frame/task factories, and host integration | `runtime/psx/game_runtime.cpp`, `runtime/psx/game_iface.cpp`, `runtime/psx/game.h` | Narrow typed fact groups under `runtime/psx/` | `runtime/psx/game_runtime.h` |
| Interactive inspection | Shared command parsing, bounded line input, and watchdog suspension scoped to each input wait | `runtime/psx/repl.cpp`, `runtime/psx/repl.h` | REPL input owns intentional idle; title drivers consume commands and frame budgets through the existing seam | `tests/test_repl_watchdog.cpp` |
| Framework tests | Hermetic production-seam tests and game-agnostic link proof | `tests/`, `tools/smoke/` | One focused test file per production owner; oracle code under `tools/oracle/` | `README.md` |
| Kernel syscall policy | Validate supported selectors before mutation, apply native critical-section semantics, and return a typed handled/refused result | `runtime/psx/kernel_syscall.cpp`, `runtime/cpu/execution_services.h` | Kernel selectors stay separate from CP0 exception mechanics in `runtime/psx/syscall_exception.cpp` | `tests/test_syscall_exception.cpp` |
| Nested CMake fixtures | Forward the configured dependency/toolchain inputs and own stable temporary build directories | `tests/cmake_fixture_paths.py`, `tests/CMakeLists.txt` | Shared fixture inputs and lifetimes stay here; individual tests own assertions | `tests/test_lightrec_dependency_ownership.py` |
| Portable consumer helpers | Shared consumer launch-environment, visual-check, and verification policy | `tools/port/` | Cohesive Python helpers under `tools/port/`; title wrappers declare data only | `tools/port/README.md` |
| Maintenance tooling | Whole-tree architecture policy, formatting, lint, source layout, diagnostics, and analysis commands | `tools/check_cpp_style.py`, `tools/repository_policy.py`, `tools/lint/`, `tools/fmv_export/` | Cohesive Python tool; reusable tools move to shared/re-harness | `AGENTS.md` |
| Tool process helpers | Bounded subprocess execution and scoped cleanup used by Python tools | `tools/automation/` | Reusable tool mechanics stay in this package | `tools/automation/process.py` |
| Binary formats | Neutral PS-X executable parsing | `tools/formats/` | Format parsing only; no product execution policy | `tools/formats/psx_exe.py` |
| MIPS analysis | Neutral instruction decoding and complete RAM disassembly coverage for binary evidence tools | `tools/mips/`, `tools/disasm.py` | Decoder semantics and explicit unknown-word refusal; no guest-source emission | `tools/disasm.md`, `tools/mips/decode.py` |
| Build and hosted verification | Framework library/dependency/test targets, exact Lightrec checkout, exact maintained-Lightning installed prefix, and canonical asset-free gate | `CMakeLists.txt`, `cmake/`, `tools/project.py`, `tools/build.py`, `tools/verify.py`, `.github/workflows/ci.yml`, `.github/actions/setup-linux/` | CMake owns targets; Python owns orchestration and installed-prefix validation; the shared setup action owns Linux CI package/dependency policy for framework and consumers under `build/deps/` | `README.md` |
| Host utilities | Title-neutral environment/process helpers | `common/` | Small cohesive module under `common/` | `common/env.h` |
| External source checkout | Existing psycross checkout used by historical development flows | `external/psycross/` | No new framework ownership; replace any live dependency with an explicit pinned resolver or remove it when unused | `external/psycross/README.md` |
| Workspace scripts | Python bootstrap, declared top-level submodule sync, and OpenBIOS helpers | `scripts/`; submodule inventory/update policy in `scripts/submodule_state.py` | Modular Python operations with thin command entry points; nested dependency gitlinks remain outside launcher sync | `docs/workspace/WORKSPACE.md` |
| Stale build trees | Disposable generated compiler output from prior verification | ignored `build-*/`, `build_*/` | No new work; clean with an explicit repository-scoped build cleanup tool | `AGENTS.md` |

## Where does new work go?

- Lightrec initialization or run-loop behavior -> runtime/cpu/lightrec_executor.*
- CPU register/cycle transfer -> runtime/cpu/lightrec_executor.* and the canonical state in runtime/psx/core.*
- A new host-visible execution stop -> runtime/cpu/execution_exit.*
- Resident/overlay identity or generation -> runtime/cpu/image_identity.*
- Native override or original-call semantics -> runtime/cpu/native_dispatch.*
- CPU/DMA/loader/debugger/savestate code invalidation -> runtime/cpu/invalidation.*
- Lightrec block-cache or executable-memory internals -> the pinned Lightrec fork, not psxport
- Interpreter semantics or independent state comparison -> tools/oracle/, never the gameplay library
- Comparison-run role and enhancement suppression -> runtime/psx/diagnostic_run.h and the configuration owner
- Fallback admission and telemetry -> runtime/cpu/fallback_policy.h and runtime/cpu/lightrec_executor.*
- PSX device behavior -> its existing `runtime/psx/` device owner
- Title-specific address, native body, or frame/task policy -> the consuming game repository
- Environment/settings parsing -> the configuration owner
- Logging behavior -> Lucent; one call site stays in the module that owns the event

## Source tree

```text
runtime/
  psx/          PSX platform, devices, host integration, rendering, audio, input, and configuration
  ui/           player-facing componentized UI
  cpu/          Lightrec executor boundary, typed exits, image identity, native calls, invalidation
common/         title-neutral host utilities
tests/          hermetic framework tests; target test-only oracle subtree
tools/
  oracle/       independent emulator/binary boundary tools
  port/         portable consumer launch, visual-check, and verification helpers
  lint/         framework policy checks
  smoke/        zero-title framework link fixture
  fmv_export/   focused FMV extraction/verification tool
cmake/          framework build composition
vendor/         pinned third-party/fork dependencies
external/       historical external checkout pending dependency audit
scripts/        Python workspace bootstrap and dependency helpers
build-*/        ignored stale build output; cleanup target, never source ownership
```
