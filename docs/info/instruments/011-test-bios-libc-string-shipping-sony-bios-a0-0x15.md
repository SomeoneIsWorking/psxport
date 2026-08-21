---
id: I011
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

test_bios_libc_string — shipping Sony BIOS A0:0x15 strcat guest-memory test

## Validated by

The Clang-built shipping HLE test passed 4/4 cases and 44 checks: terminator and surrounding-byte bounds, original-destination return, KSEG1 source alias, empty inputs, forward guest alias byte-copy order, and explicit wrong-table plus neighboring-function opposite answers.

## Known failure modes

(none recorded yet)
