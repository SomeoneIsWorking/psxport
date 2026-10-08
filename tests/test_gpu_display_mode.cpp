// GP1(08h) horizontal resolution is a split three-bit field. This exercises the pure decoder used
// by the shipping GPU and includes the previously ignored HRES2 opposite answer.
#include "gpu_display_mode.h"
#include "testutil.h"

namespace {

void test_low_resolution_bits_select_the_four_ordinary_modes() {
  CHECK_EQ(gp1_display_width(0), 256);
  CHECK_EQ(gp1_display_width(1), 320);
  CHECK_EQ(gp1_display_width(2), 512);
  CHECK_EQ(gp1_display_width(3), 640);
}

void test_hres2_selects_368_independently_of_the_low_bits() {
  for (uint32_t low = 0; low < 4; ++low) {
    CHECK_EQ(gp1_display_width((1u << 6) | low), 368);
  }
}

} // namespace

int main() {
  RUN(low_resolution_bits_select_the_four_ordinary_modes);
  RUN(hres2_selects_368_independently_of_the_low_bits);
  return pt_summary();
}
