# Driving the Tomba!2 port — input, automation, reaching a scene

How to get the PORT (`scratch/bin/tomba2_port`) to a target scene and feed it input, headless or live.
This exists because driving the game keeps getting re-figured-out. Pairs with `tomba2-newgame.md`
(title→New Game menu RE), `tomba2-scene-state.md` (state signals), `render-arch.md`, `config.md`.

## ⭐ REACHING REAL FREE-ROAM GAMEPLAY HEADLESS — `PSXPORT_AUTO_SKIP=1` (read this; it keeps getting lost)
**`PSXPORT_AUTO_SKIP=1` now drives all the way into the real, player-CONTROLLABLE free-roam field** —
implemented as a self-contained auto-drive state machine in `runtime/recomp/native_boot.cpp` (later-240).
It: (0) taps **Cross** until task0 enters the GAME stage (`stage=0x8010637C`); (1) waits for the post-NewGame
**intro cutscene** to start (cutscene-active flag `*(0x1F800137)` → 1); (2) pulses **Start** to SKIP the
cutscene (it does NOT end on its own — Start ends it) until the flag clears, then settles ~2s through the
end-fade. Lands controllable (verified: idle frame-to-frame Δ≈0px, holding a direction pans the camera
~70k px, and Start opens the pause menu). Recipe:
```
PSXPORT_AUTO_SKIP=1 PSXPORT_VK_HEADLESS=1 PSXPORT_NOAUDIO=1 PSXPORT_REPL=1 \
  ./scratch/bin/tomba2_port scratch/bin/tomba2/MAIN.EXE   # then REPL: run 400 ; shot/dumpram ; quit
```
The boot log prints `[autoskip] free-roam reached at frame N` when control is handed off (~f216).

**DEAD env vars / CORRECTED earlier claim:** `PSXPORT_AUTO_GAMEPLAY` and the old numeric `PSXPORT_AUTO_SKIP=500`
were referenced only in docs and **read by no code** — a no-input run never mashed anything; it just sat in the
**attract DEMO** (`stage=0x801062E4`, the game playing predetermined input). The attract demo is PSX-rendered
playback, NOT the GAME free-roam field — the native render orchestrator `ov_render_frame` is DORMANT there (and
even in the GAME field; the field renders via the interpreted overlay entity loop — see later-240 in journal).
Do NOT use the attract demo to judge the native render path.
Verify you're in free-roam (not the menu/cutscene) before rendering: object-list head 0x800FB168 != 0 AND the
cutscene flag `*(0x1F800137)` == 0.

## 0. Gotchas that waste time
- **Headless runs auto-SKIP the intro FMVs and fast-forward in-game FMVs** (later-134). A field probe is
  ~1.4s, not ~77s — the intro movie used to be played back in REAL TIME even headless. Just use
  `PSXPORT_VK_HEADLESS=1`; no flag needed. (`PSXPORT_NO_FMV=0` forces FMVs back on if ever required.)
  Standard fast field probe: `PSXPORT_DEBUG=<chan> PSXPORT_GEOMBLK_FRAME=600 PSXPORT_ASPECT=16:9
  PSXPORT_VK_HEADLESS=1 PSXPORT_AUTO_GAMEPLAY=1 PSXPORT_NATIVE_FRAMES=620 PSXPORT_NOAUDIO=1` → field at
  present-frame 328, stable thereafter.
- **Headless is silent automatically** — audio opens only for a real window (`PSXPORT_GPU_WINDOW`); a
  headless / `PSXPORT_VK_HEADLESS` run never touches the sound device. (`PSXPORT_NOAUDIO` still mutes a
  windowed run. WAV capture `PSXPORT_WAV` is independent and works headless.)
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
- **`PSXPORT_AUTO_SKIP=1`** — THE way to reach real free-roam gameplay headless. Drives title → NewGame
  (Cross) → GAME stage → SKIPS the intro cutscene (Start, keyed on the cutscene flag `*(0x1F800137)`) → hands
  off in the controllable field. See the ⭐ callout at the top for the full recipe + verification.
- ~~`PSXPORT_AUTO_NEWGAME`~~ / ~~`PSXPORT_AUTO_GAMEPLAY`~~ — **DEAD** (read by no code; referenced only in
  stale docs). A no-input run sits in the attract DEMO, never the GAME field. Use `PSXPORT_AUTO_SKIP=1`.
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

### RE commands (later-134) — inspect/poke/call live, no recompile-a-probe loop
- `w8 A V` / `w16 A V` / `w32 A V` — poke a byte/half/word into guest RAM (hex addr + value).
- `call A [a0 a1 a2 a3]` — run the guest function at A on the live CPU context (rec_dispatch), report
  `v0`/`v1`. SIDE EFFECTS ARE REAL (runs at the frame boundary). E.g. `call 80051c8c <node>` builds an
  object's transform; `call 80051b04 <cmd> <group> <sub>` exercises the geomblk leaf.
- `ents` — walk BOTH entity lists (heads 0x800fb168 / 0x800f2624): per node `addr type pos handler
  rflag cmds geomblk`. The fastest way to see what's spawned + each object's render-command count.
- `node A` — decode one entity node (type/state/rflag/handler/pos/rot/model/cmd-list at node+0xc0[]).
- `geomblk G S` — model-table lookup `T=*(0x800ECF58+G*4); geomblk=T+*(T+S*4+4)` (the data-driven
  geometry resolver, RE later-132).
- Headless-present NOTE: `vkshot` crops to the 4:3 display region; widescreen present width (428) is a
  separate TODO, so the wide margin is in the OT but won't SHOW in a shot yet — verify it via `rcmd`.

## 4. Scene-state signals (RE — to know WHERE you are without screenshots)
- `*(u8)0x800BE258` — **0** = StrPlayer/overlay (logos/title/Loading); **2** (sticky) = 3D engine live
  (gameplay OR attract demo).
- `*(u8)0x1F800137` — **!=0** = real play; **==0** = attract-demo driver running.
- stage pointer `*(u32)0x801fe00c`; GAME stage = `0x8010637C`.

## 5. Reaching free-roam playable gameplay headless — SOLVED (mash Start via AUTO_SKIP; see ⭐ callout)
`AUTO_GAMEPLAY` lands (post-intro, stage GAME) on the seaside→green-field with quest banner "Go to the
Burning House" and an **auto-appearing menu "Options / Load data / Quit game"** (feather cursor on
"Options"). This menu appears WITHOUT input (~f700). The earlier note that it "does NOT respond to forced
Start/Circle/Down" is **partly falsified (later-112): forced Cross (0x4000) at this menu DOES select the
cursor item** — with the cursor on "Options" it enters the Options submenu (page 3 → `FUN_8007b45c`;
verified via the `ov_options_menu` override hit, `PSXPORT_DEBUG=ui`). So this IS the live in-game pause
menu, not a stuck/save-prompt state. Its state machine is RE'd in `docs/engine_re.md` "In-game pause /
Options menu" (page byte `task+0x6B`, dispatch table `0x801062EC`). **Still open:** driving from this menu
into controllable *free-roam* (cleanly closing it back to play).

**UPDATE (later-173): free-roam IS reachable headless — the first half of §5 is SOLVED.** The "auto-appearing
menu" was an artifact of OVER-pulsing Start: `AUTO_GAMEPLAY` releases input at f328 while the post-arrival
fisherman DIALOG cutscene is still up, so Tomba is never controllable. The fix is to keep pulsing Start
through the dialog with **`PSXPORT_AUTO_SKIP=500`**, THEN hold a direction with `PSXPORT_AUTO_WALK`:
```
PSXPORT_DEBUG=state PSXPORT_VK_HEADLESS=1 PSXPORT_AUTO_GAMEPLAY=1 PSXPORT_AUTO_SKIP=500 \
  PSXPORT_AUTO_WALK=r PSXPORT_NATIVE_FRAMES=1600 PSXPORT_NOAUDIO=1 scratch/bin/tomba2_port
```
Tomba then WALKS — camera pos `_DAT_1f8000d2/d6/da` pans (holding right ~3270→5330, left ~4012→3991), and a
screenshot (`PSXPORT_VK_SHOTSEQ`) confirms the green village field is reached with Tomba present. This is a
**deterministic free-roam MOTION scene** (useful for verifying camera-follow / animation systems, which the
idle field — static, A==B — cannot exercise).
- **Knowing the state RELIABLY — `PSXPORT_DEBUG=state`.** Dumps all 3 cooperative-task slots (state@+0x00,
  entry@+0x0c at `0x801fe000 + i*0x70`) on change. A pause/in-game menu is a SEPARATE task — if one ever
  spawns it shows as a slot going alive with a new entry. **CORRECTION of an earlier (later-173) claim made
  from a BROKEN probe:** there is **NO pause menu** in these runs — across AUTO_GAMEPLAY / +AUTO_SKIP /
  +AUTO_JUMP, 0 menu tasks ever spawn and no new task appears after f178 (s0 = the GAME stage 0x8010637C runs
  throughout). The old `nav` probe read `task+0x6B` off the WRONG task (the scheduler's current-task pointer,
  not the menu task), so its "pausePage" / "Cross opens a menu" readings were garbage. **Cross is just JUMP.**
- **Movement geometry (seaside start area):** purely HORIZONTAL — Up/Down move nothing (cam Z stays 2352);
  Left/Right hit hard walls at cam-X ≈ 3991 / 5330. The hut has a visible door but a barrel blocks Tomba at
  the right wall BEFORE the door, so "walk right then Up" does not enter it.
- **`PSXPORT_AUTO_WALK` is a small input SCRIPT** (counted from max(field-reached, AUTO_SKIP)): a single token
  `r`/`l`/`u`/`d`/`x`(Cross/jump)/`o`(Circle)/`t`(Triangle)/`s`(Square) HOLDS that button forever; tokens
  combine (`rx` = right+jump); a comma phase-list `r:250,u:300,rx:120` holds each phase N frames in order then
  releases. Use it to drive toward an exit while `PSXPORT_DEBUG=state` watches for `sm[0x4a]==2`.
- **STILL OPEN — reaching an AREA TRANSITION (`sm[0x4a]==2`).** Walking into either wall does NOT transition
  (`sm[0x4a]` stays 1; `PSXPORT_DEBUG=stage` logs `sm[0x4a]`/`sm[0x4c]`). The seaside area's exit is not a
  plain walk/jump into an edge. Confirmed `ov_game_s4c` (0x80106478, the sm[0x4c] area machine) is NEVER
  entered on the field NOR during the boot area-load (`PSXPORT_DEBUG=stage` ENTER log, 0 hits) — sm[0x4c]
  there is driven by the steady handler 0x801088d8, not 0x80106478. So verifying `ov_game_s4c` needs either
  visual steering of Tomba to the exit, or RE of the exit trigger inside 0x801088d8 (GAME.BIN overlay).
