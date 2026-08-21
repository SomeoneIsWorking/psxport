---
id: C011
kind: claim
status: holds
created: 2026-08-21
tags:
depends: runtime/recomp/bios_libc_string.cpp#bios_libc_string_dispatch, runtime/recomp/hle.cpp#Hle::dispatchBios, tests/test_bios_libc_string.cpp
reconfirmed: 2026-08-21
verified_at: 2026-08-21 11:37:46
---

## Claim

Sony BIOS A0:0x15 strcat is dispatched through the shipping HLE as a guest-memory byte loop that appends the source terminator and returns the original destination.

## Evidence

test_bios_libc_string passed 4/4 cases and 44 checks through Game::hle.dispatchBios; Toy Story 2 independently reaches A0:0x15 at caller 0x8007F108 while composing measured .vh/.vb paths.

## What would falsify it

A change to Hle::dispatchBios string dispatch, Core guest byte mapping, or test_bios_libc_string; falsify if return, terminator, guard bytes, KSEG aliasing, or bytewise forward-alias behavior changes.

## Re-confirmed 2026-08-21

Final Clang build and complete framework gate passed 75/75, including test_bios_libc_string 4/4 cases / 44 checks, cpp_style format plus clang-tidy, emitter/oracle tooling, and CMake ownership. Toy Story 2 live boot no longer aborts at A0:0x15 and advances to its generated stock-libcd completion poll 0x80091B18.

## Re-confirmed 2026-08-21

Final post-extraction Clang gate passed 75/75. test_bios_libc_string passed 4/4 cases / 44 checks through Game::hle.dispatchBios, including the immediately adjacent post-terminator guard byte, KSEG mirror, forward guest alias order, and wrong-table/neighbor opposite answers. Toy Story 2 live boot advances from the former A0:0x15 abort to generated stock-libcd completion poll 0x80091B18.

## Re-confirmed 2026-08-21

Post-landing Clang build and 76/76 CTests passed; test_bios_libc_string passes 4/4 cases and 44 checks through the shipping Hle dispatcher, and Toy Story advances past A0:0x15 into its stock-libcd poll.
