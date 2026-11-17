// record_geometry.cpp — write extents of record entries.
#include "record_geometry.h"

#include <algorithm>
#include <climits>
#include <cstdint>
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

RecordRect texelBox(const present::DrawPrimitive &primitive) {
  const present::RecordVertex &origin = primitive.vertices[0];
  if (primitive.kind == present::PrimitiveKind::Sprite) {
    const int width = std::max(primitive.width, 1);
    const int height = std::max(primitive.height, 1);
    const int u0 = primitive.flipX ? origin.u - width + 1 : origin.u;
    const int v0 = primitive.flipY ? origin.v - height + 1 : origin.v;
    return {.x0 = u0, .y0 = v0, .x1 = u0 + width, .y1 = v0 + height};
  }
  RecordRect box{.x0 = origin.u, .y0 = origin.v, .x1 = origin.u, .y1 = origin.v};
  for (int index = 1; index < primitive.vertexCount; index++) {
    const present::RecordVertex &vertex = primitive.vertices[static_cast<std::size_t>(index)];
    box.x0 = std::min<int>(box.x0, vertex.u);
    box.x1 = std::max<int>(box.x1, vertex.u);
    box.y0 = std::min<int>(box.y0, vertex.v);
    box.y1 = std::max<int>(box.y1, vertex.v);
  }
  box.x1++;
  box.y1++;
  return box;
}

RecordRect primitiveBounds(const present::DrawPrimitive &primitive) {
  if (primitive.kind == present::PrimitiveKind::Sprite) {
    const present::RecordVertex &origin = primitive.vertices[0];
    return {origin.x, origin.y, origin.x + primitive.width, origin.y + primitive.height};
  }
  RecordRect rect{primitive.vertices[0].x, primitive.vertices[0].y, primitive.vertices[0].x, primitive.vertices[0].y};
  for (int v = 1; v < primitive.vertexCount; v++) {
    const present::RecordVertex &vertex = primitive.vertices[static_cast<std::size_t>(v)];
    rect.x0 = std::min(rect.x0, vertex.x);
    rect.y0 = std::min(rect.y0, vertex.y);
    rect.x1 = std::max(rect.x1, vertex.x);
    rect.y1 = std::max(rect.y1, vertex.y);
  }
  rect.x1++;
  rect.y1++;
  return rect;
}

RecordRect drawBounds(const present::DrawPrimitive &primitive) {
  RecordRect rect = primitiveBounds(primitive);
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

std::optional<AxisRectangle> axisRectangle(const present::DrawPrimitive &primitive) {
  const present::RecordVertex &origin = primitive.vertices[0];
  if (primitive.kind == present::PrimitiveKind::Sprite) {
    return AxisRectangle{.rect = {origin.x, origin.y, origin.x + primitive.width, origin.y + primitive.height}};
  }
  if (primitive.kind == present::PrimitiveKind::Line) {
    // gpu.c plots both end points; a horizontal or vertical line covers the pixels between them.
    const present::RecordVertex &end = primitive.vertices[1];
    const bool whole = origin.subX == 0.0f && origin.subY == 0.0f && end.subX == 0.0f && end.subY == 0.0f;
    if (!whole || (origin.x != end.x && origin.y != end.y)) {
      return std::nullopt;
    }
    return AxisRectangle{.rect = {std::min(origin.x, end.x),
                                  std::min(origin.y, end.y),
                                  std::max(origin.x, end.x) + 1,
                                  std::max(origin.y, end.y) + 1}};
  }
  if (primitive.kind != present::PrimitiveKind::Polygon || primitive.vertexCount != 4) {
    return std::nullopt;
  }
  const auto atX = [](const present::RecordVertex &vertex) {
    return static_cast<float>(vertex.x) + vertex.subX;
  };
  const auto atY = [](const present::RecordVertex &vertex) {
    return static_cast<float>(vertex.y) + vertex.subY;
  };
  const present::RecordVertex *left = &origin;
  const present::RecordVertex *top = &origin;
  const present::RecordVertex *right = &origin;
  const present::RecordVertex *bottom = &origin;
  for (const present::RecordVertex &vertex : primitive.vertices) {
    left = atX(vertex) < atX(*left) ? &vertex : left;
    right = atX(vertex) > atX(*right) ? &vertex : right;
    top = atY(vertex) < atY(*top) ? &vertex : top;
    bottom = atY(vertex) > atY(*bottom) ? &vertex : bottom;
  }
  unsigned corners = 0;
  for (const present::RecordVertex &vertex : primitive.vertices) {
    const bool onRight = atX(vertex) == atX(*right);
    const bool onBottom = atY(vertex) == atY(*bottom);
    if ((!onRight && atX(vertex) != atX(*left)) || (!onBottom && atY(vertex) != atY(*top))) {
      return std::nullopt;
    }
    corners |= 1u << ((onRight ? 1 : 0) + (onBottom ? 2 : 0));
  }
  if (corners != 0xFu) {
    return std::nullopt;
  }
  return AxisRectangle{.rect = {left->x, top->y, right->x, bottom->y},
                       .subX0 = left->subX,
                       .subY0 = top->subY,
                       .subX1 = right->subX,
                       .subY1 = bottom->subY};
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
  const auto *upload = std::get_if<present::VramUpload>(&entry);
  if (upload == nullptr) {
    return std::nullopt;
  }
  return clampedVramRect({upload->x, upload->y, upload->width, upload->height});
}

bool entryWritesRect(const present::RecordEntry &entry, const RecordRect &rect) {
  const std::optional<RecordRect> written = entryWrittenRect(entry);
  return written && rectsOverlap(*written, rect);
}

namespace {

std::int64_t area(const RecordRect &rect) {
  return static_cast<std::int64_t>(rect.x1 - rect.x0) * static_cast<std::int64_t>(rect.y1 - rect.y0);
}

RecordRect bound(const RecordRect &a, const RecordRect &b) {
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

bool touches(const RecordRect &a, const RecordRect &b) {
  return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

} // namespace

std::vector<RecordRect> subtractRects(const RecordRect &rect, std::span<const RecordRect> holes) {
  std::vector<RecordRect> parts;
  if (rect.x1 > rect.x0 && rect.y1 > rect.y0) {
    parts.push_back(rect);
  }
  for (const RecordRect &hole : holes) {
    std::vector<RecordRect> kept;
    for (const RecordRect &part : parts) {
      if (hole.x1 <= part.x0 || part.x1 <= hole.x0 || hole.y1 <= part.y0 || part.y1 <= hole.y0) {
        kept.push_back(part);
        continue;
      }
      const int y0 = std::max(part.y0, hole.y0);
      const int y1 = std::min(part.y1, hole.y1);
      for (const RecordRect &piece : {RecordRect{part.x0, part.y0, part.x1, y0},
                                      RecordRect{part.x0, y1, part.x1, part.y1},
                                      RecordRect{part.x0, y0, std::max(part.x0, hole.x0), y1},
                                      RecordRect{std::min(part.x1, hole.x1), y0, part.x1, y1}}) {
        if (piece.x1 > piece.x0 && piece.y1 > piece.y0) {
          kept.push_back(piece);
        }
      }
    }
    parts = std::move(kept);
  }
  return parts;
}

bool rectsCover(std::span<const RecordRect> holes, std::span<const RecordRect> rects) {
  return std::ranges::all_of(rects, [&](const RecordRect &rect) {
    return subtractRects(rect, holes).empty();
  });
}

void RectRegion::add(RecordRect rect) {
  if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
    return;
  }
  for (bool grew = true; grew;) {
    grew = false;
    for (std::size_t i = 0; i < rects_.size(); i++) {
      if (touches(rects_[i], rect)) {
        rect = bound(rects_[i], rect);
        rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(i));
        grew = true;
        break;
      }
    }
  }
  rects_.push_back(rect);
  while (rects_.size() > kMaxRects) {
    std::size_t first = 0;
    std::size_t second = 1;
    std::int64_t least = INT64_MAX;
    for (std::size_t i = 0; i < rects_.size(); i++) {
      for (std::size_t j = i + 1; j < rects_.size(); j++) {
        const std::int64_t added = area(bound(rects_[i], rects_[j])) - area(rects_[i]) - area(rects_[j]);
        if (added < least) {
          least = added;
          first = i;
          second = j;
        }
      }
    }
    const RecordRect merged = bound(rects_[first], rects_[second]);
    rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(second));
    rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(first));
    add(merged);
  }
}

void RectRegion::add(const RectRegion &other) {
  for (const RecordRect &rect : other.rects_) {
    add(rect);
  }
}

} // namespace psx::gpu
