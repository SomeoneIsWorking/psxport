---
id: C004
kind: claim
status: holds
created: 2026-08-21
tags: tooling,resource-ownership
depends: tools/formats/psx_exe.py, tests/test_psx_exe.py
reconfirmed: 2026-08-21
verified_at: 2026-08-21 11:09:16
---

## Claim

The PS-X EXE and raw-RAM loaders release their input file on successful parsing and on invalid-magic refusal.

## Evidence

`tests/test_psx_exe.py` uses retained tracking streams for valid EXE, invalid EXE, and RAM
loads.

## What would falsify it

A loader leaves its retained input stream open after returning or raising, or a new loader path bypasses the ownership test.

## Re-confirmed 2026-08-21

Retained tracking streams closed after valid EXE, invalid-magic refusal, and RAM load.
