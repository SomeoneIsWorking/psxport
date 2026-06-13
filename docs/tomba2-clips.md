# Tomba! 2 (USA, SCUS-944.54) — StrPlayer front-end clip discriminator

Goal: a deterministic, stable, game-state signal that uniquely identifies the
**"Loading....." clip** among the StrPlayer front-end clips (intro logos → opening FMV →
title → Loading → attract), without screen-brightness or drive-idle heuristics.

All values below are from real runs under `runtime/wide60rt` on the OpenBIOS HLE
(`bios/scph5501.bin` = OpenBIOS), driven via `-repl` with `PSXPORT_T2_NOINTROSKIP=1`.
Scripts/artifacts: `scratch/agent-clip/{dumps,shots,logs}`. Disasm via `tools/disasm.py`
on `scratch/bin/tomba2/state/title.bin`. Builds on `docs/tomba2-threads.md` (the
StrPlayer coroutine task array @ 0x801FE000, stride 0x70, resumePC at +0x0C) and
`docs/tomba2-scene-state.md` (the engine-live latch 0x800BE258).

NOTE: REPL `r <addr> <len>` reads `len` **bytes** little-endian, not words.
Read the resumePC word with `r 0x801FE07C 4`.

---

## TL;DR — the deterministic Loading read

```
loading_clip_active  ==  ( *(u32)0x801FE07C == 0x800452C0 )  AND  ( *(u8)0x800BE258 == 0 )
```

- `*(u32)0x801FE07C` = the StrPlayer **control coroutine's resumePC** (task1 @ 0x801FE07C).
  It is a *clip-GROUP* discriminator (function-per-mode), rock-stable for the whole clip.
- `*(u8)0x800BE258` = the engine-live latch from `docs/tomba2-scene-state.md`
  (0 while StrPlayer owns the screen; sticky 2 once the 3D engine is live = attract/gameplay).

Both reads are necessary: resumePC `0x800452C0` is **shared** by Loading AND lingers
(stale) into the attract demo, so the engine latch is what separates Loading (engine off)
from attract (engine on). See the table.

This is a *true game-state* read (a coroutine resume-pointer + an engine init latch), not a
pixel/idle heuristic.

---

## value → clip table (verified, stable across the whole clip)

Sampled every 30 frames; each value held constant across **all** in-clip samples (28+ for
title, 11+ for Loading, etc.), not just at the edge. `dark` = the runtime's black-screen
flag (shown for cross-check only — NOT used in the signal except where noted).

| clip / screen | reach | `*(u32)0x801FE07C` | `*(u8)0x800BE258` | dark | unique? |
|---|---|---|---|---|---|
| (early boot, pre-StrPlayer) | fresh boot f0–~1110 | `0x00000000` | 0 | mixed | n/a |
| clip-advance transient | each clip boundary (f1140, f1380…) | `0x8001DB38` | 0 | — | boundary only (1 frame) |
| **Whoopee Camp logo** | fresh boot ~f1170–1350 | `0x8004514C` | 0 | 0 | **yes (logo group)** |
| **opening FMV** (Tomba falling) | fresh boot ~f1410–2070+ | `0x80044F58` | 0 | mixed | shared w/ title (see below) |
| **title screen (MDEC)** | title.sav f0–810 | `0x80044F58` | 0 | 0 | shared w/ FMV |
| **"Loading....." screen** | title.sav f820–~1110 | `0x800452C0` | **0** | 1 | **yes, with latch=0** |
| **attract demo** (3D engine) | title.sav f1114+ | `0x800452C0` (stale) | **2** | 0 | latch=2 splits it |

Verified screenshots (`scratch/agent-clip/shots/`): `boot_f1170.png` = "Whoopee Camp" logo
(resumePC `0x8004514C`); `boot_f1610.png` = Tomba FMV close-up, `boot_f2210.png` = Tomba
falling (both resumePC `0x80044F58`); `loading.png` = black + "Loading....." (resumePC
`0x800452C0`, latch 0, dark 1); `attract.png` = forest gameplay w/ "DEMO" overlay (resumePC
still `0x800452C0` but latch **2**).

---

## What the resumePC actually discriminates (read this before trusting it)

`0x801FE07C` is task1's `resumePC` — the **entry function of the StrPlayer control
coroutine, re-set per playback MODE**. It is therefore a *clip-group* key, not a per-clip
key. Disassembly (all funnel through the clip-spawn helper `0x8001DC40` and the front-end
input/advance helper `0x80051FB4`):

- **`0x8004514C`** — logo-clip control. Calls `0x8001DC40` twice and **`jal 0x80044F58`**
  (so 0x80044F58 is a shared subroutine). Used by the boot logos (Whoopee Camp etc.).
- **`0x80044F58`** — FMV/title presentation control. Branches on a scratchpad state byte
  `*(*(0x1F800138) + 0x6D) == 2`, calls `0x8001DC40` with clip-id `0x800`. **Used by BOTH
  the opening FMV and the title MDEC** — they share this resumePC.
- **`0x800452C0`** — interactive front-end control with **pad polling** (reads the pad at
  `0x1F800278` / `+0x5E6`, masks the `0x2F` / Start bits, calls `0x80045258` and
  `0x80051FB4`). This is the title-idle / Loading "waiting for input or stream-done" loop.
  **Used by Loading AND left stale in 0x801FE07C through the attract demo.**
- **`0x8001DB38`** — clip-advance transient: calls `0x8001D940` (the per-clip writer that
  touches 0x800BE0E4) then yields via `0x80051FB4`. Appears for ~1 frame at each clip
  boundary; not a steady state.

Writer of 0x801FE07C (cited, `PSXPORT_WATCHW=0x801FE07C` from title.sav):
```
f820  pc=80051F3C ra=80051F40  writes 800452C0 (w4) -> [001FE07C]
```
Exactly one write at the title→Loading edge (f820); the StrPlayer scheduler at 0x80051F3C
re-creates task1 with the new entry. Between writes the value is stable (no churn) — this
is why sampling every 30f shows a single constant per phase.

---

## Why the conjunction is the right signal (and is unambiguous in the port's window)

There is **no single stable RAM word that names "Loading"** (re-confirmed; consistent with
`docs/tomba2-scene-state.md`'s strict 13-sample intersection → 0 title-vs-Loading words).
But the requested signal exists as a clean two-read conjunction:

1. `*(u32)0x801FE07C == 0x800452C0` excludes the title MDEC and the opening FMV/logos
   (those are `0x80044F58` / `0x8004514C` / `0x00000000`). The only StrPlayer mode that uses
   `0x800452C0` is the title-idle/Loading interactive loop.
2. `*(u8)0x800BE258 == 0` excludes the attract demo and real gameplay (engine live ⇒ 2),
   which keep `0x800452C0` stale in the resumePC slot.

Within the runtime's operating window (title → Loading → attract), the only state matching
both is the "Loading....." clip. The opening FMV (which shares no value with Loading anyway)
occurs strictly before the title is ever reached, so it cannot collide.

Distinguishing **title vs Loading** specifically (the thing the prior passes proved had no
stable RAM word): the resumePC **does** split them — title = `0x80044F58`, Loading =
`0x800452C0`. The earlier "no stable word" result stands for a *single data word*; the
resumePC is a code pointer in the coroutine task block, which those passes did not treat as
the discriminator. This pass confirms it is stable and splits title from Loading.

---

## Honest limits / caveats

- resumePC is a clip-GROUP key, not per-individual-clip. It cannot tell the opening FMV from
  the title (both `0x80044F58`), nor Loading from attract by itself (both `0x800452C0`). The
  engine latch resolves the latter; the former never overlaps in time with Loading.
- If a future need is "which exact logo / which exact FMV", resumePC will not give it —
  you'd need the scratchpad clip-id (`*(*(0x1F800138)+0x6C/0x6D)`, the byte the handlers
  branch on). That scratchpad (0x1F8000xx) is NOT in `dumpram`/`r` (main-RAM only), so it
  was not sampled here; out of scope for the Loading objective.

## Dead ends (do not re-chase)

- **`0x800BE0E4`** (prior "per-clip state" lead): reads **0x00000000 throughout** title,
  Loading, logos, FMV, and into attract in every sample taken here. Its writer 0x8001D95C
  pulses it at boundaries but the resting value is 0 — useless as a stable per-clip
  discriminator. Falsifies the tomba2-threads.md hint that it carries a per-clip state.
- A single stable RAM **word** that uniquely flags Loading: still none (consistent with
  tomba2-scene-state.md). The working signal is the resumePC code-pointer + latch
  conjunction above, not a data word.
- Brightness / `dark` flag: correlates (Loading is black) but is explicitly NOT relied on —
  it cannot tell a dark FMV from Loading. The conjunction above does not use it.
