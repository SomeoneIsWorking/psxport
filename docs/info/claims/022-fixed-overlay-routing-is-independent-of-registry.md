---
id: C022
kind: claim
status: holds
created: 2026-08-22
tags: overlay,routing,signature,crashbash
depends: runtime/recomp/overlay_router.cpp#overlay_resolve_fixed, tests/test_overlay_reloc.cpp
reconfirmed: 2026-08-22
verified_at: 2026-08-22 18:00:29
---

## Claim

Fixed-overlay routing is independent of registry order and names: among signature-derived resident
identities it selects the smallest containing image and refuses equal-specificity ambiguity.

## Evidence

test_overlay_reloc passes 8/8 cases and 55 checks, including Crash Bash's measured BOOT/MENU signatures and ranges at targets 0x800B5244 and 0x800B9524, reversed/renamed registry, missing MENU signature, and equal-width ambiguity; Crash Bash real consumer advances past the former MENU dispatch miss.

## What would falsify it

Dispatch, entry validation, and resident-name diagnostics stop sharing `overlay_resolve_fixed`; a
declaration/name reordering changes the owner; an enclosing image wins while a narrower nested
identity is resident; or equal-width matches are guessed.

## Re-confirmed 2026-08-22

test_overlay_reloc 8/8 cases and 55 checks in full 84/84 CTest; real Crash Bash consumer advances past 0x800B5244; independent of names/order and refuses equal-specificity ambiguity.
