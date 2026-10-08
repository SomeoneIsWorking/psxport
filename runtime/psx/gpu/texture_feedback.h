// texture_feedback.h — which textured draws read VRAM that gpu.c's texture cache may hold stale.
#pragma once

#include "frame_record.h"
#include "record_raster_setup.h"

#include <cstdint>
#include <vector>

namespace psx::gpu {

// The VRAM rect a primitive can write: its vertices or sprite extent inside the draw area.
RecordRect drawBounds(const present::DrawPrimitive &primitive);

// VRAM pixels drawn into since gpu.c last invalidated its texture cache (upload, copy, read). gpu.c fills a cache
// line on a miss and serves it until then, and a draw reads its own earlier pixels, so a texture read from a
// written pixel depends on the device's fetch order. The rasterizer samples a snapshot, which cannot say.
class TextureFeedback {
public:
  TextureFeedback();
  // gpu.c InvalidateTexCache: every later fetch sees current VRAM.
  void invalidate();
  // gpu.c SetTPage, which runs for E1 and for every textured polygon's own texpage word: the cache is
  // invalidated when the page, whether the mode is 4bpp, or TexDisable differs from the last one seen.
  void texturePage(const present::RecordDrawState &state, bool texDisable);
  // The device's cache is unknown: any texture may be stale.
  void invalidateAll();
  // Fills wrap at the VRAM edges; the part outside is not tracked.
  void written(RecordRect rect);
  // Marks what `primitive` writes; true when the texels it samples overlap a written pixel.
  bool drawn(const present::DrawPrimitive &primitive);

private:
  // Whether the VRAM the primitive's texture coordinates reach overlaps a written pixel.
  bool overlapsSampled(const present::DrawPrimitive &primitive) const;

  // Columns [x0, x1) of one row, 0 <= x0 < x1 <= kRecordVramWidth.
  void markSpan(int row, int x0, int x1);
  bool testSpan(int row, int x0, int x1) const;
  // A column range may run past the right edge, where VRAM addressing wraps to column 0.
  bool testWrapped(int row, int x0, int x1) const;

  // One bit per VRAM pixel, row-major, 64 columns per word.
  std::vector<std::uint64_t> written_;
  int pageX_ = 0;
  int pageY_ = 0;
  bool nonFourBit_ = false; // texture mode is not 4bpp
  bool texDisable_ = false;
};

} // namespace psx::gpu
