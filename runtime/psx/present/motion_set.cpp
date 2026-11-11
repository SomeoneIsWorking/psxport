#include "motion_set.h"

#include "blend.h"
#include "core.h"
#include "gte_control.h"

#include <lucent/log.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace psx::present {
namespace {

constexpr unsigned kAngle12Bits = 12;
constexpr std::uint32_t kHalfword = 0xFFFFu;

// A halfword's two samples and the ring it wraps at; a zero period takes the later sample.
struct RingHalf {
  std::uint32_t from = 0;
  std::uint32_t to = 0;
  std::uint32_t period = 0;
};

// The halfword `t` of the way round its ring, unrounded; nullopt for a zero period.
std::optional<double> ringExact(const RingHalf &half, float t) {
  if (half.period == 0u) {
    return std::nullopt;
  }
  const auto start = static_cast<std::int32_t>((half.from & kHalfword) % half.period);
  const auto end = static_cast<std::int32_t>((half.to & kHalfword) % half.period);
  return lerpRing(start, end, static_cast<std::int32_t>(half.period), t);
}

std::uint32_t ringHalf(const RingHalf &half, float t) {
  const std::optional<double> exact = ringExact(half, t);
  if (!exact) {
    return half.to & kHalfword;
  }
  const auto ring = static_cast<std::int32_t>(half.period);
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::nearbyint(*exact)) % ring);
}

RingHalf halfOf(const RingHalf &word, Halfword half) {
  const auto shift = static_cast<unsigned>(half);
  return {(word.from >> shift) & kHalfword, (word.to >> shift) & kHalfword, (word.period >> shift) & kHalfword};
}

std::uint32_t ringWord(const RingHalf &word, float t) {
  return ringHalf(halfOf(word, Halfword::Low), t) | ringHalf(halfOf(word, Halfword::High), t) << 16;
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
  case Motion::Low16:
    return (to & ~kHalfword) | lerpHalf(from, to, Halfword::Low, t);
  case Motion::Countdown:
    return static_cast<std::int32_t>(to) <= static_cast<std::int32_t>(from) ? lerpWord(from, to, t) : to;
  case Motion::Angle12x2:
    return lerpTurned(from, to, kAngle12Bits, t) | lerpTurned(from >> 16, to >> 16, kAngle12Bits, t) << 16;
  case Motion::Angle12Low:
    return (to & ~kHalfword) | lerpTurned(from, to, kAngle12Bits, t);
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
  const gte::RotationWords rotation =
      rows_ == Rows::Scaled ? scaledRowsBetween(from, to, t) : rotationBetween(from, to, t);
  std::copy(rotation.begin(), rotation.end(), out.begin());
  if (translation_ == Translation::View) {
    ViewTransform earlier{.rotation = from};
    ViewTransform later{.rotation = to};
    std::copy_n(samples.from.begin() + rotation.size(), gte::kTranslationWords, earlier.translation.begin());
    std::copy_n(samples.to.begin() + rotation.size(), gte::kTranslationWords, later.translation.begin());
    const gte::TranslationWords translation = viewTranslationBetween(earlier, later, rotation, t);
    std::copy(translation.begin(), translation.end(), out.begin() + rotation.size());
    return;
  }
  for (std::size_t i = rotation.size(); i < kWords; ++i) {
    out[i] = lerpWord(samples.from[i], samples.to[i], t);
  }
}

std::uint32_t RingRule::periodsOf(const Samples &samples, std::size_t field) const {
  return periods_ ? (*periods_)[0] | static_cast<std::uint32_t>((*periods_)[1]) << 16 : samples.to[field + 1];
}

void RingRule::blend(const Samples &samples, float t, std::span<std::uint32_t> out) const {
  const std::size_t stride = periods_ ? 1 : 2;
  for (std::size_t i = 0; i + stride <= out.size(); i += stride) {
    out[i] = ringWord({samples.from[i], samples.to[i], periodsOf(samples, i)}, t);
    if (!periods_) {
      out[i + 1] = samples.to[i + 1];
    }
  }
}

std::optional<double> RingRule::exactHalf(const Samples &samples, std::size_t field, Halfword half, float t) const {
  return ringExact(halfOf({samples.from[field], samples.to[field], periodsOf(samples, field)}, half), t);
}

MotionSet::MotionSet(std::span<const MotionTrack> tracks, std::span<MotionSource *const> sources)
    : tracks_(tracks), sources_(sources), tallies_(tracks.size() + sources.size()),
      pending_(tracks.size() + sources.size()) {}

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
    pending_[track] += {.drawn = shown, .unpaired = shown - paired};
  }
  for (std::size_t source = 0; source < sources_.size(); ++source) {
    pending_[tracks_.size() + source] += sources_[source]->write(redraw, t);
  }
}

std::optional<MotionRule::Samples> MotionSet::pairedRecord(const MotionTrack &track, std::uint32_t address) const {
  const auto index = static_cast<std::size_t>(&track - tracks_.data());
  if (!paired() || index >= tracks_.size()) {
    return std::nullopt;
  }
  const auto find = [&](const Sample &sample) -> std::optional<std::span<const std::uint32_t>> {
    const std::size_t begin = index == 0 ? 0 : sample.trackEnds[index - 1];
    const auto first = sample.addresses.begin() + static_cast<std::ptrdiff_t>(begin);
    const auto last = sample.addresses.begin() + static_cast<std::ptrdiff_t>(sample.trackEnds[index]);
    const auto at = std::lower_bound(first, last, address);
    if (at == last || *at != address) {
      return std::nullopt;
    }
    const auto record = static_cast<std::size_t>(at - sample.addresses.begin());
    return std::span(sample.words).subspan(sample.starts[record], recordWords(track));
  };
  const auto earlier = find(from_);
  const auto later = find(to_);
  const std::size_t keys = keyWords(track);
  if (!earlier || !later ||
      !std::equal(earlier->begin(), earlier->begin() + static_cast<std::ptrdiff_t>(keys), later->begin())) {
    return std::nullopt;
  }
  return MotionRule::Samples{.address = address, .from = earlier->subspan(keys), .to = later->subspan(keys)};
}

void MotionSet::settle(bool kept) {
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    if (kept) {
      tallies_[i] += pending_[i];
    }
    pending_[i] = {};
  }
}

void MotionSet::report() const {
  for (std::size_t track = 0; track < tracks_.size(); ++track) {
    lucent::info("motion",
                 "run-end: {}: {} of {} record draw(s) unpaired",
                 tracks_[track].name,
                 tallies_[track].unpaired,
                 tallies_[track].drawn);
  }
  for (std::size_t source = 0; source < sources_.size(); ++source) {
    const MotionCount &tally = tallies_[tracks_.size() + source];
    lucent::info(
        "motion", "run-end: {}: {} of {} item draw(s) unpaired", sources_[source]->name(), tally.unpaired, tally.drawn);
  }
}

} // namespace psx::present
