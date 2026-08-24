---
id: I021
kind: instrument
status: trusted
created: 2026-08-25
---

## Instrument

`test_render_queue_consumed_flush` shipping queue/presenter lifecycle gate

## Validated by

The unchanged test produced the opposite answer against the old shipping implementation: the second
empty flush re-captured the retained item and failed with presenter count 2 instead of 1. Against the
fix it observes both no-change and change answers: an empty consumed flush preserves presenter and HUD
ledger counts, while the next real push resets lazily and increments both.

## Known failure modes

The hermetic test drives the ordinary FramePresenter capture path, not a live GPU or the SBS diff-mode
emit path. The lifecycle guard is before every flush side effect, and the bounded Tomba! 2 run confirms
the real multi-DrawOTag capture symptom, but renderer emission still needs its own consumer gate if a
future change moves any side effect above the guard.
