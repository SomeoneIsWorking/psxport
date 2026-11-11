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
| `psx-spyro/` | Spyro 1/2/3 (`titles/<t>/` over a shared `game/`), one process with an in-window picker |
| `psx-toy-story-2/` | Toy Story 2 (`SLUS_008.93`) |
| `psx-crash-bash/` | Crash Bash |
| `psx-crash/` | Crash 1/2/3 (one architecture) |
| `psx-crash-team-racing/` | Crash Team Racing |
| `psx-tomba/` | Tomba! 2 (`SCUS_944.54` → `MAIN.EXE`) and Tomba! 1 |
| `psx-spider-man/` | Spider-Man 1/2 |
| `psx-vagrant-story/` | Vagrant Story (vendors the CC0 `rood-reverse` decomp) |
| `psx-mega-man-x4/` | Mega Man X4 (vendors the AGPL `mmx4` decomp — never lift it into psxport) |
| `psx-tekken-3/` | Tekken 3 |
| `psx-c12/` | C-12: Final Resistance (`SCUS_946.66`) |

Fresh machine: `git clone https://github.com/SomeoneIsWorking/psxport.git ~/repo/psx/psxport && cd ~/repo/psx/psxport && uv run --frozen python scripts/bootstrap_workspace.py`.

## Focus

Work is Tomba 2 and Spyro, one defect at a time, taken from what the user reported, until the user names another
title. The title scope below is the long-term list, not a work queue; the issue backlog is not one either.

psxport is a general PSX tool, so a framework change is not work on every title. It is built and checked on the
title that needed it, plus Tomba 2 and Spyro when it touches presentation, audio or timing. A changed API still
updates and builds its callers in the same change. Other titles are not run for it.

Agents and workflows run only when the user asks for them in that request. When a task lands, report and stop.
Time goes to reading and writing code: build once a change is complete, run only to reproduce or to show a fix.

## Title scope

Spyro 1/2/3, Crash 1/2/3, Crash Bash, CTR, Vagrant Story, Mega Man X4, Tomba! 1/2, Tekken 3, Spider-Man 1/2,
Toy Story 2, C-12: Final Resistance. Every title: widescreen and loading removal. Tekken 3, Tomba! 1 and
Mega Man X4 are already 60 fps, so no interpolation for them; the others get 60 fps interpolation. Vagrant Story's horizontal
projection word is gameplay state (battle code branches on it), so widen its canvas, never `H`.
C-12 also gets increased draw distance. Spyro 1/2/3 also get more vegetation (psx-spyro G006). Tomba! 2 also gets better water and fire (psx-tomba G006, G007). Mega Man X4 also gets drop-in/drop-out co-op
and skippable dialogue and sequences.

Start skips what the player does not control (logos, movies, flybys, transition and load fades) in every title.
Each skip is title-owned, one screen at a time, taken through that screen's own guest terminal transition and
tested; no framework-wide skip hook. A screen with no faithful terminal transition stays unskippable and is
recorded as an issue.

Each title owns its frame loop: its frame driver runs the guest's per-frame update and draw and decides the
cadence, replacing the guest's VSync waits rather than emulating them; a frame whose guest work overran its
requested fields still ends on the field the guest clock reached (`Timing::vsyncFields`, `Timing::takeStepFields`). A loop not yet owned
answers each guest VSync with one field and is recorded as an issue until it is owned.

## Framework

There is no per-game framework pin. Each game's `external/psxport` is a plain relative symlink to the
workspace's live `psxport` checkout, so a framework edit is live in every game at once; with no sibling
checkout (a fresh clone) it is a shallow clone of psxport `main`. psxport's `tools/psxport_fetch.py`
makes it; no title carries a copy. A title's launcher runs it from psxport `main` while the link is
missing (`uv run --no-project https://raw.githubusercontent.com/SomeoneIsWorking/psxport/main/tools/psxport_fetch.py --auto`),
and `scripts/bootstrap_workspace.py` runs it for every title. Lightrec stays pinned at `PSXPORT_LIGHTREC_REVISION`
(`psxport/cmake/lightrec_dependency.cmake`, `psxport_fetch.py --lightrec`).

## Build

There are no unit tests or gates. Each repo is checked by building its product with Clang:

| repo | command |
|---|---|
| psxport | `CC=clang CXX=clang++ uv run --frozen python tools/build.py --build build/agent` |
| every title | `CC=clang CXX=clang++ uv run --frozen python external/psxport/tools/port/consumer_build.py --build build/agent --target <product>` |

Products: `spyro_port`, `toystory2_port`, `crashbash_port`, `crash_port`, `ctr_port`, `tomba2_port`,
`spiderman_port`, `vagrant_port`, `megamanx4_port`, `tekken3_port`, `c12_port`.

## Rules

- Some `scratch/` dirs are provisioned disc inputs: `psx-spyro/scratch/assets/`, `psx-tomba/scratch/bin/`,
  `psx-toy-story-2/scratch/{bin,flat,raw}`, `psx-crash-bash/scratch/bin/`. Each holds a `.scratch-keep` marker, which
  `scratch_gc.py` honours for the whole subtree; a provisioner that creates such a dir writes the marker.
  Sweep only your own `scratch/<activity>/`.
- Disc images live under `/mnt/Boy/ROM/PSX CHD/`; each repo's `.env` points at its disc.
- Agent runs keep the product's window unmapped: `PSXPORT_SHOW_WINDOW` unset or 0 (`agent_environment` sets 0), never
  1. It is the player's window, presented at real speed, so a run without `PSXPORT_NOPACE` measures pacing
  (`[playcheck] presents:`); it needs the user's Wayland session (`psxport/docs/presentation.md`).
- Every judgement run plays the way the player does: `ires=0` (auto, 8x on this sink), `fps60=1`,
  `PSXPORT_PRESENT_SINK=3840x2160`. Judder is motion that jumps: a draw that moves only on real presents, steps
  by whole native pixels, lands off the halfway point of its neighbours, snaps or goes backwards. An occasional
  repeated present (the 59.82 Hz NTSC field pacing on a 60.00 Hz display, a load hold) is not judder. The unmapped window
  never scans out, so simulate the player's 60.00 Hz display from the `presentseq` times (each vblank shows the
  newest present before it) and judge the motion across the shown sequence.
- Only pacing needs real time on a quiet host. Composition, identity and halfway motion are judged unpaced
  (`PSXPORT_NOPACE=1`, same ires and sink) at any load.
- Weak hardware is met by having no needlessly expensive operations, never by generating host load: measure cost
  in load-independent units with the product's own meter (`PSXPORT_DEBUG=perf`: instructions per logic frame by
  phase, GPU draws/pixels/uploads per present, the planned 60 Hz cadence; `psxport/docs/presentation.md` Cost and
  cadence), at any load, and remove work that is redundant, repeated or scales with ires/sink on the CPU.
- Run captures stay small: dump only the presents a judgement needs, crop or downscale them for comparison,
  and before a pass ends delete its full-size dumps with `scratch_gc.py`, keeping logs, scripts and the reduced
  evidence the issue cites.
- To reach a screen, level or state for exploration, add a title-owned debug option modelled on Tomba 2's
  `warp` (`psx-tomba/game/core/debug/dev_warp.cpp`): a control-channel command arms a request and the frame
  driver applies it at a frame boundary through the game's own transition/load owners. Player paths stay
  untouched. Never steer gameplay to get there, and prefer this over timed front-end replays.
