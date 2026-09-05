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
| Bounded execution results | Typed budget, override, HLE/device, interrupt/exception, frame, thread, and fault exits | `runtime/cpu/execution_exit.*`, `runtime/cpu/execution_control.*` | Add a typed reason/result here | `docs/migration.md` |
| Code identity | Authenticate resident/module bytes and assign load generations | `runtime/cpu/image_identity.*`, `runtime/psx/overlay_glue.cpp` | The image catalog and title-owned identity policy | `runtime/cpu/image_identity.h` |
| Native/original dispatch | Resolve image-generation-plus-address overrides and scoped original calls | `runtime/cpu/native_dispatch.*`, `runtime/cpu/guest_call.*` | The per-Core dispatcher; title policy remains in the consumer | `runtime/cpu/native_dispatch.h` |
| Executable invalidation | Normalize executable writes and revoke translated blocks plus captured dispatch decisions | `runtime/cpu/invalidation.*` | Central invalidation owner | `docs/issues/0050-centralize-lightrec-code-invalidation.md` |
| Lightrec cache/code memory | Translated-block cache, chaining, executable-memory allocation/publication, and teardown | Exact pinned checkout selected by `PSXPORT_LIGHTREC_DIR` | The maintained Lightrec fork; psxport only owns its adapter | `docs/migration.md` |
| Test oracle | Independent guest execution and device evidence for focused tests only | `tools/oracle/` | Separately built test targets; never product linkage | `docs/oracle.md` |
| Guest memory and hardware bus | Address normalization, RAM/scratchpad access, MMIO dispatch, and watchpoints | `runtime/psx/mem.cpp`, `runtime/psx/io_peripherals.cpp` | Small cohesive modules under `runtime/psx/` until a responsibility-driven platform move | `runtime/psx/io_peripherals.h` |
| BIOS/SDK HLE | Sony BIOS/libc/work-area/interrupt and measured SDK service boundaries | `runtime/psx/hle.cpp`, `runtime/psx/bios_interrupt.cpp`, `runtime/psx/bios_libc_string.cpp`, `runtime/psx/platform_hle.h` | The smallest existing HLE owner; new guest execution enters through bounded exits | `docs/faithful-execution.md` |
| CD, DMA, timers, input | Disc/CDC/XA, DMA callbacks/IRQs, emulated time, root counters, and SIO pad | `runtime/psx/cd_override.cpp`, `runtime/psx/cdc_native.cpp`, `runtime/psx/dma_callbacks.cpp`, `runtime/psx/emulated_time.cpp`, `runtime/psx/sio_pad.cpp` | The relevant device module, never the CPU executor | `docs/project-state.md` |
| GTE, GPU, MDEC, SPU | PSX coprocessor and media-device state/operations | `runtime/psx/gte_beetle.cpp`, `runtime/psx/gpu_beetle.cpp`, `runtime/psx/mdec_beetle.c`, `runtime/psx/spu_beetle.cpp` | Existing device owner or maintained Beetle fork | `vendor/beetle-psx/` |
| Native renderer | Render queue, PSX/native draw paths, Vulkan/SDL_GPU presentation, and image output | `runtime/psx/render_queue.cpp`, `runtime/psx/gpu_native.cpp`, `runtime/psx/gpu_vk.cpp`, `runtime/psx/frame_presenter.cpp` | The narrow rendering owner under `runtime/psx/` | `docs/one-renderer.md` |
| Presentation enhancements | Widescreen projection, interpolation, composite policy, and title capabilities | `runtime/psx/guest_widescreen_projection.cpp`, `runtime/psx/fps60.cpp`, `runtime/psx/render_capabilities.h` | Existing presentation policy owner; never CPU selection | `docs/presentation-contract.md` |
| Audio output | SPU sample production, queue policy, and host playback | `runtime/psx/spu_audio.cpp`, `runtime/psx/audio_queue_policy.h` | Existing audio owner | `runtime/psx/spu_audio.h` |
| Input | Host events, controller mapping, and guest pad transport | `runtime/psx/pad_input.cpp`, `runtime/psx/sio_pad.cpp` | Existing input/device owner | `runtime/psx/pad_input.h` |
| Player UI | RmlUi lifetime, event routing, and componentized player controls | `runtime/ui/`, `runtime/psx/rmlui_overlay.cpp` | One component per responsibility under `runtime/ui/`; overlay keeps lifetime only | `docs/ui-architecture.md` |
| Configuration | CLI/environment/settings/runtime-diagnostic precedence, typed compare-run roles, enhancement suppression, and fallback-limit validation | `runtime/psx/config.cpp`, `runtime/psx/config_var.h`, `runtime/psx/config_vars.h`, `runtime/psx/diagnostic_run.h` | The configuration owner; no product CPU-engine setting and no title-local comparison parser | `docs/config.md` |
| Logging | Product diagnostic sink, formatting, filtering, and channels through Lucent | `vendor/lucent/`, call sites under `runtime/psx/` | Lucent for reusable behavior; call sites remain one line in the owning module | `vendor/lucent/README.md` |
| Game/framework seam | Title identity, immutable facts, frame/task factories, and host integration | `runtime/psx/game_runtime.cpp`, `runtime/psx/game_iface.cpp`, `runtime/psx/game.h` | Narrow typed fact groups under `runtime/psx/` | `runtime/psx/game_runtime.h` |
| Framework tests | Hermetic production-seam tests and game-agnostic link proof | `tests/`, `tools/smoke/` | One focused test file per production owner; oracle code under tests/oracle/ | `README.md` |
| Nested CMake fixtures | Forward the configured dependency/toolchain inputs and own stable temporary build directories | `tests/cmake_fixture_paths.py`, `tests/CMakeLists.txt` | Shared fixture inputs and lifetimes stay here; individual tests own assertions | `tests/test_lightrec_dependency_ownership.py` |
| Independent oracle | Beetle/Mednafen trace and boundary comparison tools | `tools/oracle/` | Test tooling only; never gameplay linkage | `docs/oracle.md` |
| Portable consumer helpers | Shared consumer launch-environment, visual-check, and verification policy | `tools/port/` | Cohesive Python helpers under `tools/port/`; title wrappers declare data only | `tools/port/README.md` |
| Maintenance tooling | Whole-tree architecture policy, formatting, lint, source layout, diagnostics, and analysis commands | `tools/check_cpp_style.py`, `tools/repository_policy.py`, `tools/lint/`, `tools/fmv_export/` | Cohesive Python tool; reusable tools move to shared/re-harness | `AGENTS.md` |
| Tool process helpers | Bounded subprocess execution and scoped cleanup used by Python tools | `tools/automation/` | Reusable tool mechanics stay in this package | `tools/automation/process.py` |
| Binary formats | Neutral PS-X executable parsing | `tools/formats/` | Format parsing only; no product execution policy | `tools/formats/psx_exe.py` |
| MIPS analysis | Neutral instruction decoding for binary evidence tools | `tools/mips/` | Decoder semantics only; no guest-source emission | `tools/mips/decode.py` |
| Build and hosted verification | Framework library/dependency/test targets, exact Lightrec checkout, exact maintained-Lightning installed prefix, and canonical asset-free gate | `CMakeLists.txt`, `cmake/`, `tools/project.py`, `tools/build.py`, `tools/verify.py`, `.github/workflows/ci.yml` | CMake owns targets; Python owns orchestration and validates the installed prefix; workflows only select pinned inputs and the proven host/toolchain | `README.md` |
| Host utilities | Title-neutral environment/process helpers | `common/` | Small cohesive module under `common/` | `common/env.h` |
| External source checkout | Existing psycross checkout used by historical development flows | `external/psycross/` | No new framework ownership; replace any live dependency with an explicit pinned resolver or remove it when unused | `external/psycross/README.md` |
| Workspace scripts | Python bootstrap, submodule, and OpenBIOS helpers | `scripts/` | Modular Python operations with thin command entry points | `AGENTS.md` |
| Stale build trees | Disposable generated compiler output from prior verification | ignored `build-*/`, `build_*/` | No new work; clean with an explicit repository-scoped build cleanup tool | `AGENTS.md` |

## Where does new work go?

- Lightrec initialization or run-loop behavior -> runtime/cpu/lightrec_executor.*
- CPU register/cycle transfer -> runtime/cpu/lightrec_executor.* and the canonical state in runtime/psx/core.*
- A new host-visible execution stop -> runtime/cpu/execution_exit.*
- Resident/overlay identity or generation -> runtime/cpu/image_identity.*
- Native override or original-call semantics -> runtime/cpu/native_dispatch.*
- CPU/DMA/loader/debugger/savestate code invalidation -> runtime/cpu/invalidation.*
- Lightrec block-cache or executable-memory internals -> the pinned Lightrec fork, not psxport
- Interpreter semantics or independent state comparison -> tests/oracle/, never the gameplay library
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
