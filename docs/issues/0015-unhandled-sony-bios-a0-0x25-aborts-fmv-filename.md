---
id: 15
title: Unhandled Sony BIOS A0:0x25 aborts FMV filename parsing
status: resolved
symptom: Toy Story 2 reaches BIOS A0:0x25 toupper from ra=0x800D8E50 and aborts before the FMV parser can consume the return value
tags: bios,hle,libc,toupper,toystory2
created: 2026-08-22
updated: 2026-08-22
---

## Root cause

The shared Sony-libc dispatch implemented neighboring string leaves but did not claim A0:0x25, so
the generic BIOS miss path aborted when Toy Story 2's FMV parser called `toupper`.

### Resolution (2026-08-22)

The shipping HLE seam now implements locale-independent ASCII `toupper`. Its test reaches that seam
and gates lowercase, uppercase, digit, high-byte, and wrong-table answers.
