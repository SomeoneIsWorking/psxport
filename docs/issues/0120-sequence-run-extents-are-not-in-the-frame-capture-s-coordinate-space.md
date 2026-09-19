---
id: 120
title: fps60seq run extents are not in the frame capture's coordinate space, so tile attribution never worked
status: open
symptom: a tile inside a specific run is only 1.12x (Spyro 1) / 1.25x (Tomba! 2) as likely to have moved as a tile inside no run, which is chance; every owner table built from `fps60_check.py --seq` is therefore unsound
tags: fps60, diagnostics, attribution, coordinates, retraction
created: 2026-09-19
updated: 2026-09-19
---

## What happens

`fps60_check.py --seq` credits each moving tile to the smallest `fps60seq` run covering it. That
requires run extents and captured frame pixels to describe the same place. They do not.

A run's extent is grown from `RqItem` screen vertices in `fps60_sequence_runs.cpp`, which are in the
renderer's own screen space. The frame capture is `gpu_shot`'s VRAM display region. Nothing relates
the two, and the widths do not match:

| title | captured frame | widest run |
|---|---|---|
| Tomba! 2 | 320x240 @ 0,0 | `x=[-320..641)`, 961 px |
| Spyro 1 | 684x240 @ 0,0 and @ 0,240 | `x=[-1024..1024)`, 2048 px |

## How it stayed hidden

Coverage was the only check, and coverage cannot fail. Every frame contains a screen-sized fill, and
a misaligned fill still covers every tile, so it wins whatever nothing else claims and the table
reports 100% attributed. The measured 57.5% -> 100.0% "fix" (psxport 0b638ad0, offsetting the tile
by the VRAM display origin) changed which misaligned run won and nothing else: with the lift check
below, the display-origin and raw mappings score identically on Spyro, 1.12x both.

## The discriminator

Whether being inside a run PREDICTS motion. Screen-sized fills are excluded, because they cover
everything under any mapping and so can never disagree. Under a correct mapping a tile inside an
actor's run should be far more likely to have moved than a tile inside no run at all; under a wrong
one the ratio collapses toward 1.

Measured 2026-09-19: Spyro 1 1.12x over 3,222 fences, Tomba! 2 1.25x over 709 fences. Both are
chance. `fps60_check.py` now refuses to print an owner table below 2.0x, and refuses if another
candidate offset scores better.

## What is NOT affected

Everything the tool derives from the images alone: the STATIC/BETWEEN/STALE/AHEAD classification, the
per-tile best-shift, and the finding that the forward-snapping population does not respond to the
interpolation factor. None of those consult a run.

## What would fix it

The framework has to emit run extents in the same space the capture is in, or emit the transform
between them. The renderer already knows both. A per-fence line naming the draw-area origin, the
internal-resolution scale and the widescreen widening applied to `RqItem` screen vertices would let
the analysis map one to the other instead of guessing, and would be checkable by the same lift
measure. Until then attribution stays refused, which is the honest answer rather than a table.
