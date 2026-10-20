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

// The pixels a flat untextured rectangle covers: a sprite, or a quad with axis-aligned edges; none otherwise.
std::optional<RecordRect> flatRectangle(const present::DrawPrimitive &primitive);

// A primitive's draw bounds, a fill, or a copy or upload target; none for a copy onto its own source.
std::optional<RecordRect> entryWrittenRect(const present::RecordEntry &entry);
bool entryWritesRect(const present::RecordEntry &entry, const RecordRect &rect);

} // namespace psx::gpu
