// The frame-time distribution, tested at the three things a budget depends on: that a percentile
// reads the right frame, that the worst frame is never rounded down, and that a run with frames
// past the histogram's range says so instead of reporting its last bucket as the maximum.
#include "frame_time_histogram.h"

#include "testutil.h"

namespace {

using psxport::perf::FrameTimeHistogram;

void test_an_empty_distribution_answers_zero_and_says_it_is_empty(void) {
  FrameTimeHistogram histogram;
  CHECK_EQ((long long)histogram.count(), 0);
  CHECK_EQ((long long)histogram.beyondRange(), 0);
  CHECK(histogram.meanMs() == 0.0);
  CHECK(histogram.worstMs() == 0.0);
  // Zero is also what a run of instant frames would answer, which is why the count is what tells
  // a reader the difference.
  CHECK(histogram.percentileMs(0.99) == 0.0);
}

void test_a_percentile_reads_the_frame_at_that_position(void) {
  FrameTimeHistogram histogram;
  // Ninety-nine frames at 4 ms and one at 20 ms: the median is fast and the worst is not.
  for (int i = 0; i < 99; ++i) {
    histogram.add(4.0);
  }
  histogram.add(20.0);
  CHECK_EQ((long long)histogram.count(), 100);
  CHECK(histogram.percentileMs(0.5) <= 4.25);
  CHECK(histogram.percentileMs(0.95) <= 4.25);
  // The hundredth frame is the slow one, so the top of the distribution must find it.
  CHECK(histogram.percentileMs(1.0) >= 20.0);
  CHECK(histogram.worstMs() == 20.0);
  CHECK(histogram.meanMs() > 4.0);
  CHECK(histogram.meanMs() < 5.0);
}

void test_a_percentile_never_reads_faster_than_the_bucket_it_lands_in(void) {
  FrameTimeHistogram histogram;
  histogram.add(3.3);
  // 3.3 ms lands in the bucket that ENDS at 3.5, and the answer is that upper bound: rounding the
  // other way would report a budget as met by a frame that missed it.
  CHECK(histogram.percentileMs(1.0) >= 3.3);
  CHECK(histogram.percentileMs(1.0) <= 3.5);
}

void test_a_frame_past_the_range_is_counted_and_keeps_its_exact_time(void) {
  FrameTimeHistogram histogram;
  for (int i = 0; i < 9; ++i) {
    histogram.add(5.0);
  }
  const double absurd = FrameTimeHistogram::kRangeMs + 71.5;
  histogram.add(absurd);
  CHECK_EQ((long long)histogram.count(), 10);
  CHECK_EQ((long long)histogram.beyondRange(), 1);
  // Not the last bucket, which is what folding it in would have reported.
  CHECK(histogram.worstMs() == absurd);
  CHECK(histogram.percentileMs(1.0) == absurd);
  CHECK(histogram.percentileMs(0.5) <= 5.25);
}

void test_every_frame_past_the_range_still_reports_a_worst(void) {
  FrameTimeHistogram histogram;
  histogram.add(FrameTimeHistogram::kRangeMs + 1.0);
  histogram.add(FrameTimeHistogram::kRangeMs + 9.0);
  CHECK_EQ((long long)histogram.beyondRange(), 2);
  CHECK(histogram.percentileMs(0.5) == FrameTimeHistogram::kRangeMs + 9.0);
  CHECK(histogram.worstMs() == FrameTimeHistogram::kRangeMs + 9.0);
}

void test_a_backwards_sample_is_charged_to_the_first_bucket_not_out_of_the_array(void) {
  FrameTimeHistogram histogram;
  histogram.add(-4.0);
  CHECK_EQ((long long)histogram.count(), 1);
  CHECK_EQ((long long)histogram.beyondRange(), 0);
  CHECK(histogram.worstMs() == 0.0);
  CHECK(histogram.percentileMs(1.0) == FrameTimeHistogram::kBucketMs);
}

} // namespace

int main(void) {
  RUN(an_empty_distribution_answers_zero_and_says_it_is_empty);
  RUN(a_percentile_reads_the_frame_at_that_position);
  RUN(a_percentile_never_reads_faster_than_the_bucket_it_lands_in);
  RUN(a_frame_past_the_range_is_counted_and_keeps_its_exact_time);
  RUN(every_frame_past_the_range_still_reports_a_worst);
  RUN(a_backwards_sample_is_charged_to_the_first_bucket_not_out_of_the_array);
  return pt_summary();
}
