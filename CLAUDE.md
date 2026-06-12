# psxport — Crash Bash (USA) PSX → PC static recompilation

Target game: **Crash Bash (USA)**, PlayStation 1, provided as a CHD disc image.
Oracle / reference emulator: **DuckStation**, vendored as a pinned submodule at
`vendor/duckstation` (also the source of runtime subsystems: CHD reading via libchdr,
GPU/SPU/input early on).

## Goals (two strict phases — never mixed)
1. **Faithful port**: recompiled R3000A code running natively, verified bit-exact against
   DuckStation via the differential harness.
2. **Enhancements** (only on a proven-faithful base):
   - **Widescreen** (proper FOV/projection-level, not stretch).
   - **Interpolated 60 fps**, n64recomp-style: interpolate **object transformations**
     between game logic ticks (game logic stays at native rate; render at 60 with
     per-object matrix/position interpolation).

## Layout
- `recompiler/` — offline R3000A → C recompiler (input: PSX EXE extracted from disc).
- `generated/` — emitted code (gitignored, regenerable).
- `runtime/` — host runtime: memory map, dispatch, video/audio/input (DuckStation-backed
  initially).
- `overrides/` — hand-written native replacements for generated functions.
- `harness/` — differential lockstep harness vs DuckStation. Stand this up BEFORE game
  logic; determinism (savestate freeze/restore, fixed timebase, pinned input) is a
  precondition.
- `tools/` — one-off/offline tools (disc extraction etc.).
- `scratch/` — gitignored run artifacts, separated by type (`bin/ screenshots/ raw/ wav/
  logs/`). **Never write artifacts to /tmp** (RAM tmpfs, ~6 GB quota).

## Disc provisioning
Never commit the CHD. Resolution order: CLI arg > `PSXPORT_DISC` (real env, then `.env`)
> drop-in `*.chd` in repo root. `.env` is gitignored; see `.env.example`.

## Hard rules
- Generated code is sacrosanct — fix the recompiler or add an override, never hand-edit.
- No magic constant offsets / special-cased inputs — root-cause fixes only (see global
  CLAUDE.md "No bandaids").
- Single `main` branch; verified milestones are committed and pushed immediately.
- Verification = harness output on real gameplay, cited; never a vibe.

## Status / journal
See `docs/journal.md` for session-by-session findings, dead ends, and current first
divergence. Read it before re-deriving anything.
