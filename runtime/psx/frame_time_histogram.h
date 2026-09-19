// A constant-memory distribution of frame times, so a frame-time budget can be answered with
// percentiles instead of an average.
//
// WHY NOT A LIST OF SAMPLES. A profiler that is on for a play session must not grow without bound;
// at 30 game updates a second an hour of play is over a hundred thousand samples. Buckets cost the
// same memory whether the run is a ten-second replay or an afternoon.
//
// WHY THE WORST SAMPLE IS KEPT EXACTLY. A percentile read out of a bucket is only as precise as the
// bucket, and the one number a budget cannot afford to round DOWN is the worst frame. A sample past
// the last bucket is counted separately and reported, so a run with frames slower than the
// histogram's range says so rather than quietly reporting the last bucket as its maximum.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace psxport::perf {

class FrameTimeHistogram {
public:
  // A quarter of a millisecond across 512 buckets covers 0 to 128 ms, which spans everything from a
  // frame with headroom to spare to one four times over a 30 Hz budget.
  static constexpr double kBucketMs = 0.25;
  static constexpr std::size_t kBuckets = 512;
  static constexpr double kRangeMs = kBucketMs * (double)kBuckets;

  void add(double milliseconds);

  std::uint64_t count() const {
    return count_;
  }
  // Frames slower than the histogram's range. Reported rather than folded into the last bucket,
  // because "0 frames past 128 ms" and "the last bucket is full" are different answers.
  std::uint64_t beyondRange() const {
    return beyond_;
  }
  double worstMs() const {
    return worst_;
  }
  double meanMs() const;

  // The time at or below which `fraction` of the frames fell, rounded UP to a bucket boundary. When
  // the answer lies past the histogram's range the true worst sample is returned, so a percentile
  // can never read faster than a frame that actually happened. An empty histogram answers zero,
  // which its zero `count()` is what distinguishes from a run of instant frames.
  double percentileMs(double fraction) const;

private:
  std::array<std::uint64_t, kBuckets> buckets_{};
  std::uint64_t beyond_ = 0;
  std::uint64_t count_ = 0;
  double worst_ = 0.0;
  double total_ = 0.0;
};

} // namespace psxport::perf
