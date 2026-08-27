---
id: 36
title: Memory-card directory enumeration omits its completion event
status: resolved
symptom: Crash Bash accepts its authentic disc, then watchdogs in the libmcrd HwCARD event wait after firstfile reports an empty host card.
tags: memory-card,hle,events,firstfile,nextfile,crash-bash
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

The host memory-card backend owns BIOS `B0:0x42 firstfile` and `B0:0x43 nextfile` synchronously and
preserves their `DIRENTRY`-or-zero return contract, but neither path delivered the card completion
event. Crash Bash's stock libmcrd directory reader treats a zero return as a valid empty result and
then waits for the HwCARD completion callback before continuing. Its callback flag at `0x80078810`
therefore remained zero forever inside `0x800476EC`.

Ghidra establishes the complete live chain: `0x8002C97C` calls directory reader `0x8003A554`, which
calls the BIOS firstfile wrapper and, on zero, waits in `0x800476EC`; event setup `0x8004725C` opens
class `0xF0000011`, spec `0x0004`, mode `0x1000`, with handler `0x800471DC`, whose entire body sets
`0x80078810 = 1`. The corrected authentic-disc product reached this chain and exited through the
framework watchdog before another presentation.

## Resolution gate

For every valid firstfile or nextfile request, preserve the existing return value and deliver the
existing memory-card completion event exactly once, including the empty/end-of-scan result. Exercise
the shipping BIOS dispatch and interrupt-mode callback path with both operations, then rerun the real
Crash Bash product and require progress beyond `0x800476EC` without a guest-VSync violation.

## Outcome

Confirmed as a real defect, but it was NOT the cause of the Crash Bash symptom in the title above.
The first product rerun with this fix still watchdogged at `0x800476EC`; a `PSXPORT_DEBUG=card,ev`
trace then showed that `firstfile` was never reached at all, because that title had never called
`card_overrides_init` and so the `"bu"` BIOS device was missing from the kernel device table. Its
libmcrd resolves the path by walking that table itself and returns an ambiguous 0 when no device
matches. See crashbash issue 0013.

With the title wiring corrected, this fix IS load-bearing and was verified by a direct A/B on the
real disc: with the device published but `deliverComplete` removed, `firstfile` is reached, returns
zero, and the guest still hangs in `0x800476EC` (exit 134); with both in place the run completes
120/120 frames and exits 0. The discriminator was run in both directions.

The symptom line in this issue's front matter therefore describes the wait, not its trigger.

## Candidate evidence

The shared candidate calls `Memcard::deliverComplete` exactly once after each valid directory scan,
after the original return value has been selected. `test_memcard_file_api` reaches both operations
through `Hle::dispatchBios`, installs a synthetic guest handler through the shipping override
interception point, and proves an empty firstfile plus end-of-scan nextfile each return zero and invoke
the interrupt-mode HwCARD callback once. A fresh isolated Clang 22 build passes the focused CTest and
all 10 direct cases / 50 checks; the touched-TU C++ policy checks 2/2 compile-backed files with
clang-tidy. The real Crash Bash rerun remains the resolution gate.
