// keyed_blend.cpp — pairing by key and the vertex blend.
#include "keyed_blend.h"

#include <unordered_map>
#include <variant>
#include <vector>

namespace psx::present {
namespace {

bool sameTopology(const DrawPrimitive &a, const DrawPrimitive &b) {
  return a.kind == b.kind && a.vertexCount == b.vertexCount && a.state.texPageX == b.state.texPageX &&
         a.state.texPageY == b.state.texPageY && a.state.texMode == b.state.texMode &&
         a.state.blendMode == b.state.blendMode;
}

// Blends in drawing-area space: double-buffered frames draw at different offsets.
void blendPositions(DrawPrimitive &out, const DrawPrimitive &previous, float t) {
  const int dx = out.state.offsetX - previous.state.offsetX;
  const int dy = out.state.offsetY - previous.state.offsetY;
  for (int i = 0; i < out.vertexCount; i++) {
    const auto v = static_cast<std::size_t>(i);
    RecordVertex &vertex = out.vertices[v];
    const RecordVertex &from = previous.vertices[v];
    const float fromX = static_cast<float>(from.x + dx);
    const float fromY = static_cast<float>(from.y + dy);
    const float x = fromX + (static_cast<float>(vertex.x) - fromX) * t;
    const float y = fromY + (static_cast<float>(vertex.y) - fromY) * t;
    placeVertex(vertex, x, y, out.kind);
  }
}

} // namespace

FrameRecord keyedBlend(const FrameRecord &previous, const FrameRecord &current, float t) {
  FrameRecord out = current;
  std::unordered_map<RecordKey, std::vector<const DrawPrimitive *>, RecordKeyHash> earlier;
  for (const RecordEntry &entry : previous.entries()) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive != nullptr && primitive->key) {
      earlier[primitive->key->identity()].push_back(primitive);
    }
  }
  if (earlier.empty()) {
    return out;
  }
  std::unordered_map<RecordKey, std::size_t, RecordKeyHash> uses;
  for (const RecordEntry &entry : out.entries()) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive != nullptr && primitive->key) {
      uses[primitive->key->identity()]++;
    }
  }
  for (RecordEntry &entry : out.entries()) {
    auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive == nullptr || !primitive->key) {
      continue;
    }
    // A key used twice in either frame names no single primitive: drawn as N.
    const RecordKey key = primitive->key->identity();
    const auto found = earlier.find(key);
    if (found == earlier.end() || found->second.size() != 1 || uses[key] != 1) {
      continue;
    }
    const DrawPrimitive &partner = *found->second.front();
    if (sameTopology(*primitive, partner)) {
      blendPositions(*primitive, partner, t);
    }
  }
  return out;
}

} // namespace psx::present
