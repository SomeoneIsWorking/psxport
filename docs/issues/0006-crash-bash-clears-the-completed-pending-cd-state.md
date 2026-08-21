---
id: 6
title: Crash Bash clears the completed-pending CD state before a guest-visible poll return
status: open
symptom: Crash Bash completes its paced 189-sector read and advances, but its strict true-oracle comparator refuses the missing guest-visible completion-pending result
tags: crashbash,cdrom,irq,hookentryint,returnfromexception,oracle
created: 2026-08-21
updated: 2026-08-21
---

## Proven boundary

The deterministic CDC build now agrees with the true oracle at read admission: the start and first
read return 189 with the destination and expected-sector cursor untouched, and the first INT1 occurs
only after one nominal 225,792-tick double-speed drive period. The full LBA35799..35987 read completes,
prints `done loading`, and advances to recompilation miss 0x80092BDC.

The later completion handshake is not oracle-exact. The true oracle returns completion-pending `1`
from both the sync and poll paths while `async=1`, then returns `0` after callback drain. The port
does set `async=1` transiently at cycle 44,081,261, but clears it before either guest function
returns; its only observed sync return is `0` with `async=0` at cycle 44,082,248. The strict
comparator therefore refuses the port even though the live load advances.

## Next root-cause boundary

This mismatch occurs after correct drive deadlines and sector delivery. Investigate the shared
HookEntryInt/`ReturnFromException` reentry and callback-drain ordering in a separate milestone. Do not
change CDC deadlines, inject a game wakeup, or preserve the pending bit artificially to make the
comparator green.

## Reproduction

Use Crash Bash's executable-backed read-completion comparator with a true-oracle capture and a port
capture from the same 189-sector read. The instrument must observe both answers: accept the oracle's
guest-visible `1 -> 0` sequence and refuse a port capture where `async=1` exists only between guest
return boundaries.
