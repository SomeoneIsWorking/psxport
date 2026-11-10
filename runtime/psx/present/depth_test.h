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
  // Each polygon tests and writes depth (a terrain mesh sorted per polygon). A later polygon loses only where it lies
  // farther than an earlier one by more than that one's `bin`: the guest's sort cannot order surfaces closer than that.
  Polygon,
  // A model the guest sorts into one OT slot, its parts painter-ordered: it draws over everything the slot order put
  // before it, as retail does, and writes its depth so what the slot order puts after it is tested against it.
  Object,
};

// How a marked producer's polygons test: their order, and the view units their written depth lies back by.
struct DepthMark {
  DepthOrder order = DepthOrder::Polygon;
  float bin = 0.0f;
};

class DepthTest {
public:
  // `producer`'s opaque world geometry, or a 2D fill its producer places at a world plane's depth (a portal opening);
  // never HUD, sky, translucent, decal or shadow draws. `bin` is the depth
  // (title view units) one slot of the guest's sort spans for a Polygon producer.
  void mark(std::uint32_t producer, DepthOrder order, float bin = 0.0f) {
    if (!markOf(producer)) {
      producers_.push_back({producer, {order, bin}});
    }
  }
  [[nodiscard]] bool marksAny() const {
    return !producers_.empty();
  }
  // How `primitive` tests depth, or nothing when it draws in slot order.
  [[nodiscard]] std::optional<DepthMark> tests(const DrawPrimitive &primitive) const {
    if (primitive.kind != PrimitiveKind::Polygon || primitive.semiTransparent || !primitive.key) {
      return std::nullopt;
    }
    const std::optional<DepthMark> marked = markOf(primitive.key->producer);
    if (!marked) {
      return std::nullopt;
    }
    for (int v = 0; v < primitive.vertexCount; v++) {
      if (primitive.vertices[static_cast<std::size_t>(v)].depth <= 0.0f) {
        return std::nullopt;
      }
    }
    return marked;
  }

private:
  struct Mark {
    std::uint32_t producer = 0;
    DepthMark mark;
  };
  [[nodiscard]] std::optional<DepthMark> markOf(std::uint32_t producer) const {
    const auto found = std::ranges::find(producers_, producer, &Mark::producer);
    return found != producers_.end() ? std::optional(found->mark) : std::nullopt;
  }

  std::vector<Mark> producers_;
};

} // namespace psx::present
