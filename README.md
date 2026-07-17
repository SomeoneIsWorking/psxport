# psxport

A **game-agnostic framework for porting PlayStation (PSX) games to native PC** by static
recompilation plus a native hybrid runtime.

psxport statically recompiles a PSX game's MIPS R3000A machine code into native C, then runs it
under a native platform layer — so the port behaves like a PC program, not an emulator. On top of
that substrate, a game repo reimplements the game's own engine in native C++, growing ownership
function by function and verifying each step for byte-exactness against the recompiled reference.

This repository is the **reusable framework only** — it carries no game code and `#include`s nothing
from a game. It is consumed as a submodule by a game repository; the reference consumer is
[**Tomba2Engine**](https://github.com/SomeoneIsWorking/Tomba2Engine), a native reimplementation of
*Tomba! 2*.

---

## What it provides

- **A MIPS→C static recompiler** (`tools/recomp/`) — translates a PSX executable into emitted C
  (`shard_*.c`), the "substrate" the port runs on. The generated code is sacrosanct: mistranslations
  are fixed in the recompiler, never by hand-editing the output.
- **A PSX platform layer** (`runtime/recomp/`) — native implementations of the hardware a PSX game
  needs: GPU, SPU (audio), GTE (geometry), MDEC (video), CD/XA/FMV, and BIOS/SDK HLE. The
  GTE/MDEC/SPU/CHD hardware backend comes from a vendored **beetle-psx** fork (a nested submodule).
- **A side-by-side (SBS) differential-compare harness** — runs the native path and the recompiled
  reference in lockstep and byte-compares guest state each frame, so any divergence in the port is
  caught immediately.
- **A native (SDL_GPU) renderer** — the drawing backend for both the substrate's PSX render path and
  a game's own native renderer.
- **The `psxport` static library** — everything above, linkable into a game.

### The game seam

The framework reaches a game **only** through a small interface, so it stays game-agnostic:

- `runtime/recomp/game_iface.h` — `GameConfig` (guest-address literals + config), `GameHooks` (a
  behavior vtable the framework calls into), and an opaque `void* Core::gameCtx` (the game's own
  context).
- `runtime/recomp/recomp_iface.h` — the `RecompRegistry` for the generated substrate.

The `psxport_smoke` target links the library against a zero-game stub to prove the framework builds
and links with **no game symbols**.

---

## Requirements

- **Linux:** `cmake`, `pkg-config`, `SDL3`, `libzstd`, `zlib`, `python3`, a C/C++ toolchain.
- **macOS:** `brew install cmake pkg-config sdl3 zstd zlib python3`
- A **Vulkan-capable GPU + drivers**.

## Building

psxport is a library, not a runnable game — the build produces the framework artifacts (the static
library, the `psxport_smoke` link test, and the `discdump` CHD tool):

```bash
git clone --recursive https://github.com/SomeoneIsWorking/psxport.git
cd psxport
cmake -S . -B build
cmake --build build --target psxport        # the static library
cmake --build build --target psxport_smoke  # agnosticism proof (needs a consumer's generated/ headers)
```

To actually run a game, use a consumer repo (e.g. Tomba2Engine), which vendors psxport as a submodule
and supplies the game code + the disc image.

**Porting a new PSX game:** see `docs/porting-a-new-psx-game.md` and `docs/port-framework.md`.

---

## Project structure

```
runtime/recomp/   the PSX platform + substrate glue: MIPS interp, dispatch, HLE/boot, GPU/SPU/GTE/
                  MDEC/CD natives, the CD/XA/FMV subsystems, the SBS harness, and the game_iface seam
tools/            the MIPS→C recompiler (tools/recomp/) + framework tooling
                  (abi_extract.py, decomp.sh, dbgclient.py, disasm.py, …)
common/           shared host utilities (filesystem, env, …)
vendor/           beetle-psx (nested submodule): GTE/MDEC/SPU/CHD hardware backend
cmake/            build definitions (psxport.cmake)
bios/  assets/    supply-your-own inputs (BIOS, etc.) — never shipped
docs/             framework architecture, config channels, ABI/port-framework notes
tests/            framework tests
```

---

## Contributing

psxport is the framework half of a reverse-engineering + reimplementation effort; the full working
rules are in [`CLAUDE.md`](CLAUDE.md). In short:

- **Keep it game-agnostic.** No game headers, no game-specific addresses. The game is reached only
  through `game_iface.h` (`GameConfig` / `GameHooks` / `gameCtx`) and the `RecompRegistry`;
  `psxport_smoke` must keep linking with zero game symbols.
- **The generated substrate is sacrosanct** — fix mistranslations in the recompiler, not by hand-
  editing emitted `shard_*.c`.
- **Reverse-engineer first** — decompile and understand before reimplementing (`tools/decomp.sh`,
  Ghidra headless). Reproduce the observable result, not the PSX mechanism.
- **No bandaids** — fix root causes; no magic constants, no swallowed errors, no fake-sync stopgaps.
  All I/O and timing is PC-native and synchronous, or it fails fast with a diagnostic.
- **Diagnostics go through the `cfg` channel logger** (`PSXPORT_DEBUG=chan,chan`; see `docs/config.md`),
  never raw `getenv` or scattered prints.

---

## License

The framework code is provided as-is for research and preservation. The vendored **beetle-psx**
backend (`vendor/beetle-psx`) is **GPL-2.0**; see its tree for details. No game assets, ROMs, disc
images, or BIOS files are included or distributed — supply your own legally-obtained files.
