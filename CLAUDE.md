# psxport — "wide60": generic PSX widescreen + interpolated-60fps layer

**Goal (refined 2026-06-12; originally a static-recomp port — that's dead):** a reusable
system that turns PSX games into widescreen + 60 fps **without changing game logic**,
built on a modified Beetle PSX runtime (`runtime/`, vendored `vendor/beetle-psx`, GPL-2).
Per-game reverse engineering is expected but must be *minimized*: generic emulator-side
mechanisms first, per-game work reduced to a small config/patch layer.

## Architecture: generic tier + per-game tier
PSX geometry is CPU/GTE-transformed; the GPU only sees projected 2D primitives. Hence:
- **60 fps, generic tier — primitive-level interpolation:** capture consecutive logic
  frames' display lists, match primitives across frames (draw order + prim type +
  texpage keys), lerp vertices to synthesize intermediate frames. Mismatches snap, not
  lerp. Needs a logic-rate detector (frames between display-list changes) — which also
  *measures* each game's real framerate instead of assuming it.
- **60 fps, per-game tier — GTE transform tagging:** hook GTE rotation/translation
  register writes, tag transforms per game object (per-game RE captured in a config
  file), interpolate at transform level where the generic matcher fails.
- **Widescreen, generic tier:** the emulator's GTE widescreen hack (the
  standard mednafen/Beetle GTE-scale approach).
- **Widescreen, per-game tier:** small EXE/memory patches fixing culling and HUD.

Test games (CHDs live outside the repo — never commit; locations via `.env`):
- **Crash Bash (USA)** — `SCUS_945.70`, entry `0x8002E7B0`, load `0x80010000`, size
  `0x69000`. Native framerate unverified — measure, don't assume.
- **Tomba! 2 (USA)** — `SCUS_944.54` (167936 B, disc LBA 152155). Believed 30 fps —
  measure.

## Licensing (distributable)
The runtime is built on **Beetle PSX (GPL-2, distributable)**; our fork lives in the
`vendor/beetle-psx` submodule (public). Game patches (.cht/PPF) are fine to publish.
(Historical note: a DuckStation fork was once used as an RE lab/oracle. DuckStation is
CC-BY-NC-ND-4.0 — NON-distributable — so it was removed from this repo entirely; do not
re-vendor it or commit any DuckStation source/binary.)

## PC port runtime (scope change #3, 2026-06-12)
`runtime/` is the actual PC port: it compiles the Beetle PSX (mednafen) sources
directly into `wide60rt` (GPL-2 = distributable), interpreter-only, with a
PC-keyed native-override layer (`runtime/psxport_hooks.*`, signature-checked for
overlay safety) and per-game modules (`runtime/games/`). Our Beetle changes live
**directly in a committed fork** — the `vendor/beetle-psx` submodule points at our
public fork (github.com/SomeoneIsWorking/beetle-psx-libretro, branch `psxport`),
and we edit its source in place. Do NOT maintain Beetle changes as out-of-tree
.patch files applied to a pristine upstream (the user dislikes that workflow —
keep the fork). Beetle only carries generic emulator primitives + the hook
call-sites; all HLE BIOS / game logic stays in `runtime/`.

## TWO binaries — do not confuse them (this trips up every fresh session)
There are two completely separate executables built from different sources:
- **The native PORT — `scratch/bin/tomba2_port` — IS THE THING UNDER TEST.** It is the
  recompiled MAIN.EXE (`generated/shard_*.c` from `tools/recomp/emit.py`) linked with the
  hand-written runtime in **`runtime/recomp/*.c`** (HLE BIOS, GPU/SPU/CD/pad natives, native
  boot, XA streaming, …). **NO Makefile builds it.** Build it with:
    - `./run.sh [disc.chd]` — full: extract MAIN.EXE, recompile, compile every TU, link, **and run**.
    - `tools/build_port.sh [files…|all]` — **incremental** compile+relink only (no run, ~0.5s for
      one file). Use this in a fix loop. Object cache: `scratch/obj/`. Keep its SRC list in sync
      with run.sh step 4 when you add a `runtime/recomp/*.c` file.
  Drive/observe it with `tools/drive.py` (does NOT build — runs the existing binary). See
  `docs/diff-driver.md`.
- **The ORACLE — `runtime/wide60rt` — is the Beetle reference emulator**, full emulation of the
  real disc, used to validate the port. Built by **`make -C runtime`** (this is the *only* thing
  that Makefile builds — it is NOT the port). Run: `runtime/wide60rt <chd> -bios <dir> -play`
  (OpenBIOS as scph5501.bin works; `-fastboot` needs a retail BIOS — hangs under OpenBIOS).

Full project map (modules, CD-command flow, build/drive cheat-sheet): **`docs/project-map.md`** —
read it before grepping around.

## Layout
- `runtime/` — the PC port (see above); `runtime/games/` per-game modules.
- `tools/` — offline tools (`discdump`: CHD → SYSTEM.CNF + boot EXE via libchdr).
- `patches/` — per-game patch sources + built patch files.
- `common/` — shared `.env` reader (`PSXPORT_DISC` etc.).
- `docs/journal.md` — findings, dead ends, current state. Read before re-deriving.
- `scratch/` — gitignored artifacts by type (`bin/ screenshots/ raw/ wav/ logs/`).
  **Never /tmp** (RAM tmpfs, ~6 GB quota).
- `build/` (our tools) — gitignored.

## Disc provisioning
Never commit CHDs. CLI arg > `PSXPORT_DISC` (env / `.env`) > `*.chd` drop-in.

## Hard rules
- No magic constant offsets in patches — every patched instruction/value is documented
  with what the original code does and why the change is correct (see global "No
  bandaids").
- Single `main` branch; verified milestones committed AND pushed to `origin`
  (github.com/SomeoneIsWorking/psxport.git).
- Verification = observed behavior in the emulator on real gameplay, cited.
