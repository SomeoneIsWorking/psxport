#include "standalone_frame_boundary.h"
#include "testutil.h"

#include <string_view>
#include <vector>

static void test_pending_frame_is_presented_before_a_cold_warp() {
  std::vector<std::string_view> events;
  standalone_frame_boundary(
      [&] {
        events.push_back("present-pending");
      },
      [&] {
        events.push_back("begin-capture");
      },
      [&] {
        events.push_back("apply-warp");
      },
      [&] {
        events.push_back("run-guest-frame");
      });

  const std::vector<std::string_view> expected{"present-pending", "begin-capture", "apply-warp", "run-guest-frame"};
  CHECK_EQ(events.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(events[i] == expected[i]);
  }
}

static void test_unarmed_warp_still_occupies_the_same_boundary_phase() {
  std::vector<std::string_view> events;
  standalone_frame_boundary(
      [&] {
        events.push_back("present-pending");
      },
      [&] {
        events.push_back("begin-capture");
      },
      [&] {
        events.push_back("warp-check-unarmed");
      },
      [&] {
        events.push_back("run-guest-frame");
      });

  const std::vector<std::string_view> expected{
      "present-pending", "begin-capture", "warp-check-unarmed", "run-guest-frame"};
  CHECK_EQ(events.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(events[i] == expected[i]);
  }
}

int main() {
  RUN(pending_frame_is_presented_before_a_cold_warp);
  RUN(unarmed_warp_still_occupies_the_same_boundary_phase);
  return pt_summary();
}
