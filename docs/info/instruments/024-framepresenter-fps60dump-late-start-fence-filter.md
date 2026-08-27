---
id: I024
kind: instrument
status: trusted
created: 2026-08-27
---

## Instrument

FramePresenter fps60dump late-start fence filter

## Validated by

Positive: Clang test_frame_dump_window accepts fence 2075 at first_fence 2075 and later fences; live Spyro run with PSXPORT_FPS60_DUMP_FROM=2074 wrote 186 paired interp/real PNGs beginning exactly f002074 after a 2,074-fence boot/route. Negative: the same hermetic test rejects fence 2074 at first_fence 2075, and the live dump wrote no earlier new files, so the start filter demonstrably produces both answers without consuming the 600-file cap during boot.

## Known failure modes

(none recorded yet)
