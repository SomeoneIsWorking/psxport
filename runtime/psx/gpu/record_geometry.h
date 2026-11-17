// record_geometry.h — the VRAM rects a FrameRecord entry writes, in native pixels.
#pragma once

#include "frame_record.h"
#include "vram_rect.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

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

// A primitive's vertices or sprite extent, as positioned, before any clip.
RecordRect primitiveBounds(const present::DrawPrimitive &primitive);

// The VRAM rect a primitive can write: its bounds inside the draw area.
RecordRect drawBounds(const present::DrawPrimitive &primitive);

// The texture coordinates a textured primitive reaches, half open, before the texture window and the 8-bit wrap: a
// sprite's from its origin, backwards when flipped; a polygon's or line's vertices' box.
RecordRect texelBox(const present::DrawPrimitive &primitive);

// An axis-aligned rectangle: its whole-pixel edges and how far past each a placed quad's edge lies.
struct AxisRectangle {
  RecordRect rect;
  float subX0 = 0.0f;
  float subY0 = 0.0f;
  float subX1 = 0.0f;
  float subY1 = 0.0f;
};

// A sprite, a quad with axis-aligned edges, or a horizontal or vertical line at whole pixels; none otherwise.
std::optional<AxisRectangle> axisRectangle(const present::DrawPrimitive &primitive);

// A primitive's draw bounds, a fill, or a copy or upload target; none for a copy onto its own source or a
// read-back declaration.
std::optional<RecordRect> entryWrittenRect(const present::RecordEntry &entry);
bool entryWritesRect(const present::RecordEntry &entry, const RecordRect &rect);

// Native rects of a plane, kept to a few: a growing one swallows those it touches, and past kMaxRects the two whose
// union adds least merge. Never smaller than what was added.
class RectRegion {
public:
  static constexpr std::size_t kMaxRects = 4;
  void add(RecordRect rect);
  void add(const RectRegion &other);
  void clear() {
    rects_.clear();
  }
  bool empty() const {
    return rects_.empty();
  }
  std::span<const RecordRect> rects() const {
    return rects_;
  }

private:
  std::vector<RecordRect> rects_;
};

// `rect` without any of `holes`, as disjoint rects.
std::vector<RecordRect> subtractRects(const RecordRect &rect, std::span<const RecordRect> holes);
// Whether `holes` cover every one of `rects`.
bool rectsCover(std::span<const RecordRect> holes, std::span<const RecordRect> rects);

} // namespace psx::gpu
