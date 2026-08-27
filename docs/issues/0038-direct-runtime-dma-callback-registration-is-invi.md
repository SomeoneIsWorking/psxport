---
id: 38
title: Direct-runtime DMA callback registration is invisible to interrupt delivery
status: resolved
symptom: A direct GameRuntime receives DMA3 sectors but Hle::irqPoll consumes the owed completion with callback 0, leaving the title stream in-flight guard set.
tags: runtime,dma,callback,irq,gameconfig,tomba1
created: 2026-08-27
updated: 2026-08-27
---

Direct runtimes have Core::cfg == nullptr. The existing delivery path resolves Sony DMACallback registrations only through GameConfig::dmaCallbackTable, so a title-native registration has no framework-owned state and the owed completion is acknowledged and consumed without dispatch. Tomba! 1 measured channel 3 callback 0x80066D80 relies on that dispatch to clear its stream guard.

### Resolution (2026-08-27)
Added DmaCallbackRegistry as per-Game typed host state for direct runtimes. Hle::irqPoll now reads the legacy guest table when cfg exists or the direct registry when it does not, without changing completion order, DICR gating/acknowledge, or R3000 save/restore. Shipping test_direct_dma_callbacks passes 4/4 (19 checks); full Clang framework suite passes 115/115 including format/tidy.
