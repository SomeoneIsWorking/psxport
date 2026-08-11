# psxport — a PSX→PC static-recompilation framework

This repo is the **game-agnostic framework** extracted from the Tomba!2 port: the static recompiler
(`tools/recomp/`), the `runtime/recomp/` substrate (MIPS interp + Beetle GTE/MDEC/SPU backends + the
native SDL_GPU renderer + the SBS differential harness + BIOS/SDK HLE), and the `psxport` STATIC library
a game links. It carries **no game code** — the framework `#include`s nothing from a game; a game provides
`GameConfig` + `GameHooks` (`runtime/recomp/game_iface.h`) + the recompiled substrate (`generated/`) and
links `libpsxport`. The `psxport_smoke` target proves agnosticism (links libpsxport with a stub, zero game
symbols).

**This file is the authority for how a game consumes psxport** — and, because every game vendors this
repo as `external/psxport`, everything in `docs/` reaches every game tree and every subagent working
in one. That is why the cross-repo material lives HERE rather than in a workspace directory that only
exists on one machine:

| in this repo | what it is |
|---|---|
| `docs/workspace/WORKSPACE.md` | the workspace map: which trees exist, which framework checkout is writable, how a game builds against in-progress framework work |
| `docs/workspace/PROTOCOL.md` | the multi-agent protocol (area claims) + the standing USER rules |
| `docs/workspace/LAYOUT.md` | the target directory organization for every tree |
| `docs/plans/*.md` | designs not yet implemented, each stating what is measured vs assumed |
| `scripts/bootstrap-workspace.sh` | reproduces the whole workspace from a clone of this repo |

A game's own specifics stay in that repo's `CLAUDE.md`.

- **Consuming games:** Tomba2Engine (the reference consumer), spyro, spider1 — each vendors this repo
  as `external/psxport`.
- **Porting a NEW game:** see `docs/porting-a-new-psx-game.md` + `docs/port-framework.md`.

## Where the framework source comes from — and who may edit it

**Framework edits happen in ONE checkout: the workspace's dev clone (`$PSX/psxport`).** A game's
`external/psxport` submodule is a **read-only pinned consumer** — `git checkout <pin>` territory only.
Never edit, commit, or push inside a game's `external/psxport`.

A game builds against in-progress framework work without touching its submodule:

```sh
cmake -S . -B build -DPSXPORT_DIR=$PSX/psxport      # or: PSXPORT_DIR=$PSX/psxport ./run.sh
```

`PSXPORT_DIR` defaults to `external/psxport`, so a fresh clone of a game repo alone still builds — that
property is what "each game is its own project using psxport as the framework" means, and it stays
testable. `run.sh` ANNOUNCES which checkout a run was built from (and whether it was dirty); a binary
built from in-progress framework work must never be mistaken for one built from the pin.

## Build and test

```sh
cmake -S . -B build && cmake --build build --target psxport        # the library
cmake --build build --target psxport_smoke                        # agnosticism proof: zero game symbols
cmake --build build && ctest --test-dir build --output-on-failure  # the gate
ctest --test-dir build -R <test_name>                             # one test
```

A framework change **starts with a RED hermetic test** in `tests/` (no disc, no GPU, no window) — drop
in one `tests/test_*.cpp` using `tests/testutil.h`; they are globbed, so there is no shared file to
edit and no merge point. See `docs/project-map.md` ("Tests"). Paste the failing output before the fix.

Per-game build/run (from inside a game repo):

```sh
./run.sh [/path/to/game.chd]     # end-to-end: sync submodules, build CHD tools, extract the boot
                                 # executable from the disc, statically recompile it to C, build, launch
cmake -S . -B build && cmake --build build --target <spyro_port|spiderman_port|tomba2_port> -j$(nproc)
./scratch/bin/<port> scratch/bin/<game>/<EXECUTABLE>              # run a built binary directly
PSXPORT_NOWINDOW=1 PSXPORT_NOPACE=1 PSXPORT_WATCHDOG=30 ./run.sh  # the boot gate every agent re-runs
```

The disc image is never in a repo. Resolution order is **CLI arg > env var > `.env` > a `*.chd` dropped
in the repo root**; the env var is `PSXPORT_SPYRO_DISC` / `PSXPORT_SPIDERMAN_DISC` /
`PSXPORT_TOMBA2_DISC`. Binaries land in the git-ignored `scratch/bin/`; `generated/` (the recompiled
substrate, ~200 MB of C) is git-ignored and rebuilt from your disc.

**`PSXPORT_NOWINDOW` is read by NOTHING in the binary** — only `run.sh` translates it (into
`PSXPORT_VK_HEADLESS`). It works in the gate line above; passing it to `./scratch/bin/<port>` directly
does nothing. That is harmless only because a run is HEADLESS BY DEFAULT (`gpu_vk.cpp` requires
`PSXPORT_VK_WINDOW=1` to open a window) — do not read it as evidence the flag applied.

## Configuration: knobs resolve through a CVar ladder

`Default` < `Value` (psxport_settings.ini) < `Override` (env) < `Runtime` (REPL/debug-server) —
`runtime/recomp/config_var.h`, inventory in `config_vars.h`, ladder and per-knob status in
`docs/config.md` + `docs/config-migration.md`. A knob that is set but matches nothing is NAMED at
startup:

```
[cfg:warn] UNKNOWN knob X is set and matched nothing — it did NOTHING in this run
```

**Check that line before trusting any run whose flag you are relying on.** The REPL `cvars` command
dumps every knob, its value, and which layer it resolved from.

Common knobs: `PSXPORT_NOAUDIO=1` · `PSXPORT_DEBUG=cd,gpu` (channel-gated diagnostics) ·
`PSXPORT_FORCE_RECOMP=1` · `PSXPORT_WATCHDOG=<sec>` · `PSXPORT_REPL=1` · `PSXPORT_SNAP_AT=<frames>` ·
`PSXPORT_WWATCH=<lo>,<hi>`.

## Architecture — the same two halves in every game repo

- **`external/psxport`** — this framework. `tools/recomp/` statically translates the PSX MIPS R3000A
  executable into emitted C (`shard_*.c`, the "substrate"); `runtime/recomp/` is the native PSX
  platform layer (GPU/SPU/GTE/MDEC/CD/XA/FMV + BIOS/SDK HLE), the SDL_GPU renderer, and the
  side-by-side (SBS) differential harness. It `#include`s **nothing** from a game.
- **`game/`** — the game half: `game/core/` holds the seam (`GameConfig` guest-address literals,
  `GameHooks`, the recomp registry, `main()`), plus native reimplementations that progressively take
  ownership of substrate functions.

The seam is `runtime/recomp/game_iface.h` (`GameConfig` / `GameHooks` / opaque `void* Core::gameCtx`)
and `recomp_iface.h` (`RecompRegistry`). `psxport_smoke` exists to keep that seam honest.

Consequences that bite:

- **`generated/` is sacrosanct.** Never hand-edit it. A mistranslation is fixed in the recompiler; a
  missing function is fixed by adding a **seed** to the game's `recomp_seeds.json` — with the
  rationale for how the address is reached — never by patching output. Never copy another game's
  seeds; they land mid-function and silently corrupt the recomp.
- **Never guess a guest address or an overlay load base.** A wrong one does not fail cleanly — it
  breaks boot or diverges the byte-compare in a way that reads as a framework bug. An un-RE'd
  `GameConfig` field stays `0` with a TODO naming the frontier step. Zero is honest.
- **Ownership is gated.** A native body must be proven to replace the substrate body byte-exactly,
  and you must prove the native body actually *ran* — a green gate where the override never installed
  (both sides ran substrate) is hollow.

## Start every non-trivial task by consulting the game repo's registries

Each game repo carries its own information system in-repo (greppable Markdown, so it reaches
subagents), with the same three questions:

```sh
python3 tools/info.py brief <words>          # docs/info/ — what's proven, and does it still hold?
python3 tools/re_frontier.py next            # docs/re-frontier.md — which RE step is actually ready
python3 tools/catalog.py search <symptom>    # docs/issues/ — hit before? ruled out before?
```

Plus, per repo: `spyro/tools/whatis.py 0x800xxxxx` (cross-references an address against every source
at once — run it rather than hand-cross-referencing), `Tomba2Engine/tools/codemap.py --addr <hex>`
(who already owns a `FUN_xxxx`) and `tools/findings.py` / `tools/kanban.py`.

Two invocation traps: `spider1` requires `RE_FRONTIER_ROADMAP=docs/re-frontier.md` on
`re_frontier.py` (without it the tool silently parses an empty roadmap and reports OK), and
`Tomba2Engine` reads its codemap from `docs/code-map.md` while `spyro`/`spider1` use `docs/codemap.md`.

**Navigate by the codemap, not by history.** `git bisect` is not the first move here — it says which
commit changed an output, never why, and on tapped code it measures the tap. Leave the codemap
pristine: update it in the same change that moves, adds, deletes or re-owns anything.

End the task by writing back what you proved, what you disproved, and any tool you caught lying.

## Reverse-engineer first — never black-box, never hand-walk disassembly

At any of these triggers — a magic offset, a `FUN_xxxx`/`sub_xxxx` you are about to call, a mystery
`obj[+0xNN]`, a value you cannot name — **stop and decompile** before changing anything.

```
tools/decomp.sh        Ghidra headless -> C (the entry point for all games)
tools/abi_extract.py   frame size, spill offsets, ra constants, from generated/
tools/port_gen.py      byte-faithful first draft of a port
tools/port_check.py    static store-sequence equivalence gate
```

`disasm.py` / `disas.py` are single-instruction **spot-checks after Ghidra**; using them to understand
behaviour is the banned method. And `port_check.py` compares static store sequences, so a genuine
rebuild (a table replacing an unrolled jump table) can fail it by construction — when it does, prove
equivalence by **running** both paths and diffing the 2 MB RAM dumps. Never contort readable code back
into a transcription to satisfy the checker.

The deliverable is a **readable port**, not a fix: typed struct lenses over guest blocks, named
constants for every literal address, enums for state-machine states, real C++ classes with methods
that say what the code does. Byte-exact mechanics stay byte-exact — this is about how it reads.

## Diagnostics

One channel-gated logger, `lucent::`, one line per call site, **never wrapped in an `if`** — it is
gated internally and does not evaluate its arguments when the channel is off. `cfg_log*` is a retired
shim: do not add new call sites, and convert any line you touch
(`cfg_logf("cd","%08X",x)` → `lucent::debug("cd","{:08X}",x)`; watch `%s` on a possibly-null
`const char*`, which is UB under `std::format` where printf gave `(null)`). The one legitimate guard
is around expensive **non-logging** work, using an interned `lucent::Channel`.

## Never commit

Disc images (`*.chd`), extracted executables, `generated/`, or machine-specific absolute paths
(`/home/<user>/…`). Every game repo ships `tools/go_public.py` to audit the full history for exactly
this. Everything transient goes in the git-ignored `scratch/`, split by kind (`scratch/logs/`,
`scratch/bin/`, `scratch/raw/`) — **never `/tmp`**, which is a small RAM-backed tmpfs on this machine;
diagnose "disk quota exceeded" with `quota -s`, not `df`.

## Follow DUSKLIGHT's structure — consult it BEFORE designing anything it already solved

`~/repo/dusklight` (`github.com/TwilitRealm/dusklight`, **CC0**, `git pull` before reading — it moves
daily). A shipping PC port of Twilight Princess on the `zeldaret/tp` decomp. Different console, same
problem shape, and the USER's explicit instruction is to follow it closely: *"impeccable code structure
and organization and impeccable UI"*. **Take the SHAPE, not a copy-paste**, and cite what you took.

- **UI is TWO stacks on purpose.** `src/dusk/ui/` is RmlUi for the game-facing UI; `src/dusk/imgui/` is
  ImGui for developer overlays (console, save editor, heap/process/camera overlays, actor spawner).
  Shipped UI and debug UI have different requirements and must not share a framework.
- **The RmlUi UI is COMPONENTISED**, which is the part psxport most needs: `ui/component.{hpp,cpp}` is a
  base class owning its subtree (`add_child<T>()`), with scoped event listeners (`listen()` returning a
  `ScopedEventListener`, so teardown is automatic), and state expressed as RCSS pseudo-classes
  (`selected`, `disabled`) rather than ad-hoc bools. Concrete components — `button`, `bool_button`,
  `modal`, `menu_bar`, `document`, `graphics_tuner`, `logs_window`, `mods_window`,
  `controller_config` — are each a file pair. Stylesheets live apart in `res/rml/*.rcss`.
  `runtime/recomp/rmlui_overlay.cpp` is ONE monolith by comparison; that is the gap to close, and the
  adoption order is written up in the workspace's `docs/plans/rmlui-componentisation.md`.
- **Config is a layered CVar system** — already adopted here (`runtime/recomp/config_var.h`, above).
- **Frame interpolation is RECORD-AND-REPLACE** (`src/dusk/frame_interpolation.{h,cpp}`): the sim tick
  runs untouched while each final matrix is recorded keyed by its own address; the presentation frame
  lerps prev→cur into a REPLACEMENT table that draw-time sites consult. Guest state is never mutated,
  so it cannot leak. The camera is interpolated as a POSE (eye/center/up/bank/fovy/near/far, bank via
  `remainderf` for wrap), never as a matrix lerp. Note the sim-frame vs presentation-frame split, and
  `request_presentation_sync()` for frames that must be exact.

  **The PSX caveat, and it decides when we may adopt this:** Dusklight records matrices that are NAMED
  VARIABLES in a decomp. Doing the same on our recompiled substrate means reading a guest pre-composed
  matrix out of engine state — which is banned (the picture comes from GAME STATE, never from what the
  GTE produced; `docs/workspace/PROTOCOL.md`). The model becomes legitimate exactly as the PC takes ownership of
  the code that COMPUTES the transform, because then the matrix is our own variable. Interpolation is
  not blocked forever; it is unlocked by execution ownership.
