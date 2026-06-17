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

### `tools/dualcore.py` — DUAL-CORE harness: run port + oracle together, SYNC ON GAME STATE, diff (later-98)
Runs BOTH cores at once (own FIFO each, `scratch/dual/{port,oracle}`) and gates them to the same
guest-RAM **state latch**, not a frame number (their boot timings differ). Default latch =
**`0x800BE258 == 2`** (scene/field active incl. the attract demo; `==0` at title). The stage word
`0x801fe00c` is too coarse (`0x801062E4` = attract = BOTH title and the playing demo; `0x8010649C` = the
START/logo stage).
```
tools/dualcore.py start [oraclestate=PATH]   # launch both (oracle warm-start from a .sav optional)
tools/dualcore.py stage                        # print each core's stage + scene latch
tools/dualcore.py sync [ADDR:VAL] [cap=N]      # advance each until RAM[ADDR]==VAL (default 800be258:2)
tools/dualcore.py step N                        # advance BOTH N frames
tools/dualcore.py shot NAME                     # shot both -> NAME_sbs.png (side-by-side) + NAME_diff.png
tools/dualcore.py send port|oracle "cmd" ...    # raw REPL to one core
tools/dualcore.py stop
```
**KNOWN LIMITATION (later-98):** latching on `0x800BE258==2` lands at the scene-LOAD edge (oracle shot
can be black) and the two cores' **attract demos are NOT frame-locked** — equal `step` drifts (the port's
native-boot demo cycles back to title while the oracle is still mid-scene). For a faithful water/render
compare, prefer loading an **identical guest-RAM snapshot** of a water scene into BOTH cores (oracle:
`-loadstate` .sav; port: needs a `loadram` REPL cmd — TODO — that memcpy's a 2MB dump into g_ram, then
both engines rebuild the same frame from RAM). That sidesteps demo drift entirely. Until then, use `step`
to hunt a visually-matching moment and judge the water region by eye.

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
- **Oracle title menu: use a HELD press, not `tap` (SOLVED, later-79).** REPL `tap Cross` does NOT
  register at the title menu, but a held press does: `press Cross` / `run 30` / `release Cross` selects
  the highlighted item. Cold-boot route: run ~6000f to the title (NewGame highlighted) → held Cross →
  menu becomes StartGame/Options → held Cross → prologue narration. Reusable oracle savestate at the
  prologue: `scratch/state/prologue.sav` (`load scratch/state/prologue.sav`).
- `drive.py` drain heuristic (quiet-for-settle) can return early on slow oracle runs — `out` re-reads.
- Add a `dump`/diff convenience to drive.py (auto dumpram both cores + bgm.py diff) when that recurs.

Extend these tools rather than re-deriving by hand; keep this skill and `docs/journal.md` updated when you do.
