---
id: I010
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

oracle_trace modeled-return, ordinal-call, and exact-PC boundary capture

## Validated by

Permanent 17/17 both-answer suite: ordinal 1 and 2 must produce different targets; the compatibility
alias must equal ordinal 1; a capture must contain 33 register records; ordinal 3 must refuse with
2-of-3 and no boundary; correct A(39h) must resume to a subsequent call; wrong A(38h) must refuse with
no modeled/post evidence. Exact-PC capture must reach an indirect `jalr` target before its first
instruction with 33 register records, refuse an unreachable target with its executed denominator, and
refuse a successor reached only after an unsupported hardware access. The ordered control must resume a
validated selector-1 syscall, then B(56h), seed `0x8000F818` through `oracle_main_ram`, and capture A(44h)
with the seeded word visible; the alternate seed must produce the opposite register value, and B56 before
the syscall must refuse without modeled/post evidence. `oracle_spike` separately proves
the generic resume API accepts an exact settled boundary and refuses wrong target, wrong return-PC, and
pending-load states without mutation.

## Revalidated 2026-08-26

The expanded CLI suite passed 17/17 in the full Clang 106/106 CTest run. The grounded chain observed
`0x00000C80` and the deliberate opposite `0x00000C84` at the same A(44h) boundary; B(56h) before the
requested syscall refused without emitting any modeled RAM or post-return evidence.

## Known failure modes

An emulator device callback can advance BACKED_PC before returning a hardware stop. That successor is
tainted and must not be emitted as a PC boundary; `--capture-at` accepts only an initial state or a
clean `ORACLE_STOP_BUDGET` successor. A requested PC outside the executed window is a refusal, not an
empty successful capture.

An ordered model must not arm its post-return capture after the first return: an intermediate BIOS call
would otherwise be reported as the final comparison boundary without applying its requested model.
