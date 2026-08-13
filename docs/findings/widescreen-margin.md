# Widescreen extension must not expose guest VRAM storage

Verified 2026-08-13 with an A/B run of Spyro's native title-screen producer at present 2100,
16:9, 960×720 headless VK output.

## Root cause

The guest owns only its native display rectangle. Columns made visible by a wider host projection
are still ordinary PSX VRAM, and games use them for texture and CLUT storage. The persistent VK
composite retained those atlas pixels in the extension when no authored primitive covered them.
Clearing guest VRAM would corrupt the game and clearing the native framebuffer would break ports
whose backdrop is uploaded pixels.

## Fix and discriminator

`GpuVkState::present` adds an opaque renderer-only base quad for exactly
`[sx + native_width, sx + wide_width) × [sy, sy + height)`, at the back of the 2D-background band.
It never writes guest VRAM, never covers the native display rectangle, and normal background, world,
and HUD producers render over it.

The control build (`bbe16a74`) showed the atlas across the right 240 pixels of the 960×720 sink:
5,543 colors and mean normalized intensity 0.13997. With only this renderer change, the same crop was
one color, black, with mean 0. The native producer census still contained its one expected row and
zero unscoped-native primitives. Captures and logs are local run evidence under
`spyro/scratch/screenshots/wide-ab/` and `spyro/scratch/logs/wide-margin-{control,patched}.log`.

`tests/test_wide_margin_plan.cpp` pins both the negative (4:3/invalid inputs draw nothing) and
positive geometry of the renderer-only extension.
