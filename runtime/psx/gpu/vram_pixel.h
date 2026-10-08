// vram_pixel.h — the PSX VRAM pixel format, in named pieces.
//
// A VRAM cell is ONE 16-bit halfword holding a 5-bit-each RGB colour plus a mask bit. Every read and
// write of guest and native pixels goes through that layout: bits 0-4 are red, 5-9 green, 10-14 blue.
//
// A format only: nothing here samples, blends or filters.
#ifndef PSXPORT_VRAM_PIXEL_H
#define PSXPORT_VRAM_PIXEL_H

#include <cstdint>

namespace psx::gpu {

// A VRAM halfword, split into its channels.
struct VramPixel {
  int red = 0;   // bits 0-4
  int green = 0; // bits 5-9
  int blue = 0;  // bits 10-14
  // bit 15: the per-pixel draw mask GP0(E6) sets and tests.
  bool mask = false;
};

// The 5-bit channels, WITHOUT scaling them up to 8 bits. Sampling wants these; blending works in this
// space because the PSX blend formulas are defined on 5-bit values.
constexpr VramPixel decodeVramPixel(std::uint16_t halfWord) {
  return VramPixel{static_cast<int>(halfWord & 0x1Fu),
                   static_cast<int>((halfWord >> 5) & 0x1Fu),
                   static_cast<int>((halfWord >> 10) & 0x1Fu),
                   (halfWord & 0x8000u) != 0};
}

// The same channels, SCALED to 8 bits by replication (`v << 3 | v >> 2`) — the standard 5-to-8 bit
// expansion, which is what makes a 5-bit ramp fill the 0..255 range without a visible gap. `<< 3` alone
// would top out at 248 and darken every channel by a constant; the rasterizer's output is compared
// against a console capture, so the difference is a measurable brightness offset rather than a rounding
// difference. The caller passes the value it already has, which for a sampled texel is the 5-bit form.
constexpr int scale5to8(int fiveBit) {
  return (fiveBit << 3) | (fiveBit >> 2);
}

// Pack 8-bit channels into a VRAM halfword, dropping each channel's low 3 bits. This is the ONE place
// the truncation happens: a fill's command colour, a blended result and a modulated texel all go
// through it, so they truncate the same way and a blend of two fills matches a fill of the blend.
constexpr std::uint16_t toVram555(int red, int green, int blue) {
  return static_cast<std::uint16_t>(((red >> 3) & 0x1Fu) | (((green >> 3) & 0x1Fu) << 5) |
                                    (((blue >> 3) & 0x1Fu) << 10));
}

} // namespace psx::gpu

#endif // PSXPORT_VRAM_PIXEL_H
