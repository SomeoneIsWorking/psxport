// test_video_plan — the internal-resolution scale is a function of the sink, never of a window.
#include "testutil.h"
#include "video_plan.h"

namespace {

// The player's ires setting (0 = auto) and the device's cap.
struct Ires {
  int setting = 0;
  int cap = 0;
};

VideoInputs mk(int sinkH, Ires ires) {
  VideoInputs v;
  v.sinkW = sinkH * 4 / 3;
  v.sinkH = sinkH;
  v.modsIres = ires.setting;
  v.iresCap = ires.cap;
  return v;
}

void test_a_960x720_sink_is_ires_3(void) {
  CHECK_EQ(psx::present::iresScale(mk(720, {0, 8})), 3);
}

void test_auto_ires_derives_from_the_sink_height(void) {
  struct {
    int h, want;
  } t[] = {{240, 1}, {360, 2}, {480, 2}, {600, 3}, {720, 3}, {1080, 5}, {2160, 9}};
  int checked = 0;
  for (auto &e : t) {
    CHECK_EQ(psx::present::iresScale(mk(e.h, {0, 16})), e.want);
    ++checked;
  }
  CHECK_EQ(checked, 7);
}

void test_a_fixed_ires_ignores_the_sink_entirely(void) {
  for (int fixed = 1; fixed <= 4; ++fixed) {
    CHECK_EQ(psx::present::iresScale(mk(240, {fixed, 8})), fixed);
    CHECK_EQ(psx::present::iresScale(mk(2160, {fixed, 8})), fixed);
  }
}

void test_the_memory_cap_clamps_both_auto_and_fixed(void) {
  CHECK_EQ(psx::present::iresScale(mk(2160, {0, 2})), 2);
  CHECK_EQ(psx::present::iresScale(mk(720, {4, 2})), 2);
  CHECK_EQ(psx::present::iresScale(mk(720, {4, 0})), 1); // a nonsense cap floors at 1
}

} // namespace

int main(void) {
  RUN(a_960x720_sink_is_ires_3);
  RUN(auto_ires_derives_from_the_sink_height);
  RUN(a_fixed_ires_ignores_the_sink_entirely);
  RUN(the_memory_cap_clamps_both_auto_and_fixed);
  return psx::test::summary();
}
