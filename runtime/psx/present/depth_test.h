// depth_test.h — which record primitives test and write depth: the opaque polygons of the producers a title marks,
// every vertex with a known depth. Everything else draws in OT slot order.
#pragma once

#include "frame_record.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace psx::present {

// How a marked producer's polygons order among themselves.
enum class DepthOrder : std::uint8_t {
  // Each polygon tests and writes depth (a terrain mesh sorted per polygon).
  Polygon,
  // Each object's polygons keep the producer's own order among themselves, test against the scene drawn before
  // them, then write the object's depth (a model the guest sorts into one OT slot, its parts painter-ordered).
  Object,
};

class DepthTest {
public:
  // `producer`'s opaque world geometry: never 2D, HUD, sky, translucent, decal or shadow draws.
  void mark(std::uint32_t producer, DepthOrder order) {
    if (!orderOf(producer)) {
      producers_.push_back({producer, order});
    }
  }
  [[nodiscard]] bool marksAny() const {
    return !producers_.empty();
  }
  // How `primitive` tests depth, or nothing when it draws in slot order.
  [[nodiscard]] std::optional<DepthOrder> tests(const DrawPrimitive &primitive) const {
    if (primitive.kind != PrimitiveKind::Polygon || primitive.semiTransparent || !primitive.key) {
      return std::nullopt;
    }
    const std::optional<DepthOrder> order = orderOf(primitive.key->producer);
    if (!order) {
      return std::nullopt;
    }
    for (int v = 0; v < primitive.vertexCount; v++) {
      if (primitive.vertices[static_cast<std::size_t>(v)].depth <= 0.0f) {
        return std::nullopt;
      }
    }
    return order;
  }

private:
  struct Mark {
    std::uint32_t producer = 0;
    DepthOrder order = DepthOrder::Polygon;
  };
  [[nodiscard]] std::optional<DepthOrder> orderOf(std::uint32_t producer) const {
    const auto found = std::ranges::find(producers_, producer, &Mark::producer);
    return found != producers_.end() ? std::optional(found->order) : std::nullopt;
  }

  std::vector<Mark> producers_;
};

} // namespace psx::present
