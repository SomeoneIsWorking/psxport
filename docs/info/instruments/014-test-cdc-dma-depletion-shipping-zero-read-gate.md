---
id: I014
kind: instrument
status: trusted
created: 2026-08-21
tags: cdc,dma3,fifo,hermetic
---

## Instrument

`tests/test_cdc_dma_depletion.cpp` — shipping CDC/Core DMA3 zero-read gate.

## Validated

The test initializes all 504 destination words to the opposite answer `0x0E0E0E0E`, supplies exactly
70 FIFO words, and leaves a following sector marked ready. The production path must overwrite the
suffix with 434 zeros without consuming the future sector. A second shipping DMA3 case proves the
same write reaches guest RAM, and a third deasserts BFRD and requires 504 controller zeros.

The old short-write rule was restored temporarily against the same test binary: all three cases
failed on stale `0x0E0E0E0E` (0/3, 3 failures, 144 checks reached). Restoring the candidate produced
3/3 and 1,518 checks. The instrument therefore shows both answers instead of treating unchanged RAM
as a clean result.

## Limits

The hermetic gate proves the controller-read and RAM-commit contract; it does not prove guest call
ordering, sector timing, or movie playback. Those require the real Spider-Man consumer and the
existing cross-game CDC gates.
