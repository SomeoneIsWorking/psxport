---
id: 16
title: First-match fixed-overlay routing hides a nested resident module
status: resolved
symptom: Crash Bash loads MENU.BIN at 0x800B32B4 inside still-resident BOOT.BIN; indirect dispatch to 0x800B5244 routes to BOOT and misses even though MENU signature is live.
tags: overlay,routing,nested-range,signature,crashbash
created: 2026-08-22
updated: 2026-08-22
---

Root cause: the fixed overlay loop treated registry declaration order as ownership. BOOT's broad [0x80078C90,0x800D7490) range and unchanged base signature matched before nested MENU [0x800B32B4,0x800BB2B4), so the router never considered the more specific live image. Evidence: crashbash/scratch/logs/crashbash-nested-router-red.log; RAM[0x800B9524]=0x800B5244 and both 32-byte signatures match.

### Resolution (2026-08-22)
Added one shared fixed-overlay resolver used by dispatch, entry validation, and resident-name
diagnostics. It considers signature-derived resident identities across every containing fixed range,
selects the smallest resident image independent of table order/name, and refuses equal-width
ambiguity. `test_overlay_reloc` carries the measured BOOT/MENU ranges, targets, signatures,
reversed/renamed registry, absent-signature fallback, and ambiguity negative.
