// motion_set.h — the guest words an in-between moves: sampled at each real draw section's entry, and written
// blended into a replay of the section (GuestRedraw::writeWord).
#pragma once

#include "blend.h"
#include "guest_redraw.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

class Core;

namespace psx::present {

// How one guest word moves between two samples.
enum class Motion : std::uint8_t {
  Linear,     // signed 32-bit
  Halves,     // two signed 16-bit values
  Low16,      // the low halfword a signed 16-bit value; the high halfword steps
  Countdown,  // signed 32-bit while it does not rise; a rise steps
  Angle12x2,  // two halfwords, each a 12-bit angle with whole turns above it
  Angle12Low, // the low halfword a 12-bit angle with whole turns above it; the high halfword steps
  Angle8x3,   // three 8-bit angles in the low bytes; the top byte steps
  Step,       // the later sample, never blended
};

// One word `t` of the way from `from` to `to` by `motion`; a rule blends its plain words with it.
[[nodiscard]] std::uint32_t blendedWord(Motion motion, std::uint32_t from, std::uint32_t to, float t);

struct MotionField {
  std::uint32_t offset = 0;
  Motion motion = Motion::Linear;
};

// Words that move together by the title's rule (an animation pose) rather than field by field.
class MotionRule {
public:
  virtual ~MotionRule() = default;
  // One record's address and its two samples, in its fields' order.
  struct Samples {
    std::uint32_t address = 0;
    std::span<const std::uint32_t> from;
    std::span<const std::uint32_t> to;
  };
  // `out` is the record's words `t` of the way from `samples.from` to `samples.to`.
  virtual void blend(const Samples &samples, float t, std::span<std::uint32_t> out) const = 0;
};

// A PsyQ MATRIX: five rotation words that turn along the shorter arc (rotationBetween), then three translation
// words that move straight. Its track's fields are transformFields(at), the matrix `at` bytes into the record.
class TransformRule final : public MotionRule {
public:
  static constexpr std::size_t kWords = 8;
  void blend(const Samples &samples, float t, std::span<std::uint32_t> out) const override;
};
[[nodiscard]] constexpr std::array<MotionField, TransformRule::kWords> transformFields(std::uint32_t at) {
  std::array<MotionField, TransformRule::kWords> fields{};
  for (std::uint32_t i = 0; i < TransformRule::kWords; ++i) {
    fields[i] = {.offset = at + 4u * i, .motion = Motion::Linear};
  }
  return fields;
}

// Words of two unsigned halfwords that each wrap at a period (a scroll): the short way round, rounded, wrapped
// into the period. A zero period steps its halfword.
class RingRule final : public MotionRule {
public:
  using Periods = std::array<std::uint16_t, 2>; // low halfword, high halfword
  // Every field wraps at `periods`.
  explicit constexpr RingRule(Periods periods) : periods_(periods) {}
  // The fields come in pairs, a word and then the word holding its periods, which steps.
  constexpr RingRule() = default;
  void blend(const Samples &samples, float t, std::span<std::uint32_t> out) const override;
  // Halfword `half` of field `field` `t` of the way, before blend rounds it; nullopt where it steps. A native
  // emitter that reads the word places its draw by the difference.
  [[nodiscard]] std::optional<double>
  exactHalf(const Samples &samples, std::size_t field, Halfword half, float t) const;

private:
  // Field `field`'s periods, the low halfword's in the low half.
  [[nodiscard]] std::uint32_t periodsOf(const Samples &samples, std::size_t field) const;

  std::optional<Periods> periods_;
};

// A word of a record that names the object it holds: two samples of one address pair only while every key
// word, under its mask, is unchanged.
struct MotionKey {
  std::uint32_t offset = 0;
  std::uint32_t mask = 0xFFFFFFFFu;
};

// A byte of a record and a range of its values.
struct RecordByte {
  std::uint32_t offset = 0;
  std::uint8_t low = 0;
  std::uint8_t high = 0xFF;
};

// Which of a run's records hold an object when no byte of the record says so (a pool's free list).
class MotionLiveness {
public:
  virtual ~MotionLiveness() = default;
  // Before the run's records are read.
  virtual void sample(Core &core) = 0;
  [[nodiscard]] virtual bool live(std::uint32_t record) const = 0;
};

// Where a track's records lie: one at the track's address, or a run of them `stride` bytes apart.
struct MotionRecords {
  bool throughPointer = false; // the track's address holds the first record's address
  std::uint32_t stride = 0;
  std::uint32_t count = 1;        // records at most
  std::optional<RecordByte> end;  // a record whose byte is in range ends the run
  std::optional<RecordByte> live; // a record whose byte is out of range holds no object
  MotionLiveness *liveness = nullptr;
};

// Guest records and the fields of each that move; a ruled track's fields are blended by its rule and their own
// motion is unused. A record pairs with the earlier sample's record at the same address when both are live and
// its keys match; an unpaired record (born, died, reused, re-keyed) is replayed as drawn.
struct MotionTrack {
  const char *name = "";
  std::uint32_t address = 0;
  std::span<const MotionField> fields;
  const MotionRule *rule = nullptr;
  std::span<const MotionKey> keys;
  MotionRecords records;
};

// The items an in-between drew from the later sample, and how many of them had no partner (drawn as now).
struct MotionCount {
  std::uint64_t drawn = 0;
  std::uint64_t unpaired = 0;
  MotionCount &operator+=(const MotionCount &other) {
    drawn += other.drawn;
    unpaired += other.unpaired;
    return *this;
  }
};

// Inputs a title moves by its own pairing rule where records are not one stride apart or pair by more than
// their keys (pools whose entries name lists of their own). It keeps its last two samples; the set writes it
// only while its tracks pair, so the two are always consecutive.
class MotionSource {
public:
  virtual ~MotionSource() = default;
  // The run-end report's name for it.
  [[nodiscard]] virtual const char *name() const = 0;
  // What moves now becomes the later sample; the one before becomes the earlier.
  virtual void sample(Core &core) = 0;
  // The paired inputs `t` of the way from the earlier sample to the later; what the later sample holds and how
  // much of it was unpaired.
  virtual MotionCount write(GuestRedraw &redraw, float t) const = 0;
};

class MotionSet {
public:
  MotionSet(std::span<const MotionTrack> tracks, std::span<MotionSource *const> sources);

  // The tracks' records now become the later sample; the one before becomes the earlier.
  void sample(Core &core);
  // There is no earlier sample until the next one (a cut, a load, a section that did not run last frame).
  void forget();
  [[nodiscard]] bool paired() const {
    return samples_ == 2;
  }
  // Every paired record's fields `t` of the way from the earlier sample to the later, into the replay's
  // guest memory.
  void write(GuestRedraw &redraw, float t) const;
  // The two samples of the record at `address` in `track`, one of the set's, while they pair.
  [[nodiscard]] std::optional<MotionRule::Samples> pairedRecord(const MotionTrack &track, std::uint32_t address) const;
  // The writes since the last call drew an interval that is shown (`kept`) or one the section refused.
  void settle(bool kept);
  // Run end: per track and source, the items the shown in-betweens drew and how many were unpaired (drawn as now).
  void report() const;

private:
  // One sample: per record its address and where its keys' then fields' words start; per track the end of its
  // records.
  struct Sample {
    std::vector<std::uint32_t> addresses;
    std::vector<std::size_t> starts;
    std::vector<std::uint32_t> words;
    std::vector<std::size_t> trackEnds;
  };
  void sampleTrack(Core &core, const MotionTrack &track, Sample &into) const;
  void writeRecord(GuestRedraw &redraw,
                   const MotionTrack &track,
                   std::uint32_t address,
                   std::span<const std::uint32_t> from,
                   std::span<const std::uint32_t> to,
                   float t) const;

  std::span<const MotionTrack> tracks_;
  std::span<MotionSource *const> sources_;
  Sample from_;
  Sample to_;
  mutable std::vector<std::uint32_t> ruled_;
  // Per track, then per source: over the kept intervals' in-betweens, and over the interval being written.
  std::vector<MotionCount> tallies_;
  mutable std::vector<MotionCount> pending_;
  int samples_ = 0;
};

} // namespace psx::present
