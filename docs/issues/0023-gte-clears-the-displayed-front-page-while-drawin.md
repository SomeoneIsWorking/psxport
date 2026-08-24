---
id: 23
title: Gte clears the displayed front page while drawing the back page
status: resolved
symptom: RenderPath::Gte presents an all-black frame although the guest queue contains valid textured primitives and the software PSX path renders the same stream.
tags: runtime,renderer,gte,framebuffer,double-buffer,megamanx4
created: 2026-08-25
updated: 2026-08-25
---

## Root cause

The Gte path rasterizes guest GP0 primitives into a persistent 1024x512 PC composite. `render_geom` nevertheless cleared that entire composite on every rebuilt present. X4 alternates PSX framebuffer pages: immediately before present 180 it draws y=0..256 while displaying sy=240, then draws y=240..496 while displaying sy=0. Clearing the whole target erased the completed front page just before it was scanned.

## Fix

Preserve the already-initialized PC composite for stable Gte ownership. Retain a clear for the first ownership build, keep Native per-frame clearing, and leave Psx/guest-VRAM backdrop ownership unchanged.

## Evidence

Before: Gte present 180 was 0/691200 non-black. The identical guest stream via RenderPath::Psx was 53631/691200 non-black, and offline texture decoding found 5959 non-black texels across 37 sprites. After: the corrected Gte present 180 is 29921/691200 non-black (4.3288%) and visibly renders the Mega Man X logo. `test_gte_composite_persistence` passes 3/3 cases and 5 checks. The real X4 product linked against the candidate framework produced the capture.
