// scea_splash.cpp — see scea_splash.h. The three sprite rects, texpage UVs and CLUT the boot stub drew.
#include "scea_splash.h"

#include "scea_asset.h"

namespace psx::platform {
namespace {

struct SpriteRect {
  int screenX, screenY, width, height, u0, v0;
};

constexpr SpriteRect kRects[3] = {{536, 200, 64, 32, 0, 128}, {280, 200, 256, 64, 0, 64}, {24, 200, 256, 64, 0, 0}};

std::uint8_t expand5(int channel) {
  return static_cast<std::uint8_t>((channel << 3) | (channel >> 2));
}

} // namespace

std::vector<std::uint8_t> decodeSceaSplash() {
  static_assert(kSceaSplashWidth == SCEA_DISP_W && kSceaSplashHeight == SCEA_DISP_H);
  std::vector<std::uint8_t> out(static_cast<std::size_t>(kSceaSplashWidth) * kSceaSplashHeight * 4, 0);
  for (const SpriteRect &rect : kRects) {
    for (int row = 0; row < rect.height; row++) {
      for (int column = 0; column < rect.width; column++) {
        const int u = rect.u0 + column;
        const int v = rect.v0 + row;
        // 4bpp: one texture word holds four texels, selected by u & 3.
        const int wordColumn = u >> 2;
        if (wordColumn >= SCEA_TEX_W || v >= SCEA_TEX_H) {
          continue;
        }
        const unsigned short word = scea_tex_words[v * SCEA_TEX_W + wordColumn];
        const unsigned short colour = scea_clut_words[(word >> ((u & 3) * 4)) & 0xF];
        if (colour == 0) {
          continue; // a 0x0000 CLUT entry is transparent
        }
        const int x = rect.screenX + column;
        const int y = rect.screenY + row;
        if (x >= kSceaSplashWidth || y >= kSceaSplashHeight) {
          continue;
        }
        std::uint8_t *pixel = out.data() + (static_cast<std::size_t>(y) * kSceaSplashWidth + x) * 4;
        pixel[0] = expand5(colour & 31);
        pixel[1] = expand5((colour >> 5) & 31);
        pixel[2] = expand5((colour >> 10) & 31);
        pixel[3] = 255;
      }
    }
  }
  return out;
}

} // namespace psx::platform
