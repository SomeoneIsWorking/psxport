---
id: 34
title: Unhandled Sony BIOS A0:0x1A aborts CTR startup comparison
status: resolved
symptom: CTR live run aborts at unimplemented BIOS A0:0x1A from RA 0x8001C5D4 before startup can continue.
tags: bios,hle,libc,memcmp,ctr
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

The shared Sony BIOS libc dispatcher did not own A0:0x1A memcmp. CTR reached the BIOS tail-call from RA 0x8001C5D4 with lhs=0x80081000, rhs=0x8008CFB8, n=3, so the fail-fast unimplemented-call boundary aborted the product.

## Resolution

bios_libc_string_dispatch compares bytes through Core guest memory in ascending order, stops at the first mismatch, and returns the signed difference of the two unsigned bytes in guest v0. Zero length performs no read. The behavior remains in the shared BIOS/HLE owner rather than a title-local override.

## Evidence

2026-08-27: the focused Clang build compiled and linked the current shared libpsxport and test_bios_libc_string. CTest test_bios_libc_string passed 1/1 in 0.15 seconds, covering equal prefix, positive and negative first mismatch, KSEG1 aliasing, zero length with invalid addresses, and unchanged input buffers. CTR product rerun remains the live continuation gate.
