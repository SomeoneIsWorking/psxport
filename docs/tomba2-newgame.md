# Tomba! 2 (USA, SCUS-944.54) — reaching real New-Game gameplay headless

Investigation date 2026-06-13. Goal: reach **real New-Game gameplay** headless (not the
attract demo), and characterize (1) an attract-vs-real distinguisher, and (2) the New-Game
opening states. Read `docs/tomba2-threads.md`, `docs/tomba2-scene-state.md`,
`docs/tomba2-waits.md` first.

All addresses KSEG0 virtual. Driven via `runtime/wide60rt -repl` on the OpenBIOS HLE,
from `scratch/state/title.sav`, **with default hooks** (no `PSXPORT_T2_NOINTROSKIP`).
Tools/artifacts under `scratch/agent-newgame/` (`repl.sh` driver, `bright.py` PNG mean-RGB,
`dumps/`, `shots/`).

---

## TL;DR — what was achieved

- **Real New-Game gameplay IS reachable headless.** The prior passes' claim that the title
  menu is "entangled with the attract trigger" is **wrong / falsified**: the title menu
  reads the pad fine and the cursor responds to Left/Right. The correct input sequence is
  documented below.
- **Savestate created:** `scratch/state/newgame.sav` (+ `scratch/bin/tomba2/state/newgame.bin`
  + shot `scratch/agent-newgame/shots/newgame_first.png`) at the first engine-live moment of
  the New-Game opening (the seaside fisherman intro scene, `0x800BE258==2`). Verified to
  reload to the same scene.
- **Attract-vs-real distinguisher FOUND** (semantic, not a coincidental level diff): the
  **attract-demo playback driver `0x80145674` runs every frame during the attract demo and
  never during real gameplay**, gated by **`*(u8)0x1F800137`** (PSX scratchpad): `==0` →
  attract demo active, `!=0` → real play. Proven by execution count, see §3.

---

## 1. The input sequence to reach real New Game (from `title.sav`)

The title is a two-page menu, both pages read the pad through the title-menu state machine
at **`0x80106320`** (state object `0x801FE000`; menu state index = `*(u16)0x801FE048`,
dispatched via 8-entry jump table at `0x8010622C`; cursor byte = `*(u8)0x801FE068`).

Page 1 (the `title_chk.png` screen): **`NewGame`** (left, cursor=0) / **`LoadGame`**
(right, cursor=1).
Page 2 (after confirming NewGame): **`StartGame`** (left, cursor=0) / **`Options`** (right).

**Confirm = Cross (X)** (also Start works for the first confirm). **Circle = cancel/ignored.**
Cursor moves with Left/Right and is clamped at both ends (verified: `0x801FE068` 0↔1).

Reliable sequence (verified, lands engine-live New Game ~f2700–2970, with run-to-run
jitter of ~hundreds of frames because the opening cutscene is streamed):

```
load scratch/state/title.sav
run 40            # let the menu settle (cursor already on NewGame=0)
tap Cross 6       # NewGame -> page 2 (StartGame/Options)
run 60
tap Cross 6       # StartGame -> begins New Game
run ~2700+        # streamed opening cutscene plays; engine latches 0x800BE258=2
```

`scratch/agent-newgame/save_ng.txt` is the exact script used to produce `newgame.sav`
(it `run 3960` after the second Cross and saves at the first engine-live frame).

### Verified screens along the path (shots in `scratch/agent-newgame/shots/`)
- `title_f2.png` — page 1, NewGame highlighted (brightness ~111).
- `state3_screen.png` — page 2 after first Cross: **"StartGame" / "Options"**.
- `ng2_b.png` — first opening story panel (streamed): *"Tomba is living peacefully in the
  country when Zippo finds a mysterious letter addressed to Tomba."*
- `ng3_900.png`, `ng4_2100.png` — more streamed story panels (*"...friend Tabby has
  disappeared..."*, *"...Tomba jumps into the sea"*, *"And then..."*).
- `newgame_first.png` / `newgame_reload.png` — **engine live**: Tomba on the seaside cliff,
  the old fisherman, dialogue box *"No wonder they weren't biting."* (`0x800BE258==2`,
  `0x800A3EC8==0x70`).
- `reach1.png`, `adv_2.png` — further into the scripted opening (Tabby + Tomba, *"hmm?"*,
  *"Do you smell something burning? Tomba, wait here..."*).

---

## 2. The New-Game opening states & gating

The New-Game opening is, in order:
1. **Streamed story-text panels** (MDEC, StrPlayer overlay): `0x800BE258==0`, CPU in the
   StrPlayer pace loop `0x80050CE4` (same machinery as title/Loading per the other docs).
   These **auto-advance on a timer** (verified: panels change content over ~150-frame
   intervals with NO input — see `story_p1/p2/p3.png`). They are NOT interactive text; there
   is no per-character advance gate to skip — they behave exactly like the title/FMV MDEC and
   would be collapsed by the same `FmvDwellSkip`-style latch proposed in
   `docs/tomba2-fmv-skip.md`, not a new mechanism.
2. **Engine latch:** `*(u8)0x800BE258` flips 0→2 and `*(u32)0x800A3EC8` → 0x70 (the same
   engine-on latch documented in `docs/tomba2-scene-state.md`; writer PC `0x80075374`). After
   this the seaside level is loaded and rendering.
3. **In-engine scripted dialogue** (the fisherman / Tabby intro): interactive text boxes with
   a typewriter effect, advanced by **Cross** (verified: `dlg_b.png` empty box →
   `dlg_d.png` full line *"No wonder they weren't biting."*). This is a multi-scene scripted
   sequence with camera cuts before the player gets free roam.

**Story-text/dialogue advance variable — NOT pinned (honest):** the streamed panels need no
variable (timer-driven MDEC). For the in-engine dialogue, the text-progress counter was not
isolated cleanly — the monotonic-counter dump diff (`dlg_typing.bin` vs `dlg_full.bin`)
surfaced only object/animation coordinates (e.g. `0x800F4F24`, writer `0x80075FB4` — a render
path, NOT a text counter). The prior docs already note the glyph-draw site is referenced
indirectly and isn't trivially hookable; pinning the exact text-advance latch needs a hook on
the font/glyph renderer, left for a focused next pass. **Cross advances the dialogue**, which
is the lever a skip would drive.

**First playable / level load:** the first level (seaside cliff) is already resident by the
time the engine latches — the bottleneck on this path is the **streamed opening cutscene**
(MDEC pacing), not a CD level-load. `cdclog` was not the limiter here (the prior `cd_instant`
+ BIOS HLE layer already covers the read bursts).

---

## 3. Attract-demo vs real New-Game distinguisher (the open item — RESOLVED)

Both run the 3D engine, so `0x800BE258==2` / `0x800A3EC8==0x70` / nonzero render hits do
**NOT** separate them (confirmed). The distinguisher is the **attract-demo playback driver**:

- **Driver function `0x80145674`** (per-frame, `a0=0x801005C8` = the demo control struct,
  invoked from `0x80146458`). It is the demo's recorded-input replay engine: it range-checks
  the vblank tick `*(u16)0x1F800160` against scripted windows (e.g. `[0x1388, 0x1388+0x1e79)`)
  to release recorded input segments, and OR-sets a per-actor "demo-driven" bit across a
  5-entry actor table at `0x801005D8`.
- **The gate:** the dispatcher `0x801463EC` runs **every frame in BOTH** attract demo and
  real play (`a0=0x801005C8`, demo state byte `*(u8)0x801005CC==1` in both — so neither the
  dispatcher nor that state byte separates them). The deciding branch is at `0x8014643C`:
  `lbu $v0, 0x137($1F80)` → `bnez 0x80146468` (skip). I.e. the driver call at `0x80146450`
  is taken **only when `*(u8)0x1F800137 == 0`**.

### Proof (execution counts, cited)
| event | attract demo (idle from title, ~240f) | real New Game (`newgame.sav`, ~200f) |
|---|---|---|
| driver `0x80145674` hits | **142** (every frame) | **0** |
| driver-call site `0x80146450` hits | **97** | **0** |
| dispatcher `0x801463EC` hits | 98 | 150 (runs in both!) |
| state byte `*(u8)0x801005CC` | 1 | 1 (does NOT separate) |

So the deterministic distinguisher is:

> **attract demo ⇔ the demo replay driver `0x80145674` executes this frame**
> ⇔ **`*(u8)0x1F800137 == 0`** (and demo state `*(u8)0x801005CC==1`).
> **real New-Game play ⇔ `*(u8)0x1F800137 != 0`** (driver never runs).

`0x1F800137` is in the PSX **scratchpad** (0x1F800000 region), NOT in the 2 MB main-RAM
`dumpram`/`r`-readable space (the REPL `r` masks `&0x1FFFFF`, aliasing it into low RAM — do
not read it that way). To consume this in the port, hook the branch at `0x8014643C`, or
simply detect whether the demo driver `0x80145674` ran this frame (a `Tomba2_GetAndReset`
counter on a hook, like the render-hit hook already in the runtime). The driver-running
signal is the cleanest, hook-friendly form of the flag.

### Why the RAM-dump diff candidates are NOT the answer (dead end recorded)
A 3-demo × 3-newgame stable-intersection diff (`scratch/agent-newgame`, all dumps stable
within group, differing across) yielded ~46 boolean-like words, e.g. the 4-word run
`0x801005D8..E4` (=1,1,1,1 in demo, 0 in newgame). **These are the per-actor "demo-driven"
bits the driver itself sets — and most other candidates are just level-content differences
between two different levels (seaside vs forest).** A word that merely differs between the
demo's forest and the New-Game seaside is NOT a proven attract-vs-real flag. The execution-
count proof above is the rigorous result; the dump-diff words are downstream effects, not the
gate. Do not treat a single level-diff word as the distinguisher.

---

## 4. Falsified / corrected from prior docs
- **`docs/tomba2-waits.md` "title→menu entanglement" is overstated.** The title menu reads the
  pad and the cursor responds to Left/Right (`*(u8)0x801FE068`); New Game is reachable with
  two Cross presses (NewGame→StartGame). The earlier failure was input *timing/sequence*
  (single early Cross, or not knowing it is a two-page menu), not a genuine pad entanglement.
- The opening **story text is streamed MDEC (auto-advancing), not interactive** — it is the
  same StrPlayer pace-loop content as title/Loading, so it needs the same FMV-dwell latch,
  not a text-skip mechanism.

## 5. Reusable levers for the port
- **Title menu:** state `0x80106320`; menu-state index `*(u16)0x801FE048` (jump table
  `0x8010622C`); cursor `*(u8)0x801FE068` (0=NewGame/StartGame, 1=LoadGame/Options).
- **Engine-on latch:** `*(u8)0x800BE258==2` (writer `0x80075374`) / `*(u32)0x800A3EC8==0x70`.
- **Attract-vs-real:** demo driver `0x80145674` runs (⇔ `*(u8)0x1F800137==0`). Hook it.
