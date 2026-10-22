// record_geometry.h — the VRAM rects a FrameRecord entry writes, in native pixels.
#pragma once

#include "frame_record.h"
#include "vram_rect.h"

#include <optional>

namespace psx::gpu {

// Half open.
struct RecordRect {
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;

  bool operator==(const RecordRect &) const = default;
};

// `rect` clipped to VRAM.
RecordRect clampedVramRect(VramRect rect);
bool rectsOverlap(const RecordRect &a, const RecordRect &b);

// The VRAM rect a primitive can write: its vertices or sprite extent inside the draw area.
RecordRect drawBounds(const present::DrawPrimitive &primitive);

// A flat untextured rectangle: its whole-pixel edges and how far past each a placed quad's edge lies.
struct FlatRectangle {
  RecordRect rect;
  float subX0 = 0.0f;
  float subY0 = 0.0f;
  float subX1 = 0.0f;
  float subY1 = 0.0f;
};

// A sprite, or a quad with axis-aligned edges; none otherwise.
std::optional<FlatRectangle> flatRectangle(const present::DrawPrimitive &primitive);

// A primitive's draw bounds, a fill, or a copy or upload target; none for a copy onto its own source.
std::optional<RecordRect> entryWrittenRect(const present::RecordEntry &entry);
bool entryWritesRect(const present::RecordEntry &entry, const RecordRect &rect);

} // namespace psx::gpu
