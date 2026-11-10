#include "motion_set.h"

#include "blend.h"
#include "core.h"
#include "gte_control.h"

#include <lucent/log.h>

#include <algorithm>
#include <utility>

namespace psx::present {
namespace {

constexpr std::uint32_t kAngle12 = 0xFFFu;
constexpr std::uint32_t kHalfword = 0xFFFFu;

// A halfword holding a 12-bit angle and the whole turns above it: the short way round, carried into the turns.
std::uint32_t turnedHalf(std::uint32_t from, std::uint32_t to, float t) {
  const auto start = static_cast<std::int32_t>(from & kHalfword);
  const std::int32_t step = ringStep(from & kAngle12, to & kAngle12, kAngle12 + 1);
  return static_cast<std::uint32_t>(lerpInt(start, start + step, t)) & kHalfword;
}

bool holds(const RecordByte &byte, std::uint8_t value) {
  return value >= byte.low && value <= byte.high;
}

std::size_t keyWords(const MotionTrack &track) {
  return track.keys.size();
}

std::size_t recordWords(const MotionTrack &track) {
  return keyWords(track) + track.fields.size();
}

} // namespace

std::uint32_t blendedWord(Motion motion, std::uint32_t from, std::uint32_t to, float t) {
  switch (motion) {
  case Motion::Linear:
    return lerpWord(from, to, t);
  case Motion::Halves:
    return lerpHalves(from, to, t);
  case Motion::Countdown:
    return static_cast<std::int32_t>(to) <= static_cast<std::int32_t>(from) ? lerpWord(from, to, t) : to;
  case Motion::Angle12x2:
    return turnedHalf(from, to, t) | turnedHalf(from >> 16, to >> 16, t) << 16;
  case Motion::Angle12Low:
    return (to & ~kHalfword) | turnedHalf(from, to, t);
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

void TransformRule::blend(const Samples &samples, float t, std::span<std::uint32_t> out) const {
  gte::RotationWords from{};
  gte::RotationWords to{};
  std::copy_n(samples.from.begin(), from.size(), from.begin());
  std::copy_n(samples.to.begin(), to.size(), to.begin());
  const gte::RotationWords rotation = rotationBetween(from, to, t);
  std::copy(rotation.begin(), rotation.end(), out.begin());
  for (std::size_t i = rotation.size(); i < kWords; ++i) {
    out[i] = lerpWord(samples.from[i], samples.to[i], t);
  }
}

MotionSet::MotionSet(std::span<const MotionTrack> tracks, std::span<MotionSource *const> sources)
    : tracks_(tracks), sources_(sources) {}

void MotionSet::sampleTrack(Core &core, const MotionTrack &track, Sample &into) const {
  const MotionRecords &records = track.records;
  const std::uint32_t first = records.throughPointer ? core.mem_r32(track.address) : track.address;
  if (records.liveness != nullptr) {
    records.liveness->sample(core);
  }
  for (std::uint32_t index = 0; index < records.count; ++index) {
    const std::uint32_t record = first + index * records.stride;
    if (!core.isGuestStorage(record, std::max<std::uint32_t>(records.stride, 4u))) {
      return;
    }
    if (records.end && holds(*records.end, core.mem_r8(record + records.end->offset))) {
      return;
    }
    if (records.live && !holds(*records.live, core.mem_r8(record + records.live->offset))) {
      continue;
    }
    if (records.liveness != nullptr && !records.liveness->live(record)) {
      continue;
    }
    into.addresses.push_back(record);
    into.starts.push_back(into.words.size());
    for (const MotionKey &key : track.keys) {
      into.words.push_back(core.mem_r32(record + key.offset) & key.mask);
    }
    for (const MotionField &field : track.fields) {
      into.words.push_back(core.mem_r32(record + field.offset));
    }
  }
}

void MotionSet::sample(Core &core) {
  std::swap(from_, to_);
  to_.addresses.clear();
  to_.starts.clear();
  to_.words.clear();
  to_.trackEnds.clear();
  for (const MotionTrack &track : tracks_) {
    sampleTrack(core, track, to_);
    to_.trackEnds.push_back(to_.addresses.size());
  }
  for (MotionSource *source : sources_) {
    source->sample(core);
  }
  samples_ = samples_ < 2 ? samples_ + 1 : 2;
}

void MotionSet::forget() {
  samples_ = 0;
}

void MotionSet::writeRecord(GuestRedraw &redraw,
                            const MotionTrack &track,
                            std::uint32_t address,
                            std::span<const std::uint32_t> from,
                            std::span<const std::uint32_t> to,
                            float t) const {
  const std::size_t count = track.fields.size();
  if (track.rule != nullptr) {
    ruled_.resize(count);
    track.rule->blend({.address = address, .from = from, .to = to}, t, ruled_);
    for (std::size_t i = 0; i < count; ++i) {
      redraw.writeWord(address + track.fields[i].offset, ruled_[i]);
    }
    return;
  }
  for (std::size_t i = 0; i < count; ++i) {
    const MotionField &field = track.fields[i];
    redraw.writeWord(address + field.offset, blendedWord(field.motion, from[i], to[i], t));
  }
}

void MotionSet::write(GuestRedraw &redraw, float t) const {
  std::size_t fromRecord = 0;
  std::size_t toRecord = 0;
  for (std::size_t track = 0; track < tracks_.size(); ++track) {
    const MotionTrack &motion = tracks_[track];
    const std::size_t words = recordWords(motion);
    const std::size_t keys = keyWords(motion);
    const std::size_t fromEnd = from_.trackEnds[track];
    const std::size_t toEnd = to_.trackEnds[track];
    std::size_t paired = 0;
    const std::size_t shown = toEnd - toRecord;
    // Both runs are in address order.
    while (fromRecord < fromEnd && toRecord < toEnd) {
      const std::uint32_t before = from_.addresses[fromRecord];
      const std::uint32_t after = to_.addresses[toRecord];
      if (before < after) {
        ++fromRecord;
        continue;
      }
      if (after < before) {
        ++toRecord;
        continue;
      }
      const auto earlier = std::span(from_.words).subspan(from_.starts[fromRecord], words);
      const auto later = std::span(to_.words).subspan(to_.starts[toRecord], words);
      if (std::equal(earlier.begin(), earlier.begin() + static_cast<std::ptrdiff_t>(keys), later.begin())) {
        writeRecord(redraw, motion, after, earlier.subspan(keys), later.subspan(keys), t);
        ++paired;
      }
      ++fromRecord;
      ++toRecord;
    }
    fromRecord = fromEnd;
    toRecord = toEnd;
    lucent::debug("motion", "{}: {} of {} records paired at t={:.2f}", motion.name, paired, shown, t);
  }
  for (const MotionSource *source : sources_) {
    source->write(redraw, t);
  }
}

} // namespace psx::present
