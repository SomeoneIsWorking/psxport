# psxport

psxport is the reusable PlayStation half of native PC game ports. Its intended product combines
verified host-native functions and subsystems with a Lightrec dynamic recompiler for every remaining
MIPS R3000A instruction. The user's original game image is authenticated and consumed at runtime;
the shipped path does not generate or compile guest functions ahead of time.

This repository contains framework code only. It owns PSX CPU integration, GPU, SPU, GTE, MDEC, CD,
XA/FMV, BIOS/SDK HLE, rendering, input/platform services, and title-neutral verification seams. A game
repository supplies title identity, native overrides, frame/task policy, and the legally obtained game
files. psxport never includes game headers or title-specific addresses.

## Current status

The native platform owners and several title seams are already exercised by maintained consumers, but
the product CPU migration has not landed. The current tree still contains the offline MIPS-to-C
pipeline, generated-code dispatch interfaces, an in-library interpreter, and a runtime engine selector.
Those are open migration gaps, not the target architecture. See
[`docs/project-state.md`](docs/project-state.md) for the factual inventory and
[`docs/migration.md`](docs/migration.md) for the implementation order.

The settled target is:

- one maintained Lightrec revision pinned directly by psxport;
- one Lightrec state per live `Core`;
- native overrides keyed by authenticated image/module generation plus guest address;
- normal calls that honor overrides and scoped original calls that execute the guest body through
  Lightrec without recursion;
- explicit state synchronization, bounded executor exits, and executable-memory invalidation; and
- no interpreter or generated guest corpus in gameplay objects, links, selectors, UI, or fallbacks.

Lightrec owns its translated-block cache and executable memory. psxport does not duplicate those
mechanisms through `jit-common`; it owns only the PSX-specific integration around the embedded core.

## Framework boundary

The target CPU ownership is split under `runtime/cpu/`:

| Owner | Responsibility |
| --- | --- |
| Lightrec executor | One per-`Core` Lightrec lifetime and bounded execution |
| State bridge | GPR, HI/LO, PC/delay state, CP0, GTE, interrupt, and cycle synchronization |
| Code identity | Authenticated resident/module identity and load generation |
| Native dispatch | Image-scoped overrides and one-call original dispatch |
| Invalidation | CPU/DMA/loader/debugger/savestate executable-write notification |
| Execution exit | Typed budget, native/HLE, interrupt, frame, thread, and fault exits |

Existing console and host owners remain under `runtime/recomp/` until their responsibility-driven
moves are implemented. The complete current/target placement map is
[`docs/codemap.md`](docs/codemap.md).

## Interpreter and oracle policy

PSX is not choosing between an interpreter and a JIT. The gameplay executor is Lightrec. An
interpreter may exist only in a separately built test/oracle target and must not be present in the
`psxport` gameplay library or any consumer product. The product also cannot use Lightrec's internal
interpreter fallback; difficult or unsupported blocks must fail by name until the dynarec handles
them.

Independent emulator, hardware, binary, and test-only interpreter evidence may diagnose divergence.
Boot logos, menus, and FMV are checkpoints, not representative-gameplay conformance.

## Requirements

- Linux or macOS with CMake, Ninja, pkg-config, SDL3, zstd, zlib, Python, and a supported C/C++
  compiler.
- A Vulkan-capable GPU and driver for the current renderer.
- Legally obtained game files supplied by each consumer; no game data or BIOS is shipped here.

The direct Lightrec dependency and its exact maintained revision will be added by the migration. It
must be pinned through normal dependency metadata; local patch files and an indirectly bundled copy
inside the Beetle hardware backend are not acceptable product dependencies.

## Framework development

psxport is a library, not a standalone game. Current framework-only builds are configured explicitly;
agents use Clang and Ninja for verification:

```sh
CXX=clang++ cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Do not interpret this command as proof that the dynamic product exists today. During the plans-first
migration, use only documentation validation. Once implementation begins, focused tests cover the
production executor seams and the combined gate runs after the semantic batch is frozen.

Consumers provide the runnable product and user-facing launcher. The launcher must authenticate the
user's game image and start the Lightrec/native hybrid without Ghidra, an offline translator, a
generated corpus, or an engine-selection flag.

## Contributing

Read [`AGENTS.md`](AGENTS.md), then consult the project information system before non-trivial work.
The critical rules are:

- fix the owning cause rather than a title-address symptom;
- keep psxport game-agnostic and split new CPU work across cohesive owners;
- use one Lucent logging boundary and one typed configuration owner;
- never add a gameplay interpreter, engine selector, generated-code dependency, or silent fallback;
- preserve valid binary/runtime facts, but do not use generated C as the new reference; and
- verify representative interactive gameplay before claiming a title migrated.

## License

The framework code is provided for research and preservation. The vendored Beetle PSX backend is
GPL-2.0; see its tree for details. Lightrec integration must retain the selected upstream/fork
license and provenance. No copyrighted game assets, ROMs, disc images, executables, or BIOS files are
included or distributed.
