#include "guest_geometry_interpolation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <optional>

namespace psxport::temporal {
namespace {

// The GPU reads a vertex axis as 11-bit two's complement, and the GTE saturates SXY to that range, so
// this is the one comparison space for a packet's word and the projection that wrote it.
int16_t gpuAxis(int32_t value) {
  return static_cast<int16_t>(static_cast<int32_t>(static_cast<uint32_t>(value) << 21) >> 21);
}

uint32_t screenKey(int32_t x, int32_t y) {
  return (static_cast<uint32_t>(static_cast<uint16_t>(gpuAxis(x))) << 16) | static_cast<uint16_t>(gpuAxis(y));
}

using ScreenRange = std::pair<std::vector<std::pair<uint32_t, uint32_t>>::const_iterator,
                              std::vector<std::pair<uint32_t, uint32_t>>::const_iterator>;

ScreenRange producersOf(const std::vector<std::pair<uint32_t, uint32_t>> &byScreen, uint32_t key) {
  const auto first = std::lower_bound(
      byScreen.begin(), byScreen.end(), key, [](const std::pair<uint32_t, uint32_t> &entry, uint32_t wanted) {
        return entry.first < wanted;
      });
  auto last = first;
  while (last != byScreen.end() && last->first == key) {
    ++last;
  }
  return {first, last};
}

// How a reading of a primitive's corners came out.
enum class Fit { none, unique, ambiguous };

struct Assignment {
  Fit fit = Fit::none;
  std::array<VertexIdentity, 4> vertices{};
};

// A face's corners come out of as few GTE instructions as hold them: one RTPT for a triangle, an RTPT
// and an adjacent RTPS (or a second RTPT) for a quad.
uint32_t groupSpan(int corners) {
  return corners > 3 ? 1u : 0u;
}

// A candidate projection may stand for a corner when it was projected under the anchor corner's scope and
// transform and, for a grouped reading, by the instructions starting at `window`.
bool admits(
    bool grouped, uint32_t span, const ProjectedVertex &anchor, uint32_t window, const ProjectedVertex &candidate) {
  if (candidate.identity.scope != anchor.identity.scope || candidate.identity.epoch != anchor.identity.epoch) {
    return false;
  }
  return !grouped || candidate.instruction - window <= span;
}

// Every way of reading the corners under one rule. Two vertices that land on the same pixel are
// `interchangeable` when the in-between would draw them identically. A reading in which a corner has two
// admissible vertices that are not interchangeable names no vertex for it and is dropped; the face's own
// instructions never read that way unless the face is degenerate. Two readings that are not
// interchangeable corner by corner, or only conflicted readings, are ambiguous: the pairing cannot see
// through that coincidence.
template <class Interchangeable>
Assignment assign(bool grouped,
                  int corners,
                  const std::array<ScreenRange, 4> &producers,
                  std::span<const ProjectedVertex> projections,
                  const Interchangeable &interchangeable) {
  const uint32_t span = grouped ? groupSpan(corners) : 0u;
  Assignment result;
  bool conflicted = false;
  for (auto anchorIt = producers[0].first; anchorIt != producers[0].second; ++anchorIt) {
    const ProjectedVertex &anchor = projections[anchorIt->second];
    for (uint32_t offset = 0; offset <= span; ++offset) {
      const uint32_t window = anchor.instruction - offset;
      std::array<VertexIdentity, 4> vertices{};
      vertices[0] = anchor.identity;
      bool complete = true;
      for (int corner = 1; corner < corners && complete; ++corner) {
        std::optional<VertexIdentity> found;
        for (auto it = producers[corner].first; it != producers[corner].second && complete; ++it) {
          const ProjectedVertex &candidate = projections[it->second];
          if (!admits(grouped, span, anchor, window, candidate)) {
            continue;
          }
          if (found && !interchangeable(*found, candidate.identity)) {
            conflicted = true;
            complete = false;
          }
          found = candidate.identity;
        }
        complete = complete && found.has_value();
        if (complete) {
          vertices[corner] = *found;
        }
      }
      if (!complete) {
        continue;
      }
      if (result.fit == Fit::unique) {
        for (int corner = 0; corner < corners; ++corner) {
          if (!interchangeable(result.vertices[corner], vertices[corner])) {
            return {.fit = Fit::ambiguous, .vertices = {}};
          }
        }
      }
      result = {.fit = Fit::unique, .vertices = vertices};
    }
  }
  if (result.fit == Fit::none && conflicted) {
    result.fit = Fit::ambiguous;
  }
  return result;
}

} // namespace

void GuestGeometryInterpolation::beginFrame(std::span<const RqItem> items,
                                            std::span<const ProjectedVertex> projections) {
  items_ = items;
  resolutions_.clear();
  owned_.assign(items.size(), 0);
  census_ = {};
  census_.items = static_cast<uint32_t>(items.size());

  current_.clear();
  byScreen_.clear();
  byScreen_.reserve(projections.size());
  for (uint32_t index = 0; index < projections.size(); ++index) {
    const ProjectedVertex &vertex = projections[index];
    current_[vertex.identity] = {gpuAxis(vertex.screenX), gpuAxis(vertex.screenY)};
    byScreen_.emplace_back(screenKey(vertex.screenX, vertex.screenY), index);
  }
  std::sort(byScreen_.begin(), byScreen_.end());

  for (size_t index = 0; index < items.size(); ++index) {
    resolve(index, items[index], projections);
  }
}

void GuestGeometryInterpolation::resolve(size_t index,
                                         const RqItem &item,
                                         std::span<const ProjectedVertex> projections) {
  if (!item.has_guest_xy || item.nv < 3 || item.has_xyf) {
    return;
  }
  ++census_.polygons;
  // The in-between moves a vertex by its guest-space displacement, which is only the host-space
  // displacement when the host placed the packet at the guest's coordinates plus one offset.
  for (int corner = 1; corner < item.nv; ++corner) {
    if (item.xs[corner] - item.guest_x[corner] != item.xs[0] - item.guest_x[0] ||
        item.ys[corner] - item.guest_y[corner] != item.ys[0] - item.guest_y[0]) {
      ++census_.mapped;
      return;
    }
  }
  std::array<ScreenRange, 4> producers{};
  for (int corner = 0; corner < item.nv; ++corner) {
    producers[corner] = producersOf(byScreen_, screenKey(item.guest_x[corner], item.guest_y[corner]));
    if (producers[corner].first == producers[corner].second) {
      ++census_.unmatched;
      return;
    }
  }

  // A face projected by its own RTPT (and RTPS) is read from those instructions first: their outputs are
  // the packet's corners, which no other face's vertices on the same pixels can be. A producer that
  // projects a vertex pool and indexes it afterwards has no such grouping and is read under its
  // transform alone.
  //
  // Coincident vertices are told apart only where it matters: the in-between moves a vertex toward where
  // it was, so two vertices on one pixel that were also on one pixel (or both nowhere) a frame earlier
  // draw identically whichever produced the packet. Two producers drawing the same world geometry, at
  // different model scales, is the case this admits.
  const auto interchangeable = [this](const VertexIdentity &a, const VertexIdentity &b) {
    if (a == b) {
      return true;
    }
    const auto pastA = previous_.find(a);
    const auto pastB = previous_.find(b);
    if (pastA == previous_.end() || pastB == previous_.end()) {
      return pastA == pastB;
    }
    return pastA->second.x == pastB->second.x && pastA->second.y == pastB->second.y;
  };
  Assignment chosen = assign(true, item.nv, producers, projections, interchangeable);
  if (chosen.fit == Fit::none) {
    chosen = assign(false, item.nv, producers, projections, interchangeable);
  }
  if (chosen.fit == Fit::ambiguous) {
    ++census_.ambiguous;
    return;
  }
  if (chosen.fit == Fit::none) {
    ++census_.unmatched;
    return;
  }
  ++census_.resolved;
  owned_[index] = 1;
  resolutions_.push_back({.item = index, .vertices = chosen.vertices});
}

bool GuestGeometryInterpolation::owns(const RqItem &item) const {
  if (items_.empty() || &item < items_.data() || &item >= items_.data() + items_.size()) {
    return false;
  }
  return owned_[static_cast<size_t>(&item - items_.data())] != 0;
}

void GuestGeometryInterpolation::emit(float t, RenderQueue &sink) {
  const float back = 1.0f - t;
  census_.interpolatedVertices = 0;
  census_.heldVertices = 0;
  for (const Resolution &resolution : resolutions_) {
    const RqItem &source = items_[resolution.item];
    RqItem *const target = sink.push();
    // Every field, including the captured seq/draw_seq, so the merge places it exactly where the real
    // frame drew it.
    *target = source;
    for (int corner = 0; corner < source.nv; ++corner) {
      const auto previous = previous_.find(resolution.vertices[corner]);
      if (previous == previous_.end()) {
        ++census_.heldVertices;
        continue;
      }
      const int dx =
          static_cast<int>(std::lround(back * static_cast<float>(previous->second.x - source.guest_x[corner])));
      const int dy =
          static_cast<int>(std::lround(back * static_cast<float>(previous->second.y - source.guest_y[corner])));
      target->xs[corner] += dx;
      target->ys[corner] += dy;
      target->guest_x[corner] = static_cast<int16_t>(target->guest_x[corner] + dx);
      target->guest_y[corner] = static_cast<int16_t>(target->guest_y[corner] + dy);
      ++census_.interpolatedVertices;
    }
  }
}

void GuestGeometryInterpolation::rotate() {
  previous_ = std::move(current_);
  current_.clear();
  havePrevious_ = !previous_.empty();
  items_ = {};
  resolutions_.clear();
  owned_.clear();
}

void GuestGeometryInterpolation::clear() {
  previous_.clear();
  havePrevious_ = false;
}

} // namespace psxport::temporal
