#include "active_low_edges.h"
#include "testutil.h"

static void test_press_hold_release_and_reset() {
  ActiveLowEdges edges;
  constexpr uint16_t Start = 0x0008u;
  constexpr uint16_t Cross = 0x4000u;

  edges.sample(0xFFFFu);
  CHECK(edges.pressed() == 0u);
  CHECK(edges.released() == 0u);

  edges.sample((uint16_t)(0xFFFFu & ~Start));
  CHECK(edges.pressed(Start));
  CHECK(edges.pressed() == Start);
  CHECK(!edges.released(Start));

  edges.sample((uint16_t)(0xFFFFu & ~Start));
  CHECK(!edges.pressed(Start));
  CHECK(edges.pressed() == 0u);

  edges.sample(0xFFFFu);
  CHECK(edges.released(Start));
  CHECK(!edges.pressed(Start));

  edges.sample((uint16_t)(0xFFFFu & ~(Start | Cross)));
  CHECK(edges.pressed() == (Start | Cross));

  edges.reset((uint16_t)(0xFFFFu & ~Start));
  CHECK(edges.pressed() == 0u);
  CHECK(edges.released() == 0u);
  edges.sample((uint16_t)(0xFFFFu & ~Start));
  CHECK(!edges.pressed(Start));
  edges.sample(0xFFFFu);
  edges.sample((uint16_t)(0xFFFFu & ~Start));
  CHECK(edges.pressed(Start));
}

int main() {
  RUN(press_hold_release_and_reset);
  return pt_summary();
}
