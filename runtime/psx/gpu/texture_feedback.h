// texture_feedback.h — which textured draws read VRAM that gpu.c's texture cache may hold stale.
#pragma once

#include "frame_record.h"
#include "record_raster_setup.h"

#include <bitset>

namespace psx::gpu {

// VRAM rows and columns covered by one tracking tile.
inline constexpr int kFeedbackTileWidth = 64;
inline constexpr int kFeedbackTileHeight = 32;

// The VRAM rect a primitive can write: its vertices or sprite extent inside the draw area.
RecordRect drawBounds(const present::DrawPrimitive &primitive);

// Tiles drawn into since gpu.c last invalidated its texture cache (upload, copy, read). gpu.c fills a cache
// line on a miss and serves it until then, and a draw reads its own earlier pixels, so a texture read from a
// written tile depends on the device's fetch order. The rasterizer samples a snapshot, which cannot say.
class TextureFeedback {
public:
  // gpu.c InvalidateTexCache: every later fetch sees current VRAM.
  void invalidate();
  // gpu.c SetTPage, which runs for E1 and for every textured polygon's own texpage word: the cache is
  // invalidated when the page, whether the mode is 4bpp, or TexDisable differs from the last one seen.
  void texturePage(const present::RecordDrawState &state, bool texDisable);
  // The device's cache is unknown: any texture may be stale.
  void invalidateAll();
  // Fills wrap at the VRAM edges; the part outside is not tracked.
  void written(RecordRect rect);
  // Marks what `primitive` writes; true when its texture page overlaps a written tile.
  bool drawn(const present::DrawPrimitive &primitive);

  static constexpr int kColumns = 1024 / kFeedbackTileWidth;
  static constexpr int kRows = 512 / kFeedbackTileHeight;

private:
  bool overlapsPage(const present::DrawPrimitive &primitive) const;

  std::bitset<kColumns * kRows> written_;
  int pageX_ = 0;
  int pageY_ = 0;
  bool nonFourBit_ = false; // texture mode is not 4bpp
  bool texDisable_ = false;
};

} // namespace psx::gpu
