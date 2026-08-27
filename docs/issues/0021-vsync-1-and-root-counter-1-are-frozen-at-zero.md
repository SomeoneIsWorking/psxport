---
id: 21
title: VSync(1) and root counter 1 are frozen at zero
status: resolved
symptom: Guests polling Sony libetc VSync(1) for an intra-field scanline never leave the loop even while VBlank advances.
tags: timing,vsync,hsync,root-counter,guest-time
created: 2026-08-22
updated: 2026-08-27
---

Root cause at resolution time: the framework's `Timing::vsync(mode=1)` returned a dummy zero and
`Core::io_read` left root counter 1 (`0x1F801110`) unmapped. The root-counter half remains resolved:
shared `EmulatedTime` supplies the deterministic NTSC/PAL HSync count and `test_hsync_counter` covers
the shipping MMIO path, line-248 progression, field geometry, and invalid cadence.

The VSync half was superseded on 2026-08-27 by issue 31's stronger ownership contract. There is no
successful direct Timing seam now: every product's measured libetc VSync entry aborts for every mode,
and only the host-native frame driver may advance fields.
