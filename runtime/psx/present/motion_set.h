// motion_set.h — the guest words an in-between moves: sampled at each real draw section's entry, and written
// blended into a replay of the section (GuestRedraw::writeWord).
#pragma once

#include "guest_redraw.h"

#include <cstdint>
#include <span>
#include <vector>

class Core;

namespace psx::present {

// How one guest word moves between two samples.
enum class Motion : std::uint8_t {
  Linear,     // signed 32-bit
  Angle12x2,  // two 12-bit angles, one per halfword
  Angle12Low, // a 12-bit angle in the low halfword; the high halfword steps
  Angle8x3,   // three 8-bit angles in the low bytes; the top byte steps
  Step,       // the later sample, never blended
};

struct MotionField {
  std::uint32_t offset = 0;
  Motion motion = Motion::Linear;
};

// Words that move together by the title's rule (an animation pose) rather than field by field.
class MotionRule {
public:
  virtual ~MotionRule() = default;
  // A ruled track's two samples, in its fields' order.
  struct Samples {
    std::span<const std::uint32_t> from;
    std::span<const std::uint32_t> to;
  };
  // `out` is the track's words `t` of the way from `samples.from` to `samples.to`.
  virtual void blend(const Samples &samples, float t, std::span<std::uint32_t> out) const = 0;
};

// A guest record at a fixed address and the fields of it that move; a ruled track's fields are blended by its
// rule and their own motion is unused.
struct MotionTrack {
  const char *name = "";
  std::uint32_t address = 0;
  std::span<const MotionField> fields;
  const MotionRule *rule = nullptr;
};

class MotionSet {
public:
  explicit MotionSet(std::span<const MotionTrack> tracks);

  // The tracks' words now become the later sample; the one before becomes the earlier.
  void sample(Core &core);
  // There is no earlier sample until the next one (a cut, a load, a section that did not run last frame).
  void forget();
  [[nodiscard]] bool paired() const {
    return samples_ == 2;
  }
  // Every field `t` of the way from the earlier sample to the later, into the replay's guest memory.
  void write(GuestRedraw &redraw, float t) const;

private:
  std::span<const MotionTrack> tracks_;
  std::vector<std::uint32_t> from_;
  std::vector<std::uint32_t> to_;
  mutable std::vector<std::uint32_t> ruled_;
  int samples_ = 0;
};

} // namespace psx::present
