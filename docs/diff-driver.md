# Differential debugging vs the oracle — tools (drive.py / bgm.py)

_This is the committed copy of the local Claude skill `.claude/skills/diff-driver` (`.claude/` is
gitignored, so it doesn't travel with the repo). Keep both in sync._


# Differential debugging vs the oracle (Tomba!2 port)

**Rule: always compare against the oracle.** The native port (`scratch/bin/tomba2_port`, recompiled
MAIN.EXE + HLE) is the thing under test; the Beetle oracle (`runtime/wide60rt`, full emulation of the
real disc) is ground truth. Any "is this right?" question is answered by getting BOTH cores to the same
scene and diffing state — not by reading native-port values in isolation. This is the
[[recomp-harness]] methodology applied to this project.

## Tools

### `tools/drive.py` — persistent INTERACTIVE driver (native OR oracle)
Keeps one core alive across calls via a FIFO, so you drive it like a player — send a command, look,
send the next — instead of a predetermined input script. (Continuous forced input is a trap: pulsing
Start = pause menu = stops BGM; it poisons captures. Drive interactively.)

```
tools/drive.py start native                 # FMVs off + BGMDBG on by default
tools/drive.py start oracle                  # cold boot
PSXPORT_LOADSTATE=scratch/state/newgame.sav tools/drive.py start oracle   # oracle from a savestate
tools/drive.py start native headed           # open a window (SDL; needs a display)
tools/drive.py send "run 30" "tap x 6" "run 40" "shot scratch/screenshots/x.ppm" "r 800bed80 2"
tools/drive.py out 40                         # reprint last 40 lines (e.g. if a slow run drained early)
tools/drive.py stop
```
REPL commands (both cores): `run N | r/rw addr [n] | w/w8 | watch lo hi (native) / watch addr (oracle) |
unwatch | hits | press/release/tap <btn> [n] | regs | seq | shot PATH | dumpram PATH | stage | quit`.
Native buttons lowercase (`start x o ...`); oracle Capitalized (`Start Cross ...`).
Note: a slow run (oracle boot/FMVs) can make `send` return before it finishes — use `out` to re-read,
or just send the next command (the process persists). Convert PPM shots with PIL to view.

### `tools/bgm.py` — BGM / libsnd state inspector for ANY 2MB RAM dump
Works on oracle scene snapshots (`scratch/bin/tomba2/state/*.bin`) and native `dumpram`/`PSXPORT_RAMDUMP`
dumps — the reliable, navigation-free way to diff sound state across cores/scenes.
```
tools/bgm.py dump   <ram> [ram2 …]   # one-line summary: current song, active slots, seqtbl, playmask
tools/bgm.py slots  <ram>            # per-slot table (flag/bit0, read-ptr vs base, volumes)
tools/bgm.py callers <ram> <hexaddr> # jal callers + data-word refs (RE aid)
```

## Reliable references (no menu navigation needed)
- Oracle savestates: `scratch/state/title.sav`, `scratch/state/newgame.sav` (post-New-Game prologue,
  fisherman scene, song 4 playing). Oracle RAM snapshots: `scratch/bin/tomba2/state/{title,demo,
  newgame,loading}.bin`.
- Native: reach scenes via `tap x` (Cross confirms menus/advances cutscenes — NOT Start, which pauses).
  `PSXPORT_NO_FMV=1` skips intro FMVs (drive.py sets it). `PSXPORT_BGMDBG=1` logs every FUN_80074BF8
  start / FUN_80074E48 stop with caller `ra`, plus a per-frame `[bgmtick]` read-ptr-advance probe.

## Known gaps / TODO — extend the tools as needed
- **Oracle title-menu input via REPL `tap`/`press` does NOT navigate the menu** (cause TBD). So driving
  the oracle from cold boot / title to a fresh scene is currently blocked; use the savestates above, or
  fix this (it's the main lever for catching first-time triggers like the prologue BGM-start). When
  fixed, document it here.
- `drive.py` drain heuristic (quiet-for-settle) can return early on slow oracle runs — `out` re-reads.
- Add a `dump`/diff convenience to drive.py (auto dumpram both cores + bgm.py diff) when that recurs.

Extend these tools rather than re-deriving by hand; keep this skill and `docs/journal.md` updated when you do.
