---
id: 21
title: VSync(1) and root counter 1 are frozen at zero
status: resolved
symptom: Guests polling Sony libetc VSync(1) for an intra-field scanline never leave the loop even while VBlank advances.
tags: timing,vsync,hsync,root-counter,guest-time
created: 2026-08-22
updated: 2026-08-22
---

Root cause: the framework's Timing::vsync(mode=1) explicitly returned a dummy zero and Core::io_read left root counter 1 (0x1F801110) unmapped. Sony libetc VSync samples that HBlank-clocked 16-bit counter and subtracts a saved baseline, so intact guest code could never observe an intra-field phase. The shared EmulatedTime owner now derives one deterministic NTSC/PAL HSync count used by both MMIO and the direct Timing seam. test_hsync_counter exercises the shipping MMIO path, direct query/reset behavior, line-248 progression, NTSC/PAL field geometry, and invalid cadence. No consumer address, scanline threshold, or forced transition is encoded.
