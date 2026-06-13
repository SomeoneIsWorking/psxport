# Tomba! 2 (SCUS-944.54) — intro / logo sequence, toward a NATIVE skip

Goal (user, 2026-06-13): make the SCEA + Whoopee Camp logos **natively skippable**
(instant, via RE'd game logic) — NOT emulator fast-forward. The shipped
`Tomba2_WantTurbo` Start-held fast-forward is explicitly rejected: "I want native
skip via RE + port." Methodology priority the user set: **tooling → reverse
engineering → native port → patch the native port**.

All addresses KSEG0 virtual. EXE `SCUS_944.54`: T_ADDR `0x80010000`,
**T_SIZE `0x28800`** (167936 − 0x800 header), entry `0x80018b6c`. NOTE: the
older scene-orchestrator doc says size `0x69000` — that is WRONG (a Crash Bash
copy-paste); the real Tomba2 EXE is 0x28800, spanning 0x80010000–0x80038800.

## Verified intro timeline (headless, instant CD default, no input)

Captured with `tools/frames.py` (contact sheet) — see
`scratch/screenshots/introseq/sheet_native.png`:

| frames        | screen | nature |
|---------------|--------|--------|
| ~0–200        | black  | boot (EXEC at f82, card/GTE patches f1361) |
| ~200–600      | "Sony Computer Entertainment America Presents" | SCEA license (load mask) |
| ~600–850      | black  | transition / load |
| ~850–1700     | Whoopee Camp logo (colour on white) | developer logo (load mask) |
| ~1800+        | opening movie (Tomba face, village…) | **MDEC FMV stream** |
| (after FMV)   | "TOMBA 2 The Evil Swine Returns" | title screen |

Frame counter here = emulator presented frames (g_frame), ~60/s.

## Skippability — measured on the running system (REPL `shot`)

- **The opening FMV IS natively Start-skippable.** Driven via REPL: reach f1900
  (FMV), `press Start`, run 90 → title screen by **f1990**, whereas a no-input run
  is still mid-FMV at f2160+. So the movie player polls the pad and aborts on Start.
- **SCEA + Whoopee are NOT natively skippable.** Holding Start from boot still shows
  SCEA at f300 and Whoopee through f1500 (`scratch/screenshots/repl/hold_sheet.png`).
  They are load masks with display-hold timers; the pad is not polled to skip them.
- The gameplay render heartbeat `0x8003CCA4` is **dormant through the entire intro**
  (render-hits = 0, GAMELOG). Expected: SCEA/Whoopee/FMV don't use the in-engine
  object draw path. Do NOT treat render-hits=0 during the intro as a hang.

## Verified intro structure (the real one — supersedes prior guesses)

The intro is **straight-line blocking code**, not a data-driven state machine
(confirmed: a phase-grouped RAM diff over SCEA/white/Whoopee frames found NO
small-int stage word — only one phase-constant flag `0x80026BEC`). The driver:

- **`0x800111B4` = the intro-logos sequencer.** Entered ONCE (verified by trace:
  1 hit, at ~f88, from `0x80018C0C`/return `0x80018C10`); it blocks the whole
  logo sequence by calling sub-functions that each block internally, then returns
  (`jr $ra` @0x8001137C). It does NOT play the FMV — the FMV + title run in its
  caller after it returns. Its key calls, in order:
  - `0x80010D54` — **SCEA license display + hold.** Entered f88, returns ~f396.
  - `0x80011a0c`, `0x80011a78` — white transition between logos.
  - poll loop `0x800112BC: do { v0 = sub_8001138C() } while(!v0)` —
    **`0x8001138C` = Whoopee Camp animation player.** Entered ~f670, returns
    ~f1181 (teardown `0x8001E0CC` runs at both ends of the loop).
- **`0x80010D54` (SCEA)** is a per-frame loop dispatching on phase `$s5`
  (GPR[21]): 0=fade-in, 1=hold(180 frames, `$s4` countdown), 2=fade-out,
  3=done→cleanup→return. Dispatch beq-chain starts at `0x80010ED0`.
- **`0x8001138C` (Whoopee)** runs one displayed frame per loop iteration
  (`0x80011414`..`0x80011528`) and exits when **`*(0x800253EC)==1`**, set by
  `0x800118C0` once the decode position (`*(a0+8)`) passes frame 250.

### Corrections to earlier notes (this session falsified the falsifications)
- **`0x800111B4` IS the sequencer.** The prior note calling the
  `0x80011B4→0x80018C10` chain a "stale stack-scan artifact" was WRONG — it was
  reached by testing an unrelated function (`0x8001e0cc`) for "zero hits". Direct
  tracing proves `0x800111B4` runs (1 hit) and both logo functions run.
- **`0x8001E0CC` DOES execute** (2 hits, the Whoopee poll-loop setup/teardown) —
  contradicting the prior "zero hits" claim.
- **`0x800253EC` is the Whoopee animation-done terminator** (written =1 by
  `0x800118C0` at ~f1179), not a "scene step". (It is NOT a usable skip lever:
  mid-animation the code is deep in the frame-player and only re-checks it at
  pass boundaries — poking it =1 mid-pass does nothing.)
- `0x800675CC` remains confirmed-not-the-intro-driver (BIOS clear only).

## The native skip (IMPLEMENTED — runtime/games/tomba2.cpp)

PC owns the *advance decision* of each logo; the emulated code still does all
display/load. Default ON (inert unless Start held); opt out `PSXPORT_T2_NOINTROSKIP`.

- **SCEA — `SceaSkip` @0x80010ED0** (sig `0x12A20019`, `beq $s5,$v0`): on Start,
  set `$s5=3` so the next dispatch jumps straight to done/return — skips the hold
  AND the fade-out directly. The function's entry setup already ran, so the done
  cleanup is the legitimate terminal path.
- **Whoopee — `WhoopeeSkip` @0x80011414** (sig `0x3C028004`, `lui $v0,0x8004`): on
  Start, write the loop's own terminator `*(0x800253EC)=1`, set `v0=1`, and
  redirect to the epilogue `0x80011530` so the caller's poll loop advances.

Verified (Start held from boot): sequencer reaches the post-Whoopee FMV stage at
~f505 vs ~f1181 baseline (~676 frames / ~11s earlier). With SCEA direct-skip,
Whoopee is reached ~f396 vs f670 baseline.

## OPEN: loads + paced sequencing are still slow (NEXT — make them PC-instant)

Skipping the logos *unmasks* the inter-stage black/white gaps (SCEA→Whoopee
≈234 frames even with the logo skipped; then the opening FMV streams in).

**Measured cause (this is the important part): the gaps are the game PACING ITSELF
one step per frame, NOT disk latency.** Evidence:
- VSync-wait trace during the gap: the global frame counter `0x800267B4` ticks +1
  per real frame and the game waits exactly 1 frame at a time (`0x80017FC4` called
  with a0 = counter+1). There is NO far-target multi-frame dwell to "satisfy early".
- `psxport_cd_instant` is a **bitmask** (bit0=1 misc, bit1=2 zero seek, bit2=4 short
  startup, bit3=8 instant ReadN data). Full mask **15** reaches Whoopee at the SAME
  f670 as native — making the disk instant does NOT shrink the gap. (Partial masks
  2/8/10/14 desync the game so it never reaches Whoopee — instant-CD is fragile and
  cannot be a blanket default.)

### Inter-stage state machine `0x80011a78` (keyed on `*(0x80025454)`, jump table @`0x80010054`)
One call runs the whole load/transition, looping internally through states 1→0x13.
Per-state timing (Start held, direct logo skip), via `PSXPORT_WATCHW=80025454`:
- states 1–5 instant; **state 5→6 ≈53 frames** (REAL CD/load wait: state-5 handler
  `0x80011D64` polls `0x800123b0` load-status + flags `0x8002544c` — LEFT INTACT,
  forcing it races the loader = white screen).
- states 6–0xD instant; **state 0xE→0xF ≈200 frames** = a PURE timed dwell (handler
  `0x80012148`: `wait until counter > *(0x80038498)+0xC8`, screen black).
- states 0xF–0x13 instant → state machine returns, Whoopee begins.

**COLLAPSED (implemented):** `DwellSkip` @`0x80012164` forces state 0xE's own
advance path on Start → Whoopee now enters **f163** vs f364 (and f670 no-skip). The
remaining inter-stage time is just the ~53f real load.

### FMV phase — NEXT FRONTIER (separate overlay subsystem)
After the logos, the opening FMV plays. Skipping the logos unmasks its load (white/
black, ~f163→~f740). Characterised:
- **NOT disk-bound:** full instant-CD (mask 15) gives the SAME last-ReadN frame
  (f743) and does not shrink it. It is per-frame-paced like the inter-stage machine.
- Uses its OWN timing, not the intro VSync (`0x800267B4` reads 0 here; `0x80017E4C`/
  `0x80017FC4` not called).
- Runs from a **separate overlay**, NOT the 0x80010000 EXE: PCCOV intersection of two
  FMV-load windows = code at **`0x80085000`–`0x8009A000`** (the StrPlayer/MDEC FMV
  player) plus low kernel/libcd `0x80001000`–`0x80004000`. The FMV stream overwrites
  the 0x80011xxx/0x80018xxx intro code, consistent with this.
- The FMV is separately Start-skippable *while playing* (needs a press edge during
  playback; a held-from-boot Start does not skip it).
- **TODO:** RE the StrPlayer overlay's per-frame load/decode loop and collapse its
  pacing dwells the same way (find the per-frame gate / timed wait, force-advance on
  Start). This is a distinct subsystem and its own RE pass.

### Timing/IRQ primitives — mapped, intentionally LEFT EMULATED (do not native-own)
Owning these natively would fake hardware timing (a bandaid). They depend on the
emulated VBLANK/IRQ/HW registers:
- **`0x80017E4C`** = VSync/elapsed-frame wrapper; reads frame counter
  `0x800267B4`, markers `0x80025684`/`88`; calls **`0x80017FC4`** = spin-wait
  until `0x800267B4 ≥ a0` (with timeout `0x8001effc`).
- **`0x800157B0`/`0x8001577C`** = display "deadline" check/arm (`0x80025650`).
- **`0x800182D8`** = root IRQ dispatcher (reads I_STAT `0x1F801070` / I_MASK
  `0x1F801074`; callback table @`0x800256F0`, mask `0xD` = VBLANK+CDROM+DMA).
- **`0x800181E8`** = I_MASK halfword get/set.

## Tooling added this session
- **REPL `shot <path>`** (runtime/main.cpp): dump the current presented framebuffer
  to a PPM on demand while driving via `-repl`. VideoCb caches the last frame
  (tightly packed) into `g_last_fb`. This is the interactive analogue of
  `PSXPORT_FRAMEDUMP` and is what made the skippability tests above possible.
- Reused: `tools/frames.py` (contact sheets), `PSXPORT_WATCHW`, `PSXPORT_PCCOV`,
  `PSXPORT_RAMDUMP`, `tools/disasm.py`. KEY: dump RAM **during the logo phase**
  (f400/f1000), not during the FMV — the FMV stream overwrites the intro driver code
  at 0x80011xxx/0x80018xxx.
