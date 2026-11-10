// depth_test.h — which record primitives test and write depth: the opaque polygons of the draw functions a title marks,
// every vertex with a known depth. Everything else draws in OT slot order.
#pragma once

#include "frame_record.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace psx::present {

// How a marked draw's polygons order among themselves.
enum class DepthOrder : std::uint8_t {
  // Each polygon tests and writes depth (a terrain mesh sorted per polygon). A later polygon loses only where it lies
  // farther than an earlier one by more than that one's `bin`: the guest's sort cannot order surfaces closer than that.
  Polygon,
  // A model the guest sorts into one OT slot, its parts painter-ordered: it draws over everything the slot order put
  // before it, as retail does, and writes its depth so what the slot order puts after it is tested against it.
  Object,
};

// How a marked draw's polygons test: their order, and the view units their written depth lies back by.
struct DepthMark {
  DepthOrder order = DepthOrder::Polygon;
  float bin = 0.0f;
};

class DepthTest {
public:
  // The opaque world polygons whose every vertex `function` placed: its guest SWC2s, or the stores of the native
  // override at that entry. World geometry, or a 2D fill placed at a world plane's depth (a portal opening); never HUD,
  // sky, translucent, decal or shadow draws. `bin` is the depth (title view units) one slot of the guest's sort spans
  // for a Polygon draw.
  void mark(std::uint32_t function, DepthOrder order, float bin = 0.0f) {
    if (!find(function)) {
      marks_.push_back({function, {order, bin}});
    }
  }
  [[nodiscard]] bool marksAny() const {
    return !marks_.empty();
  }
  // How `primitive` tests depth, or nothing when it draws in slot order.
  [[nodiscard]] std::optional<DepthMark> tests(const DrawPrimitive &primitive) const {
    if (primitive.kind != PrimitiveKind::Polygon || primitive.semiTransparent) {
      return std::nullopt;
    }
    const std::optional<DepthMark> marked = find(primitive.site);
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
    std::uint32_t function = 0;
    DepthMark mark;
  };
  [[nodiscard]] std::optional<DepthMark> find(std::uint32_t function) const {
    if (function == 0) {
      return std::nullopt;
    }
    const auto found = std::ranges::find(marks_, function, &Mark::function);
    return found != marks_.end() ? std::optional(found->mark) : std::nullopt;
  }

  std::vector<Mark> marks_;
};

} // namespace psx::present
