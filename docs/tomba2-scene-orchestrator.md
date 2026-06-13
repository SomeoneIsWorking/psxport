# Tomba! 2 (SCUS-944.54) — intro scene orchestrator (reverse engineered)

> **⚠️ FALSIFIED FOR THE INTRO (2026-06-13).** The central premise below — that
> `0x800675CC` is the "scene phase" driving the SCEA license + Whoopee Camp logos —
> is WRONG. A write-watchpoint (`PSXPORT_WATCHW=800675CC`) over f0–1800 logs exactly
> ONE write to that word: the BIOS RAM-clear at f25 (value 0). Nothing in the actual
> SCEA/Whoopee intro touches it. The EXE size here is `0x28800` (not `0x69000` as
> stated below — that was a Crash Bash copy error). The real intro RE lives in
> **`docs/tomba2-intro.md`**; the logos are driven by code at 0x80017E4C /
> 0x800181E8 with timing against the frame counter `0x800267B4`. This `0x800675CC`
> machinery, if real at all, belongs to some later (in-game) scene system, not the
> boot logos. Treat everything below as unverified for the intro-skip task.


Reverse engineered 2026-06-13 with the psxport RE tooling (write-watchpoint
`PSXPORT_WATCHW`, RAM-diff scene enumeration, `tools/disasm.py`, frame capture
`tools/frames.py`). All addresses are KSEG0 virtual; the boot EXE `SCUS_944.54`
is resident at `0x80010000` (size `0x69000`), so this code never overlay-swaps.

This replaces the **falsified** earlier note that "`0x800253EC` is the intro
step that stays 0 then goes 1". `0x800253EC` is a constant pointer
(`0x26A60010`); writing 1 to it corrupts a pointer. The real scene state is
`0x800675CC` (see below).

## TL;DR — what drives the intro

```
scene_update (0x8002C97C, called once per frame)
  └─ if dwell_gate(0x8003A7C4) == 0:  return          # scene holds this frame
     phase = [0x800675CC]                              # 0..7 scene phase
     if phase >= 8: return
     jump  table_0x8004CDC0[phase]                     # run phase handler
                                                        # handler advances phase

advance_pump (0x8003AAA4, called per frame from the main loop)
  └─ if ready_signal(0x80047174):                      # VSync/CD "step ready"
        [0x80076AC0] = 1                                # fire the advance event

dwell_gate (0x8003A7C4) consumes [0x80076AC0]:
   returns 1 (advance) only on the frame the event is pending, then clears it;
   returns 0 (hold) otherwise.
```

So **every scene holds until the advance event fires**, and the event is paced
by the kernel ready-signal `0x80047174` (VSync/CD streaming). This is why the
whole intro is timing-paced and why *forcing* the event races the asset loaders
(it reproduces the white-screen "Whoopee Camp doesn't play" symptom). The safe
way to shorten the intro is to run the emulation faster (fast-forward), which
advances the ready-signal-paced dwells *and* lets loads complete — not to poke
the state.

## Key state (all in the resident scratch region 0x8006xxxx)

| Address       | Meaning |
|---------------|---------|
| `0x800675CC`  | **scene phase** 0..7 (the orchestrator's main state). Verified by write-watchpoint. |
| `0x80067830`  | **substate** within a phase (phase 1/2/4/5/7 dispatch on it via secondary tables). |
| `0x800655B8`  | display/stream resource handle (passed to the 0x80039xxx/0x8003Axxx primitives). |
| `0x800655C0`  | scene flag word (OR'd with 0x2/0x20/0x42/0x100/0x12/0xa/0x22 by handlers). |
| `0x800655C4`  | a one-shot flag checked by phases 3/5/7. |
| `0x800675C8`  | a countdown decremented in the phase-3 handler. |
| `0x80076AB8`  | base of the **advance-event struct** consumed by the dwell gate. `+8` (`0x80076AC0`) = event-pending; `+0x44` = optional callback; `+0x50` = frame tick counter. |

## Observed phase timeline (no-skip, headless)

| frame | phase | writing PC | screen |
|-------|-------|-----------|--------|
| f207  | 0     | 0x8002C8CC (scene_init) | SCEA "…America Presents" (license) |
| f305  | 1     | 0x8002CA08 (phase-0 handler) | SCEA fading → black |
| f564  | 2     | 0x8002D074 (phase-1 handler) | black / loading Whoopee assets |
| f843  | 3     | 0x8002CD64 (phase-2 handler) | Whoopee Camp logo (colour on white) |
| f846+ | 3↔4   | 0x8002D074 (phase-3/4 loop) | logo + opening cutscene render loop |

(SCEA is **phase 0**; the colourful Whoopee Camp logo is reached at **phase 3**.
The 3↔4 toggle every ~4 frames is the active-scene double-buffer/animation loop
that continues through the opening cutscene.)

## Functions

### `scene_init` — 0x8002C894
Resets the orchestrator: `[0x800675CC]=0`, clears the per-scene arrays at
`0x800657C8` (stride 0x194, 4 entries) and `0x800655C8`, zeroes flags
`0x800655B8/55C0/55C4`. Writes phase 0 at `0x8002C8CC`.

### `scene_update` — 0x8002C97C  (per-frame pump)
1. `dwell_gate(1, &0x8006782C, &0x80067830)` (0x8003A7C4). If it returns 0, the
   scene holds — `scene_update` returns without touching the phase.
2. `phase = [0x800675CC]`; bounds-check `phase < 8`.
3. `jr table_0x8004CDC0[phase]` → phase handler.

Phase jump table `0x8004CDC0`:

| phase | handler    | role |
|-------|-----------|------|
| 0 | 0x8002C9E4 | SCEA shown; stop SCEA resource, clear `55C0`, **set phase=1**. |
| 1 | 0x8002CA0C | dispatch on substate `[0x80067830]` via table `0x8004CDE0`; kicks loads, **phase=2**. |
| 2 | 0x8002CA68 | clear arrays, dispatch on substate; on ready **set phase=3** (Whoopee visible). |
| 3 | 0x8002CD74 | active-scene loop: decrement countdown `0x800675C8`, OR scene flags, **toggle 3↔4**. |
| 4 | 0x8002CF44 | dispatch on substate via table `0x8004CDF8`. |
| 5 | 0x8002CF8C | checks `55C4`/substate, conditionally **phase=3**. |
| 6 | 0x8002D078 | terminal/idle (just returns). |
| 7 | 0x8002CFEC | checks substate, conditionally **phase=3** / sets substate. |

Secondary substate tables: `0x8004CDE0` (phase 1/2, on `[0x80067830]`) and
`0x8004CDF8` (phase 4).

### `dwell_gate` — 0x8003A7C4  (advance-event consumer)
Operates on the event struct at `0x80076AB8`. With `a0!=0` (scene_update's call):
returns 1 and clears the pending flag when `[0x80076AC0] != 0`; otherwise returns
0 and snapshots the current event values to `*a1`/`*a2`. Returns -1 in the
degenerate empty case.

### `advance_pump` — 0x8003AAA4  (advance-event producer)
```
if (ready_signal(0x80047174)) return;     // already pending/busy
do_step(0x80047108);                       // advance the underlying resource
if (!ready_signal(0x80047174)) return;     // not ready yet → hold
[0x80076AC0] = 1;                          // fire advance event  (writing PC 0x8003AADC)
snapshot fields to 0x80076AA8/0x80076AAC; clear; if callback [base+0x44] call it
[base+0x50]++;                             // frame tick
```
`0x80047174` / `0x80047108` are the kernel ready/step primitives (VSync- or
CD-driven). They pace the entire intro.

## Implications for the skip feature

- **"Whoopee Camp doesn't play" root cause**: the old auto-`LogoSkip` wrote `1`
  into `0x800253EC` (a pointer) whenever the audio clock ticked; in `-play` that
  corrupts the pointer mid-intro. Removing that write makes Whoopee Camp play.
- **Skipping cleanly**: do not force `0x80076AC0`/phase values — it races the
  loaders (white screen). Instead, fast-forward the emulation while the skip
  button is held; the ready-signal-paced dwells and the loads both advance
  consistently. Scope the fast-forward to the intro by testing the phase word
  `0x800675CC` (the orchestrator only drives it during the attract/intro
  sequence).

## DECISIVE: the intro dwells are CD-load-bound, not timed (2026-06-13)

The scene advances are paced by **CD reads**, not a display timer. Measured by
toggling `PSXPORT_CD_INSTANT` and watching phase `0x800675CC`:

| transition           | CD_INSTANT=1 | CD_INSTANT=0 |
|----------------------|--------------|--------------|
| 0→1 (SCEA ends)      | f337         | f483         |
| 2→3 (Whoopee loads)  | f602         | f1173        |

The SCEA "…America Presents" screen is a **load mask** for the Whoopee Camp
assets: CDC log over f0–340 shows phase 0 is ~**18 load-chunk cycles**
(`Setloc → SeekL → ReadN → Pause`, each with command/IRQ round-trips). The
advance fires when the loads complete (ready-signal `0x80047174`), which is why
forcing scene state or the advance event does nothing — there's nothing to skip
*to* until the assets are resident.

This **revives the "make the disc read instant" lever** and corrects the journal
dead-end note ("logo is jingle-paced, CD won't help") — that was true only for
the Whoopee jingle (ReadS/STRSND, consumer-paced by audio), NOT for the SCEA /
asset-load phases (ReadN, CD-bound).

**But** `psxport_cd_instant` already defaults to -1 (all implemented bits on:
seek/reset/startup/ReadN-pacing) and SCEA *still* takes ~306 frames. Frame-gap
analysis of the CDC log refines the cause: the actual CD load bursts finish fast
(chunks at f5–61, f146, f240–250) but there are large **idle gaps of 85–94
frames between bursts where the CD is doing nothing** — i.e. SCEA is a few fast
CD reads plus **fixed display-hold timers** in the VSync job VM. So even a
perfectly instant CD cannot make SCEA zero-frame; the holds remain. The CD
dependence the toggle test showed is real but is only part of the total. So the routes to a
true instant logo skip are, in order of cleanliness:
1. **Fast-forward while Start held** (implemented). Runs both the CD-paced loads
   AND the fixed display holds faster; loads still complete, no corruption.
   Robust, ~sub-second. This is the recommended skip.
2. **Find + zero the display-hold timers** in the job VM (the 85–94f gaps). Needs
   disassembling the SCEA display job that the per-frame VSync callback runs; the
   elapsed counters found so far (0x800654AC etc.) are free-running, not the gate.
   This is the remaining RE for a zero-frame skip, and it must be combined with CD
   acceleration to matter (the holds are only part of the total).

Forcing scene state is a **dead end** (verified): poking `0x80076AC0` (advance
event), `0x8006E0CC` (job-stack index), and the elapsed counter `0x800654AC`
during SCEA either no-ops (the VSync machinery re-manages the fields) or stalls
(the job never reports done) — never a clean advance.

## Job-VM dig: the hold is framework-driven, not a pokeable timer (2026-06-13)

Followed the instruction to keep reversing the job VM toward a zero-frame skip.
Findings, in order:

- The SCEA hold (phase 0, ~f207–305, ~98 frames) is **not a spin-wait**: PC
  coverage over a hold gap (f70–140) executes **5962 unique code words** vs ~2900
  during a CD load burst. The game is doing real per-frame work in the gap.
- The hold is **not a single frame counter**: no word counts *down* to 0 across
  the hold; the up-counters that do exist (e.g. `0x800654AC`, 0→~95 over the hold)
  are **free-running, not the gate** — poking `0x800654AC=1000` mid-SCEA did NOT
  end it early (SCEA still advanced on time).
- The advance at f305 is the **phase-0 handler itself** calling enqueue_advance
  (watchpoint on the event struct: f305 write has `ra=0x8002C9F4`, inside the
  handler). The handler only runs once the event is already pending, so the hold
  is upstream in the async callback chain set up at f207 by `0x80039338`.
- That setup registers handlers through an **OO dispatch**: `0x800319B0` does a
  virtual call into a scene object at `[0x80078B84]` (method offsets 0x10/0x14/
  0x18), and `0x8004725C` installs **GPU/DMA completion callbacks** (the
  `[0x8008xxxx]=1` stubs at 0x8004718C–0x800471DC). The intro is an
  object/method async state framework, not a flat timer loop.

**Verdict:** there is no single timer/flag whose poke yields a clean zero-frame
skip — the hold is real per-frame work dispatched through the engine's async
object framework, part CD-load-bound and part display-hold. A zero-frame skip
would require either driving the scene object's state machine through its own
methods (re-implementing the transition the engine does), or hooking the
streaming/decompression to complete in one frame — both are large, fragile,
game-specific efforts. **Fast-forward (shipped) is the correct practical skip**:
it collapses the framework's per-frame work to near-zero wall-clock while letting
every load/decompress/transition complete exactly as the engine intends. This is
the documented terminal conclusion of the job-VM investigation.

## Tooling used (reusable)
- `PSXPORT_WATCHW="hexaddr"` — log every CPU store to a word (writing PC + value
  + caller `$ra`). Found the orchestrator from the scene-phase variable, and the
  CD-load callers of the dwell counter, in single runs.
- `PSXPORT_POKE="frame:addr=val;A..B:addr=val"` — poke RAM at a frame/range to
  test a hypothesis.
- `PSXPORT_FRAMEDUMP` + `tools/frames.py` — exact-frame PNG capture / contact
  sheet to correlate state with what's on screen.
- `tools/disasm.py <ramdump> <start> <end>` — MIPS disassembly of a RAM dump.
