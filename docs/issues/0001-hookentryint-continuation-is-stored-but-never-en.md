---
id: 1
title: HookEntryInt continuation is stored but never entered
status: resolved
symptom: Games reach a pending CD-ROM IRQ2 with I_STAT bit 2 set, but their registered CD callback never runs and boot waits until the watchdog.
tags: bios,irq,cdrom,hookentryint,crashbash,megamanx4,vagrant
created: 2026-08-21
updated: 2026-08-21
---

## Evidence

Crash Bash and Mega Man X4 independently install a `HookEntryInt` jmp_buf and register IRQ2 callbacks
in their custom master interrupt tables. In both ports, psxport walks only the `SysEnqIntRP` chain.
The observed chain elements are VBlank-only and correctly decline CD bit 2; the saved custom-exit
continuation is never entered.

Vagrant Story supplied a third independent saved context: `0x80031084` contains RA `0x8001FAD0`,
the instruction after libetc's setjmp call at `0x8001FAC8`. Entering the fixed framework path reached
that address, proving the formerly skipped path was now executing.

## Root cause

`B0:0x19 HookEntryInt` stores the guest jmp_buf in `Hle::exception_exit_buf`, but `Hle::irqPoll` never
restored that buffer and dispatched its saved return address. Consequently the guest master interrupt
dispatcher could not scan its IRQ source table and invoke the CD callback.

The first executable consumer then exposed the other half of the control contract:
`B0:0x17 ReturnFromException` must return through the typed execution boundary. In Vagrant Story, falling through
from the saved `0x8001FAD0` path into one-time interrupt initialization at `0x8001FAE0`, repeatedly
cleared DMA4's DICR enable and left `_waitTransferAvailable` stuck.

## Falsifier

A framework delivery path that enters the exact saved context after the BIOS chain, plus consumer
traces showing the true IRQ2 callbacks execute, falsifies the defect. A forced-negative buffer with no
saved continuation must remain undispatched.

### Resolution (2026-08-21)

Resolved by restoring the measured HookEntryInt jmp_buf after the SysEnq chain, dispatching its
saved continuation, and making B0:0x17 a non-returning execution exit. MMX4 calls IRQ2
0x800E7944 and clears I_STAT; Crash Bash reaches 0x8003F5F0 and its CD drain; Vagrant retains DICR
0x00900000, repeatedly dispatches DMA4 0x8001DE94, and advances VBlank 0 to 179. Hermetic gates prove
restored/refused contexts and non-return versus fallthrough.
Vagrant durable evidence: `tools/re_vblank.py`, `docs/re-frontier.md` RE-02/RE-10, claims 012/014,
issues 0014/0015.
