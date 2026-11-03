# 0197 — Tool runs reset the player's settings

Status: fixed (2026-11-03). Owner: `config/config.cpp:load_settings`/`save_settings`, `game/mods.cpp:Mods::save`.

## Observed
The player set Auto resolution and Auto aspect and found them changed after agent work. Seven title repos
held a `psxport_settings.ini` in the repository root (Vagrant Story's read `aspect=0 ires=1`).

## Cause
`PSXPORT_SETTINGS` defaulted to `psxport_settings.ini` in the working directory, and `run.sh` and agent runs
both start in the repository root, so player and tool runs shared one file. The checkout guard in
`save_settings` never fired for that relative path. `Mods::save` wrote every field's live value, so a menu
change in a run with `PSXPORT_IRES`/`PSXPORT_ASPECT` set persisted the override. Mega Man X4's `live_play.py`
named the root file as its agent settings.

## Fix
- Unset `PSXPORT_SETTINGS` with a shown window is the player's `<user-data>/<executable stem>/settings.ini`;
  a hidden run without it has no file and refuses to save.
- `agent_environment` gives every tool run a copy under `scratch/settings/`.
- `Mods::save` writes only values that differ from the knob's effective value.
- The seven root files moved to `~/.config/<product>_port/settings.ini` with `ires = 0`, `aspect = 3`.

## Shown
C-12 hidden runs (`psx-c12/scratch/settings-run/`): with `PSXPORT_IRES=2 PSXPORT_ASPECT=1 PSXPORT_FPS60=1` and an
Escape/Return menu toggle the tool file saved `aspect = 2`, `ires = 1`, `fps60 = false` (the toggle, no override);
a direct hidden run logged `settings: none` and `settings not saved`; the player's file kept SHA-256
`fcc53683…` across both. Not shown: a shown-window run loading the user-data file (agents never map the window).
