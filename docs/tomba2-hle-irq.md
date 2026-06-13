# HLE BIOS — the f1284 IRQ crash root cause (2026-06-13)

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
