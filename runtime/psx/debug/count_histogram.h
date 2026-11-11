// count_histogram.h — a constant-memory distribution of work counts (instructions, draws, pixels) for percentiles.
// Exact below 64; above, 64 buckets per power of two, so a percentile is at most 1.6% above the true value.
// The maximum and the total are exact.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace psx::debug {

class CountHistogram {
public:
  void add(std::uint64_t value);

  [[nodiscard]] std::uint64_t count() const {
    return count_;
  }
  [[nodiscard]] std::uint64_t total() const {
    return total_;
  }
  [[nodiscard]] std::uint64_t max() const {
    return max_;
  }
  // The top of the bucket `fraction` of the samples fall at or below, never above max(); 0 when empty.
  [[nodiscard]] std::uint64_t percentile(double fraction) const;

private:
  static constexpr unsigned kSubBits = 6;
  static constexpr std::size_t kSub = std::size_t{1} << kSubBits;
  static constexpr std::size_t kBuckets = kSub + (64 - kSubBits) * kSub;
  static std::size_t bucketOf(std::uint64_t value);
  static std::uint64_t bucketTop(std::size_t bucket);

  std::array<std::uint32_t, kBuckets> buckets_{};
  std::uint64_t count_ = 0;
  std::uint64_t total_ = 0;
  std::uint64_t max_ = 0;
};

} // namespace psx::debug
