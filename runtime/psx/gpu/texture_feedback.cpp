// texture_feedback.cpp — written-tile tracking for draws that sample VRAM the device's cache may not have seen.
#include "texture_feedback.h"

#include <algorithm>

namespace psx::gpu {
namespace {

constexpr int kPageRows = 256;

// Page width in halfwords: 4bpp 64, 8bpp 128, 15bpp 256.
int pageHalfwords(int texMode) {
  return 64 << std::clamp(texMode, 0, 2);
}

std::size_t tileIndex(int row, int column) {
  return static_cast<std::size_t>(row) * TextureFeedback::kColumns + static_cast<std::size_t>(column);
}

} // namespace

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
  rect.x1 = std::min({rect.x1, state.clipX1 + 1, kRecordVramWidth});
  rect.y1 = std::min({rect.y1, state.clipY1 + 1, kRecordVramHeight});
  if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
    return {};
  }
  return rect;
}

void TextureFeedback::invalidate() {
  written_.reset();
}

void TextureFeedback::texturePage(const present::RecordDrawState &state, bool texDisable) {
  const bool notFourBit = state.texMode != 0;
  if (notFourBit != nonFourBit_ || state.texPageX != pageX_ || state.texPageY != pageY_ || texDisable != texDisable_) {
    written_.reset();
  }
  nonFourBit_ = notFourBit;
  pageX_ = state.texPageX;
  pageY_ = state.texPageY;
  texDisable_ = texDisable;
}

void TextureFeedback::invalidateAll() {
  written_.set();
}

void TextureFeedback::written(RecordRect rect) {
  rect.x0 = std::max(rect.x0, 0);
  rect.y0 = std::max(rect.y0, 0);
  rect.x1 = std::min(rect.x1, kRecordVramWidth);
  rect.y1 = std::min(rect.y1, kRecordVramHeight);
  if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
    return;
  }
  for (int row = rect.y0 / kFeedbackTileHeight; row <= (rect.y1 - 1) / kFeedbackTileHeight; row++) {
    for (int column = rect.x0 / kFeedbackTileWidth; column <= (rect.x1 - 1) / kFeedbackTileWidth; column++) {
      written_.set(tileIndex(row, column));
    }
  }
}

bool TextureFeedback::overlapsPage(const present::DrawPrimitive &primitive) const {
  const present::RecordDrawState &state = primitive.state;
  const int first = state.texPageX / kFeedbackTileWidth;
  const int last = (state.texPageX + pageHalfwords(state.texMode) - 1) / kFeedbackTileWidth;
  for (int row = state.texPageY / kFeedbackTileHeight; row <= (state.texPageY + kPageRows - 1) / kFeedbackTileHeight;
       row++) {
    for (int column = first; column <= last; column++) {
      if (written_.test(tileIndex(row, column % kColumns))) {
        return true;
      }
    }
  }
  return false;
}

bool TextureFeedback::drawn(const present::DrawPrimitive &primitive) {
  written(drawBounds(primitive));
  return primitive.textured && overlapsPage(primitive);
}

} // namespace psx::gpu
