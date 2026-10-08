# 0133 — the rasterizer's blending constants were unnamed, and two of them were load-bearing

**State:** recorded as a finding, and the NAMING half is already done. The behavioural question below
is not.

Found 2026-09-28 while splitting the software rasterizer out of `gpu_native.cpp` into
`runtime/psx/gpu/gpu_native_raster.cpp`.

## What was there

The semi-transparency block was four bare literals in a `switch` with a `default:` that stood for the
fourth mode:

```c
static inline int sat5(int v) { return v < 0 ? 0 : v > 31 ? 31 : v; }
static inline uint16_t blend555(uint16_t bg, int fr, int fg, int fb, int mode) {
  ...
  case 0: rr = (br + fr) >> 1; ... break;   // which mode is this?
  case 1: rr = sat5(br + fr); ...  break;
  case 2: rr = sat5(br - fr); ...  break;
  default: rr = sat5(br + (fr >> 2)); ...    // and this one?
```

The four are `B/2+F/2`, `B+F`, `B-F`, `B+F/4`. A reader of that block had to know the PSX blend table
to know whether the arms were in the right order, and the `default:` gave no way to tell whether the
fourth arm was mode 3 or a catch-all for the three values the field cannot produce.

They are now `kBlendAverage`, `kBlendAdditive`, `kBlendSubtractive`, `kBlendAdditiveQuarter`, with the
`default:` kept and documented as unreachable — the blend field is two bits, so no fifth value exists.

## The part that is still unknown

The saturation clamp is `sat5`, and it clamps EVERY mode including `kBlendAverage`. That is almost
certainly right — the average of two in-range 5-bit values is in range, so the clamp cannot fire there —
but it means `kBlendAverage`'s arms read as though they need saturating when they cannot.

This is recorded rather than changed because the two are not separable: proving the clamp is
unreachable for one mode is a change to the arms, and the arms are the pixels. If a port ever wants to
trust the average path unclamped, that is the measurement, and it should be taken against a console
capture rather than reasoned about.

## Where

- `runtime/psx/gpu/gpu_native_raster.cpp` — the blend constants, `sat5`, `blend555`.
- `runtime/psx/gpu/gpu_native_internal.h` — `s_tp_blend`, the texpage field that selects the mode.
