#include "testutil.h"
#include "wide_margin_plan.h"

static void test_spyro_extension_only(void) {
  const WideMarginPlan p = plan_wide_margin(0, 248, 512, 684, 240);
  CHECK(p.draw);
  CHECK_EQ(p.x0, 512);
  CHECK_EQ(p.x1, 684);
  CHECK_EQ(p.y0, 248);
  CHECK_EQ(p.y1, 488);
}

static void test_origin_is_preserved(void) {
  const WideMarginPlan p = plan_wide_margin(32, 8, 320, 428, 224);
  CHECK(p.draw);
  CHECK_EQ(p.x0, 352);
  CHECK_EQ(p.x1, 460);
  CHECK_EQ(p.y0, 8);
  CHECK_EQ(p.y1, 232);
}

static void test_4_3_and_invalid_are_noops(void) {
  CHECK(!plan_wide_margin(0, 0, 512, 512, 240).draw);
  CHECK(!plan_wide_margin(0, 0, 512, 400, 240).draw);
  CHECK(!plan_wide_margin(0, 0, 0, 684, 240).draw);
  CHECK(!plan_wide_margin(0, 0, 512, 684, 0).draw);
}

int main(void) {
  RUN(spyro_extension_only);
  RUN(origin_is_preserved);
  RUN(4_3_and_invalid_are_noops);
  return pt_summary();
}
