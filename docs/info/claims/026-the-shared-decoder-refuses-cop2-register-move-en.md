---
id: C026
kind: claim
status: holds
created: 2026-08-22
tags: recompiler,decoder
depends: tools/recomp/decode.py#decode, tools/recomp/test_decode.py
---

## Claim

The shared decoder refuses COP2 register-move encodings whose reserved bits 10 through 0 are nonzero.

## Evidence

Canonical CTC2 passes, four noncanonical reserved-bit controls including Tekken data word 0x48CCCCCE decode UNKNOWN, focused emitter/decoder pytest passes 55/55, and full Clang CTest passes 85/85.

## What would falsify it

A nonzero-reserved COP2 move decodes as an instruction or a canonical register move is refused.
