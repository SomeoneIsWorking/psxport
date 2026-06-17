# Driving the Tomba!2 port — input, automation, reaching a scene

How to get the PORT (`scratch/bin/tomba2_port`) to a target scene and feed it input, headless or live.
This exists because driving the game keeps getting re-figured-out. Pairs with `tomba2-newgame.md`
(title→New Game menu RE), `tomba2-scene-state.md` (state signals), `render-arch.md`, `config.md`.

## 0. Gotchas that waste time
- **Headless exits at 120 frames unless you set `PSXPORT_NATIVE_FRAMES=N`.** For a long/interactive run
  set it high (e.g. `=100000`). For a capture, set it just past your last frame.
- **Never `pkill -f tomba2_port` from a shell** whose own command line contains "tomba2_port" — `-f`
  matches the full command line and kills your own shell (exit 144). Use `pkill -x tomba2_port`.
- Backgrounding the port with the agent Bash tool's `&` gets the process group reaped — the live debug
  server then dies. Run it in a persistent session (a real terminal / instance-control) if you need the
  server to stay up across agent turns.

## 1. Pad buttons (active-low 16-bit mask; a pressed button CLEARS its bit; PAD_NONE = 0xFFFF)
| button | bit | active-low hold mask | | button | bit | active-low |
|---|---|---|---|---|---|---|
| Start | 0x0008 | `FFF7` | | Cross/X | 0x4000 | `BFFF` |
| Select| 0x0001 | `FFFE` | | Circle/O| 0x2000 | `DFFF` |
| Up    | 0x0010 | `FFEF` | | Triangle| 0x1000 | `EFFF` |
| Right | 0x0020 | `FFDF` | | Square  | 0x8000 | `7FFF` |
| Down  | 0x0040 | `FFBF` | | | | |
| Left  | 0x0080 | `FF7F` | | | | |
The game reads input as EDGES (`current & ~prev`, FUN_800788ac), so a menu advances once per press; a
held direction is what gameplay reads for movement.

## 2. Scripted (deterministic, headless) — env flags
- **`PSXPORT_AUTO_NEWGAME=1`** — owns title→New Game; pulses Cross until the GAME prologue stage
  (`0x8010637C`), then auto-pauses. `=2` also freezes via the debug server.
- **`PSXPORT_AUTO_GAMEPLAY=1`** — owns title→NewGame→fisherman cutscene; pulses **Start** until the chan4
  area music has looped 150 frames, then RELEASES input. ⚠️ The heuristic fires EARLY: it releases at the
  seaside intro (~f328); the green field follows ~f600. **It does NOT reach free-roam gameplay** (see §5).
- **`PSXPORT_FORCE_BUTTONS=<hex>`** — pulse a mask (8 frames on / 24 off, = edges) from frame 0.
- **`PSXPORT_FORCE_HOLD=<hex>` + `PSXPORT_FORCE_HOLD_AT=N`** — HOLD a mask continuously from frame N
  (overrides the pulse; use for movement, or a single edge by also setting STOP_AT a few frames later).
- **`PSXPORT_FORCE_STOP_AT=N`** — release ALL forced input at frame N (go hands-off).
  Example — press Start once at ~f760: `PSXPORT_FORCE_HOLD=FFF7 FORCE_HOLD_AT=760 FORCE_STOP_AT=768`.
  NOTE the FORCE frame counter is the pad-service frame `s_fc`, which may differ slightly from the present
  frame used by `PSXPORT_VK_SHOTSEQ`.

## 3. Live / interactive — the debug server (drive while it runs)
Launch with `PSXPORT_DEBUG_SERVER=1` (port 5959) **and a high `PSXPORT_NATIVE_FRAMES`**. Drive with
`tools/dbgclient.py <cmd>` (or no arg = REPL):
- `tap <btn> [frames]`, `press <btn>`, `release <btn>` — btn = `start x o triangle square up down left right select`.
- `stage`, `scene` (on-demand classified display list), `frame`, `r <addr> [n]` / `rw <addr> [n]` (read mem).
- `vkshot [path]` (headless VK readback → PPM), `shot [path]` (SW), `gputrace [path]` (arm a gpu_differ capture).
- `pause` / `play` / `step`.

## 4. Scene-state signals (RE — to know WHERE you are without screenshots)
- `*(u8)0x800BE258` — **0** = StrPlayer/overlay (logos/title/Loading); **2** (sticky) = 3D engine live
  (gameplay OR attract demo).
- `*(u8)0x1F800137` — **!=0** = real play; **==0** = attract-demo driver running.
- stage pointer `*(u32)0x801fe00c`; GAME stage = `0x8010637C`.

## 5. OPEN: reaching free-roam playable gameplay headless
`AUTO_GAMEPLAY` lands (post-intro, stage GAME) on the seaside→green-field with quest banner "Go to the
Burning House" and an **auto-appearing menu "Options / Load data / Quit game"** (feather cursor on
"Options"). Observed (2026-06-17, headless): this menu appears WITHOUT input (~f700) and does NOT respond
to forced **Start / Circle / Down** (cursor doesn't move) — so either forced input isn't reaching this
state's handler, or it's a stuck/save-prompt state, NOT normal interactive gameplay. **Unresolved:** how
to drive past this into controllable free-roam. Candidates to try next: Cross to confirm a menu item;
verify forced input actually reaches the menu handler (it works during boot/title); check whether this is
a memory-card/save prompt the port's memcard stub mishandles. If you (the user) know the intended
sequence, capture it here.
