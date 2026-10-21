// vram_pixel.h — the PSX VRAM pixel format, texpage and CLUT words, and the texel sampler over them.
#pragma once

#include "vram_rect.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace psx::gpu {

// A VRAM halfword split into its 5-bit channels and mask bit.
struct VramPixel {
  int red = 0;       // bits 0-4
  int green = 0;     // bits 5-9
  int blue = 0;      // bits 10-14
  bool mask = false; // bit 15, set and tested by GP0(E6)
};

// Unscaled: sampling and the PSX blend formulas work on 5-bit channels.
constexpr VramPixel decodeVramPixel(std::uint16_t halfWord) {
  return VramPixel{static_cast<int>(halfWord & 0x1Fu),
                   static_cast<int>((halfWord >> 5) & 0x1Fu),
                   static_cast<int>((halfWord >> 10) & 0x1Fu),
                   (halfWord & 0x8000u) != 0};
}

// 5-to-8-bit expansion by bit replication, so 31 maps to 255.
constexpr int scale5to8(int fiveBit) {
  return (fiveBit << 3) | (fiveBit >> 2);
}

// 8-bit channels into a VRAM halfword; the one place the low 3 bits are dropped.
constexpr std::uint16_t toVram555(int red, int green, int blue) {
  return static_cast<std::uint16_t>(((red >> 3) & 0x1Fu) | (((green >> 3) & 0x1Fu) << 5) |
                                    (((blue >> 3) & 0x1Fu) << 10));
}

// Texture colour depth as a texpage word encodes it; 3 samples as 15-bit.
enum class TexDepth : int { Indexed4 = 0, Indexed8 = 1, Direct15 = 2, Reserved = 3 };

// A texpage word (GP0 E1 low bits, or a textured polygon's second UV halfword).
struct TexPage {
  int x = 0;
  int y = 0;
  int blend = 0;
  TexDepth depth = TexDepth::Indexed4;
};

constexpr TexPage decodeTexPage(std::uint16_t attribute) {
  return TexPage{static_cast<int>(attribute & 0xFu) * 64,
                 static_cast<int>((attribute >> 4) & 1u) * 256,
                 static_cast<int>((attribute >> 5) & 3u),
                 static_cast<TexDepth>((attribute >> 7) & 3u)};
}

// A CLUT word: the palette's first entry in VRAM.
constexpr VramPoint decodeClut(std::uint16_t clut) {
  return VramPoint{static_cast<int>(clut & 0x3Fu) * 16, static_cast<int>((clut >> 6) & 0x1FFu)};
}

// The palette entries an indexed texture mode samples: 16 at 4bpp, 256 at 8bpp.
constexpr std::size_t clutEntryCount(TexDepth depth) {
  return depth == TexDepth::Indexed8 ? 256u : 16u;
}

// One VRAM halfword, both axes wrapped as the hardware wraps them.
constexpr std::uint16_t vramHalfWord(std::span<const std::uint16_t> vram, VramPoint at) {
  const auto column = static_cast<std::size_t>(at.x & (kVramWidth - 1));
  const auto row = static_cast<std::size_t>(at.y & (kVramHeight - 1));
  return vram[(row * kVramWidth) + column];
}

// The texel at (u, v) of a page as the GPU fetches it, without the texture window; 0 is transparent.
// `halfWordAt(VramPoint)` reads the VRAM halfword there.
template <class HalfWordAt>
constexpr std::uint16_t fetchTexel(const HalfWordAt &halfWordAt, TexPage page, VramPoint clut, int u, int v) {
  if (page.depth == TexDepth::Direct15 || page.depth == TexDepth::Reserved) {
    return halfWordAt(VramPoint{page.x + u, page.y + v});
  }
  const int shift = 2 - static_cast<int>(page.depth); // log2 of the texels in one halfword
  const int bits = 16 >> shift;
  const std::uint16_t word = halfWordAt(VramPoint{page.x + (u >> shift), page.y + v});
  const int index = (word >> ((u & ((1 << shift) - 1)) * bits)) & ((1 << bits) - 1);
  return halfWordAt(VramPoint{clut.x + index, clut.y});
}

// fetchTexel over a whole VRAM image.
constexpr std::uint16_t sampleTexel(std::span<const std::uint16_t> vram, TexPage page, VramPoint clut, int u, int v) {
  return fetchTexel(
      [vram](VramPoint at) {
        return vramHalfWord(vram, at);
      },
      page,
      clut,
      u,
      v);
}

} // namespace psx::gpu
