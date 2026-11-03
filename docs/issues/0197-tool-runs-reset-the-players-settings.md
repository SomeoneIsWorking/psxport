# 0197 — Tool runs reset the player's settings

Status: fixed (2026-11-03). Owner: `config/config.cpp:load_settings`/`save_settings`, `game/mods.cpp:Mods::save`, `lucent::cvar::save_file`.

## Observed
The player set Auto resolution and Auto aspect and found them changed after agent work. Seven title repos
held a `psxport_settings.ini` in the repository root (Vagrant Story's read `aspect=0 ires=1`); Mega Man X4's and
Spider-Man's were tracked.

## Cause
`PSXPORT_SETTINGS` defaulted to `psxport_settings.ini` in the working directory, and `run.sh` and agent runs
both start in the repository root, so player and tool runs shared one file. The checkout guard in
`save_settings` never fired for that relative path. `Mods::save` wrote every field's live value, so a menu
change in a run with `PSXPORT_IRES`/`PSXPORT_ASPECT` set persisted the override. Mega Man X4's `live_play.py`
named the root file as its agent settings. `lucent::cvar::save_file` wrote every saved knob, its default when
it held no file value, so one save pinned every default; with `PSXPORT_FPS60` set, `Mods::save` skipped fps60 and
the file got the default `fps60 = false`, which is what C-12's root file held.

## Fix
- Unset `PSXPORT_SETTINGS` with a shown window is the player's `<user-data>/<executable stem>/settings.ini`;
  a hidden run without it has no file and refuses to save.
- `agent_environment` gives every tool its own copy under `scratch/settings/`, named by its PID.
- `Mods::save` sets the Value layer only for a menu field that differs from the knob's live value, and
  `lucent::cvar::save_file` writes only knobs that hold a Value layer.
- The root files were deleted; each player file in `~/.config/<product>_port/settings.ini` holds `ires = 0`,
  `aspect = 3`, and `fps60 = true` where the old file had it on. Values equal to the default and keys no binary
  registers (`light`, `ssao`, ...) were dropped, since the old save wrote them whoever chose them.

## Shown
C-12 hidden tool run (`psx-c12/scratch/settings-run/prove.py`, `tool.log`) with `PSXPORT_IRES=2 PSXPORT_ASPECT=1
PSXPORT_FPS60=1` and an Escape/Return menu toggle: the run's empty seed file afterwards holds only `aspect = 2`, the
toggle; the player's file kept SHA-256 `eff1dc03…`. A hidden run seeded from a copy of the player's file
(`seeded.py`) boots at auto aspect (`[wide] native picture: aspect=1`) and leaves the player's file identical.
Not shown: a shown-window run resolving the user-data path (agents never map the window).
