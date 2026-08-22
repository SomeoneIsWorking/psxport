---
id: C023
kind: claim
status: holds
created: 2026-08-22
tags: bios,hle,libc,toupper
depends: runtime/recomp/bios_libc_string.cpp#bios_libc_string_dispatch, runtime/recomp/hle.cpp#Hle::dispatchBios, tests/test_bios_libc_string.cpp
reconfirmed: 2026-08-22
verified_at: 2026-08-22 18:00:29
---

## Claim

Sony BIOS A0:0x25 toupper is dispatched through the shipping HLE with ASCII-only, locale-independent semantics.

## Evidence

test_bios_libc_string passes through Game::hle.dispatchBios and proves t->T, T->T, digit unchanged, 0xE0 unchanged, and wrong-table Z:0x25 unclaimed; full Clang framework CTest passes 84/84.

## What would falsify it

A0:0x25 no longer reaches bios_libc_string_dispatch; a lowercase ASCII byte is not uppercased; or any non-lowercase byte changes.

## Re-confirmed 2026-08-22

Shipping-dispatch toupper controls pass in full Clang framework CTest 84/84: t->T, T/digit/0xE0 unchanged, wrong BIOS table unclaimed.
