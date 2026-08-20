// test_sbs_pane_layout.cpp — pin WHICH core's picture you are looking at, and where.
//
// The SBS harness runs two Games in one process: core A is the port under test, core B is the recomp
// oracle. The window must show BOTH, A left and B right. gpu_vk.cpp used to carry a stopgap that
// presented core A's frame over the whole window and dropped B entirely; the pane geometry it stood in
// for now lives in runtime/recomp/sbs_pane_layout.h, which is pure integer geometry and therefore
// testable with no GPU, no window and no disc.
//
// The cases below are written so that the stopgap rule ("pane = the whole window, for A only") FAILS
// them: A must not reach the right half, B must not be empty, and the two must not overlap.
#include "../runtime/recomp/sbs_pane_layout.h"
#include "testutil.h"

// A 4:3 pane in a 16:9-ish window column is pillarboxed; a pane wider than its column is letterboxed.
static void test_letterbox_fits_and_centres(void) {
  // Source wider than the box -> bars top and bottom, full width.
  PaneRect wide = pane_letterbox(640, 240, 640, 480);
  CHECK_EQ(wide.w, 640);
  CHECK_EQ(wide.h, 240);
  CHECK_EQ(wide.x, 0);
  CHECK_EQ(wide.y, 120);
  // Source taller than the box -> bars left and right, full height.
  PaneRect tall = pane_letterbox(320, 240, 640, 240);
  CHECK_EQ(tall.h, 240);
  CHECK_EQ(tall.w, 320);
  CHECK_EQ(tall.x, 160);
  CHECK_EQ(tall.y, 0);
  // Exact aspect match -> fills the box, no bars at all.
  PaneRect exact = pane_letterbox(320, 240, 640, 480);
  CHECK_EQ(exact.x, 0);
  CHECK_EQ(exact.y, 0);
  CHECK_EQ(exact.w, 640);
  CHECK_EQ(exact.h, 480);
}

// The layout must never scale a pane up or down past its column: it fits INSIDE, always.
static void test_letterbox_never_exceeds_the_box(void) {
  const int boxes[][2] = {{640, 480}, {1280, 720}, {960, 540}, {300, 900}, {7, 5}};
  const int srcs[][2] = {{320, 240}, {368, 240}, {640, 480}, {512, 256}, {1, 1}};
  int scanned = 0;
  for (unsigned b = 0; b < sizeof boxes / sizeof boxes[0]; b++) {
    for (unsigned s = 0; s < sizeof srcs / sizeof srcs[0]; s++) {
      PaneRect r = pane_letterbox(srcs[s][0], srcs[s][1], boxes[b][0], boxes[b][1]);
      CHECK(r.x >= 0 && r.y >= 0);
      CHECK(r.x + r.w <= boxes[b][0]);
      CHECK(r.y + r.h <= boxes[b][1]);
      scanned++;
    }
  }
  CHECK_EQ(scanned, 25); // the denominator: 5 boxes x 5 sources, all checked
}

// THE regression the stopgap was: core A's frame took the whole window. A belongs in the LEFT half.
static void test_pane_a_stays_in_the_left_half(void) {
  const int winW = 1280, winH = 480;
  PaneRect a = sbs_pane_rect(SBS_PANE_A, 320, 240, winW, winH);
  CHECK(a.w > 0 && a.h > 0);
  CHECK(a.x >= 0);
  CHECK(a.x + a.w <= winW / 2); // fails outright under "present A fullscreen"
}

// ...and core B must actually be drawn, in the RIGHT half. Under the stopgap B had no rect at all.
static void test_pane_b_stays_in_the_right_half(void) {
  const int winW = 1280, winH = 480;
  PaneRect b = sbs_pane_rect(SBS_PANE_B, 320, 240, winW, winH);
  CHECK(b.w > 0 && b.h > 0);
  CHECK(b.x >= winW / 2);
  CHECK(b.x + b.w <= winW);
}

// The two panes must be disjoint: if they overlap, one core's picture is hiding the other's, which is
// exactly the failure the differential harness exists to make visible.
static void test_panes_do_not_overlap(void) {
  const int winW = 1000, winH = 400;
  // Deliberately mismatched sources: a widescreen port pane next to a 4:3 oracle pane.
  PaneRect a = sbs_pane_rect(SBS_PANE_A, 368, 240, winW, winH);
  PaneRect b = sbs_pane_rect(SBS_PANE_B, 320, 240, winW, winH);
  CHECK(a.x + a.w <= b.x);
  // Each keeps its OWN aspect rather than being forced to a shared one. Both sources are taller than
  // 500x400 allows, so both fill the column width — and the WIDER source ends up SHORTER. Were the two
  // forced to one shared aspect these heights would be equal.
  CHECK_EQ(a.w, b.w);
  CHECK(a.h < b.h);
}

// Pane index is the only thing that moves a pane horizontally; the fitted size is index-independent.
static void test_pane_index_only_shifts_x(void) {
  const int winW = 800, winH = 300;
  PaneRect a = sbs_pane_rect(SBS_PANE_A, 320, 240, winW, winH);
  PaneRect b = sbs_pane_rect(SBS_PANE_B, 320, 240, winW, winH);
  CHECK_EQ(a.w, b.w);
  CHECK_EQ(a.h, b.h);
  CHECK_EQ(a.y, b.y);
  CHECK_EQ(b.x - a.x, winW / 2);
  CHECK_EQ(SBS_PANE_COUNT, 2);
}

int main(void) {
  RUN(letterbox_fits_and_centres);
  RUN(letterbox_never_exceeds_the_box);
  RUN(pane_a_stays_in_the_left_half);
  RUN(pane_b_stays_in_the_right_half);
  RUN(panes_do_not_overlap);
  RUN(pane_index_only_shifts_x);
  return pt_summary();
}
