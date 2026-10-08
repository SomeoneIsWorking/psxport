# `~/repo/psx` — the PSX-port workspace

A map. The workspace directory is not a repo; `AGENTS.md`/`CLAUDE.md` here are symlinks to this file.

| read | for |
|---|---|
| `psxport/AGENTS.md` | how a game consumes the framework (Lightrec/native seam, overrides, invalidation) |
| `psxport/docs/codemap.md` | where framework code lives |
| `<game>/AGENTS.md`, `<game>/docs/codemap.md` | that game's specifics and layout |
| `psxport/docs/workspace/GHIDRA.md` | Ghidra queries: `external/psxport/tools/decomp_pipeline.py --image <exe> --target/--callers/--refs/--function-at 0xADDR` |

## Repos

All public under `github.com/SomeoneIsWorking`, side by side, no superproject.

| path | what |
|---|---|
| `psxport/` | the framework — the one writable framework checkout |
| `spyro/` | Spyro 1/2/3 (`titles/<t>/` over a shared `game/`), one process with an in-window picker |
| `toystory2/` | Toy Story 2 (`SLUS_008.93`) |
| `crashbash/` | Crash Bash |
| `crash/` | Crash 1/2/3 (one architecture) |
| `ctr/` | Crash Team Racing |
| `Tomba2Engine/` | Tomba! 2 (`SCUS_944.54` → `MAIN.EXE`) and Tomba! 1 |
| `spider1/` | Spider-Man 1/2 |
| `vagrant/` | Vagrant Story (vendors the CC0 `rood-reverse` decomp) |
| `megamanx4/` | Mega Man X4 (vendors the AGPL `mmx4` decomp — never lift it into psxport) |
| `tekken3/` | Tekken 3 |

Fresh machine: `git clone https://github.com/SomeoneIsWorking/psxport.git ~/repo/psx/psxport && cd ~/repo/psx/psxport && uv run --frozen python scripts/bootstrap_workspace.py`.

## Title scope

Spyro 1/2/3, Crash 1/2/3, Crash Bash, CTR, Vagrant Story, Mega Man X4, Tomba! 1/2, Tekken 3, Spider-Man 1/2,
Toy Story 2. Every title: widescreen and loading removal. Tekken 3, Tomba! 1 and Mega Man X4 are already
60 fps, so no interpolation for them; the others get 60 fps interpolation. Vagrant Story's horizontal
projection word is gameplay state (battle code branches on it), so widen its canvas, never `H`.

## Framework

There is no per-game framework pin. Each game's `external/psxport` is a plain relative symlink to the
workspace's live `psxport` checkout, created by `tools/psxport_fetch.py --auto` (run by `run.sh`), so a
framework edit is live in every game at once; with no sibling checkout (CI, a fresh clone) it shallow-clones
psxport `main` instead. Lightrec stays pinned at `PSXPORT_LIGHTREC_REVISION`
(`psxport/cmake/lightrec_dependency.cmake`, `psxport_fetch.py --lightrec`).

## Gates

| repo | command |
|---|---|
| psxport | `uv run --frozen python tools/verify.py --build build` |
| spyro | `uv run --frozen python tools/verify.py --jobs 6` |
| toystory2 | `CXX=clang++ CC=clang CMAKE_BUILD_PARALLEL_LEVEL=6 uv run --frozen python tools/verify.py` |
| crashbash | `uv run --frozen python tools/verify.py` |

## Rules

- Never commit disc images, extracted executables, or `/home/<user>/…` paths.
- Never write run artifacts to `/tmp`; use the repo's gitignored `scratch/`.
- Some `scratch/` dirs are provisioned disc inputs: `spyro/scratch/assets/`, `Tomba2Engine/scratch/bin/`,
  `toystory2/scratch/{bin,flat,raw}`, `crashbash/scratch/bin/`. Each holds a `.scratch-keep` marker, which
  `scratch_gc.py` honours for the whole subtree; a provisioner that creates such a dir writes the marker.
  Sweep only your own `scratch/<activity>/`.
- Disc images live under `/mnt/Boy/ROM/PSX CHD/`; each repo's `.env` points at its disc.
- One game instance at a time unless isolated; kill by PID, never `pkill`.
- To reach a screen, level or state for exploration, add a title-owned debug option modelled on Tomba 2's
  `warp` (`Tomba2Engine/game/core/dev_warp.cpp`): a control-channel command arms a request and the frame
  driver applies it at a frame boundary through the game's own transition/load owners. Player paths stay
  untouched. Never steer gameplay to get there, and prefer this over timed front-end replays.
