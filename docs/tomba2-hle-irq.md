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
