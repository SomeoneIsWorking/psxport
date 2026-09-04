---
id: C039
kind: claim
status: holds
created: 2026-08-27
tags: dma,direct-runtime,interrupts
depends: runtime/psx/dma_callbacks.cpp#DmaCallbackRegistry::exchange, runtime/psx/hle.cpp#Hle::irqPoll
---

## Claim

Direct runtimes deliver a registered DMA completion through per-Game typed callback state, while legacy runtimes retain their guest callback table.

## Evidence

test_direct_dma_callbacks drove the shipping DMA3 CHCR completion and Hle::irqPoll path: direct registered callback dispatched once with R3000 restored, a second poll did not redeliver, direct unregistered dispatched zero and cleared pending work, and a conflicting direct entry did not override the legacy guest table. 4/4 tests, 19 checks; full Clang CTest 115/115 including cpp_style.

## What would falsify it

Any direct registered completion dispatches zero or more than once, any unregistered completion dispatches, legacy callback delivery reads direct state, interrupted R3000 registers change, or the 115-test denominator is not reached.
