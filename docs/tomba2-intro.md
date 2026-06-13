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

## Loads: the inter-stage gaps were ARTIFICIAL CD delays (FIXED — instant on PC)

Skipping the logos unmasks the inter-stage black/white gaps. Earlier notes guessed
these were "the game pacing itself per frame, not disk" and that instant-CD "did not
shrink the gap" — **both FALSIFIED this session.** The cause was two *artificial*
CD-command completion delays the emulator models for physical-drive realism, which
are meaningless on PC (no drive). Found via `cdclog` (the decisive tool — it prints
every CD command per frame; static disasm of the loader gave only stale leads):

1. **`ReadTOC` (state 5) = the ~53-frame SCEA→Whoopee gap.** The inter-stage machine
   issues `ReadTOC` at f88; its result lands at f141. `PS_CDC_Command_ReadTOC` returns
   `30,000,000` cycles (≈0.886s ≈53f) — its own comment calls this "a gross
   approximation" of the physical TOC-scan time. On PC the TOC is already resident
   (`CDIF_ReadTOC` is a memcpy). Earlier "instant-CD mask 15 didn't help" was correct
   *because mask 15 had no ReadTOC bit* — the conclusion "not disk-bound" was the
   error.
2. **`Pause` completion = the Whoopee→FMV per-sector cost.** The asset loader does
   `Setloc→SeekL→Setmode→ReadN→Pause` **per single sector**; `PS_CDC_Command_Pause`'s
   ACK→COMPLETE delay is `(1,124,584 + …)·… + rand` ≈ ~1.1M cyc ≈ **2 frames every
   Pause**. Hundreds of sectors × 2f = the bulk of the gap. Also "an approximation"
   of drive spin-down; instant on PC.

**FIXED (`vendor/beetle-psx/.../cdc.c`, gated on `psxport_cd_instant`):**
- **bit 16** → `ReadTOC` returns a 50000-cyc ACK→COMPLETE headroom instead of 30M.
- **bit 32** → `Pause` returns 50000-cyc headroom instead of ~1.1M.
- The PC-port default is now the **full mask `0x3F`** (`runtime/main.cpp`); override
  `PSXPORT_CD_INSTANT=0` for native HW timing in RE/oracle runs. Streaming (XA/STRSND)
  audio keeps native pacing (bit 8 excludes it), so FMV audio is unaffected.

**Verified, full pipeline, mask `0x3F`, no desync/crash:** boot → (logos skipped) →
inter-stage machine completes **f70** (was f163) → FMV → title menu → New Game →
opening story text → in-level 3D scene. The ~53f ReadTOC gap is gone; per-sector
Pause cost dropped ~22×.

### Inter-stage state machine `0x80011a78` (keyed on `*(0x80025454)`, jump table @`0x80010054`)
One call runs the whole load/transition, looping internally through states 1→0x13.
Per-state timing (Start held, direct logo skip), via `PSXPORT_WATCHW=80025454`:
- **state 5→6**: was ≈53f = the `ReadTOC` above (now instant via bit 16). State-5
  handler `0x80011D64` polls `0x800123b0` (which the game uses to await the TOC/queue
  result); with ReadTOC instant the poll satisfies immediately — no race, no white
  screen (the earlier "forcing it races the loader" fear was about poking the flag,
  not about making the underlying CD op fast).
- states 6–0xD instant; **state 0xE→0xF ≈200 frames** = a PURE timed dwell (handler
  `0x80012148`: `wait until counter > *(0x80038498)+0xC8`, screen black) — collapsed
  by `DwellSkip` (below).
- states 0xF–0x13 instant → state machine returns, Whoopee begins.

**COLLAPSED (implemented):** `DwellSkip` @`0x80012164` forces state 0xE's own advance
path on Start. With the CD fixes + DwellSkip the machine now finishes ~f70.

### Whoopee→FMV — two residuals, one collapsed, one open
After the inter-stage machine (~f70 with the CD fixes), reaching the visible FMV has
two distinct black spans (Start held, instant-CD), found by PC-sampling (`bt`) inside
each gap:

1. **Post-Whoopee FMV pre-roll dwell — COLLAPSED (`FmvDwellSkip`).** A loaded overlay
   at `0x80050xxx` frame-paces itself with a spin at `0x80050CE4`:
   `while (*(0x800E809C) < threshold@0x1F800235)` (`0x800E809C` is a VBLANK-ticked
   counter). This is the post-Whoopee dead black gap (~f440→620 of pure black). On
   Start we redirect to the loop exit `0x80050CF8` so the wait elapses immediately
   (same technique as DwellSkip; the loop's own decode work still runs). Verified: the
   movie itself stays intact, the FMV becomes visible ~100f earlier, and the run still
   reaches the title / New Game cleanly. Default ON, Start-gated.

2. **Pre-Whoopee load (~f70→230) — OPEN, and it is NOT the CD.** `bt` shows the CPU
   spinning in **BIOS** (`0xBFC05774`), and `cdclog` shows ~70 CD commands but only
   ~17 actual sectors over those ~160 frames — i.e. the game/BIOS asks for a sector
   only every ~9 frames. So this is **per-sector game/BIOS pacing (or per-sector
   decode), not disk latency** — instant-CD already makes each sector cheap and does
   not shrink it. Collapsing it needs RE of the loader/BIOS-read loop's per-iteration
   gate (what the BIOS spin at `0xBFC05774` waits on between sectors), not a CD knob.
   Distinct, deeper pass.

The FMV itself then streams real-time XA. Also still open: make the FMV skippable on
*held* Start (currently it needs a fresh press edge during playback). The FMV player
is a separate overlay at **`0x80085000`–`0x8009A000`** (StrPlayer/MDEC) + low
kernel/libcd `0x80001000`–`0x80004000`; the FMV stream overwrites the
0x80011xxx/0x80018xxx intro code.

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

## HLE BIOS — the decisive fix for the Whoopee→FMV load dwell (2026-06-13)
The residual Whoopee→FMV slowness was NOT game logic and NOT raw CD transfer.
A PC-sampling profile of the black dwell (`prof N` in the REPL) is **0% game,
~56% BIOS ROM, ~44% kernel RAM**: the CPU sits entirely in OpenBIOS
`cdromBlockReading` (ROM `0xBFC03A9C`) doing a per-sector Setloc→SeekL→ReadN→
Pause cadence with blocking `TestEvent` spins (`0xBFC05774`). `bioslog` showed
the game read its 162 KiB main EXE (81 sectors → `0x80010000`) plus ISO
metadata through this one routine.

Fix = **HLE the BIOS routine** (runtime `BiosHleCdBlockRead`, hook at
`0xBFC03A9C` sig `0x24A30096` = `addiu v1,a1,0x96` i.e. `sector+=150`):
read the sectors from the CHD straight into PSX RAM at host speed
(`psxport_hle_cd_read2048` in cdc.c), set `v0=count`, return to `ra`. The
events it waits on are internal to the routine, so this is a complete
emulation. **LBA mapping (critical):** `cdromBlockReading` adds 150 to form an
absolute MSF, but SeekL→`AMSF_to_LBA`→`ABA_to_LBA` subtracts 150 again, so
CDIF is handed the original filesystem LBA — read `lba+i`, **NOT** `lba+150`
(the +150 overshoot reads garbage and wedges the load). User data = 2048 bytes
at offset 24 of the 2352 raw sector (Mode2/Form1). Generic (OpenBIOS, not
per-game), signature-checked; `PSXPORT_BIOS_HLE=0` opts out for RE/oracle.

Two CDC pacing fixes landed alongside (submodule `2e7969b`): bit8 excludes only
`MODE_STRSND` not the stray `MODE_CDDA` bit (the loader uses `Mode=0x01` data
ReadN; real CD-DA plays via Play/DS_PLAYING which never sets
`psxport_read_is_readn`); and the seek-arm rotational-settle term (~451584 cyc
@1x) collapses to a small fixed delay for non-STRSND reads under instant-CD
bit1.

**Result (Start held):** EXE+metadata load instantly at f3–6, intro state
machine done by f16, CDDA music `Play` at f16, FMV `ReadS`/XA stream at ~f29,
FMV on screen at **f45** — vs content not appearing until **~f240** before
(~160 frames / ~2.7 s removed). The FMV itself stays real-time XA (do not
accelerate STRSND); `FmvDwellSkip` handles skipping it on held Start.
Dead end removed: the bit64 "unbounded consumer-paced delivery" experiment
deadlocked BIOS CD init (defer-forever when the consumer legitimately isn't
draining) and is gone; bit8 already delivers consumer-paced without deadlock.

## Inter-FMV "Whoopee Camp logo" 6s gap (2026-06-13, IN PROGRESS)
User report: "~6 seconds from pressing Enter at Whoopee to the next FMV." Mapped
the real intro structure (natural, no input): SCEA "presents" text (f0-300) ->
**FMV#1** (ReadS f334) -> FMV#1 ends (Pause f836) -> 698 KiB overlay load
(instant via HLE, f839, lba=24 count=349 -> 0x80010000) -> **Whoopee Camp studio
logo held ~390 frames (~6.5s)** -> **FMV#2** (ReadS f1232). The "Whoopee" the user
skips is this *studio-logo hold*, not the earlier Whoopee animation.

Root cause of the hold (profiled + watchpointed): it is NOT a load (the overlay
is already instant) and NOT a simple display timer. During f926-1232 the game
slow-**prebuffers FMV#2** via direct libcd `Setloc->SeekL->ReadN->Pause`, ONE
sector per ~16 frames (~19 sectors total), interleaved with the FMV player's
per-frame display callback (dispatcher `0x80086290` iterates 8 callback slots;
the logo pace callback at `0x800506C4` cycles counter `0x800E809C` 0->1->2 every
2 frames, gated by threshold `0x1F800235`). Each read completes in ~4 frames; the
~12-frame spacing between reads is the GAME pacing itself (confirmed: not CD-speed
bound -- reads finish fast under instant-CD). This read path does NOT go through
OpenBIOS `cdromBlockReading`, so the BIOS HLE does not cover it -- it is the game's
own libcd in the 698 KiB overlay.

`FmvDwellSkip` (0x80050CE4) fires here (13.8M times) but only exits the inner
1-vblank pace wait (counter 1->2); the logo's overall duration is gated by the
prebuffer/state machine, so exiting it does not advance -- which is why held-Start
does not skip this. User chose "skippable on Start." NEXT STEP: RE the FMV
player's segment/advance state (around `0x800862D0`/`0x80050xxx` in the f839
overlay; dump RAM at f1000 = scratch/bin/tomba2/ram_f1000.bin) to find the
"logo done -> start FMV#2" gate and force it on Start. Tooling in place: `prof N`,
`bioslog` (logs A0/B0/C0 + the HLE + FmvDwellSkip counter/threshold).

## Tooling added this session
- **REPL `shot <path>`** (runtime/main.cpp): dump the current presented framebuffer
  to a PPM on demand while driving via `-repl`. VideoCb caches the last frame
  (tightly packed) into `g_last_fb`. This is the interactive analogue of
  `PSXPORT_FRAMEDUMP` and is what made the skippability tests above possible.
- Reused: `tools/frames.py` (contact sheets), `PSXPORT_WATCHW`, `PSXPORT_PCCOV`,
  `PSXPORT_RAMDUMP`, `tools/disasm.py`. KEY: dump RAM **during the logo phase**
  (f400/f1000), not during the FMV — the FMV stream overwrites the intro driver code
  at 0x80011xxx/0x80018xxx.

## HLE BIOS Stage 3 — native IRQ/exception delivery (2026-06-13, LANDED)
Stage 3 makes hardware interrupts reach the game under PSXPORT_HLE_BIOS=1, which
was the blocker past the second-stage loader. New file `runtime/hle_irq.cpp`;
generic beetle accessors only (`psxport_cpu_cop0/_set_cop0` in cpu.c,
`psxport_irq_status/_mask/_ack` in irq.c). Build/run isolated:
`make -C runtime OBJDIR=obj_irq EXE=wide60rt_irq` / `./runtime/wide60rt_irq`.

Mechanism (all policy in runtime/, no magic offsets):
- beetle's interpreter already vectors to the general-exception entry when an
  IRQ is pending and SR allows it. Under HLE that vector is EMPTY (no ROM
  handler installed), so the CPU derailed into ROM at **0xBFC00180** (BEV=1,
  never cleared by our HLE boot). Hook BOTH 0x80000080 and 0xBFC00180.
- On entry read COP0 CAUSE. ExcCode 0 (interrupt): invoke the game's root IRQ
  dispatcher as a subroutine via a trampoline ($ra = sentinel 0x80000040; a hook
  there does the single RFE + register restore + resume at EPC/TAR). ExcCode 8
  (syscall): Enter/ExitCriticalSection set/clear SR **0x401** (IEc + IM-IP2).
- The dispatcher is resolved generically: HookEntryInt's arg (B0:0x19, recorded
  in hle_kernel.cpp) is an ExCB whose word[0] is the BIOS-vectored entry; that
  entry tail-`jal`s the real dispatcher (0x800182D8 for Tomba2) — we decode the
  jal target. The dispatcher reads real I_STAT (via the kernel ptr table at
  0x80026778/7C), services sources, bumps the VBLANK frame counter 0x800267B4
  (the VSync wait at 0x80017FC4 spins on it), and acks I_STAT.

Three bugs found + fixed, in order:
1. **SR IM bits were 0** → CPU_RecalcIPCache never raised a pending IRQ even with
   I_STAT/I_MASK set. ExitCriticalSection must set **0x401** (IEc + IM-IP2 bit
   10), not just IEc. (Was the reason VBLANK never delivered at all.)
2. **Reentrancy**: an IRQ firing while EPC was inside the dispatcher re-invoked
   it and spun. Guard: if already servicing, mask (clear IEc) and resume the
   in-progress dispatcher without re-dispatching.
3. **$ra (and full reg file) clobbered**: the trampoline overwrote the
   interrupted code's $ra with the sentinel; resuming at a `jr $ra` jumped to the
   sentinel → infinite loop. Fix: save the FULL register file (GPR1..31 except
   $sp, + LO/HI) at dispatch, restore at the sentinel RFE. (Faithful to what the
   BIOS exception handler does with the process register-save area.)

VERIFIED (PSXPORT_HLE_BIOS=1, no Start): VBLANK counter 0x800267B4 advances 1:1
with frames (100@f100, 1000@f1000). The EXE runs its OWN logic — **no longer
derails into ROM / 0x80040000**. At ~f1000 the game reconfigures the display
(280x240 -> 700x480) and renders content (nonblack pixels). PSXPORT_IRQ_LOG=1
traces each exception; =2 adds an I_STAT/I_MASK/SR probe.

### NEXT BLOCKER (Stage 4 — missing syscalls, NOT IRQ): crash at ~f1284
After rendering starts, the game faults: `UNHANDLED exception code=4 (ADEL) at
epc=0x0000000E` — a jalr through a null/garbage function pointer (ra=0xE),
deep in the Whoopee animation player (0x8001138C <- 0x800111B4 sequencer). The
likely cause is one of the still-UNIMPL syscalls returning v0=0 where the game
expects a valid pointer/handle. Surfaced UNIMPL (return-0 stubs) at the crash:
A0(0x44) FlushCache, A0(0x49), B0(0x15), B0(0x35), B0(0x57), C0(0x02)
SysEnqIntRP, C0(0x03) SysDeqIntRP. Implement these (esp. the EnqIntRP/event/dev
ones) next; the IRQ path itself is complete.

## HLE BIOS Stage 4 — remaining syscalls + f1284 crash root cause (2026-06-13)
Implemented the syscalls the intro hits past Stage 3 (all in `runtime/hle_kernel.cpp`,
identified from the nocash psx-spx table):
- **A0(0x44) FlushCache** — no-op (interpreter has no I-cache), v0=0.
- **A0(0x49) GPU_cw(gp0)** — no-op v0=0 (game writes GP0/GP1 via MMIO directly;
  this helper's result is unused; no generic GP0 accessor in this stage).
- **B0(0x15) Krom2RawAdd** — return -1; Tomba2's int-init discards it.
- **B0(0x35) FileWrite(fd,buf,len)** — fd 1/2 -> emit to TTY (stderr); return len.
- **B0(0x57) GetB0Table** — returns a native HLE work page (0x8000F000). Tomba2's
  int-init (0x80017D54) reads `table[+0x16C]` to reach a control struct Q and
  derives BIOS pad-handler entry points `Q+0x884`/`Q+0x894` that it later `jr`s to.
  We publish Q at 0x8000E000 with `jr $ra;nop` no-op stubs at +0x884/+0x894 (pad
  is serviced natively, so these are correctly no-ops). Region 0x8000E000-0x10000
  is BIOS-reserved + verified-zero in fresh and mid-intro RAM dumps.
- **C0(0x02) SysEnqIntRP / C0(0x03) SysDeqIntRP** — maintain a real per-priority
  singly-linked interrupt-handler chain in PSX RAM (elem->next at word0). NOTE:
  Tomba2's root dispatcher (0x800182D8) reads I_STAT directly and does NOT walk
  this chain, so for this game it is bookkeeping — but it's faithful, not a no-op.

VERIFIED: with these syscalls (and the *pristine* Stage-3 hle_irq), the game now
reaches the **FMV at 700x480 by f1000** exactly as the ROM-BIOS path does — the
syscalls cause no regression. (Bisected: "pristine hle_irq + new hle_kernel" ->
700x480; the syscalls were each checked individually.)

### f1284 crash is NOT a missing syscall — it is a Stage-3 IRQ-prologue RACE
The `UNHANDLED exception code=4 (ADEL) at epc=0x0000000E` is unchanged by the new
syscalls (B0(0x57) returning a real table fixed the *jr-to-null-table* path the
journal earlier suspected, but the crash persists). Root-caused with a write
watchpoint on the corrupted stack slot:
```
f1283 pc=80019B98 ra=8001A188 writes 80018DF8 -> [001FFE74]   ; live frame slot
f1283 pc=800182DC ra=80000040 writes 0000000E -> [001FFE74]   ; dispatcher clobbers it
```
0x800182DC is the dispatcher prologue (`sw $s1,0x14($sp)`), `ra=0x80000040` = our
IRQ trampoline sentinel. The Stage-3 trampoline runs the game's root dispatcher as
a subroutine **on the interrupted thread's $sp**. When a VBLANK IRQ lands in the
1-instruction gap of a callee's prologue (between `addiu $sp,-N` and `sw $ra,..($sp)`),
the not-yet-saved $ra slot is still dispatcher-reachable scratch; the dispatcher's
own prologue write lands on it, so when that callee (0x8001A0C8) later runs its
epilogue `lw $ra,0x34($sp); jr $ra` it jumps to 0x0000000E -> AdEL.

Tried: a **dedicated IRQ/exception stack** (switch $sp to a private page on
dispatch, restore on RFE — what the real BIOS does). It STOPS the f1284 crash but
**regresses progression**: the game then never reaches 700x480 (stuck 280x240),
because the dispatcher / its event callbacks depend on running on the interrupted
stack (or the private stack interferes). So the SP-switch is the wrong shape here.
Reverted; hle_irq.cpp left pristine. NEXT: a faithful fix needs to save the
interrupted context to a separate area WITHOUT changing the SP the dispatcher
runs on — e.g. only redirect the *return* path, or detect the prologue-gap race.
The new syscalls are correct and kept; the IRQ-race is the standalone next blocker.
