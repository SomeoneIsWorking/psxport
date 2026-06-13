# Tomba! 2 (SCUS-944.54) — high-level scene/mode state in RAM

Goal: find the RAM variable(s) that deterministically identify which screen/mode the
game is in — intro logos, **title screen**, **attract demo**, **"Loading....." screen**,
and **in-game (real gameplay)** — without screen heuristics. Special focus on a clean
"Loading screen is up" signal and an "attract-demo vs real gameplay" signal.

Investigation date 2026-06-13. All addresses KSEG0 virtual. Method: multi-sample RAM
diffs of `dumpram` snapshots + live `PSXPORT_WATCHW` writer tracing + `shot` PNG
brightness labelling, all driven from `scratch/state/title.sav` (which idles at the
title, then auto-rolls into Loading then the attract demo). Scripts and artifacts under
`scratch/agent-scene/`.

---

## TL;DR — the honest result

There is **NO single "current screen/mode" enum**, and (critically) **NO stable RAM word
that distinguishes the title screen from the "Loading....." screen.** This is a *proven*
structural fact, not a failure to look hard enough (see "Why" below). What DOES exist is a
clean, reliable **two-way classifier**:

| signal | StrPlayer-driven screens (intro logos / title / Loading) | in-engine (gameplay incl. attract demo) |
|---|---|---|
| `*(u8)0x800BE258` | **0** | **2** (set once, sticky) |
| `*(u32)0x800A3EC8` | **0** | **0x70** (set once, sticky) |
| render heartbeat (hook @`0x8003CCA4`) | **0 hits/frame** | **nonzero** (intermittent) |

So the deterministic split the port can rely on is **"is the 3D gameplay engine live?"**
The intro logos, the title screen, and the Loading screen are **all the same StrPlayer/MDEC
overlay** playing different streamed video — they are indistinguishable by any *stable*
game-side variable; the only differences between them are per-frame StrPlayer ring/buffer
churn (caught differently in any single snapshot).

This corroborates the prior journal finding (`docs/tomba2-waits.md`): title, Loading, and
the opening FMV all run through the StrPlayer pace loop `0x80050CE4` with byte-identical CPU
registers. This doc adds the matching DATA result: they are also indistinguishable by any
*stable* RAM word.

---

## State-by-state table (verified)

Frame numbers are from `scratch/state/title.sav` (`load`, then `run`). Brightness =
mean RGB of the `shot` PNG (0=black, ~111=title MDEC, ~70=bright demo gameplay).

| state | how to reach (from title.sav) | screen / brightness | `0x800BE258` | `0x800A3EC8` | render hits | StrPlayer active? |
|---|---|---|---|---|---|---|
| intro logos | fresh boot, f~0–1700 | SCEA / Whoopee / FMV | 0 | 0 | 0 | partial (FMV only) |
| **title screen** | f~0–810 | streamed title MDEC, ~111 | **0** | **0** | **0** | **yes** |
| **"Loading....." screen** | f~820–1110 | black + dots, ~0.3 | **0** | **0** | **0** | **yes** |
| **attract demo** | f~1114+ | forest gameplay, ~70 | **2** | **0x70** | nonzero | no (torn down) |
| **real in-game** | (see "attract vs real" below) | gameplay | **2** (expected) | **0x70** (expected) | nonzero | no |

The Loading→demo (StrPlayer→engine) handoff is sharp at **f1114**: the engine object
tables populate, `0x800BE258`/`0x800A3EC8` latch, and the StrPlayer per-stream control
block stops being written.

---

## (a) Is there a single "current mode" enum? — NO

A strict multi-sample intersection (4 title + 5 Loading + 4 demo `dumpram` snapshots,
`scratch/agent-scene/diff4.py`) found **171923** words that are stable-within-each-state
AND differ across states — and **every single one** falls in the class
`title == Loading != demo`. **Zero** words are stable and differ between title and Loading.

Every candidate that *looked* like a title/Loading discriminator in a single-snapshot diff
(`0x80008F7C`, `0x80009138`, `0x800ABFC8`, `0x800ABCC4`, `0x800AC298`, `0x8010274C`, …) was
falsified by watchpoint/dense-sampling: each is **per-frame StrPlayer or kernel churn**
caught at a snapshot, not a mode latch. Documented dead ends below.

## (b) Smallest set of flags that disambiguates the states

Because title and Loading are RAM-identical, the 5 requested states collapse, by *stable
RAM*, into exactly **two distinguishable classes**:

1. **StrPlayer/overlay class** (intro logos ∪ title ∪ Loading): `0x800BE258 == 0`.
2. **In-engine class** (attract demo ∪ real gameplay): `0x800BE258 == 2` (sticky).

To split *within* the StrPlayer class (title vs Loading vs which logo) you must look at
**transient/streaming state**, not a stable word — see "(c)" and "How to detect Loading".

## (c) "Is the Loading screen active?" and "attract vs gameplay"

### "Loading screen active" — no stable flag; use the StrPlayer + screen-black combo
There is no stable "loading mode" word. The robust port-side test is the **conjunction**:

```
loading_screen_active  ≈  (engine NOT live)  AND  (presented frame is ~black)
                       =  (0x800BE258 == 0)  AND  (mean framebuffer brightness ≈ 0)
```

- `0x800BE258 == 0` excludes the demo/gameplay.
- the title MDEC is bright (~111); the "Loading....." screen is black (~0.3). The dark/idle
  detector the runtime already has (`-repl` reports `dark=…`; see `runtime/main.cpp`) is the
  intended black-screen signal. The intro FMV/logos can also be black, but those occur only
  before the title is reached, so within the title↔demo window this conjunction is
  unambiguous for "Loading".

This matches `docs/tomba2-waits.md`'s conclusion that a clean Loading-only skip needs a
*scene-content* discriminator (the StrPlayer command-list / the "Loading" glyph string at
`0x800107D4`), because no register/flag separates Loading from the idle title — confirmed
here at the data level.

### "Attract demo vs real gameplay" — same engine; NOT split by the above latch
Both the attract demo and real New-Game gameplay are the real 3D engine, so both have
`0x800BE258 == 2` and nonzero render hits. The latch does **not** separate them. A dedicated
"is this the attract demo" flag was **NOT found** in this pass — reaching real New-Game
gameplay was not achieved (the title is a streamed MDEC and Start is entangled with the
attract trigger; see `docs/tomba2-waits.md` "title→menu entanglement"). The attract demo is
the only in-engine content reached here, so a demo-vs-real flag could not be diffed (need
both states captured). This is flagged as the open item; concrete next step below.

---

## Writer PCs (cited from live `PSXPORT_WATCHW` runs)

- **`0x800BE258`** (engine-active latch, =2): single writer **PC `0x80075374`** (caller
  `$ra=0x801163A8`, the in-engine render/object path). Written exactly once at f1114, then
  stable. This is the engine object-table init — the cleanest engine-on latch.
- **`0x800A3EC8`** (engine-active, =0x70): writer **PC `0x8004542C`** at f1006 (engine
  setup), stable through the demo.
- **`0x800ED8DC`** (an in-engine actor field, =1): writer **PC `0x8012A110`** at f1115.
- StrPlayer churn (NOT mode vars — dead ends): `0x800ABFC8` toggles 2↔0x22 every ~12 f via
  **PC `0x8008A338`**; `0x800ABCF4` toggles 1↔2 via PCs `0x8008B824`/`0x800863B4`;
  `0x8010274C` is a StrPlayer loop counter incremented by **PC `0x8008AE80`**;
  `0x80009138`/`0x8000907C` are kernel scratch slots written by PC `0x00002890`/`0x00002894`
  with pointer garbage.

---

## Dead ends (do not re-chase)

- **No stable title-vs-Loading word exists** (strict 13-sample intersection → 0). Title and
  Loading are the same StrPlayer playing different streamed video.
- **`0x80008F7C` / `0x80009138` / `0x8000903C`** — looked enum-like in single dumps; are
  volatile kernel/thread scratch (flicker 0↔2 frame-to-frame even within one state).
- **`0x800ABFC8`, `0x800ABCC4`, `0x800ABCDC`, `0x800ABCD4`, `0x800ABCF4`, `0x800AC298`** —
  the StrPlayer per-stream control block (0x800ABCxx–0x800ACxxx): every word is rewritten
  multiple times per frame (ring/segment pointers, double-buffer phase). Single dumps catch
  them mid-cycle; none are stable.
- **`0x8010274C`** — a StrPlayer array-fill loop induction variable (counts 0,1,2,… each
  frame via PC 0x8008AE80); its per-state "stable" snapshot was just the loop's terminal
  count.
- **`0x800675CC` / `0x80067830`** — already falsified for the intro (`docs/tomba2-intro.md`)
  and not revisited; not touched during title/Loading/demo here either.

---

## Open item + concrete next step (attract-demo vs real gameplay)

Need: a flag that is set for the attract demo but clear for real New-Game gameplay (or vice
versa). Could not be diffed because real gameplay was not reached this pass.

Plan:
1. Reach **real New-Game gameplay** (the blocker is input timing into the streamed title
   menu — `docs/tomba2-waits.md` proposes hooking the StrPlayer pad read near
   `0x800856xx`/`0x8008A7xx` and driving a single well-timed Start on the deterministic
   "title is up" edge; the engine-active latch `0x800BE258` flipping 0→2 is now exactly that
   edge detector and removes the FMV-abort frame jitter).
2. With a real-gameplay `dumpram` in hand, re-run `scratch/agent-scene/diff4.py` adding a
   `gameplay` group and look for `attract != gameplay` stable words (likely a single
   "demo/attract mode" boolean the demo playback driver sets; the attract demo replays a
   recorded input stream, so the input-source / "playing demo" flag is the thing to find).

---

## Tooling produced (reusable, under `scratch/agent-scene/`)

- `diff.py` / `diff2.py` / `diff3.py` / `diff4.py` — RAM-dump diff with **multi-sample
  stability** filtering (the key technique: a word must be constant across N snapshots of a
  state to count, which kills the per-frame StrPlayer/kernel churn that defeats single-dump
  diffs). `diff4.py` is the final, strict 3-state intersection + classifier.
- `dumps/` — labelled multi-sample `dumpram` snapshots (t1–t4 title, l1–l5 Loading, d1–d4
  demo).
- `shots/`, `logs/rh.csv` — brightness-labelled `shot` PNGs and the per-frame render-hit log
  (`PSXPORT_GAMELOG`).

### Suggested in-binary helper (NOT built — per task constraint)
A REPL `classify` command (or a `Tomba2_SceneClass()` in `runtime/games/tomba2.cpp`)
returning an enum from the verified signals would make this directly consumable by the port:
`ENGINE_LIVE` if `*(u8)0x800BE258==2`; else `LOADING` if the presented frame is ~black; else
`STRPLAYER_BRIGHT` (title/logo). This needs no new RE — it just packages the findings above.
