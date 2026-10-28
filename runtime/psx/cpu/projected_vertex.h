// projected_vertex.h — a screen vertex as a packet carries it: the SXY word the GPU reads, and where the vertex
// lies before that word rounded it.
#pragma once

#include "gp0_command.h"
#include "gte_state.h"

#include <cmath>
#include <cstdint>
#include <optional>

namespace psx::gte {

class ProjectedVertex {
public:
  // The vertex at the origin, word 0.
  constexpr ProjectedVertex() = default;
  // `word` as the guest rounds the vertex at `place`; the GTE makes one per projection, a producer one per vertex
  // it computes itself (a clip crossing, a split, a corner offset by a scaled size).
  constexpr ProjectedVertex(std::uint32_t word, GtePlace place) : word_(word), place_(place) {}

  // A vertex no projection made: it lies on the pixel its word names, read as the GPU reads it.
  [[nodiscard]] static constexpr ProjectedVertex whole(std::uint32_t word) {
    const gpu::Gp0VertexPos position = gpu::Gp0Command(word).vertexPos();
    return {word, {static_cast<float>(position.x), static_cast<float>(position.y)}};
  }

  // A vertex a producer computed at `place`, its word the pixel nearest it.
  [[nodiscard]] static ProjectedVertex nearest(GtePlace place) {
    const auto x = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(place.x)));
    const auto y = static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(place.y)));
    return {x | (static_cast<std::uint32_t>(y) << 16), place};
  }

  // This vertex moved by the whole-pixel step the guest takes from its word to `word`.
  [[nodiscard]] constexpr ProjectedVertex steppedTo(std::uint32_t word) const {
    const GtePlace from = whole(word_).place_;
    const GtePlace to = whole(word).place_;
    return {word, {place_.x + to.x - from.x, place_.y + to.y - from.y}};
  }

  [[nodiscard]] constexpr std::uint32_t word() const {
    return word_;
  }
  [[nodiscard]] constexpr GtePlace place() const {
    return place_;
  }

private:
  std::uint32_t word_ = 0;
  GtePlace place_{};
};

// A packet vertex word with no place of its own: it lies on the pixel it names.
struct VertexWord {
  std::uint32_t value = 0;
};

// The place of the vertex stored into a packet word; none for a word stored any other way.
using WordPlace = std::optional<GtePlace>;

} // namespace psx::gte
