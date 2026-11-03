// depth_test.h — which record primitives test and write depth against each other: the opaque polygons of the
// producers a title marks, every vertex with a known depth. Everything else draws in OT slot order.
#pragma once

#include "frame_record.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace psx::present {

class DepthTest {
public:
  // `producer`'s opaque world geometry: never 2D, HUD, sky, translucent, decal or shadow draws.
  void mark(std::uint32_t producer) {
    if (!marks(producer)) {
      producers_.push_back(producer);
    }
  }
  [[nodiscard]] bool marks(std::uint32_t producer) const {
    return std::ranges::find(producers_, producer) != producers_.end();
  }
  [[nodiscard]] bool tests(const DrawPrimitive &primitive) const {
    if (primitive.kind != PrimitiveKind::Polygon || primitive.semiTransparent || !primitive.key ||
        !marks(primitive.key->producer)) {
      return false;
    }
    for (int v = 0; v < primitive.vertexCount; v++) {
      if (primitive.vertices[static_cast<std::size_t>(v)].depth <= 0.0f) {
        return false;
      }
    }
    return true;
  }

private:
  std::vector<std::uint32_t> producers_;
};

} // namespace psx::present
