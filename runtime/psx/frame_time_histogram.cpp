#include "frame_time_histogram.h"

#include <algorithm>

namespace psxport::perf {

void FrameTimeHistogram::add(double milliseconds) {
  // A monotonic clock cannot run backwards, but a caller that measured across a reset could hand
  // one over; charge it to the first bucket rather than indexing out of the array.
  const double sample = std::max(milliseconds, 0.0);
  ++count_;
  total_ += sample;
  worst_ = std::max(worst_, sample);
  const auto bucket = (std::size_t)(sample / kBucketMs);
  if (bucket >= kBuckets) {
    ++beyond_;
    return;
  }
  ++buckets_[bucket];
}

double FrameTimeHistogram::meanMs() const {
  return count_ == 0 ? 0.0 : total_ / (double)count_;
}

double FrameTimeHistogram::percentileMs(double fraction) const {
  if (count_ == 0) {
    return 0.0;
  }
  const double clamped = std::clamp(fraction, 0.0, 1.0);
  // The smallest sample position whose cumulative share reaches the fraction.
  const auto wanted = (std::uint64_t)((double)count_ * clamped + 0.5);
  std::uint64_t seen = 0;
  for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
    seen += buckets_[bucket];
    if (seen >= wanted && seen > 0) {
      return (double)(bucket + 1) * kBucketMs;
    }
  }
  return worst_;
}

} // namespace psxport::perf
