---
id: 144
title: Interrupt handlers ran on the interrupted $sp, so a title that repurposes $sp as a data pointer had its list overwritten by the ISR's frames
symptom: Spyro 3 faults in its main-loop draw, `invalid load/store at address 0x1f800400`, after a display-list culler walked a corrupted pointer list
state_items: S017
tags: interrupt,exception-stack,hle,irqpoll,spyro3,root-cause
created: 2026-10-01
updated: 2026-10-01
---

## Answer

**`Hle::irqPoll` dispatched every guest interrupt handler (chain verifier and handler, DMA completion
callback, the framework's CD ready callback) on the interrupted context's `$sp`. The retail BIOS does
not: it runs the chain on the kernel's exception stack.** A title that uses `$sp` as a general register
while interrupts are enabled is therefore safe on hardware and was corrupted here.

Bytes, from `SCPH1001.BIN` (the exception handler, `bfc10830..bfc10920`): the handler saves the
interrupted GPRs into the TCB (`bfc108a8 sw $sp,0x74($k0)`), then **replaces `$sp`** with the kernel's
own exception stack before it walks the priority chain and calls each verifier and handler:

    bfc108c8  lui  $sp,0
    bfc108d0  lw   $sp,0x6cf0($sp)      ; the kernel exception stack pointer
    bfc10908  jalr $s1                  ; verifier
    bfc10920  jalr $s0                  ; handler

The `HookEntryInt` (custom exit) path already honoured this, taking `$sp` from its jmp_buf
(`bios_interrupt_enter_custom_exit`); the chain walk and the two completion arms did not.

### The victim: Spyro 3's display-list culler

`SCUS_944.67` `0x8001C3E8` (saved-register prologue at `0x8001C368`) loads a count and a list pointer
from `0x8006E334`/`0x8006E338`, then sets `$sp` = list pointer, `$ra` = end-of-flags, `$fp` = flags
(`0x1F8003C0`) and walks the sorted list with `lw $gp,($sp) ; addi $sp,$sp,4`. It is called raw from the
draw (`0x8001E544 jal 0x8001C368`) with interrupts enabled and no critical section (no `syscall` or
`mtc0` in the routine). A host turn boundary inside it, followed by the field's interrupt, put the
VBlank chain element's frame below `$sp` = `0x8009ABF4`, i.e. over list entries 0..8. Measured by
logging the list per step: valid at step 116 (`l0=8009AC74 l1=8009AF08`), overwritten between steps 337
and 338 (`l1=801AC2A8`, `l9=1F8003EB` = the interrupted `$ra`), with the culler suspended at
`0x8001C614` and `$sp=8009ABF4`. A pointer that was not a model record then gave a vertex count of 0
(`andi $t5,$s4,0xff` = 0); the do-while transform loop at `0x8001C4EC..0x8001C564` (exit test
`bne $t6,$t5`) never matched and stored past the 1 KiB scratchpad. The fault address
`0x1f800400` is the **store address** (`sw $v1,-4($t6)` at `0x8001C568` with `$t6 = 0x1F800404`), not a
guest PC: nothing executes from the scratchpad, and the faulting word `0x4A180001` is an RTPS in
resident text.

### The fix

`Hle::enterExceptionStack` puts the handler on `Hle::kExceptionStackTop` (`kWorkBase`, directly under
the BIOS work area). It is applied to the DMA completion callbacks, the chain walk (verifier and
handler) and the framework's CD ready callback; every site restores the interrupted register file it
already saved. Tests: `test_cd_ready_delivery` (guest element; framework arm) and
`test_direct_dma_callbacks` assert the handler's `$sp` and that the interrupted `$sp` is restored; both
go red with the body of `enterExceptionStack` removed.

## What is NOT claimed

- The exact value of the retail cell at `0x6cf0` was not read; the framework uses its own stack under the
  work area. Only the switch itself is established from bytes.
- `cd_drive_stock_read`'s inline callback dispatch and the FieldOwner's field callback (its own
  `handlerStackTop`) were not changed; they do not run under an arbitrary interrupted `$sp`.
- Other titles were not re-measured here; the change is to a path every interrupt-driven title takes.
