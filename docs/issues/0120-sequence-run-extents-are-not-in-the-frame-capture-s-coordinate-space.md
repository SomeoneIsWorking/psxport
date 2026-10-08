---
id: 120
title: fps60seq described the captured queue, not the stream that was drawn, so tile attribution never worked
status: open
symptom: a tile inside a specific run was no more likely to have moved than a tile inside none, and a deliberately wrong mapping scored better than the real one; every owner table built from `fps60_check.py --seq` is unsound
tags: fps60, diagnostics, attribution, retraction
created: 2026-09-19
updated: 2026-09-19
---

## What happens

`fps60_check.py --seq` credits each moving tile to the smallest `fps60seq` run covering it. That
requires the runs to describe the prims that painted those pixels. They did not.

`Fps60::presentPass` merges the reconstructed sink over the captured queue: **every item the temporal
scene source owns is replaced before anything is rasterised**. `dumpSequenceRuns` grouped
`frame.items` — the captured queue — so it described prims that were thrown away, and omitted the
reconstruction that was actually drawn. Measured on Tomba! 2's `seaside-sweep` replay: the captured
queue peaked at 672 items, the emitted stream at 1,081.

## The coordinate theory was wrong

This issue first blamed the coordinate space, on the grounds that the widest run (961 px on a 320 px
frame) could not fit the capture. That run is a clip guard at `x=[-320..641)` = 3W+1, not a
screen-width fill, and the mapping was never the defect. `RqItem` vertices are VRAM pixel coordinates
(`tritex.vert`: `i_pos` is "VRAM pixel coords (post draw-offset)"), `gpu_vk_shot` writes the display
region at 1:1, and subtracting the display origin — which the analysis already did — is the whole
conversion.

## How it stayed hidden

Two instrument defects, both of which make a wrong answer look like a measurement:

- **Coverage cannot fail.** Every frame carries a screen-sized fill, and a misaligned fill still
  covers every tile. The measured 57.5% -> 100.0% "fix" (psxport `0b638ad0`) changed which run won
  and nothing else.
- **The tile statistic saturates.** Measured on the same replay: 72% of pixels are *identical*
  between consecutive real frames, but 88.7% of 16x16 tiles contain at least one changed pixel. At
  tile resolution almost everything is "moved", so no mapping can score above chance. Every lift
  number in the first version of this issue was computed through that saturation.

## The discriminator

Whether being inside a run PREDICTS motion, measured **per pixel**, with screen-sized runs excluded
(they cover everything under any mapping) and against deliberately wrong control mappings.

| runs grouped from | display (true) | shift +40,+40 | shift +120,+80 |
|---|---|---|---|
| captured queue (before) | 1.14x | **1.46x** | 1.06x |
| emitted stream (after) | **1.32x** | 1.20x | 0.93x |

Before the fix a deliberately wrong mapping beat the real one — the tell that the runs were not on
the pixels. After it the true mapping ranks first at every box-size threshold. That is the evidence
the cause named above is the right one.

## Still refused, and why

1.32x is the right sign but it is not enough to license a per-producer table. Tomba!2's `seaside-sweep`
pans the camera, so 36-41% of pixels change everywhere and "inside a run" has little room to
discriminate. `fps60_check.py` keeps its refusal, including the negative-control check, so the
retracted tables cannot silently return.

## What would lift it

A scene with a still camera, where motion is confined to actors, gives the measure the contrast it
needs. Failing that, credit a tile only when a run's own prims changed between the two real frames,
rather than when the tile changed — that compares like with like and is immune to a panning
background.

## What is NOT affected

Everything the tool derives from the images alone: the STATIC/BETWEEN/STALE/AHEAD classification, the
per-tile best-shift, and the finding that the forward-snapping population does not respond to the
interpolation factor. None of those consult a run.
