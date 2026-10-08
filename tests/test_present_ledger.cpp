// test_present_ledger — the CAPTURED-vs-PRESENTED ledger must report a drop, and must not cry wolf.
//
// This is the gate for the class of bug that has been the most expensive in this project and the least
// visible: a producer pushes prims, something between the push and the rasterizer discards them, and
// every existing instrument stays green because they all count prims that ARRIVE. Measured 2026-08-16
// (kanban #94/#35): the whole 2D panel/prompt/dialog layer was missing from the screen for weeks while
// the producer census reported `prims seen 1728103 = attributed 1708014 + unscoped-native 20089`.
//
// The test asserts BOTH answers, because an instrument that has only ever been seen agreeing is not an
// instrument. It also pins the NEVER-FED wording apart from OK — silence reading like success is what
// let those regressions ship.
#include "present_ledger.h"
#include "testutil.h"

namespace {

void test_a_captured_but_unpresented_layer_is_reported(void) {
  PresentLedger led;
  led.beginFrame();
  led.inRealPresent = true;
  // The exact shape of the panel bug: the 2D chrome is captured, the world is captured AND presented.
  for (int i = 0; i < 21; i++) {
    led.noteCaptured(2); // overlay — captured, never emitted
  }
  for (int i = 0; i < 642; i++) {
    led.noteCaptured(1);
    led.noteEmitted(1);
  } // world — fine
  CHECK_EQ(led.reconcile(/*frame=*/930, /*fatal=*/false), 1);
  CHECK_EQ((int)led.framesDropped, 1);
}

void test_two_dropped_layers_are_both_named(void) {
  PresentLedger led;
  led.beginFrame();
  led.inRealPresent = true;
  for (int i = 0; i < 9; i++) {
    led.noteCaptured(2);
  }
  for (int i = 0; i < 12; i++) {
    led.noteCaptured(3);
  }
  CHECK_EQ(led.reconcile(/*frame=*/1, /*fatal=*/false), 2);
}

void test_a_matched_frame_does_not_cry_wolf(void) {
  PresentLedger led;
  led.beginFrame();
  led.inRealPresent = true;
  for (int i = 0; i < 5; i++) {
    led.noteCaptured(0);
    led.noteEmitted(0);
  }
  for (int i = 0; i < 7; i++) {
    led.noteCaptured(3);
    led.noteEmitted(3);
  }
  CHECK_EQ(led.reconcile(/*frame=*/2, /*fatal=*/false), 0);
  CHECK_EQ((int)led.framesDropped, 0);
}

// A layer PRESENTED without being captured is NOT a drop: the world legitimately arrives from the
// present-time build rather than the capture (docs/one-renderer.md). Asserting exact conservation here
// would make the gate fire on every field frame and be switched off within a day.
void test_presented_without_captured_is_not_a_drop(void) {
  PresentLedger led;
  led.beginFrame();
  led.inRealPresent = true;
  for (int i = 0; i < 640; i++) {
    led.noteEmitted(1); // world from the sink, nothing captured
  }
  CHECK_EQ(led.reconcile(/*frame=*/3, /*fatal=*/false), 0);
}

// Counting only during the real present is what makes the ledger meaningful under fps60, where the
// interpolated present also emits. An emit outside that window must not mask a drop.
void test_emits_outside_the_real_present_do_not_count(void) {
  PresentLedger led;
  led.beginFrame();
  led.inRealPresent = false;
  for (int i = 0; i < 4; i++) {
    led.noteCaptured(2);
  }
  for (int i = 0; i < 4; i++) {
    led.noteEmitted(2); // the in-between drew them; the real frame did not
  }
  led.inRealPresent = true;
  CHECK_EQ(led.reconcile(/*frame=*/4, /*fatal=*/false), 1);
}

void test_the_shipping_selftest_passes(void) {
  CHECK_EQ(PresentLedger::selftest(), 0);
}

} // namespace

int main(void) {
  RUN(a_captured_but_unpresented_layer_is_reported);
  RUN(two_dropped_layers_are_both_named);
  RUN(a_matched_frame_does_not_cry_wolf);
  RUN(presented_without_captured_is_not_a_drop);
  RUN(emits_outside_the_real_present_do_not_count);
  RUN(the_shipping_selftest_passes);
  return pt_summary();
}
