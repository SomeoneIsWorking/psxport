#include "precise_projection.h"

#include <algorithm>
#include <cmath>

namespace psx::present {
namespace {

// Open addressing over a power of two; a render projects a few thousand points.
constexpr std::size_t kSlots = 1u << 14;
constexpr std::size_t kMask = kSlots - 1u;
constexpr std::size_t kMaxProbe = 32;

std::size_t slotOf(std::uint32_t sxy) {
  return (sxy * 0x9E3779B1u) >> 18;
}

void recordPoint(void *context, std::uint32_t sxy, float x, float y) {
  static_cast<PreciseProjection *>(context)->record(sxy, x, y);
}

std::uint32_t packedWord(const RecordVertex &vertex) {
  return static_cast<std::uint16_t>(vertex.x) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(vertex.y)) << 16);
}

// The GTE truncates, so the point lies in [whole, whole + 1); the float divide can stray past either end.
float fractionPast(int whole, float precise) {
  return std::clamp(precise - static_cast<float>(whole), 0.0f, std::nextafter(1.0f, 0.0f));
}

} // namespace

PreciseProjection::PreciseProjection() : points_(kSlots) {}

void PreciseProjection::clear() {
  ++generation_;
  if (generation_ == 0) {
    std::fill(points_.begin(), points_.end(), Point{});
    generation_ = 1;
  }
}

void PreciseProjection::record(std::uint32_t sxy, float x, float y) {
  std::size_t slot = slotOf(sxy);
  for (std::size_t probe = 0; probe < kMaxProbe; ++probe, slot = (slot + 1u) & kMask) {
    Point &point = points_[slot];
    if (point.generation != generation_) {
      point = Point{.sxy = sxy, .generation = generation_, .x = x, .y = y};
      return;
    }
    if (point.sxy == sxy) {
      return;
    }
  }
}

const PreciseProjection::Point *PreciseProjection::find(std::uint32_t sxy) const {
  std::size_t slot = slotOf(sxy);
  for (std::size_t probe = 0; probe < kMaxProbe; ++probe, slot = (slot + 1u) & kMask) {
    const Point &point = points_[slot];
    if (point.generation != generation_) {
      return nullptr;
    }
    if (point.sxy == sxy) {
      return &point;
    }
  }
  return nullptr;
}

void PreciseProjection::refine(DrawPrimitive &primitive) const {
  if (primitive.kind != PrimitiveKind::Polygon) {
    return;
  }
  for (int i = 0; i < primitive.vertexCount; ++i) {
    RecordVertex &vertex = primitive.vertices[static_cast<std::size_t>(i)];
    if (const Point *point = find(packedWord(vertex))) {
      vertex.subX = fractionPast(vertex.x, point->x);
      vertex.subY = fractionPast(vertex.y, point->y);
    }
  }
}

PreciseProjection::Capture::Capture(psx::cpu::Gte &gte, PreciseProjection &points) : gte_(gte) {
  gte_.reportPrecise(&recordPoint, &points);
}

PreciseProjection::Capture::~Capture() {
  gte_.reportPrecise(nullptr, nullptr);
}

} // namespace psx::present
