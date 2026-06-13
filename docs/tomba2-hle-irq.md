# HLE BIOS — the f1284 IRQ crash root cause (2026-06-13)

> **RESOLVED 2026-06-13 (commit 4ad7ff2).** The real bug was the branch-delay-slot
> exception resume: on an IRQ taken in a `jr $ra` epilogue's delay slot, we resumed at
> the branch TARGET (`bd ? TAR : epc`) and SKIPPED the delay-slot `addiu $sp,+N`, so the
> frame never popped → `$sp` leaked by N → the function later read a stale return slot
> and `jr`'d to 0 (the f961/f1284 derail). Fix: always resume at EPC (the branch), so the
> branch+delay slot re-execute — faithful MIPS. Combined with the now-implemented kernel
> thread model (`__globals`/Process/TCB + OpenThread/ChangeThread + TCB exception
> save/restore), the HLE BIOS now **boots and renders the Whoopee Camp intro logo
> stably** (was: derail). NEXT blocker: it holds at the intro logo instead of advancing
> to the FMV/title — a separate issue. The analysis below is kept for the record.
>
> **NEXT BLOCKER (2026-06-13): HLE holds at the Whoopee Camp intro logo.** Verified: a
> continuous HLE run renders the Whoopee logo and the framebuffer is STATIC (meanRGB
> ~205, unchanged f1500→f6000), yet the CPU is NOT spinning — `prof` is 100% game spread
> across ~2210 PCs doing per-object math (e.g. the loop at 0x8001F9DC calling the object
> dispatcher 0x80077768). So the game runs but the intro-advance gate never fires. Per
> `docs/tomba2-intro.md` the Whoopee logo exits when its animation decode passes frame
> 250 (sets the loop terminator) and the scene sequencer advances on a VSync/CD
> ready-signal. NEXT: check whether the intro frame counter / the Whoopee decode actually
> advances under HLE (read the candidate gate vars over time), and whether the animation
> decode (MDEC/stream) is being driven — the advance likely waits on something the HLE
> still doesn't deliver during the logo. (The IRQ/VBLANK delivery itself now works — that
> was the f961 fix.)

## Symptom
With `PSXPORT_HLE_BIOS=1`, Tomba!2 boots, runs to the opening-FMV stage (audio/XA
plays, a 700×480 buffer is allocated — but it is NOT actually decoding video, per the
user's observation), then at ~f1284 takes an **AdEL** (exception code 4, cause 0x10)
with **epc=0x0000000E** — i.e. a `jr $ra` jumped to the bogus address 0x0E and the
instruction fetch faulted.

## Root cause (verified, real-debugged — do NOT just revert/retry)
**Our HLE BIOS never builds the OpenBIOS kernel `__globals` / process / thread (TCB)
tables.** Dump check on `scratch/bin/tomba2/state/hle_f900.bin`:

```
*(0x80000100) __globals[0]    = 0x00000000
*(0x80000108) processes ptr   = 0x00000000
*(0x80000110) TCB array base  = 0x00000000
*(0x80000114) threadBlockSize = 0x00000000
```

All zero. Under the **real OpenBIOS ROM** these are populated (see
`docs/tomba2-threads.md`: TCB array @ `*(0x80000110)`, 4 slots ×0xC0; the exception
handler `vendor/openbios-src/.../kernel/vectors.s` computes
`$k0 = __globals.processes[0].thread + 8` and saves the full interrupted register
file there, and `returnFromException` restores from it). Game IRQ handlers /
dispatcher read the TCB the same way. With `__globals==0`, such a read uses base 0,
loads a return address from low memory (~0x0..0x80, the exception-vector page), and
`jr`s to a tiny garbage value → the `jr→0x0E` AdEL.

The current HLE (`runtime/hle_irq.cpp`) sidesteps the kernel tables: it saves the
interrupted regs to a host array `s_saved_gpr[]` and runs the game dispatcher
(0x800182D8, resolved correctly — it's real code under HLE with a normal
`addiu $sp,-0x28` prologue) on the **interrupted thread's $sp**, then restores
`s_saved_gpr`. That happens to survive until f1284, where a handler that reads the
zero TCB finally derails.

### Why the "separate exception stack" patch regressed (don't retry as-is)
Switching `$sp` to a dedicated `kExcStackTop` (mirroring OpenBIOS `g_exceptionStackPtr`)
and restoring `$sp` on return removed the f1284 ADEL but regressed to a 280×240 derail
at ~f700 (`pc→0x14/0x20`). Same wall the prior session hit. Reason: it's still missing
the TCB — moving the stack doesn't give handlers the kernel state they read. The stack
swap is necessary but **not sufficient**; the TCB model is the missing half.

## The faithful fix (next task)
Replicate OpenBIOS's kernel state + exception protocol in the HLE:
1. At HLE boot, build `__globals` @ 0x80000100 (struct per `src/mips/openbios/kernel/
   globals.h`): `processes` ptr, `threads` ptr → a TCB array, `threadBlockSize`,
   `events` ptr. Create process[0] + thread[0] (the main thread) as USED.
2. Exception entry: save the full interrupted register file into the **current
   thread's TCB** at the `vectors.s` offsets (GPRs +0x08, returnPC +0x88 [$k0+0x80],
   SR +0x94, Cause +0x98, hi/lo +0x8C/+0x90), switch `$sp` to a kernel exception
   stack, then run the handler chain / game dispatcher.
3. `returnFromException`: reload from the **current** thread's TCB (so a handler that
   ChangeThread'd resumes the right thread) and `jr returnPC; rfe`.
4. Implement OpenThread/ChangeThread (B0 0x0E/0x10) over the TCB array (the front-end
   StrPlayer is a 2-thread coroutine system — see `docs/tomba2-threads.md`).

Until this lands, the ROM path (OpenBIOS `bios/scph5501.bin`) is the working way to
play; the HLE is bring-up only.

### f961 derail pinned (2026-06-13, tooling: PSXPORT_TRAP_LOWPC)
Added a native trap (`runtime/psxport_hooks.cpp`, `PSXPORT_TRAP_LOWPC=1`): fires once
when execution derails below RAM-text, dumping the culprit jump PC + register file.
Result on the thread-model build: **DERAIL pc=0, jumped from 0x8001A4C8 with ra=0** —
i.e. function F (the per-frame loop, prologue ~0x8001A2D4, epilogue 0x8001A4A4:
`lw $ra,0x34($sp); jr $ra; addiu $sp,0x38`) executed `jr $ra` with `$ra` loaded as 0.
F's `$sp` at the epilogue was 0x801FFEF0, so it read `$ra` from **0x801FFF24** — and a
`PSXPORT_WATCHW=1FFF24` shows that slot is **written exactly once, value 0 at f0**, and
never by F. So F never saved its own `$ra` at that slot.

The IRQ-return log (added `$sp`/`$ra` to ExceptionReturnHook): the LAST good return,
**#961 @ f960, resumes F at PC=0x8001A318 with sp=0x801FFEF0** (and that return line
prints ~5× for the single IRQ count #961 — the sentinel `ExceptionReturnHook` fires
repeatedly for one entry, a re-entrancy smell). The next return (#962 @ f961) resumes
PC=0. So between resuming F at 0x8001A318 (sp=0x801FFEF0) and its epilogue, F reads a
stale `$ra` (0) → derails. Conclusion: **the thread-model exception protocol restores a
`$sp` (0x801FFEF0) that is inconsistent with F's actual frame** (F's prologue never
wrote `$ra` at 0x801FFF24), i.e. the outer-IRQ `$sp` save (`s_outer_gpr[29]`) or the
re-entrant-sentinel restore captures/returns a `$sp` that doesn't match where F saved
its return address. NEXT: log the saved `$sp` at outer-IRQ *entry* vs F's real running
`$sp`, and fix the re-entrant sentinel (one ExceptionReturnHook per outer entry). The
thread-model changes remain UNCOMMITTED (do not ship — verified still-broken).

### Deeper (2026-06-13, ENTRY/return seq log): two distinct problems
- **The "IRQ storm" is the sentinel's location, NOT new IRQs.** Logging both outer-IRQ
  ENTRY and the sentinel return with `s_irq_count` shows a HEALTHY cadence through
  #961: entries #958..#961 resume their exact epc (80017E54 → 800181C4 → 8001A394 →
  8001A318), one return each. Then a SINGLE entry #961 produces *infinite* returns
  (all "#961"). Cause: the trampoline sentinel `kSentinelRA = 0x80000040` is **inside
  the low exception-vector page (0x0..0x80)**. When F derails to PC=0 and walks upward
  it reaches 0x40 → `ExceptionReturnHook` re-fires → restores F at 0x8001A318 → F
  derails again → loops forever. **Actionable: move kSentinelRA to a high reserved
  address outside 0x0..0x80** (e.g. in the BIOS-reserved 0x8000xxxx work region) so a
  derail can't re-trigger it — this stops the storm and lets PSXPORT_TRAP_LOWPC catch
  the FIRST derail cleanly. (Does not fix the root, but removes the confounder.)
- **Root: F's frame/`$sp` is inconsistent.** F (per-frame loop, one function, internal
  loop 0x8001A2D4–0x8001A44C, common epilogue 0x8001A4A4 `lw $ra,0x34($sp); jr $ra;
  addiu $sp,0x38`) reads `$ra` from `$sp+0x34 = 0x801FFF24` with `$sp=0x801FFEF0`. A
  `PSXPORT_WATCHW=1FFF24` proves that slot is written **only once, value 0, at boot,
  and NEVER by F** — even though F runs at the `$sp` that owns that slot. So F never
  saved its own `$ra` there, which is impossible for a normally-entered function ⇒ the
  thread-model IRQ save/restore is resuming F at a `$sp` that does not match where F's
  prologue actually ran. NEXT: find F's real entry/prologue and watchpoint F's own
  `sw $ra` to learn F's true `$sp`, then compare to the IRQ-saved `s_outer_gpr[29]` —
  the discrepancy is the bug.

Tooling added this pass (committed): `PSXPORT_TRAP_LOWPC` (derail trap). Uncommitted
debug instrumentation in hle_irq.cpp: `$sp` in the return log + ENTRY log near f960.

### DECISIVE (2026-06-13): the thread model shifts the main thread's stack
`PSXPORT_WATCHW=1FFF24` comparison:
- **Baseline (committed, no thread model):** 0x801FFF24 is written **469×** with live
  values (e.g. `pc=8001ADB0 val=0x801FFF70`, `pc=80011004/8/18` 153× each). The main
  thread genuinely uses the `0x801FFFxx` stack.
- **Thread model:** 0x801FFF24 is written **once** (f0, value 0) and never again.

So the thread-model main thread runs on a **different / shifted stack** essentially
from boot — it is not a late corruption. F later pops to `sp=0x801FFEF0` and reads the
stale `0x801FFF24`(=0) → `jr 0`. ROOT to chase next: how the thread model sets the main
thread's `$sp` — (a) thread[0]'s TCB `$sp` initialization at HLE boot, and/or (b) the
exception entry/exit `$sp` save/restore (incl. the syscall path's `tcb_save`→
`return_from_exception`/`tcb_load`, which restores `$sp` from the TCB). Diff the main
thread's `$sp` at boot baseline-vs-thread-model to find where it first diverges.

## Empirical: partial `__globals` makes it WORSE — it's all-or-nothing (2026-06-13)
Tried laying down a minimal kernel (just `__globals` @ 0x100 -> Process[0] -> a 4-slot
TCB array @ 0x8000C000) + saving/restoring the interrupted register file via the
current thread's TCB. Result: **regressed from "reaches the FMV stage, ADEL @ f1284"
to a 280×240 derail by ~f1000** — and the regression happens **with OR without** the
separate exception stack. So the derail is caused by *populating `__globals` itself*:
once `processes`/`threads` are non-null, the game takes its kernel **thread** path
(OpenThread/ChangeThread/scheduler), which the HLE does not implement, and it derails
*earlier* than the f1284 read the population was meant to satisfy. With `__globals==0`
(committed) the game skips that path and gets further.

Conclusion: the f1284 crash cannot be fixed by a partial kernel — populating the
tables only helps if the **whole** thread model is implemented at once
(OpenThread/ChangeThread over the TCB array + the exception save/restore-from-current-
TCB protocol, so the activated thread code actually works). That is the real, sized
piece of remaining HLE work. Do NOT re-attempt partial `__globals` population — it is
a verified regression. (Reverted; baseline kept.)

## Full thread model implemented — still derails at ~f961 (2026-06-13, NOT committed)
Implemented the complete OpenBIOS thread model (changes left UNCOMMITTED in the
working tree — `runtime/hle_kernel.cpp`, `hle_irq.cpp`, `hle_bios.h`, `main.cpp`):
- `__globals` @ 0x80000100 → Process[1] @ 0x8000C000, 4-slot TCB array @ 0x8000C100
  (stride 0xC0). thread[0]=USED(0x4000). Laid down at HLE boot. Verified clean at
  f958: `proc=8000c000 thr=8000c100 procsz=4 thrsz=300`, thread[0] retpc=0x80017e54,
  no collision with the work area (0x8000E000) or exc stack.
- OpenThread/CloseThread/ChangeThread (B0 0x0E/0x0F/0x10) over the TCB array
  (handles 0xFF0000NN), mirroring threads.c.
- Exception protocol rewritten to mirror vectors.s + syscallVerifier: save the
  interrupted frame into the current TCB at entry; syscall a0=1/2/3 operate on the
  saved frame (critical-section bits use **0x404 on the pushed SR** = IEp|IM2, the
  faithful OpenBIOS value, not 0x401); a0=3 / B0 0x10 swap process[0].thread and
  load the target frame (`Hle_Irq_ThreadSwitch`). Nested **hardware** IRQs are
  masked BEFORE any TCB touch (so they can't clobber the outer frame); the outer
  IRQ stashes the interrupted frame in a host store and the sentinel restores from
  it (TCB may be transiently clobbered by a mid-dispatcher syscall) UNLESS a
  ChangeThread switched process[0].thread, in which case it loads the new TCB.

### Result (verified, both BAD — hence not committed)
- **Tables ON:** progresses to ~f960 (further than nothing), then at **f961 the game
  jumps to PC=0** and the IRQ vector storms (62785 IRQs/host-frame, last_pc cycles
  0x3C/0x34/0x00). dark=1, 280×240. Same ~f1000 wall as the partial attempt.
- **Tables OFF** (same new protocol, `__globals`==0): no crash and no storm, but the
  game is **completely frozen** — 0 bytes of main RAM change between f1500 and f2500;
  it sits in a tight no-RAM loop (last_pc=0x80017D00 = the LeaveCriticalSection
  syscall body, not informative). So "no crash" here is a dead hang, NOT progress.
- **ROM path intact**: without PSXPORT_HLE_BIOS the Whoopee Camp logo renders
  normally at f800 (dark=0) — the HLE code is fully gated behind the flag.

### What's actually blocking (next session)
- Through f799 the tables-ON IRQ cadence MATCHES the committed baseline exactly
  (#800 @ f799, sr=0x00600404, pending=0x0001). They diverge only after ~f800: by
  f960 the IRQ count explodes → I_STAT VBLANK is **no longer being acked**. The
  game's root dispatcher (0x800182D8) was disassembled and is **__globals-INDEPENDENT**
  (it reads a flag @0x800256ec and the I_STAT shadow @0x80026778; never 0x800000xx),
  so the ack-failure is not the dispatcher consulting kernel state directly.
- Only **1** `syscall` (a0=2) fires in 960 frames and **0** OpenThread/ChangeThread —
  i.e. the StrPlayer coroutine path is NOT being entered before the derail. So the
  derail is NOT in the thread primitives themselves; populating `__globals` changes
  some *other* game code path (between f800–f960) that then (a) stops acking VBLANK
  and (b) jumps to 0. The delay/loop counter @0x80039EC4 advances to 0x131d5b then
  freezes at f961 (PC went to 0 mid-loop), confirming the bad jump is a `jr`/`jalr`
  to a null target reached only on the tables-ON path.
- NEXT: single-step / watch the f800→f961 window with tables ON to find the exact
  `jr`/`jalr` to 0 and the kernel field it derived that target from (candidates: a
  handler installed via SysEnqIntRP whose chain `s_int_chain_head` the game now
  walks; or a `getFreeTCBslot`/thread-count read that returns a value the game
  indexes a table with). Tooling to build: a "trap PC<0x10000" hook in hle_irq that
  logs the prior PC + the load that produced the target. The fix is likely a small
  missing field, not a redesign — the exception/SR protocol itself is sound (it runs
  clean for 800 frames and the tables-OFF run never storms).

## After the intro renders: the HLE is missing the BIOS CD-filesystem + LoadExec API (2026-06-13, macOS test)
On a real macOS HLE run the intro renders (SCEA after a dwell, then Whoopee), but:
```
CD timeout: CD_cw:(CdlNop) Sync=NoIntr, Ready=NoIntr
[hle] UNIMPL B0(14) a0=2 a1=1F8010F4 a2=4 ra=80017B30
[hle] UNIMPL A0(71) a0=1F8010F0 a1=FFFFFFFF a2=14 ra=80011368
[hle] UNIMPL A0(51) a0=800253A4 a1=80200000 a2=0 ra=8001137C
```
Identified by disassembling the call-site trampolines (`li $t2,0xA0/0xB0; jr $t2;
li $t1,N`) in an early HLE RAM dump (scratch/agent-hle/early_boot.bin):
- **A0(0x51) = LoadExec** (wrapper 0x80012D2C). Args (filename/exec-hdr ptr 0x800253A4,
  stackbase 0x80200000, stackoffset 0) → the intro **LoadExec's the next stage**.
- **A0(0x71) = _96_init** (wrapper 0x80012D3C) → initialize the BIOS "cdrom:" file device.
- **B0(0x14)** + **B0(0x15)** (wrappers 0x80017CCC/0x80017CDC) → PAD/kernel init.

So both the CD timeout and the post-logo stall are the same gap: the game drives the
PSX **BIOS CD-ROM filesystem API** (_96_init → open/read "cdrom:" files) and **LoadExec**
to load/launch what follows the logo, and our HLE doesn't implement them (we only have
the low-level cdromBlockReading override + instant-CD, not the file/exec API). NEXT
(scoped): implement _96_init + the cdrom: file open/read path (reuse hle_iso's ISO9660
+ EXE loader from psxport_hle_boot) and A0(0x51) LoadExec (load a PS-X EXE mid-run onto
the given stack and jump to it), plus the B0(0x14)/0x15 init stubs. The IRQ/exception
core is solid now (f961 fixed); this is a fileio/exec layer on top.

## HLE CD-ROM now works; FMV stream stuck on CD-IRQ delivery (2026-06-13)
Big step: implemented the CD interrupt-enable (the BIOS CdInit normally sets
IRQOutTestMask / 1F801802.idx1; pure HLE skipped it). `psxport_cd_set_irq_enable(0x1F)`
at HLE boot → beetle now asserts IRQ_CD, the game manages IER to 0x07, **CD (0004) +
DMA (0008) IRQs deliver, the primary CdlNop CD timeout is GONE**, and the HLE
LoadExec's MAIN.EXE and reaches the StrPlayer FMV player streaming the opening movie
(`ReadS`, 1123 data-ready `irq 1`s). Committed (beetle 3f66f8f, main 12e8b80).

NEXT blocker — **the FMV ring buffer never fills**: the StrPlayer spins in its
per-command prebuffer wait (loop @0x8008AE6C incrementing `*(0x8010274C)` vs a
timeout; loop @0x80085924 waiting for a value to stabilize). Root: beetle raises ~1156
CD IRQ asserts (1123 INT1 data-ready), but only **~13** reach our exception handler
(`pending=0004`) — the rest are acked/cleared without an exception, so the game's CD
data handler doesn't run to DMA each sector into the ring. So CD data-ready IRQ
delivery is lossy under HLE. NEXT: instrument I_STAT bit2 set/clear vs exception-taken
to see who acks the CD INT without delivering it (candidates: our cdromBlockReading /
instant-CD override consuming/acking the streaming reads; or the dispatcher acking
I_STAT before re-arming; or I_MASK bit2 toggling). The game may stream via the CD
INT1->DMA3(CDROM) path that needs faithful per-sector delivery, not instant-CD.

Residual: one intermittent `CdlSetloc` Sync timeout (f848) despite assert_CD=1 — same
delivery-race class.

### FMV-ring rule-outs + the real shape (2026-06-13)
- **Not the mode-0x2000 callback gap.** The HLE never invokes mode-0x2000 event
  callbacks (hle_kernel.cpp:139,468 "NOT invoked"), but bioslog shows every OpenEvent
  the game makes has **func=0** (classes F0000009/F0000011/F4000001, spec 0x04/0x20,
  mode 0x2000) — they're WaitEvent/TestEvent events, NOT callbacks, and none are CDROM
  (F0000003). So wiring callbacks won't help the FMV.
- **Not instant-CD.** `cd 0` (instant-CD off) under HLE leaves it equally stuck.
- **The shape:** the CDC raises ~1156 CD INTs (1123 INT1 data-ready) and they get acked
  ~1156× (the game POLLS the response FIFO — reading it acks), yet only ~13 reach the
  main exception (pending=0004) and only ~17 DMAs occur. So the game streams the FMV by
  **polling the CD FIFO**, not the main CD IRQ, and is NOT DMAing each ready sector into
  the StrPlayer ring buffer — so the ring never reaches its prebuffer target and the
  StrPlayer spins in the wait at 0x8008AE6C (per-command counter @0x8010274C).
- NEXT: trace the StrPlayer per-sector read/DMA issue path (0x8008AE00 issues the reads;
  the read helper is ~0x8008B0C8 per docs/tomba2-fmv-skip.md) under HLE — find where the
  per-sector CDROM DMA (DMA3) is gated/skipped: is the data-ready poll seeing beetle's
  INT1? does the game arm DMA3 but it doesn't transfer? Watchpoint the ring write index
  and the DMA3 CHCR. The fix is in the CD-streaming data path, not the event/IRQ core
  (which now works).

### 2026-06-14 — FALSIFIED the whole "FMV-ring / lossy CD-IRQ / per-sector DMA" framing
**The two sections above are WRONG.** Re-instrumented with frame-stamped DMA3 + BFRD
probes (beetle dma.c `[dma3]`, cdc.c `[cd-reqdata]`, gated on PSXPORT_CDC_LOG) and a new
`irq` REPL command (dumps I_STAT/I_MASK/SR.IEc/EPC/Cause). Findings, all on the HLE path:
- **FMV#1 streams PERFECTLY.** Real counts to f835: **1969 DMA3 arms, 472081 words
  (~922 sectors)** moved into the ring. The "only ~17 DMAs" claim was a measurement
  error. CD INT1→DMA3 delivery is NOT lossy; the ring fills fine for FMV#1.
- **The stall is AFTER FMV#1, at the FMV#2 prebuffer**, not a CD-data problem. Sequence:
  FMV#1 ReadS@f324 → Pause@f842 → Reset@f845 → Demute/Setmode@f847 → **Setloc@f848 (acks
  INT3 fine) → then ZERO further CD commands** (no SeekL, no ReadN, no 2nd ReadS) for
  450+ frames. (ROM-BIOS at the same point runs Setloc→SeekL→ReadN→Pause prebuffer cycles
  f926-1230, then ReadS@f1232 = FMV#2. 12 SeekL / 20 ReadN under ROM, **0 SeekL under HLE**.)
- **Root cause = interrupts stuck DISABLED.** At the stall the CPU spins in the StrPlayer
  prebuffer wait `0x8008AE54` (exits only when `*(0x800ABDE0) > 0x3C3`; the `0x3C0000`
  timeout at 0x8008AE88 is effectively never). `irq` shows the deadlock unambiguously,
  identical at f880/f980/f1280:
  ```
  I_STAT=0005 I_MASK=000D pending=0005 | SR=40600000 IEc=0 | EPC=800808A4 Cause=00000420
  ```
  VBLANK(bit0)+CDROM(bit2) are **pending AND unmasked** (pending=0x0005) but **SR.IEc=0**,
  so beetle never vectors the exception. EPC=0x800808A4 = the **LeaveCriticalSection**
  syscall (a0=2; the EnterCriticalSection stub is 0x80080890 a0=1). The game is wedged
  inside an open critical section.
- **Why that wedges the FMV:** the prebuffer gate `*(0x800ABDE0)` is a per-frame counter
  bumped ONLY by the game's VBLANK callback `0x800909C0`, which is dispatched by the
  game's I_STAT-polling callback dispatcher `0x80085D8C` (`pending = I_STAT & I_MASK &
  0x000D`; bit0=VBLANK → table@0x800AAD1C[0]=0x800909C0; it ACKs the bit at 0x80085E4C).
  Counts: dispatcher `0x80085D8C` runs **578× under ROM** (≈every frame, caller game-code
  ra=0x80085D2C) vs **11× under HLE** (all f845-848, caller = IRQ trampoline ra=0x80000040),
  then 0. With IEc=0 the pending VBLANK can't vector → dispatcher never runs → counter
  frozen at 3 → wait needs >0x3C3 → spins forever → no prebuffer reads → no FMV#2.
- **So the prior "wire mode-0x2000 callbacks" / "per-sector CD-poll/DMA" NEXTs are both
  dead ends.** The defect is in the **IRQ-enable / critical-section state**, not CD data
  and not the event-callback table. The game's `0x800909C0` is delivered by a polling
  I_STAT dispatcher, not a BIOS event callback, so DeliverEvent wiring is irrelevant here.

**Verified contrast:** EnterCS(0x80080894)/LeaveCS(0x800808A4) trace shows the HLE going
net **+1 Enter (10 vs 9)** across the f845-848 transition then silent — the outermost
LeaveCriticalSection that should restore IEc=1 is never reached (the game is spinning
before it). On ROM the same transition recovers (IEc toggles back to 1, VBLANK fires
every frame). DEAD ENDS confirmed: instant-CD on/off identical; the ring/DMA path is fine.

**NEXT (the actual fix):** find why HLE leaves IEc=0 here where ROM doesn't. Concrete plan:
log IEc before/after every Enter/LeaveCriticalSection syscall (hle_irq.cpp:296-328) and the
return_from_exception RFE-pop (line 169) around f845-848, and compare to ROM. Hypotheses to
test in order: (a) a LeaveCriticalSection whose `saved_sr|=0x404` writes a TCB that
return_from_exception doesn't reload (thread-switch/`s_in_exception` interaction —
Cause=0x420 shows a HW IRQ pending *at syscall time*, the fragile case); (b) the game's
outermost Leave is genuinely gated behind a step that on ROM completes via an IRQ that HLE
swallowed earlier (so fixing an earlier missed VBLANK delivery unblocks it). Tooling for
this is in the tree: `irq`, `gpr` REPL cmds; `[dma3]`/`[cd-reqdata]`/`[hretrace]`/
`[timerN-mode]` probes under PSXPORT_CDC_LOG.
