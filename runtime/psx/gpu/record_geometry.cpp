// record_geometry.cpp — write extents of record entries.
#include "record_geometry.h"

#include <algorithm>
#include <variant>

namespace psx::gpu {

RecordRect clampedVramRect(VramRect rect) {
  const int x0 = std::clamp(rect.x, 0, kVramWidth);
  const int y0 = std::clamp(rect.y, 0, kVramHeight);
  return {x0, y0, std::clamp(rect.x + rect.width, x0, kVramWidth), std::clamp(rect.y + rect.height, y0, kVramHeight)};
}

bool rectsOverlap(const RecordRect &a, const RecordRect &b) {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

RecordRect drawBounds(const present::DrawPrimitive &primitive) {
  RecordRect rect{};
  if (primitive.kind == present::PrimitiveKind::Sprite) {
    const present::RecordVertex &origin = primitive.vertices[0];
    rect = {origin.x, origin.y, origin.x + primitive.width, origin.y + primitive.height};
  } else {
    rect = {primitive.vertices[0].x, primitive.vertices[0].y, primitive.vertices[0].x, primitive.vertices[0].y};
    for (int v = 1; v < primitive.vertexCount; v++) {
      const present::RecordVertex &vertex = primitive.vertices[static_cast<std::size_t>(v)];
      rect.x0 = std::min(rect.x0, vertex.x);
      rect.y0 = std::min(rect.y0, vertex.y);
      rect.x1 = std::max(rect.x1, vertex.x);
      rect.y1 = std::max(rect.y1, vertex.y);
    }
    rect.x1++;
    rect.y1++;
  }
  const present::RecordDrawState &state = primitive.state;
  rect.x0 = std::max({rect.x0, state.clipX0, 0});
  rect.y0 = std::max({rect.y0, state.clipY0, 0});
  rect.x1 = std::min({rect.x1, state.clipX1 + 1, kVramWidth});
  rect.y1 = std::min({rect.y1, state.clipY1 + 1, kVramHeight});
  if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
    return {};
  }
  return rect;
}

std::optional<RecordRect> flatRectangle(const present::DrawPrimitive &primitive) {
  const present::RecordVertex &origin = primitive.vertices[0];
  if (primitive.textured || primitive.gouraud) {
    return std::nullopt;
  }
  if (primitive.kind == present::PrimitiveKind::Sprite) {
    return RecordRect{origin.x, origin.y, origin.x + primitive.width, origin.y + primitive.height};
  }
  if (primitive.kind != present::PrimitiveKind::Polygon || primitive.vertexCount != 4) {
    return std::nullopt;
  }
  RecordRect rect{origin.x, origin.y, origin.x, origin.y};
  for (const present::RecordVertex &vertex : primitive.vertices) {
    rect.x0 = std::min(rect.x0, vertex.x);
    rect.y0 = std::min(rect.y0, vertex.y);
    rect.x1 = std::max(rect.x1, vertex.x);
    rect.y1 = std::max(rect.y1, vertex.y);
  }
  unsigned corners = 0;
  for (const present::RecordVertex &vertex : primitive.vertices) {
    const bool atX = vertex.x == rect.x0 || vertex.x == rect.x1;
    const bool atY = vertex.y == rect.y0 || vertex.y == rect.y1;
    if (!atX || !atY || vertex.subX != 0.0f || vertex.subY != 0.0f) {
      return std::nullopt;
    }
    corners |= 1u << ((vertex.x == rect.x1 ? 1 : 0) + (vertex.y == rect.y1 ? 2 : 0));
  }
  if (corners != 0xFu) {
    return std::nullopt;
  }
  return rect;
}

std::optional<RecordRect> entryWrittenRect(const present::RecordEntry &entry) {
  if (const auto *primitive = std::get_if<present::DrawPrimitive>(&entry)) {
    return drawBounds(*primitive);
  }
  if (const auto *fill = std::get_if<present::VramFill>(&entry)) {
    return clampedVramRect({fill->x, fill->y, fill->width, fill->height});
  }
  if (const auto *copy = std::get_if<present::VramCopy>(&entry)) {
    // A copy onto its own source changes no pixel (the guest issues a 2x1 one as a fence).
    if (copy->srcX == copy->dstX && copy->srcY == copy->dstY) {
      return std::nullopt;
    }
    return clampedVramRect({copy->dstX, copy->dstY, copy->width, copy->height});
  }
  const auto &upload = std::get<present::VramUpload>(entry);
  return clampedVramRect({upload.x, upload.y, upload.width, upload.height});
}

bool entryWritesRect(const present::RecordEntry &entry, const RecordRect &rect) {
  const std::optional<RecordRect> written = entryWrittenRect(entry);
  return written && rectsOverlap(*written, rect);
}

} // namespace psx::gpu
