---
id: C004
kind: claim
status: holds
created: 2026-08-21
tags: tooling,resource-ownership
depends: tools/recomp/psexe.py, tools/recomp/test_psexe.py
reconfirmed: 2026-08-21
verified_at: 2026-08-21 02:53:49
---

## Claim

The PS-X EXE and raw-RAM loaders release their input file on successful parsing and on invalid-magic refusal.

## Evidence

2026-08-21: tools/recomp/test_psexe.py passed 12/12 using retained tracking streams for valid EXE, invalid EXE, and RAM loads.

## What would falsify it

A loader leaves its retained input stream open after returning or raising, or a new loader path bypasses the ownership test.

## Re-confirmed 2026-08-21

2026-08-21: retained tracking streams closed after valid EXE, invalid-magic refusal, and RAM load; psexe loader 12/12 and integrated Clang CTest 70/70.
