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
| Product composition | Construct title-neutral runtime owners and run the host loop | `runtime/recomp/native_boot.cpp` | Keep orchestration thin; CPU implementation moves to runtime/cpu/ | `runtime/recomp/game_runtime.h` |
| Canonical PSX state | Per-`Core` registers, RAM/scratchpad, devices, and game association | `runtime/recomp/core.h`, `runtime/recomp/core.cpp` | `Core` owns the composed CPU executor but not its implementation | `runtime/recomp/r3000.h` |
| Lightrec lifetime | One dynarec-only Lightrec state and callback context per `Core` | No product owner | runtime/cpu/lightrec_executor.* | `docs/migration.md` |
| Architectural-state bridge | Synchronize GPR, HI/LO, PC/delay state, CP0, GTE, interrupt, and cycle state at every host boundary | No product owner | runtime/cpu/state_bridge.* | `docs/migration.md` |
| Bounded execution results | Typed budget, override, HLE/device, interrupt/exception, frame, thread, and fault exits | Generated-call and exception behavior is spread through `runtime/recomp/` | runtime/cpu/execution_exit.* | `docs/migration.md` |
| Code identity | Authenticate resident/module bytes and assign load generations | `runtime/recomp/guest_program_image.h`, `runtime/recomp/overlay_router.cpp` | runtime/cpu/code_identity.* | `runtime/recomp/overlay_router.h` |
| Native/original dispatch | Resolve image-generation-plus-address overrides and scoped original calls | `runtime/recomp/dispatch.cpp`, `runtime/recomp/override_registry.cpp`, `runtime/recomp/guest_call.h` | runtime/cpu/native_dispatch.* | `docs/issues/0049-replace-generated-symbol-calls-with-image-scoped-dispatch.md` |
| Executable invalidation | Normalize executable writes and revoke translated blocks plus captured dispatch decisions | Device-specific calls are distributed across `runtime/recomp/` | runtime/cpu/invalidation.* | `docs/issues/0050-centralize-lightrec-code-invalidation.md` |
| Lightrec cache/code memory | Translated-block cache, chaining, executable-memory allocation/publication, and teardown | No direct product dependency | The pinned Lightrec repository/fork | `../../shared/jit-common/docs/migration.md` |
| Test interpreter | Independent MIPS execution for focused oracle tests only | `runtime/recomp/interp.cpp`, `runtime/recomp/interp_diagnostics.cpp` | tests/oracle/ as a separate non-product target | `docs/oracle.md` |
| Guest memory and hardware bus | Address normalization, RAM/scratchpad access, MMIO dispatch, and watchpoints | `runtime/recomp/mem.cpp`, `runtime/recomp/io_peripherals.cpp` | Small cohesive modules under `runtime/recomp/` until a responsibility-driven platform move | `runtime/recomp/io_peripherals.h` |
| BIOS/SDK HLE | Sony BIOS/libc/work-area/interrupt and measured SDK service boundaries | `runtime/recomp/hle.cpp`, `runtime/recomp/bios_interrupt.cpp`, `runtime/recomp/bios_libc_string.cpp`, `runtime/recomp/platform_hle.h` | The smallest existing HLE owner; new guest execution enters through bounded exits | `docs/faithful-execution.md` |
| CD, DMA, timers, input | Disc/CDC/XA, DMA callbacks/IRQs, emulated time, root counters, and SIO pad | `runtime/recomp/cd_override.cpp`, `runtime/recomp/cdc_native.cpp`, `runtime/recomp/dma_callbacks.cpp`, `runtime/recomp/emulated_time.cpp`, `runtime/recomp/sio_pad.cpp` | The relevant device module, never the CPU executor | `docs/project-state.md` |
| GTE, GPU, MDEC, SPU | PSX coprocessor and media-device state/operations | `runtime/recomp/gte_beetle.cpp`, `runtime/recomp/gpu_beetle.cpp`, `runtime/recomp/mdec_beetle.c`, `runtime/recomp/spu_beetle.cpp` | Existing device owner or maintained Beetle fork | `vendor/beetle-psx/` |
| Native renderer | Render queue, PSX/native draw paths, Vulkan/SDL_GPU presentation, and image output | `runtime/recomp/render_queue.cpp`, `runtime/recomp/gpu_native.cpp`, `runtime/recomp/gpu_vk.cpp`, `runtime/recomp/frame_presenter.cpp` | The narrow rendering owner under `runtime/recomp/` | `docs/one-renderer.md` |
| Presentation enhancements | Widescreen projection, interpolation, composite policy, and title capabilities | `runtime/recomp/guest_widescreen_projection.cpp`, `runtime/recomp/fps60.cpp`, `runtime/recomp/render_capabilities.h` | Existing presentation policy owner; never CPU selection | `docs/presentation-contract.md` |
| Audio output | SPU sample production, queue policy, and host playback | `runtime/recomp/spu_audio.cpp`, `runtime/recomp/audio_queue_policy.h` | Existing audio owner | `runtime/recomp/spu_audio.h` |
| Input | Host events, controller mapping, and guest pad transport | `runtime/recomp/pad_input.cpp`, `runtime/recomp/sio_pad.cpp` | Existing input/device owner | `runtime/recomp/pad_input.h` |
| Player UI | RmlUi lifetime, event routing, and componentized player controls | `runtime/ui/`, `runtime/recomp/rmlui_overlay.cpp` | One component per responsibility under `runtime/ui/`; overlay keeps lifetime only | `docs/ui-architecture.md` |
| Configuration | CLI/environment/settings/runtime-diagnostic precedence, validation, and typed immutable values | `runtime/recomp/config.cpp`, `runtime/recomp/config_var.h`, `runtime/recomp/config_vars.h` | The configuration owner; no product CPU-engine setting | `docs/config.md` |
| Logging | Product diagnostic sink, formatting, filtering, and channels through Lucent | `vendor/lucent/`, call sites under `runtime/recomp/` | Lucent for reusable behavior; call sites remain one line in the owning module | `vendor/lucent/README.md` |
| Game/framework seam | Title identity, immutable facts, frame/task factories, and compatibility projection | `runtime/recomp/game_runtime.cpp`, `runtime/recomp/game_iface.cpp`, `runtime/recomp/legacy_game_config.h`, `runtime/recomp/legacy_game_hooks.h` | Narrow typed fact group under `runtime/recomp/`; compatibility bags do not grow | `docs/plans/game-seam-redesign.md` |
| Framework tests | Hermetic production-seam tests and game-agnostic link proof | `tests/`, `tools/smoke/` | One focused test file per production owner; oracle code under tests/oracle/ | `README.md` |
| Independent oracle | Beetle/Mednafen trace and boundary comparison tools | `tools/oracle/` | Test tooling only; never gameplay linkage | `docs/oracle.md` |
| Static translation removal | Current decoder/emitter and generated-code inspection tools pending deletion after dynamic conformance | `tools/recomp/`, `tools/abi_extract.py`, `tools/port_gen.py`, `tools/port_check.py` | No new work; replace callers with runtime/binary/oracle seams | `docs/migration.md` |
| Portable launch helpers | Shared consumer launch-environment and visual-check policy | `tools/port/` | Cohesive Python helpers under `tools/port/` | `tools/port/README.md` |
| Maintenance tooling | Formatting, lint, source-layout, diagnostics, and analysis commands | `tools/`, `tools/lint/`, `tools/fmv_compare/`, `tools/fmv_export/` | Cohesive Python tool; reusable tools move to shared/re-harness | `AGENTS.md` |
| Build composition | Framework library, dependency, tool, and test targets | `CMakeLists.txt`, `cmake/`, `tools/CMakeLists.txt` | Existing CMake owner; gameplay and oracle targets remain disjoint | `README.md` |
| Host utilities | Title-neutral environment/process helpers | `common/` | Small cohesive module under `common/` | `common/env.h` |

## Where does new work go?

- Lightrec initialization or run-loop behavior -> runtime/cpu/lightrec_executor.*
- CPU register/cycle transfer -> runtime/cpu/state_bridge.*
- A new host-visible execution stop -> runtime/cpu/execution_exit.*
- Resident/overlay identity or generation -> runtime/cpu/code_identity.*
- Native override or original-call semantics -> runtime/cpu/native_dispatch.*
- CPU/DMA/loader/debugger/savestate code invalidation -> runtime/cpu/invalidation.*
- Lightrec block-cache or executable-memory internals -> the pinned Lightrec fork, not psxport or
  `jit-common`
- Interpreter semantics or comparison diagnostics -> tests/oracle/, never the gameplay library
- PSX device behavior -> its existing `runtime/recomp/` device owner
- Title-specific address, native body, or frame/task policy -> the consuming game repository
- Environment/settings parsing -> the configuration owner
- Logging behavior -> Lucent; one call site stays in the module that owns the event

## Source tree

```text
runtime/
  recomp/       current PSX platform, host integration, generated dispatch, and interpreter
  ui/           player-facing componentized UI
  cpu/          target Lightrec integration owners (created by implementation, not by this doc pass)
common/         title-neutral host utilities
tests/          hermetic framework tests; target test-only oracle subtree
tools/
  oracle/       independent emulator/binary boundary tools
  port/         portable consumer launch and visual-check helpers
  recomp/       current offline translator pending removal
  lint/         framework policy checks
  smoke/        zero-title framework link fixture
  fmv_compare/  focused FMV comparison tool
  fmv_export/   focused FMV extraction/verification tool
cmake/          framework build composition
vendor/         pinned third-party/fork dependencies
```
