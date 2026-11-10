#include "motion_set.h"

#include "blend.h"
#include "core.h"

#include <utility>

namespace psx::present {
namespace {

std::uint32_t blended(Motion motion, std::uint32_t from, std::uint32_t to, float t) {
  switch (motion) {
  case Motion::Linear:
    return lerpWord(from, to, t);
  case Motion::Angle12x2:
    return lerpTurn(from, to, 12, t) | lerpTurn(from >> 16, to >> 16, 12, t) << 16;
  case Motion::Angle12Low:
    return (to & 0xFFFF0000u) | lerpTurn(from, to, 12, t);
  case Motion::Angle8x3: {
    std::uint32_t word = to & 0xFF000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
      word |= lerpTurn(from >> shift, to >> shift, 8, t) << shift;
    }
    return word;
  }
  case Motion::Step:
    return to;
  }
  return to;
}

} // namespace

MotionSet::MotionSet(std::span<const MotionTrack> tracks) : tracks_(tracks) {
  std::size_t fields = 0;
  for (const MotionTrack &track : tracks_) {
    fields += track.fields.size();
  }
  from_.resize(fields);
  to_.resize(fields);
}

void MotionSet::sample(Core &core) {
  std::swap(from_, to_);
  std::size_t at = 0;
  for (const MotionTrack &track : tracks_) {
    for (const MotionField &field : track.fields) {
      to_[at++] = core.mem_r32(track.address + field.offset);
    }
  }
  samples_ = samples_ < 2 ? samples_ + 1 : 2;
}

void MotionSet::forget() {
  samples_ = 0;
}

void MotionSet::write(GuestRedraw &redraw, float t) const {
  std::size_t at = 0;
  for (const MotionTrack &track : tracks_) {
    if (track.rule != nullptr) {
      const std::size_t count = track.fields.size();
      ruled_.resize(count);
      track.rule->blend(
          {.from = std::span(from_).subspan(at, count), .to = std::span(to_).subspan(at, count)}, t, ruled_);
      for (std::size_t i = 0; i < count; ++i) {
        redraw.writeWord(track.address + track.fields[i].offset, ruled_[i]);
      }
      at += count;
      continue;
    }
    for (const MotionField &field : track.fields) {
      redraw.writeWord(track.address + field.offset, blended(field.motion, from_[at], to_[at], t));
      ++at;
    }
  }
}

} // namespace psx::present
