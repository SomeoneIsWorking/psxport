---
id: 44
title: Untextured Vulkan coverage uses half-pixel sample phase
status: resolved
symptom: A PSX G3 triangle whose vertex exactly owns an integer-coordinate pixel is missed by the ordinary Vulkan path
tags: gpu,renderer,raster,coverage,sample-phase,crashbash
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The ordinary untextured vertex shader mapped integer PSX coordinates directly to Vulkan clip space.
PSX polygon coverage evaluates integer pixel coordinates, while Vulkan evaluates fragments at
half-integer pixel centers. The missing half-pixel transform moved narrow triangle edges across the
coverage test even though packet geometry, colors, ordering, and material state were exact.

Crash Bash packet `0x800C89EC` supplies the discriminator: object `0x801E18B0`, frame `0x2001`, face
27, material `0x002C`, with SXY `(90,127)/(85,137)/(90,138)`. Retail includes the second vertex at
`(85,137)` and writes white; the unshifted Vulkan path exposes the darker preceding face.

## Resolution

`tri.vert` shifts untextured positions by `(+0.5,+0.5)` native pixels before clip-space conversion,
aligning PSX integer-coordinate coverage with Vulkan pixel centers. The shipping 2x GPU discriminator
changes from `1C04` to the exact box-resolved `350B`. Its expected resolve includes the older gradient
under partial subpixel coverage and therefore exercises the production render path rather than a
separate 1x approximation.

The same transform was tested on the textured vertex shader and rejected: Crash Bash's exact
frame-300 diff worsened from 5,546 to 15,053 pixels. Textured UV/raster phase remains under its
separate 28-case discriminator.
