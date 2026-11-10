// motion_set.h — the guest words an in-between moves: sampled at each real draw section's entry, and written
// blended into a replay of the section (GuestRedraw::writeWord).
#pragma once

#include "guest_redraw.h"

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
  Countdown,  // signed 32-bit while it does not rise; a rise steps
  Angle12x2,  // two 12-bit angles, one per halfword
  Angle12Low, // a 12-bit angle in the low halfword; the high halfword steps
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

// Where a track's records lie: one at the track's address, or a run of them `stride` bytes apart.
struct MotionRecords {
  bool throughPointer = false; // the track's address holds the first record's address
  std::uint32_t stride = 0;
  std::uint32_t count = 1;        // records at most
  std::optional<RecordByte> end;  // a record whose byte is in range ends the run
  std::optional<RecordByte> live; // a record whose byte is out of range holds no object
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

// Inputs a title moves by its own pairing rule where records are not one stride apart or pair by more than
// their keys (pools whose entries name lists of their own). It keeps its last two samples; the set writes it
// only while its tracks pair, so the two are always consecutive.
class MotionSource {
public:
  virtual ~MotionSource() = default;
  // What moves now becomes the later sample; the one before becomes the earlier.
  virtual void sample(Core &core) = 0;
  // The paired inputs `t` of the way from the earlier sample to the later.
  virtual void write(GuestRedraw &redraw, float t) const = 0;
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
  int samples_ = 0;
};

} // namespace psx::present
