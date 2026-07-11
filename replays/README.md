# Replay library

Deterministic pad-capture replays — each is a recorded button sequence (2-byte active-low PSX pad
mask per frame, written by `PSXPORT_PAD_RECORD=<path>`) that reproduces a specific game scenario
**headlessly, bit-for-bit**. Use these to reproduce a bug or reach a game state WITHOUT live user
input — critical for scenarios that need real world-navigation (e.g. walking into a hut) that
AUTO-NAV can't drive.

## How to record a new replay

On a **windowed** run (the user's, or your own with a window), input is recorded by default to
`scratch/bin/pad_session.pad`. To capture a specific scenario to the library:

```
PSXPORT_PAD_RECORD=replays/<category>/<name>.pad ./run.sh
# play the scenario with the keyboard/gamepad, then close the game
```

Then add an entry to this README (category / scenario / how-to-replay / what it surfaces) in the
SAME commit. Categories: `boot-smoke/` (boot + menu nav), `scene-transitions/` (entering
huts/areas/doors), `bugs/` (repros for a specific tracked bug).

## How to replay one

```
# standalone (REPL, inspect state the scenario produces):
printf 'newgame\nrun 9999\nquit\n' \
  | PSXPORT_REPL=1 PSXPORT_VK_HEADLESS=1 PSXPORT_NOAUDIO=1 \
    PSXPORT_PAD_REPLAY=replays/<category>/<name>.pad ./scratch/bin/tomba2_port

# under SBS-full (surface memory divergences the scenario reaches):
PSXPORT_NOWINDOW=1 PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_NOAUDIO=1 \
  PSXPORT_PAD_REPLAY=replays/<category>/<name>.pad ./scratch/bin/tomba2_port

# under SBS-full WITH the debug server (pause at the diverge, then `sbs watch`/`sbs bt`):
PSXPORT_NOWINDOW=1 PSXPORT_DEBUG_SERVER=1 PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_NOAUDIO=1 \
  PSXPORT_PAD_REPLAY=replays/<category>/<name>.pad ./scratch/bin/tomba2_port &
# then: python3 tools/dbgclient.py sbs status   (connects to 127.0.0.1:5959)
```

`newgame` pulses Start to reach the GAME prologue, then the replay's button sequence drives from
there. Replays ALWAYS start from a fresh boot — `PSXPORT_PAD_REPLAY` overrides input from frame 0.
Once the recorded sequence ends, input falls through to the host (so `run N` after it just idles).

## Index

### scene-transitions/

| file | frames | scenario | surfaces |
|------|--------|----------|----------|
| `hut-entry-door-freeze.pad` | 934 | AUTO-NAV to free-roam, then walk right + up into the fisherman's-hut door | **SBS diverge @0x801FE91A (f389)** + a door-transition freeze (`FUN_80073328 case 3`) reaching both cores; see docs/findings/scene.md "Door/area-transition freeze" + docs/findings/sbs.md "spawn-leaf frame residual". The cleanest repro for the hut-entry diverge the live windowed session hits. |
| `hut-entry-alt.pad` | 535 | alternate, shorter hut-entry capture | same hut-entry transition; a second capture for cross-checking. |

### bugs/

| file | frames | scenario | surfaces |
|------|--------|----------|----------|
| `dark-screen-repro.pad` | 61030 | long session reaching a dark/wrong-screen state | the screen-fade / scene-darkening bug family (docs/findings/render.md); use under a window to eyeball. |

### boot-smoke/

| file | frames | scenario | surfaces |
|------|--------|----------|----------|
| `start-mash-smoke.pad` | 1500 | Start-mash through the intro to free-roam | boot→menu→field smoke check (should reach GAME stage cleanly). |
| `short-session.pad` | 426 | short input session | minimal boot capture. |
| `general-session.pad` | 1200 | general play session | generic smoke. |

## Notes

- **Format:** each `.pad` is a raw stream of little-endian `uint16_t` active-low PSX pad masks, one
  per frame. Decode with: `python3 -c "import struct; [print(hex(struct.unpack('<H',open('X.pad','rb').read()[i:i+2])[0])) for i in range(0,len(open('X.pad','rb').read()),2)]"`.
  Active-low bits (0=pressed): 0=R 1=L 2=U 3=D 4=tri 5=cross 6=o 7=sq 8=L2 9=R2 10=L1 11=R1 … start.
- **Determinism:** replays are bit-exact (validated by the door_freeze finding — reproduces
  frame-for-frame across runs). Safe to gate fixes on.
- **Originals** also live at `scratch/bin/*.pad` (referenced by older findings); the copies here are
  the canonical, categorized home going forward.
