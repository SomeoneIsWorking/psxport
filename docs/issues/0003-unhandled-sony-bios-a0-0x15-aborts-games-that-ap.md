---
id: 3
title: Unhandled Sony BIOS A0:0x15 aborts games that append asset suffixes
status: resolved
symptom: FATAL: unimplemented BIOS A0:0x15 (libc) immediately after disc open; Toy Story 2 caller RA 0x8007F154
tags: bios,hle,libc,strcat,toystory2
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

The common HLE omitted Sony libc A0:0x15 (`strcat`). Toy Story 2 function 0x8007F108 calls it twice to
append measured `.vh` and `.vb` suffixes, so the generic dispatcher aborted before CD loading could
proceed.

## Resolution

`bios_libc_string_dispatch`, called by `Hle::dispatchBios`, now implements the leaf as a guest-address
byte loop through `Core::mem_r8`/`mem_w8`, writes the source terminator, and returns the original
destination. `test_bios_libc_string` gates 4 cases / 44 checks including KSEG aliasing, forward alias
order, surrounding-byte bounds, empty inputs, and wrong-table/neighbor opposite answers.
