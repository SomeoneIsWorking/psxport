---
id: C025
kind: claim
status: holds
created: 2026-08-22
tags: recompiler,control-flow
depends: tools/recomp/emit.py#_scan_computed_offset, tools/recomp/test_emit.py, tools/recomp/decode.py
reconfirmed: 2026-08-22
verified_at: 2026-08-22 19:02:18
---

## Claim

The recompiler generically preserves bounded inline computed-offset tables built with lui plus ori while refusing clobbered, partial, or unaligned target candidates.

## Evidence

RED-first four-case inline trampoline execution and exact-target census now pass; two Vagrant-derived negatives reject a load-clobbered partial lui and unaligned masked base; Toy Story emits exactly 32 measured slots and reaches both edge cases without a recomp miss; Vagrant fresh no-argument emission completes 799 resident plus 137 TITLE functions; focused pytest passes 55/55 and full Clang CTest passes 85/85.

## What would falsify it

Any supported executable loses a legitimate computed target, admits an extra or unaligned target, Toy Story slot census changes, or Vagrant fresh emission regresses.

## Re-confirmed 2026-08-22

Post-commit 55/55 focused pytest and 85/85 Clang CTest pass; Toy exact 32-slot/local execution and Vagrant fresh 799-resident plus 137-TITLE emission are green.
