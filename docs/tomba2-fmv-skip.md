# Tomba! 2 — making the inter-FMV "Whoopee Camp logo" hold skippable

User goal (2026-06-13): between the two opening FMVs, the Whoopee Camp studio logo
is held as a *still image* for ~6 s (~390 frames at 60 Hz, FMV#1 Pause @f836 →
FMV#2 ReadS @f1232 with no input). The user wants **pressing Start to jump to
FMV#2 immediately**, instead of waiting.

This doc records what gates that hold, the empirical results of trying to force the
gate early, and a concrete proposed `runtime/games/tomba2.cpp` change. It builds on
`docs/tomba2-intro.md` (the "Inter-FMV ... 6s gap" section) — read that first.

All addresses KSEG0 virtual. RE is against the f839 player overlay (resident
~f839–1232); static disasm offsets come from `scratch/bin/tomba2/ram_f1000.bin`
(2 MB RAM dumped during the hold). Disasm tool: `python3 tools/disasm.py <dump> <a> <b>`.

---

## 1. What triggers FMV#2's ReadS — the prebuffer gate (RE'd)

The hold is the StrPlayer **prebuffering FMV#2's MDEC stream into a ring buffer**,
filling at the rate the still-logo display consumes it. The fill loop lives in the
player overlay; the two structurally-identical wait functions are at **`0x8008A700`**
and **`0x8008AE00`** (two stream channels). Their gate is:

```
8008A720  jal   0x80085900          ; v0 = current buffered ring position
8008A748  addiu $v0, $v0, 0x3c0      ; target = pos + 0x3C0 (960 units)
8008A750  sw    $v0, 0x2748($at)     ; *(0x80102748) = TARGET (rewritten EVERY frame)
8008A760  sw    $zero, 0x274c($at)   ; *(0x8010274C) = 0  (per-command wait counter)
...
8008A76C  jal   0x80085900          ; v0 = current pos (re-read)
8008A778  lw    $v1, 0x2748($v1)     ; v1 = TARGET
8008A780  slt   $v1, $v1, $v0        ; v1 = (TARGET < pos)?
8008A784  bnez  $v1, 0x8008a7b8      ; pos > target  -> prebuffer SATISFIED, advance
8008A78C  lw    $v0, 0x274c ...       ; else: wait-counter++ and a timeout check
```

Variables (verified with `r`/`PSXPORT_WATCHW`):
- **`0x80102748`** — prebuffer TARGET = `pos + 0x3C0`. **Rewritten every frame** by the
  per-frame ticker `0x8008A750` (watchpoint shows `pc=8008A750` writing it +1-ish each
  frame; values 0x416@f926 → 0x546@f1232). Because it chases `pos`, you cannot freeze it.
- **`0x8010274C`** — per-command wait counter, compared against `0x3C` (60) at `0x8008AE84`
  (`slt 0x3c, $v1`). Held at 1 for almost the whole hold, then spikes to **0xA0 (160)** at
  ~f1226, immediately before ReadS@f1232 — i.e. a near-end timeout, not the long pacer.
- **`0x80085900`** returns the live buffered ring position (the thing `target` chases). The
  fill advances only when the consumer (logo display / MDEC) drains the ring.

The script that issues the per-sector reads is the processor at **`0x8008AE00`** (walks a
2-byte-record command list at `$s2`; opcode `*(s2)==0` ends/loops; the read-issue helper is
`0x8008B0C8`). With no input the prebuffer issues `Setloc→SeekL→ReadN→Pause` ~1 sector per
~12–16 frames; the transition to **`cmd 1B ReadS`** (FMV#2 playback) lands at **f1232**
(`cdclog`: `[cdc f1232] cmd 1B (ReadS)`).

### Why it is vblank/consumer-paced, not CD-bound
`prof 100` taken inside the hold (no input) is **98.7% game, ~80% in the spin
`0x80050CE4–CF4`** (the per-frame display-pace loop) and **3.4% in `0x8008B6D0`**
(an MDEC/GPU status-register poll spin, `and 0x01000000` on `*(0x800B02C4)`). So the CPU
sits in the per-frame pace wait + MDEC decode-complete poll — both legitimate
hardware/timing waits — between the slow reads. CD-instant does not shrink this (already
known); the bottleneck is the consumer-paced ring fill.

---

## 2. Empirical test: can the gate be forced early?

Forcing the gate **counters** does NOT work — they are recomputed from live buffer state
every frame:

| Poke (during hold) | Result |
|---|---|
| `w 80102748 0` once @f1000 | no early ReadS (still f1232-ish) — overwritten by `0x8008A750` next frame |
| `w 80102748 0` every 2f across hold | no early ReadS — same reason |
| `w 800ABDE0 7FFFFFFF` every 2f (the per-frame dispatch counter the inter-read spin watches) | no early ReadS |
| `w 8010274C A0` once @f1000 | no early ReadS |
| `w 8010274C 200` **every frame** f920–1080 | no early ReadS — overwritten to 0 by `0x8008A760`/`0x8008AE10` |

So there is **no single pokeable "prebuffered >= target" latch** — the fill is genuinely
consumer-paced and the gate variables are derived, not state.

### What DOES work, and is glitch-free: escaping the per-frame pace dwell (held Start)
The existing `FmvDwellSkip` hook (`0x80050CE4`, redirect to loop-exit `0x80050CF8`) only
fires while Start is held — but when it fires every frame it **does** accelerate the
prebuffer, because escaping the per-frame pace wait lets the read/decode loop iterate
faster. Measured (`cdclog`, `cmd 1B`):

| Input | FMV#1 ReadS | FMV#2 ReadS | logo hold |
|---|---|---|---|
| none (baseline) | f334 | **f1232** | ~898f between the two ReadS |
| Start **held** from f900 | f334 | **f1111** | **−121f** vs baseline |
| Start held from boot | f29 | f310 | ~281f (whole intro compressed) |
| Start single **tap** at logo (held 3f) | f334 | **f1224** | only −8f — a tap does almost nothing |

**FMV#2 plays correctly when reached early — no glitch / underrun.** Captured frames
(`scratch/screenshots/tomba2-fmvskip/`, meanRGB via PIL):
- held-from-f900: `held_f1210` (60,35,9) and `held_f1330` (97,74,46) — clear Tomba
  characters, warm tones, evolving content (see `held_f1210.png`, `held_f1330.png`).
- held-from-boot: `heldboot_f440` (77,40,19) — clean character close-up (`heldboot_f440.png`).
Baseline FMV#2 for comparison: `base_f1300` (40,19,1), `base_f1360` (76,40,18) — same
content range. No corruption, blackout, or stutter in the early-reached cases.

### The residual ~120f cannot be safely forced
Even with Start held, the prebuffer `ReadN`s stay widely spaced (f938→f983→f1013→f1085,
30–70f gaps): those gaps are the consumer-paced ring drain + the MDEC decode poll
(`0x8008B6D0`), i.e. the still logo being decoded/displayed at its fixed rate, NOT the
`0x80050CE4` dwell. Pushing past them would mean starting ReadS before the ring is primed
— exactly the underrun risk. The ~120f that held-Start removes is the achievable, safe
win; the rest is genuine decode/fill pacing.

**Honest conclusion:** there is no clean "prebuffer-complete" boolean to force. The correct
lever is the one already in the tree (`FmvDwellSkip` escaping the per-frame pace dwell); the
defect is purely its **input model** — it requires Start to be *continuously held*, and a
single tap is worthless (−8f). The user's "press Start to jump" maps to: latch skip-mode on
a Start press and keep `FmvDwellSkip` firing until FMV#2 actually starts.

---

## 3. Proposed `runtime/games/tomba2.cpp` change (latch the dwell-skip)

No new RE hook is needed for the gate (there is no pokeable gate). Instead, make the
existing `FmvDwellSkip` fire on a **latch** rather than on live held-Start, so one Start
press collapses the whole hold to the ~f1111 result, and clear the latch when FMV#2
actually begins so the movie itself is not fast-forwarded.

### Hook (unchanged PC + signature)
- **PC:** `0x80050CE4`
- **Signature WORD:** `0x9482809C` (`lhu $v0, -0x7f64($a0)` — the pace-loop body;
  disasm.py's LE byte column shows `9c808294`). Already registered:
  `psxport_add_hook(0x80050CE4, 0x9482809C, FmvDwellSkip);`

### New: a Start latch + an FMV-start clear hook
1. Add a module latch `bool s_fmv_dwell_latch`. In `FmvDwellSkip`, fire the redirect when
   `s_skip_held || s_fmv_dwell_latch`, and **set** the latch on the rising edge of Start
   (so a tap is enough):
   ```cpp
   int FmvDwellSkip(uint32_t pc, uint32_t* gpr, uint32_t* redirect_pc) {
     if (s_skip_held) s_fmv_dwell_latch = true;     // a press latches skip-mode
     if (!s_fmv_dwell_latch) return PSXPORT_HOOK_CONTINUE;
     *redirect_pc = 0x50CF8 | (psxport_last_pc & 0xE0000000); // loop exit (unchanged)
     return PSXPORT_HOOK_REDIRECT;
   }
   ```
2. **Clear the latch when FMV#2 actually starts**, so playback runs at native speed. The
   clean clear point is the ReadS-issue path of the script processor — hook the prebuffer
   wait's "satisfied/advance" target `0x8008A7B8` (the `bnez ... 0x8008a7b8` taken when
   `pos > target`), OR, simpler and overlay-safe, clear it from the player main-loop body
   once per FMV start. Recommended: a tiny clear hook at the prebuffer-satisfied branch
   target:
   - **PC:** `0x8008A7B8`  (branch target of `8008A784 bnez $v1,0x8008a7b8`)
   - **Signature WORD:** `0x3C040280` (`lui $a0,0x8002` in `ram_f1000.bin`; disasm.py LE
     byte column shows `0280043c`). Confirmed present in the f839 overlay dump.
   - **Action:** `s_fmv_dwell_latch = false; return PSXPORT_HOOK_CONTINUE;`  (pure observer —
     no register/PC change, so it cannot perturb the player).

   If a clear hook there proves fiddly, an acceptable alternative is to clear the latch when
   the visible FMV is confirmed running (e.g. on the first `cmd 1B ReadS` after the hold via
   the CD path), but the `0x8008A7B8` clear is the most direct "prebuffer done" signal.

### Why this is a real fix, not a bandaid
- It uses the player's **own** per-frame pace-loop exit (`0x80050CF8`) — the same path the
  loop takes when the wait elapses — exactly as `FmvDwellSkip` already does and as
  `docs/tomba2-intro.md` validated for the post-Whoopee pre-roll. We are not poking derived
  gate variables (proven futile above) and not faking buffer state.
- The latch only changes the **input model** (tap-to-latch vs continuous-hold), which is the
  measured defect; the achievable speedup (~120f) and FMV#2 integrity are already verified.
- Clearing the latch at the prebuffer-satisfied branch means the latch is scoped to the
  hold; the FMV stream then plays at native XA/MDEC rate (do NOT accelerate STRSND — see
  intro doc).

### What this does NOT attempt (and why)
It does not try to eliminate the residual ~120f (the consumer-paced ring fill + MDEC decode
poll at `0x8008B6D0`). Forcing past that risks an MDEC underrun on FMV#2 start; the
empirical tests above show the safe floor is the held-Start result. If the residual ever
needs removing, it requires RE of the ring-buffer producer/consumer indices feeding
`0x80085900` and proof that FMV#2 still primes — a deeper, separate pass.

---

## Verification artifacts
- CD-command timelines: `cdclog 1` runs (baseline f1232, held-from-f900 f1111,
  held-from-boot f310, tap f1224).
- Profiles: `prof 100` inside the hold (80% `0x80050CE4` pace spin, 3.4% `0x8008B6D0`
  MDEC poll, 98.7% game).
- Frames: `scratch/screenshots/tomba2-fmvskip/{held_f1210,held_f1330,heldboot_f440,
  base_f1300,base_f1360}.png` — FMV#2 content clean when reached early.
- Watchpoints: `PSXPORT_WATCHW=102748` (writer `0x8008A750` every frame),
  `PSXPORT_WATCHW=0xBF870` & `0xAC298` (no writes during hold — confirms the
  `0x80050DE4` state machine and the script base are not the long pacer).
