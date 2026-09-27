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

## The projection centre and the left margin are the framework's numbers; ask for them by name (2026-09-27)

Two quantities decide where a widened picture begins, and both are computed by the framework:

- the **horizontal centre** — `gpu_vk_wide_engine_ofx`, the render width for the selected aspect over two;
- the **left margin** — `gpu_vk_wide_left_margin`, `(wide_w - native_w) / 2`, the columns the widening adds.

Both were being recomputed at call sites, and the failure mode is identical in both cases: the copy is
equal to the original **today** and diverges the moment the original's definition moves, with nothing to
signal it. Two instances found by grepping for the recomputation rather than for the concept:

**1. Crash Bash, three sites, two spellings.** The model producer wrote
`gpu_vk_wide_engine_ofx(&core) - gpu_vk_native_w(&core) / 2` and the sprite producer wrote
`(gpu_vk_wide_engine_w(&core) - gpu_vk_native_w(&core)) / 2` twice. These differ whenever the wide width
is even and the native width is odd, by exactly one column — measured, and pinned by
`tests/test_wide_left_margin.cpp` over a parity sweep. They are equal now because
`video_wide_native_w` ends with `w &= ~1` and every PSX display mode has an even width, neither of which
is a property of the margin. All three now call `gpu_vk_wide_left_margin`.

**2. Spyro 1, one site the title's own consolidation missed — NOT FIXED, and why.** Spyro already hit this
class of bug and fixed it: issue 0124 had the paired actor drawn about the 4:3 centre while the world around
it was drawn about the widened one. The fix introduced `spyro::wide_screen_space::horizontalCenter(Core*)`,
which returns `gpu_vk_wide_engine_ofx`, with a comment recording that it "used to spell the wide half-width
here and fall back to `projParams.geomOfx()`, which is a second implementation of the same number".

**`game/render/fx_field_tracers.cpp` still spells it itself**, at the line that overrides the projection
centre when the wide engine is on:

```cpp
if (gpu_vk_wide_engine(core)) {
  projection.ofx = (gpu_vk_wide_engine_w(core) / 2) << 16;   // should be horizontalCenter(core)
}
```

The value is correct today, because `gpu_vk_wide_engine_ofx` is *literally* `wide_native_w / 2` — so this
is the same latent coupling as Crash Bash's, one file narrower.

**It is deliberately NOT fixed in this commit.** `spyro` is being worked by another agent right now, and the
workspace rule is explicit ownership so that no two agents edit the same tree. The change is one line and is
mechanically equivalent today; it is recorded here so it is a known item rather than a fresh discovery, and
it should be landed by whoever next owns that repository:

    projection.ofx = spyro::wide_screen_space::horizontalCenter(core) << 16;

**THE CENSUS IS CLOSED, with its denominator.** Both accessors were grepped across every in-scope title's
`game/` and `titles/` for the recomputation EXPRESSION — `wide_engine_w(...) / 2` and
`wide_engine_ofx(...) - ... / 2` — and every `ofx =` assignment site was read to see whether it asks or
computes. Result: **two instances in the whole workspace, the two above.**

| title | `ofx =` sites | verdict |
|---|---|---|
| Spyro 1 | many | one recomputation, `fx_field_tracers.cpp` — the finding above |
| Crash Bash | 3 | three recomputations, all fixed to `gpu_vk_wide_left_margin` |
| Tomba! 1 / 2 | 3 | **asks.** `projection.cpp` takes `ofx` as a parameter and `native_terrain.cpp` reads it from `fps60(*c->game).sceneCam(...)`, so one owner supplies `ofx`/`ofy`/`H` together and nothing re-derives them |
| Mega Man X4 | 0 | no `ofx =` site; widening goes through its own widescreen profile |
| Tekken 3 | 0 | same |
| Spider-Man 1 | 0 | same |
| Crash Team Racing | 0 | no widescreen owner at all, so nothing to re-derive |

**The generalisable rule, which is why this is written here rather than as a Crash Bash commit note:** when
the framework computes a number, a title should call the accessor, not re-derive it. A grep for the CONCEPT
finds almost nothing — the copies spell the arithmetic, not the name — so the census has to be for the
expression, which is how both instances above were found.
