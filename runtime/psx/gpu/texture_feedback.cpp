// texture_feedback.cpp — written-pixel tracking for draws that sample VRAM the device's cache may not have seen.
#include "texture_feedback.h"

#include <algorithm>

namespace psx::gpu {
namespace {

constexpr int kTexelMax = 255;

// Texel coordinates one axis samples, before the page base.
struct TexelSpan {
  int first = 0;
  int last = kTexelMax;
};

// A span that wraps the 8-bit coordinate or passes through a texture window may read anywhere.
TexelSpan sampledSpan(int first, int last, int windowMask) {
  if (windowMask != 0 || first < 0 || last > kTexelMax) {
    return {};
  }
  return {first, last};
}

// Halfwords per texel step: 4bpp packs 4, 8bpp 2, 15bpp 1.
int texelShift(int texMode) {
  return 2 - std::clamp(texMode, 0, 2);
}

} // namespace

TextureFeedback::TextureFeedback() = default;

void TextureFeedback::invalidate() {
  written_.clear();
}

void TextureFeedback::texturePage(const present::RecordDrawState &state, bool texDisable) {
  const bool notFourBit = state.texMode != 0;
  if (notFourBit != nonFourBit_ || state.texPageX != pageX_ || state.texPageY != pageY_ || texDisable != texDisable_) {
    invalidate();
  }
  nonFourBit_ = notFourBit;
  pageX_ = state.texPageX;
  pageY_ = state.texPageY;
  texDisable_ = texDisable;
}

void TextureFeedback::invalidateAll() {
  written_.fillAll();
}

bool TextureFeedback::testWrapped(int row, ColumnRun run) const {
  if (run.x1 <= kVramWidth) {
    return written_.any(row, run);
  }
  return written_.any(row, {run.x0, kVramWidth}) || written_.any(row, {0, std::min(run.x1 - kVramWidth, kVramWidth)});
}

void TextureFeedback::written(RecordRect rect) {
  rect.x0 = std::max(rect.x0, 0);
  rect.y0 = std::max(rect.y0, 0);
  rect.x1 = std::min(rect.x1, kVramWidth);
  rect.y1 = std::min(rect.y1, kVramHeight);
  if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
    return;
  }
  for (int row = rect.y0; row < rect.y1; row++) {
    written_.mark(row, {rect.x0, rect.x1});
  }
}

bool TextureFeedback::overlapsSampled(const present::DrawPrimitive &primitive) const {
  const present::RecordDrawState &state = primitive.state;
  const present::RecordVertex &origin = primitive.vertices[0];
  TexelSpan u;
  TexelSpan v;
  if (primitive.kind == present::PrimitiveKind::Sprite) {
    // A sprite steps u/v once per pixel from its origin, backwards when flipped.
    const int spanU = std::max(primitive.width - 1, 0);
    const int spanV = std::max(primitive.height - 1, 0);
    u = sampledSpan(primitive.flipX ? origin.u - spanU : origin.u,
                    primitive.flipX ? origin.u : origin.u + spanU,
                    state.windowMaskX);
    v = sampledSpan(primitive.flipY ? origin.v - spanV : origin.v,
                    primitive.flipY ? origin.v : origin.v + spanV,
                    state.windowMaskY);
  } else {
    int u0 = origin.u;
    int u1 = origin.u;
    int v0 = origin.v;
    int v1 = origin.v;
    for (int index = 1; index < primitive.vertexCount; index++) {
      const present::RecordVertex &vertex = primitive.vertices[static_cast<std::size_t>(index)];
      u0 = std::min<int>(u0, vertex.u);
      u1 = std::max<int>(u1, vertex.u);
      v0 = std::min<int>(v0, vertex.v);
      v1 = std::max<int>(v1, vertex.v);
    }
    // Interpolated coordinates stay inside the vertices' box, give or take a rounding step.
    u = sampledSpan(std::max(u0 - 1, 0), std::min(u1 + 1, kTexelMax), state.windowMaskX);
    v = sampledSpan(std::max(v0 - 1, 0), std::min(v1 + 1, kTexelMax), state.windowMaskY);
  }
  const int shift = texelShift(state.texMode);
  const int x0 = state.texPageX + (u.first >> shift);
  const int x1 = state.texPageX + (u.last >> shift) + 1;
  for (int texel = v.first; texel <= v.last; texel++) {
    if (testWrapped((state.texPageY + texel) % kVramHeight, {x0, x1})) {
      return true;
    }
  }
  return false;
}

bool TextureFeedback::drawn(const present::DrawPrimitive &primitive) {
  written(drawBounds(primitive));
  return primitive.textured && overlapsSampled(primitive);
}

} // namespace psx::gpu
