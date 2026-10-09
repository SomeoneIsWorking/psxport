#pragma once

#include "guest_draw_rows.h"

#include <algorithm>
#include <array>
#include <optional>

// Renderer-only base coverage for storage added to the RIGHT of a guest framebuffer. PSX games use
// that VRAM for textures/CLUTs, so the extension must be covered in the host composite without ever
// clearing guest VRAM. The native framebuffer itself is deliberately excluded: preserve-backdrop
// ports need its uploaded pixels intact.
//
// THE RECT IS IN VRAM HALFWORDS, AND THE DISPLAY DEPTH DECIDES WHERE A DISPLAY COLUMN LIVES. At
// 15bpp one display pixel is one halfword, so the two spaces coincide and the distinction is
// invisible. At 24bpp a pixel is RGB888 packed across 1.5 halfwords (present.frag reads display
// column x at byte disp.x*2 + x*3), so display column x sits at halfword x*3/2 — and a rect built
// from display widths without that conversion lands at two thirds of its intended position.
//
// MEASURED, on Spyro's 24bpp Universal boot logo at 16:9 (spyro issue 0118, 2026-09-19). The logo
// is an upload-only guest-VRAM picture in 512x240 24bpp, widened to 684. This plan returned
// halfwords [512,684), which the 24bpp present samples as display columns [341,456) — a black band
// straight through the middle of the picture, cutting the globe and wordmark and punching a hole in
// the copyright line — while the actual margin (display columns [512,684), halfwords [768,1026))
// was never covered at all. The band was measured at columns 342..454 against 341.3 and 456.0
// predicted, and outside it the picture matched a correct 24bpp read to a mean |diff| of 1.29.
struct WideMarginPlan {
  bool draw = false;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// VRAM is 1024 halfwords wide; nothing can be stored past that, so a 24bpp margin that would run
// off the end is clamped here rather than left to the rasterizer's clip. A 512-wide 24bpp display
// widened to 684 needs halfwords up to 1026, so this clamp is reached by a real case, not a
// defensive guess.
inline constexpr int kVramWidthWords = 1024;

// Where a display column lives, in VRAM halfwords, for the two display depths PSX has.
inline int wide_margin_display_x_to_words(int display_x, bool rgb24) {
  return rgb24 ? (display_x * 3) / 2 : display_x;
}

inline WideMarginPlan plan_wide_margin(int sx, int sy, int native_w, int wide_w, int h, bool rgb24) {
  WideMarginPlan p{};
  if (native_w <= 0 || wide_w <= native_w || h <= 0) {
    return p;
  }
  const int x0 = sx + wide_margin_display_x_to_words(native_w, rgb24);
  int x1 = sx + wide_margin_display_x_to_words(wide_w, rgb24);
  if (x1 > kVramWidthWords) {
    x1 = kVramWidthWords;
  }
  if (x1 <= x0) {
    return p; // the extension starts at or past the end of VRAM: there is nothing to cover
  }
  p.draw = true;
  p.x0 = x0;
  p.y0 = sy;
  p.x1 = x1;
  p.y1 = sy + h;
  return p;
}

// WHERE THE HOST LAYS THE BASE, given the rows the guest draws into the displayed buffer itself.
//
// The base above is laid at present over the DISPLAYED buffer. That is right where nobody else draws
// the margin — a host renderer building the frame it shows, an upload-only guest screen — and wrong
// where the guest does: a guest that widened its own projection draws its frames into a widened draw
// area, behind its own widened backdrop clear, and on a double-buffered guest the displayed buffer was
// drawn one composite EARLIER, so a base laid over it at present erased what the guest drew there.
// MEASURED, Spyro 2 in Glimmer at 16:9 (1280x720 present): 24/24 consecutive presented fields had
// display columns 512..683 black, while every one of the 81 primitives per frame reaching past column
// 511 was submitted under a 0..683 draw area.
//
// `guestDrawnRows` is the displayed buffer's own draw rows when the guest draws its margin, else none.
// The base then covers only the margin rows OUTSIDE them — a letterboxed guest's border rows, which
// it never draws and whose margin storage would otherwise show VRAM texture pages — as at most two
// bands, above and below. With no guest rows the whole margin is one band, as before.
struct WideMarginBands {
  int count = 0;
  std::array<WideMarginPlan, 2> band{};
};

inline WideMarginBands host_margin_bands(const WideMarginPlan &margin,
                                         std::optional<psx::gpu::RowSpan> guestDrawnRows) {
  WideMarginBands out;
  if (!margin.draw) {
    return out;
  }
  if (!guestDrawnRows) {
    out.band[out.count++] = margin;
    return out;
  }
  if (guestDrawnRows->top > margin.y0) {
    WideMarginPlan above = margin;
    above.y1 = std::min(margin.y1, guestDrawnRows->top);
    out.band[out.count++] = above;
  }
  if (guestDrawnRows->bottom < margin.y1) {
    WideMarginPlan below = margin;
    below.y0 = std::max(margin.y0, guestDrawnRows->bottom);
    out.band[out.count++] = below;
  }
  return out;
}

// The margins of a frame whose 4:3 picture the host centres in the wide canvas: one band on each
// side of the picture, in the same VRAM halfword space as plan_wide_margin. Nothing under the
// picture is covered.
inline WideMarginBands plan_centred_wide_margins(int sx, int sy, int native_w, int wide_w, int h, bool rgb24) {
  WideMarginBands out;
  if (native_w <= 0 || wide_w <= native_w || h <= 0) {
    return out;
  }
  const int margin = (wide_w - native_w) / 2;
  const int bounds[4] = {0, margin, margin + native_w, wide_w};
  for (int i = 0; i < 4; i += 2) {
    WideMarginPlan b{};
    b.x0 = sx + wide_margin_display_x_to_words(bounds[i], rgb24);
    b.x1 = std::min(sx + wide_margin_display_x_to_words(bounds[i + 1], rgb24), kVramWidthWords);
    b.y0 = sy;
    b.y1 = sy + h;
    if (b.x1 > b.x0) {
      b.draw = true;
      out.band[out.count++] = b;
    }
  }
  return out;
}
