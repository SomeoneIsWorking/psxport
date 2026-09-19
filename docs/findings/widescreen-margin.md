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

## The rect is in VRAM halfwords, not display columns (2026-09-19)

The 2026-08-13 verification above ran on a 15bpp display, where one display pixel IS one VRAM
halfword and the two spaces coincide. At 24bpp a pixel is RGB888 packed across 1.5 halfwords
(`present.frag` reads display column x at byte `disp.x*2 + x*3`), so a rect built from display widths
alone lands at two thirds of its intended position.

Measured on Spyro's Universal boot logo — an upload-only guest-VRAM picture, 512×240 24bpp, widened
to 684 (spyro issue 0118). VRAM was byte-identical between the 4:3 and 16:9 legs (0 of 524288 words
differ) and the guest programmed GP1(08)=08000012 24-BIT identically in both, so this was purely a
presentation defect. The plan returned halfwords [512,684), which the 24bpp present samples as
display columns [341,456): a black band straight through the picture, while the real margin was never
covered. The band was measured at columns 342..454 against 341.3 and 456.0 predicted, and outside it
the picture matched a correct 24bpp read to a mean |diff| of 1.29.

`plan_wide_margin` now takes the display depth and converts display columns to halfwords, clamping to
VRAM's 1024-halfword width (reached by this real case: 684 columns need halfword 1026). Seeding the
old identity conversion back in fails 4 of the 7 tests in `tests/test_wide_margin_plan.cpp`, which is
what makes them evidence rather than decoration. The drawing itself moved out of the
4,253-line `gpu_vk.cpp` into `runtime/psx/gpu_vk_wide_margin.cpp`; the file's legacy cap ratcheted to
4,238.
