# Tomba! 2 (SCUS-944.54) — gameplay-side waits survey (post-intro)

Survey date 2026-06-13. Goal: find waits/dwells/loads AFTER the already-handled
opening logos + FMVs that cost the player noticeable time and could be natively
skipped/accelerated, the same treatment as the intro. **RE + PROPOSE ONLY** — no
source edits, no build, no commit. Read `docs/tomba2-intro.md` and
`docs/tomba2-fmv-skip.md` first; the logo dwells, inter-stage black dwell,
inter-FMV Whoopee-logo prebuffer, and CD-load acceleration (instant-CD + BIOS
`cdromBlockReading` HLE) are already handled and are **not** re-reported here.

All addresses KSEG0 virtual. Driven via the already-built `./runtime/wide60rt`
`-repl` (no `PSXPORT_HLE_BIOS`). Screenshots/meanRGB in
`scratch/screenshots/waits/`; RAM dumps in `scratch/bin/tomba2/`. meanRGB parser
`scratch/ppm.py`.

---

## How far I got, and the honest limiting factor

Reached, via REPL (held-Start intro skip + Start-tap FMV abort): boot → logos
(skipped) → **opening movie** → **post-movie load (black)** → **title screen** →
**attract DEMO (in-engine 3D gameplay)**. I confirmed the gameplay engine renders
(the demo runs the in-engine display-deadline VSync wait `0x801157xx`, see below).

**I could NOT reliably land on the title MENU and start New Game.** Reason, RE'd and
worth recording: the **title screen is itself a streamed MDEC sequence** running in
the StrPlayer/FMV overlay (profile of the idle title screen is 97% game with the top
PCs all in the FMV player range `0x80082xxx–0x8009Axxx`, `0x800834A0` StrPlayer main
loop + `0x8008B6D0` MDEC decode-complete poll — identical to FMV playback). The
title's "press Start" handling is therefore entangled with the FMV player's pad/abort
logic, and a Start press at the title is consumed by the attract-demo trigger rather
than opening a New/Continue menu. Combined with the only feedback channel being
periodic screenshots and the FMV-abort landing on a **jittery frame count** (the abort
fires when the MDEC decoder reaches a segment boundary, which varies run-to-run by tens
of frames), threading the exact press needed to open the menu was not achievable in
this pass. The waits below are the ones on the reachable path; the New-Game story-text
dwell and the first-level load are flagged as **un-surveyed, next pass** with a
concrete plan.

---

## Prioritized waits found

### 1. Post-opening-movie → title-screen load (black). MDEC/FMV-drain + CD. ~240 f / ~4 s.
**Where:** immediately after the opening story movie ends (or is aborted), before the
title screen appears. Screen is **black** the whole time.
**Measured:** with the FMV aborted by Start-taps, black spans ~f520 → ~f760
(`scratch/screenshots/waits/b520..b760.ppm`, all meanRGB ≈ (0.5,0.5,0.5)), then the
title/demo overlay appears. ~240 frames.
**Kind:** mixed **(d) MDEC/FMV + (a) CD**. `cdclog 1` over the gap shows *continuous*
`irq 1` (CD data-ready) every frame — it is still streaming. `prof 120` inside the gap
is 97.9% game, top PCs `0x800834A0` (13.9%, StrPlayer main loop), `0x8008B6D0` (4.1%,
MDEC decode-complete poll, `and 0x01000000` on the GPU/MDEC status word — documented in
`docs/tomba2-fmv-skip.md`), `0x80082DF0–E00` (a tight 5-instr StrPlayer spin). So this
is the FMV player **finishing its stream drain and loading the next (title) overlay**,
not new game-logic work.
**Gating mechanism:** the StrPlayer stream-end + ring-drain path (same machinery as the
inter-FMV hold, `docs/tomba2-fmv-skip.md` §1): `0x8008A700`/`0x8008AE00` prebuffer waits,
ring position from `0x80085900`, decode poll `0x8008B6D0`. No single pokeable
"done" latch (proven futile in the FMV doc).
**Proposed skip:** this is the **same FMV-drain/prebuffer pacing** the FMV-skip latch
already addresses. The clean lever is the existing `FmvDwellSkip` (`0x80050CE4` → loop
exit `0x80050CF8`) made to fire on a *latch* (the change proposed in
`docs/tomba2-fmv-skip.md` §3): one Start press collapses the per-frame pace dwell across
this drain too, the same way it collapses the inter-FMV hold. Instant-CD already makes
each streamed sector cheap; the residual is consumer-paced decode, so the safe win is
escaping the pace dwell, **not** forcing the ring (underrun risk). Honest note: a chunk
of this gap is genuine MDEC decode of the final movie frames + title-overlay fill and
cannot be forced to zero without an MDEC underrun. Net: **no NEW hook needed** — it is
covered by the FMV-skip latch already proposed; verify it also shortens this drain when
that latch lands.

### 2. Title-screen background. Streamed MDEC, looping. (not a "wait" — but explains #1 and the menu entanglement.)
**Where:** the title screen (teal tower against a shimmering sky;
`scratch/screenshots/waits/menu_c.png`).
**Kind:** **(d) MDEC** — the title background is a **looping streamed sequence**, not a
static image. `prof 60` on the idle title is 97.1% game, top PCs `0x800834A0` (4.6%,
StrPlayer loop), `0x8008B6D0` (1.1%, MDEC poll), `0x80082DF0–E00`, `0x8009A3F0–444`
(StrPlayer ring helpers), with the in-engine VSync wait `0x801157xx` also present.
**Why it matters for skipping:** it is not itself a long wait, but it is the reason the
title→menu transition is not a clean game-logic gate — the pad is read through the FMV
player's path. Any "instant title menu" work must hook the FMV player's pad/abort, not a
plain menu state machine. **Not a skip target on its own.**

### 3. Attract DEMO = in-engine gameplay; its segment gaps are stream pacing, not level loads.
**Where:** if no input is given at the title, the game rolls into an **attract demo**
that is real in-engine 3D gameplay (`scratch/screenshots/waits/g780.png`,
`y2.png`, `mn4.png` — Tomba in the forest/over water; a "DEMO" label seen at
`g1280.png`).
**Kind:** the demo's CD activity (`cdclog 1`, f934–1134) is the classic XA-stream
cadence `Demute → Pause → Setfilter → Setloc → SeekL → ReadS`, then a long run of
`GetlocL` position polls, then `Pause`; with a **~110-frame gap** between demo segments
(`Pause` f1018 → next `Demute` f1129). That gap is the **demo's own scripted
stream-segment boundary** (the demo plays a recorded input/stream), **not** a level load.
Instant-CD already covers the read bursts. **Not a user-facing gameplay wait** — the
demo is attract content, not something a player sits through deliberately.

### 4. Render engine is alive during gameplay (baseline, for the next pass).
`0x801157xx` (the display-deadline/VSync wait family `0x800157B0`/`0x8001577C`, mapped in
`docs/tomba2-intro.md`) appears in every in-engine profile (demo, title) at ~0.2% — i.e.
the render loop hits its per-frame VSync deadline normally; there is **no in-engine spin
hog** in the reachable content. This is the intentionally-LEFT-EMULATED timing primitive
(do not native-own it — it would fake hardware timing). Recorded so the next pass does
not mistake the VSync wait for a skippable dwell.

---

## Un-surveyed (next pass) — what remains and how to reach it

These are the genuinely-new gameplay waits the user most cares about; I flag them
honestly rather than guess.

- **New-Game opening story text/dialogue dwell.** Reaching it needs the title MENU
  (New Game), which is gated behind the streamed-title pad entanglement above. **Plan:**
  hook the FMV player's pad read at the title (the StrPlayer pad/abort site near
  `0x800856xx`/`0x8008A7xx`) to learn how "press Start at title" routes, then drive a
  *single* well-timed press that opens the menu instead of the demo. Once in-game story
  text appears, the expected wait is a **(c) fixed display hold / (b) game-paced text
  timer** — find the per-character/per-line dwell counter via `PSXPORT_WATCHW` on the
  text-advance variable and propose a Start-gated skip (advance-to-end), in the
  `DwellSkip` style.
- **First playable level load** (after the story). Expected **(a) CD-load bound** — with
  the instant-CD layer + BIOS `cdromBlockReading` HLE already in the tree this is likely
  *already mostly covered*; verify by `cdclog` over the first level-entry black screen
  and `prof` it (if it sits in BIOS `cdromBlockReading`/`0xBFC03A9C`, the HLE covers it;
  if it sits in the game's own libcd like the inter-FMV prebuffer, it needs a per-loader
  pass). Measure before proposing.
- **Area-to-area transitions during play** (Tomba 2 streams contiguous areas). Expected
  short **(a) CD** bursts, likely covered by instant-CD; measure with `cdclog` at a
  door/edge transition once New Game is reachable.

### Concrete next-pass tooling note
The blocker is *input timing into a streamed menu*, not RE depth. The cheapest unblock is
a tiny REPL addition: a **`waituntil <meanRGB-changes>`-style frame-advance-until-screen-
changes** helper (or logging `Tomba2_GetAndResetRenderHits` per frame to stderr) so the
driver can detect "title is up" deterministically instead of guessing frame counts, then
issue the menu press on that edge. That removes the FMV-abort jitter that defeated the
menu this pass. Build that helper first next time.

---

## Summary for the user

On the reachable post-intro path, the only multi-second wait that is NOT already
handled is the **post-movie → title black load (#1, ~4 s)** — and it is the **same
FMV-drain/prebuffer pacing** the already-proposed `FmvDwellSkip` *latch*
(`docs/tomba2-fmv-skip.md` §3) collapses; it needs **no new hook**, just verification
that the latch also shortens this drain. The title background and attract demo are
streamed MDEC content, not skippable game-logic dwells. The genuinely-new gameplay
waits the user asked about — **New-Game story text** and the **first level load** — sit
just past a streamed-title menu that this pass could not reliably open; they are flagged
with a concrete plan and the small tooling fix (deterministic screen-change detection)
that will unblock them next pass. The first-level load is likely already covered by the
existing instant-CD + BIOS-HLE layer — measure before adding anything.

---

## "Loading....." screen, fully characterized (2026-06-13) — and why a clean skip is hard

Repro (reliable, headless): boot → hold-Start native intro skip → tap-Start abort the
opening FMV → **title screen** (New Game / Load Game). Idle at the title; after a
timeout the game shows a **black screen with "Loading....."** (growing dots), then rolls
into the in-engine **attract demo** (Tomba in the forest/seesaw, HUD).

### Measured: "Loading....." is an IDLE dwell, NOT CD-bound
`cdclog` across the transition (psxport_frame stamps, one representative run):
- f820–f1108: a short load burst — ~6 chunks of `Setloc→SeekL→ReadN→Pause`. The demo
  data is resident by **f1108**.
- **f1108 → f2914: ~1800 frames with ZERO CD activity** — drive fully idle, screen shows
  "Loading.....", CPU pinned in the pace loop `0x80050CE4`.
- f2914–f2971: final `Setloc/SeekL/ReadN` + `Setmode/ReadS` → demo XA stream starts.

So the bulk of "Loading....." is a **pure idle dwell** (load already done at f1108);
instant-CD cannot shrink it. `prof` in the window is ~93% game / 1.2% biosrom — it is the
StrPlayer pace loop, **not** the BIOS. (NB: the OpenBIOS `_patch_card/_patch_card2` tty
lines at f41–49 are a ONE-TIME `InitCARD` from the FMV overlay; they print right before
the FMV wait and look correlated, but profiling shows the card path is ~0% of the wait —
the delay is the StrPlayer/MDEC FMV+dwell, not card/SIO.)

### The pace dwell (0x80050CE4) and its loop
```
80050CC8 lui  $v1,0x1f80
80050CD0 lbu  $v1,0x235($v1)     ; v1 = threshold = *(0x1F800235)  (a byte, <=255)
80050CCC lhu  $v0,-0x7f64($s6)   ; $s6=0x800F0000 -> counter *(0x800E809C) (vblank tick)
80050CD8 sltu $v0,$v0,$v1
80050CDC beqz $v0,0x80050CF8     ; counter>=threshold -> exit dwell
80050CE4 lhu  $v0,-0x7f64($a0)   ; inner spin: reload counter (a0=0x800F0000)
80050CEC sltu $v0,$v0,$v1
80050CF0 bnez $v0,0x80050CE4     ; stay while counter<threshold
...
80050D00 lbu  $v1,0x19c($s5)     ; scene/command byte
80050D08 beq  $v1,$s2,0x80050C7C ; outer loop: repeat while command unchanged
```
One dwell ≤255 frames; the ~1800f "Loading" hold is the **outer StrPlayer command loop**
(0x80050C7C) grinding through multiple scripted pause commands until the command byte
`*(s5+0x19c)` changes (next scene ready).

### Why the simple skip can't work (the e94ea86 lesson, now root-caused)
The **title screen, the opening FMV, the "Loading....." screen, AND the demo's own frame
pacing all run through this SAME 0x80050CE4 loop with byte-identical CPU registers**
(verified: `bt` at the title vs deep in the dwell → s4=800C0000, s5=1F800000, s6=800F0000,
s2=1, threshold 0x86, all identical). So:
- There is **no PC/register/drive-idle discriminator** between "Loading....." and the idle
  menu. e94ea86 collapsed the loop whenever the drive was idle → it fast-forwarded the
  menu's attract/demo countdown (the regression the user caught). **Reverted in 42a5d4b.**
- A safe "skip Loading only" needs a **scene-content discriminator** — which command-list
  / scene the StrPlayer is currently playing. The "Loading" text lives at **0x800107D4**
  (present in a Loading-state RAM dump, absent at the title), but it is referenced
  indirectly (string table / pointer), not via a direct `lui/addiu`, so the draw site
  isn't trivially hookable.

### Next step for a clean skip (not yet done)
Identify the StrPlayer scene/command id for the Loading screen (RE the command-list the
outer loop at 0x80050C7C walks, or find the indirect load of 0x800107D4 / the font-draw
of the "Loading" glyphs), then gate a dwell-collapse on *that scene only*. Do NOT gate on
drive-idle (re-breaks the menu). Until that discriminator exists, leave the dwell alone.
