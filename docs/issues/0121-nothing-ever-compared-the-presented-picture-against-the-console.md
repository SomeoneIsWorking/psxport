---
id: 121
title: Nothing ever compared the presented picture against the console, and the VRAM tool reported a blank-vs-blank zero as a pass
status: fixed
symptom: a user reported the save menu "not being like the oracle" while every oracle report in the repository said 0 divergences; the only pixel tool returned "differing 0/524288 (0.00%)" on the native path, where both buffers are blank
tags: oracle, diagnostics, rendering, attribution
created: 2026-09-19
updated: 2026-09-19
---

## What happened

A user looked at Tomba! 2's save menu and said it did not look like the real game. Every oracle
report in the repository said 0 divergences. Both were correct, and the gap between them is this
issue.

`compare.py` compares **guest RAM** at title-owned checkpoints. A menu drawn in the wrong place,
with the wrong colours, or missing a panel writes exactly the same guest state as a correct one, so
no number it produces can ever move. Every "0 divergences" recorded anywhere is a statement about
the simulation, never about the picture.

The one pixel-level instrument, a consumer's `vram_oracle.py`, compares the guest GPU command feed
rasterised into emulated VRAM. On a **native** render path the picture is not produced that way at
all — the native producers draw through the host renderer — so both VRAM dumps come back blank and
it printed a zero difference as a pass. Measured on `save-card-pages` f1690/1740/1800/1870,
2026-09-19: `ours drew 0 prim(s), beetle dispatched 0`, both display rects 0.0% non-black, and
`whole VRAM 1024x512: differing 0/524288 (0.00%)`.

That is the uniform-output tell the diagnostics rule exists for, and it had been passing for as long
as the native path has existed.

## What was done

`tools/oracle/picture.py` compares the frame each core **presents**, captured through each core's own
present path: the product through the REPL's `shot` (`gpu_native_shot`, so it follows the active
render path and the wide presentation region), the reference through its libretro framebuffer. Both
cores are driven to the same state by the same title checkpoints `compare.py` uses, so the two
pictures are of the same moment in the same route. `CoreSession` gained `capture(destination)`; both
session classes implement it against what they already had.

The reference publishes a padded 350x240 scanline where the product presents 320x240. The picture
tool asks the core for `crop_overscan=static`, its own HORIZONTAL-only crop, rather than copying the
core's 350->320 offset table into Python as a second source of truth. The RAM comparison does not
pass it and keeps its pinned contract unchanged; the option is video-only.

It refuses rather than returning a number when either picture is blank or uniform, when the two
dimensions differ, or when a capture was not written. The first of those is the exact failure it
replaces. `vram_oracle.py` in the consumer now refuses the native path by name and refuses any
blank-vs-blank comparison wherever it arises.

The comparator was shown both answers before any zero from it was believed: 60 frames of the title's
own route change 76,781 pixels, and a picture compared with itself differs in 0.

## The first result was half my own misalignment, and the fix is in the product's hands

The native render path deliberately presents MORE rows than the console scanned out — the port
declares the real count in `GameConfig::guestDisplayHeight` and the framework keeps drawing the rest
(USER 2026-08-19: "PC is fine, oracle isn't"). Comparing the raw frames therefore measures a
difference nobody considers a defect. On Tomba! 2 it alone accounted for half the reported
difference and produced an apparent 7-pixel vertical offset that does not exist.

The alignment now comes from what each core reports about ITSELF, never from fitting the two
pictures to each other. The product states `guest_scan=<rows>` on its shot reply, from the GPU
state; the reference is asked for its own active area (`crop_overscan=smart`) and publishes 320x224,
agreeing with the title's declared 224 independently. A fitted offset would have been the tool
finding the answer that made its own number look best.

The policy behind those two counts is now one pure function, `runtime/psx/display_scanout.h`, split
out of `gpu_native.cpp` with its resolution in `gpu_native_scanout.cpp` rather than raising that
file's line cap (ratcheted 4051 -> 4030). `tests/test_display_scanout.cpp` asserts the presented and
scanned counts separately in every combination that produced a wrong answer once, 21 checks; it was
falsified by making a native path present its declared count, which fails it.

## What it found immediately

Tomba! 2, free-roam gameplay, 400 frames into the title's route, both rects 320x240, guest RAM
byte-identical at 405 checkpoints in the same route, both sides 320x224 after the alignment above:
**33,786 of 71,680 pixels differ (47.13%)**, mean absolute difference 24.7/765 and **median 0** —
most of the picture matches exactly, and no whole-pixel shift improves it.

One defect is measured rather than eyeballed: the product's sky shows brightness discontinuities at
columns 16, 80, 96, 112, 128, 144, 160 — exact multiples of 16 — where the reference's column spikes
over the same band sit only on content edges. The remaining 4,528 pixels differing by more than 96,
concentrated in the ground, are not yet attributed. See Tomba! 2 issue 0012.

## What this does not establish

The two cores are different renderers, so a small evenly-spread difference is dithering and
sub-pixel sampling and is expected. The tool reports whether the difference is concentrated or
spread, because that is what separates a defect with a place to look from a renderer difference. It
gives the number and the pictures; looking at them is the rest of the check.
