# Tomba2Engine — a PC-native reimplementation of Tomba! 2's engine

A **real, native PC port** of the PlayStation game *Tomba! 2: The Evil Swine Return*. The
game's original MIPS R3000A machine code is statically recompiled to native C; on top of
that, **Tomba! 2's own game engine is being reimplemented in native C** — the main loop,
entity-list iteration, per-object cull/LOD, render submission, camera, and 60fps
interpolation + widescreen.

**This is not an emulator.** There is no PSX-on-PC interpreter running the whole game. The
**engine** is native PC code; the **gameplay logic** (per-entity AI, physics, game rules)
stays as recompiled PSX code in PSX guest memory, which the native engine reads from and
calls into. The hardware a PSX game needs — geometry math, video decode, audio, disc I/O,
the BIOS — is provided as native modules.

The reusable PSX→PC framework underneath (recompiler + PSX hardware natives + BIOS HLE +
diff harness) is being separated out, N64Recomp-style, into its own submodule so future
game ports can reuse it; **Tomba2Engine** is the game repo on top.

> **Status: playable in-engine; native-engine reimplementation underway.** The recompiled
> core boots and runs gameplay (HLE BIOS, native CD/file I/O, cooperative scheduler on real
> threads, GTE geometry, GPU/MDEC/SPU, FMV, XA audio, BGM). Current work: lifting the
> engine layer (object manager + render submission) to native C, validated per-function
> against the recompiled body as an oracle. See `docs/engine_re.md` and `docs/journal.md`.

---

## Quickstart

You supply your own, legally-obtained Tomba! 2 disc image as a **CHD**. One command builds
everything from the repo and that disc, then launches it:

```sh
./run.sh /path/to/Tomba2.chd
```

You can also point it at the disc without an argument, via any of:

- `PSXPORT_TOMBA2_DISC=/path/to/Tomba2.chd` in your environment,
- a `PSXPORT_TOMBA2_DISC=...` (or `PSXPORT_DISC=...`) line in a `.env` file at the repo root,
- or simply dropping a `*.chd` file next to `run.sh`.

`run.sh` is fully end-to-end: it builds the CHD tooling, extracts `MAIN.EXE` from your
disc, statically recompiles the game core to C, compiles the native runtime, and opens a
window — no manual steps.

### One-time dependencies

```sh
# macOS
brew install cmake sdl2 zstd zlib python3

# Linux (Debian/Ubuntu)
sudo apt install cmake libsdl2-dev libzstd-dev zlib1g-dev python3 build-essential

# Linux (Fedora)
sudo dnf install cmake SDL2-devel libzstd-devel zlib-devel python3 gcc gcc-c++
```

### Environment knobs

| Variable | Effect |
| --- | --- |
| `PSXPORT_NOAUDIO=1` | Mute — skip the SDL audio sink. |
| `PSXPORT_NOWINDOW=1` | Headless run (no SDL window). |
| `PSXPORT_GPU_DUMP=<dir>` | Dump rendered frames as PPM images into `<dir>`. |
| `CC=clang` / `CC=gcc` | Override the C compiler. |

---

## How it works

PSX games are a good fit for static recompilation: the CPU and GTE do all the geometry,
and the GPU only ever sees flat 2D primitives. So most of the game is just MIPS integer
code that can be translated to C, with a handful of hardware subsystems behind it.

### The recompiler (`tools/recomp/`)

A static recompiler (`tools/recomp/emit.py`, plus the `decode.py` / `psexe.py` front end)
translates `MAIN.EXE` into native C — **one C function per game function**, all operating
on a shared modeled R3000A CPU state and a flat memory image (`runtime/recomp/r3000.h`,
`mem.c`).

- **Binary-only function discovery.** Functions are discovered purely from the binary:
  starting at the entry point and following direct calls to a fixpoint. No Ghidra output
  or other decompiler-derived data is required to build — just the repo and your disc.
- **Hybrid interpreter fallback.** Code reached only indirectly (through function
  pointers), and overlay code loaded from disc at runtime above the resident image, can't
  be statically recompiled ahead of time. Those run through a fallback interpreter
  (`runtime/recomp/interp.c`) that uses the *same* instruction semantics as the emitter,
  so interpreted code behaves identically to recompiled code.

The generated C is treated as sacrosanct output — never hand-edited. To change it, you fix
the emitter and regenerate.

### Native subsystems (`runtime/recomp/`)

Everything the recompiled game calls into is native:

- **HLE BIOS** (`hle.c`) — high-level emulation of the PSX kernel calls the game uses:
  heap, events, the A0/B0/C0 vector tables, etc. No real PSX BIOS image is run.
- **Native CD / file I/O** (`disc.c`, `cd_override.c`) — the disc is read directly from
  the CHD by LBA via **libchdr**, and the game's CD/streaming functions are replaced with
  native, synchronous file reads. There is no emulated CD controller and no IRQ handshake.
- **Native BIOS threads** (`threads.c`) — the game's cooperative scheduler runs real BIOS
  threads. Each PSX thread gets its own native stack via **ucontext**; thread switches are
  real `swapcontext` calls. This is the genuinely hard part of statically recompiling a
  threaded title, and it's implemented natively rather than simulated.
- **GTE / MDEC / SPU** (`gte_beetle.c`, `mdec_beetle.c`, `spu_beetle.c`) — the geometry
  coprocessor, video decoder, and sound unit are **lifted from the GPL-2 Beetle PSX fork**
  and compiled in as native modules (each via a thin adapter over the upstream C source).
  These are faithful implementations of well-defined hardware, used as native libraries.
- **From-scratch native GPU rasterizer** (`gpu_native.c`) — *not* GPU emulation. The game
  submits its draw work as a GP0 command stream (via GPU DMA walking ordering-table linked
  lists); we parse that stream and rasterize it with our own native renderer to an SDL
  window (or PPM dumps). Because the renderer is ours, native widescreen / higher
  resolution / 60 fps are natural future extensions.
- **Native VSync / event model** (`timing.c`, `sync_overrides.c`) — the BIOS event and
  VSync surface is provided natively, and the game's hardware busy-waits (vblank
  pace-dwells, CD timeout spins) are converted into non-stalling native behavior instead
  of being satisfied by faked interrupts.

---

## Requirements and platforms

- **Platforms:** macOS and Linux.
- **Dependencies:** as listed in [Quickstart](#one-time-dependencies) — CMake, SDL2,
  zstd, zlib, Python 3, and a C/C++ toolchain.
- **A disc image you own.** You must supply your own, legally-obtained Tomba! 2 disc as a
  CHD. **No game assets are included in this repository, and none are distributable.** The
  port is the translation engine plus native runtime — it is useless (and ships nothing)
  without your own copy of the game.

---

## Licensing

The native runtime builds on a fork of **Beetle PSX (GPL-2)** — vendored as the
`vendor/beetle-psx` submodule — from which the GTE, MDEC, and SPU modules are lifted. The
port is therefore distributable as source under GPL-2.

**Game assets are never shipped.** No ROMs, disc images, or executables extracted from the
game live in this repository or its history, and they never will. See `CLAUDE.md` for the
full project rules.

---

## Repository layout

| Path | Contents |
| --- | --- |
| `runtime/` | The native PC port runtime; `runtime/recomp/` holds the modeled CPU, HLE BIOS, native CD/threads/GPU/GTE/MDEC/SPU, and the hybrid interpreter. |
| `tools/recomp/` | The static recompiler (MIPS R3000A → C): `emit.py` (emitter), `decode.py` (decoder), `psexe.py` (PS-X EXE loader). |
| `tools/` | Offline tooling, including `discdump` (CHD → files via libchdr). |
| `vendor/beetle-psx` | The GPL-2 Beetle PSX fork (submodule) the native modules are lifted from. |
| `common/` | Shared helpers, including the `.env` / disc-path reader. |
| `patches/` | Per-game patch sources and built patch files. |
| `docs/journal.md` | The full progress log and findings — the source of truth for current state. |

---

## Status

Honest snapshot of where the port is today.

**Working:**

- The recompiler translates `MAIN.EXE` to native C (binary-only discovery; ~1150 functions
  recompiled, the indirectly-reached remainder run via the hybrid interpreter).
- The core **boots and runs the full game engine**: HLE BIOS init, native CD init and
  by-LBA file reads, the cooperative scheduler running tasks on real native BIOS threads,
  loading and executing the first code overlay (`START.BIN`) through the hybrid
  interpreter, and the StrPlayer main loop executing per-frame work.
- GTE geometry, the native GPU rasterizer, MDEC, and SPU audio are all integrated and
  unit-verified (e.g. GTE projection is bit-correct on a known RTPS case; the rasterizer's
  fill/gouraud/geometry are pixel-correct).

**Not yet working / in progress:**

- **No visible in-game frame yet.** The intro FMV path is never reached: the game inits
  MDEC, but the interpreted intro overlay stalls before chaining to load and play the
  logo, so the streaming CD → MDEC → display path is never fed. This is the deep
  intro-sequencing blocker, and it needs further reverse-engineering of what the sequencer
  is waiting on. MDEC and SPU are wired and ready but, until the intro streams, are
  unexercised end-to-end.

We are deliberately not overclaiming: the engine runs, but you will not see Tomba! on
screen yet. Track progress in `docs/journal.md`.
