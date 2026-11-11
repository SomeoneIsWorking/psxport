#include "count_histogram.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace psx::debug {

std::size_t CountHistogram::bucketOf(std::uint64_t value) {
  if (value < kSub) {
    return static_cast<std::size_t>(value);
  }
  const unsigned shift = static_cast<unsigned>(std::bit_width(value)) - 1U - kSubBits;
  return kSub + shift * kSub + static_cast<std::size_t>((value >> shift) - kSub);
}

std::uint64_t CountHistogram::bucketTop(std::size_t bucket) {
  if (bucket < kSub) {
    return bucket;
  }
  const std::size_t shift = (bucket - kSub) / kSub;
  const std::uint64_t mantissa = kSub + (bucket - kSub) % kSub;
  return ((mantissa + 1U) << shift) - 1U;
}

void CountHistogram::add(std::uint64_t value) {
  ++buckets_[bucketOf(value)];
  ++count_;
  total_ += value;
  max_ = std::max(max_, value);
}

std::uint64_t CountHistogram::percentile(double fraction) const {
  if (count_ == 0) {
    return 0;
  }
  const auto rank =
      std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(count_))));
  std::uint64_t seen = 0;
  for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
    seen += buckets_[bucket];
    if (seen >= rank) {
      return std::min(bucketTop(bucket), max_);
    }
  }
  return max_;
}

} // namespace psx::debug
