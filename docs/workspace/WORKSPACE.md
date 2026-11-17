# `~/repo/psx` — the PSX-port workspace

A map. The workspace directory is not a repo; `AGENTS.md`/`CLAUDE.md` here are symlinks to this file.

| read | for |
|---|---|
| `psxport/AGENTS.md` | how a game consumes the framework (interpreter/native seam, overrides, invalidation) |
| `psxport/docs/codemap.md` | where framework code lives |
| `<game>/AGENTS.md`, `<game>/docs/codemap.md` | that game's specifics and layout |
| `psxport/docs/workspace/GHIDRA.md` | Ghidra queries: `external/psxport/tools/decomp_pipeline.py --image <exe> --target/--callers/--refs/--function-at 0xADDR` |

## Repos

All public under `github.com/SomeoneIsWorking`, side by side, no superproject.

| path | what | decomp (`decomp-split` skill) |
|---|---|---|
| `psxport/` | the framework — the one writable framework checkout | — |
| `psx-spyro/` | Spyro 1/2/3 (`titles/<t>/` over a shared `game/`), one process with an in-window picker | split: forks `open-spyro`, `spyro-2`, `spyro-3` |
| `psx-toy-story-2/` | Toy Story 2 (`SLUS_008.93`) | split: `toystory2-decomp` (ours) |
| `psx-crash-bash/` | Crash Bash | not split (`crash_bash_decomp`, MIT, pinned) |
| `psx-crash/` | Crash 1/2/3 (one architecture) | split: forks `crash-bandicoot-decomp`, `crash2-decomp`; `crash3-decomp` (ours) |
| `psx-crash-team-racing/` | Crash Team Racing | not split (`ctr-native`, GPL-3, evidence) |
| `psx-tomba/` | Tomba! 2 (`SCUS_944.54` → `MAIN.EXE`) and Tomba! 1 | Tomba 2 split: `tomba2-decomp` (ours); Tomba 1 split: `tomba1-decomp` (ours) |
| `psx-spider-man/` | Spider-Man 1/2 | not split (none exists: ours to create) |
| `psx-vagrant-story/` | Vagrant Story (vendors the CC0 `rood-reverse` decomp) | not split (`rood-reverse`, CC0) |
| `psx-mega-man-x4/` | Mega Man X4 (vendors the AGPL `mmx4` decomp — never lift it into psxport) | not split (`mmx4`, AGPL) |
| `psx-tekken-3/` | Tekken 3 | not split (`tekken3-xport`, MIT) |
| `psx-c12/` | C-12: Final Resistance (`SCUS_946.66`) | split: `c12-decomp` (ours) |

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
Toy Story 2, C-12: Final Resistance. Every title: widescreen and loading removal. Interpolation is only for a title
whose logic runs at 30 fps (two fields per logic frame, read from its frame loop or `[playcheck] cadence:`); a 60 fps
title gets none. Tekken 3, Tomba! 1 and Mega Man X4 are 60 fps; measure any other title before planning it. Vagrant Story's horizontal
projection word is gameplay state (battle code branches on it), so widen its canvas, never `H`.
C-12 also gets increased draw distance and a right-stick camera (psx-c12 G004). Spyro 1/2/3 also get more vegetation (psx-spyro G006) and a right-stick camera (psx-spyro G007). Tomba! 2 also gets better water and fire (psx-tomba G006, G007). Mega Man X4 also gets drop-in/drop-out co-op
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
and `scripts/bootstrap_workspace.py` runs it for every title. psxport has no third-party CPU dependency to pin:
guest code runs on its own executor: a run-time recompiler like DuckStation's (decided 2026-11-17), with the
interpreter (`runtime/cpu/guest_executor.*`) as reference and fallback.

The record rasterizer follows re-harness's in-pass destination rule: Metal framebuffer fetch on macOS today, the
other backends as our SDL fork (`SomeoneIsWorking/SDL`, pinned in `tools/port/target-libraries.json`) gains them.

## Build

There are no unit tests or gates. Each repo is checked by building its product with Clang:

| repo | command |
|---|---|
| psxport | `CC=clang CXX=clang++ uv run --frozen python tools/build.py --build build/agent` |
| every title | `CC=clang CXX=clang++ uv run --frozen python external/psxport/tools/port/consumer_build.py --build build/agent --target <product>` |

Products: `spyro_port`, `toystory2_port`, `crashbash_port`, `crash_port`, `ctr_port`, `tomba2_port`,
`spiderman_port`, `vagrant_port`, `megamanx4_port`, `tekken3_port`, `c12_port`.

## Second machine (`ssh ally`)

The Ally (Bazzite, 16 CPUs, 11 GB) holds its own clone of this workspace at `~/repo/psx` and
`~/repo/shared/{bug-report,setup-ui,re-harness}`, made by the fresh-machine bootstrap above; each title's `.env`
there points at `~/ROM/PSX CHD/<disc>.chd`. It builds inside the `my-distrobox` distrobox (Fedora 43: clang, lld,
cmake, ninja-build, glslc, SDL3-devel, SDL3_image-devel, freetype-devel, zlib-devel; `uv` lives only in the box)
and runs on the host, whose glibc is newer than the box's. Heavy steps (builds, runs) take
`flock ~/agents/.heavy.lock`; other agents run there too. Only pushed commits reach it: push first, then sync.

```sh
# sync a repo to a pushed branch or commit (psxport too, for a framework change)
ssh ally 'git -C ~/repo/psx/psx-tomba fetch -q origin && git -C ~/repo/psx/psx-tomba checkout -q --detach <commit>'
# build in the box
ssh ally 'flock ~/agents/.heavy.lock distrobox enter my-distrobox -- env --chdir=$HOME/repo/psx/psx-tomba \
  CC=clang CXX=clang++ uv run --frozen python external/psxport/tools/port/consumer_build.py --build build/agent --target tomba2_port'
# run on the host, hidden and silent, from the title root (the product reads ./.env and extracts
# scratch/bin/<slug>/ on a no-argument run)
ssh ally 'R=~/repo/psx/psx-tomba O=$R/scratch/ally; mkdir -p $O; : > $O/settings.ini; flock ~/agents/.heavy.lock \
  env --chdir=$R XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 PSXPORT_SHOW_WINDOW=0 PSXPORT_NOAUDIO=1 \
  PSXPORT_NOPACE=1 PSXPORT_SETTINGS=$O/settings.ini PSXPORT_ASSET_DIR=$HOME/repo/psx/psxport PSXPORT_NATIVE_FRAMES=400 \
  build/agent/bin/tomba2_port scratch/bin/tomba2/SCUS_944.54 > $O/run.log 2>&1'
```

The host has no `uv`, so a host run sets `agent_environment`'s keys by hand as above. The Wayland socket is the
gamescope session's (`ls /run/user/1000/wayland-*`). A clean build of one title takes about ten minutes.

## Rules

- Some `scratch/` dirs are provisioned disc inputs: `psx-spyro/scratch/assets/`, `psx-tomba/scratch/bin/`,
  `psx-toy-story-2/scratch/{bin,flat,raw}`, `psx-crash-bash/scratch/bin/`. Each holds a `.scratch-keep` marker, which
  `scratch_gc.py` honours for the whole subtree; a provisioner that creates such a dir writes the marker.
  Sweep only your own `scratch/<activity>/`.
- Disc images live under `/mnt/Boy/ROM/PSX CHD/` (on a Mac, `~/Games/PSX/`); each repo's `.env` points at its disc.
- Agent runs are windowless and silent, on every machine and for every binary, installed apps included:
  `PSXPORT_SHOW_WINDOW=0` and `PSXPORT_NOAUDIO=1`, never a shown window or audible sound. Start runs through
  `agent_environment` (`tools/port/launch_environment.py`), which sets both and gives the run its own settings
  file, so the player's settings are never read or written. The shown window is the player's, presented at real
  speed, so a run without `PSXPORT_NOPACE` measures pacing (`[playcheck] presents:`); that needs the user's
  display session, so pacing is measured only when the user asks for it (`psxport/docs/presentation.md`).
- Every judgement run plays the way the player does: `ires=0` (auto, 8x on this sink), `fps60=1`,
  `PSXPORT_PRESENT_SINK=3840x2160`. Judder is motion that jumps: a draw that moves only on real presents, steps
  by whole native pixels, lands off the halfway point of its neighbours, snaps or goes backwards. An occasional
  repeated present (the 59.82 Hz NTSC field pacing on a 60.00 Hz display, a load hold) is not judder. The unmapped window
  never scans out, so simulate the player's 60.00 Hz display from the `presentseq` times (each vblank shows the
  newest present before it) and judge the motion across the shown sequence.
- Only pacing needs real time on a quiet host. Composition, identity and halfway motion are judged unpaced
  (`PSXPORT_NOPACE=1`, same ires and sink) at any load.
- Cost is measured as the `perf-units` skill says. The meter is `PSXPORT_DEBUG=perf`: the run-end `[perf] cpu …`
  (instructions per logic frame by phase) and `[perf] gpu …` (work per present) lines, and `[playcheck] cadence:`;
  `psxport/docs/presentation.md` Cost and cadence.
- Run captures stay small: dump only the presents a judgement needs, crop or downscale them for comparison,
  and before a pass ends delete its full-size dumps with `scratch_gc.py`, keeping logs, scripts and the reduced
  evidence the issue cites.
- To reach a screen, level or state for exploration, add a title-owned debug option modelled on Tomba 2's
  `warp` (`psx-tomba/game/core/debug/dev_warp.cpp`): a control-channel command arms a request and the frame
  driver applies it at a frame boundary through the game's own transition/load owners. Player paths stay
  untouched. Never steer gameplay to get there, and prefer this over timed front-end replays.
