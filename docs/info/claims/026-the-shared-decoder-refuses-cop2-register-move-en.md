---
id: C026
kind: claim
status: holds
created: 2026-08-22
tags: tooling,decoder,mips
depends: tools/mips/decode.py#decode
reconfirmed: 2026-08-22
verified_at: 2026-08-22 19:02:18
---

## Claim

The shared decoder refuses COP2 register-move encodings whose reserved bits 10 through 0 are nonzero.

## Evidence

Canonical CTC2 passes, while four noncanonical reserved-bit controls including Tekken data word
0x48CCCCCE decode as unknown.

## What would falsify it

A nonzero-reserved COP2 move decodes as an instruction or a canonical register move is refused.

## Re-confirmed 2026-08-22

The canonical COP2 positive and four reserved-bit negatives pass in the focused decoder suite.
