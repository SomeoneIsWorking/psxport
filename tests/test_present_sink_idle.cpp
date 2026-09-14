// The sink's idle latch (runtime/psx/gpu_present_sink.h). An unavailable swapchain image is an idle
// field, and only the TRANSITION is reported: this is what turns "a window the compositor is not
// showing" from a hung guest thread (measured: SIGABRT "[watchdog] STUCK: no frame presented", parked in
// the blocking acquire) into a skipped blit.
//
// The SDL boundary itself (sink_acquire) needs a window and a swapchain and is therefore exercised by a
// run, not here; what these cases pin down is the logic the shipping call site depends on, including the
// negative ones: a steady state must announce NOTHING, so a missing latch cannot pass as correct.
#include "gpu_present_sink.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *what) {
  if (!condition) {
    std::cerr << "present_sink_idle: " << what << '\n';
    std::exit(1);
  }
}

// A sink that keeps presenting starts up-to-date and says nothing.
void testSteadyPresentsAreSilent() {
  SinkIdleState s;
  for (int i = 0; i < 6; i++) {
    require(!s.observe(true), "a presented field outside idle is not a transition");
  }
  require(s.presented == 6 && s.skipped == 0, "counts presented fields only");
  require(!s.idle, "a presenting sink is not idle");
}

// Entering idle is announced exactly once, no matter how long it lasts.
void testEnteringIdleIsAnnouncedOnce() {
  SinkIdleState s;
  require(!s.observe(true), "one presented field on its own is not a transition");
  require(s.observe(false), "the first skip after a present is a transition into idle");
  require(s.skipped == 1 && s.presented == 1, "both sides are counted");
  require(s.idle, "the latch is set");
  for (int i = 0; i < 5; i++) {
    require(!s.observe(false), "a continuing idle field is not a transition");
  }
  require(s.skipped == 6, "the skipped count is cumulative, not per transition");
}

// Leaving idle is announced exactly once, and the counts cross the gap.
void testLeavingIdleIsAnnouncedOnce() {
  SinkIdleState s;
  s.observe(true);
  s.observe(false);
  s.observe(false);
  require(s.observe(true), "the first present after idle is a transition out of idle");
  require(!s.idle && s.presented == 2 && s.skipped == 2, "counts carry across the transition");
  require(!s.observe(true), "and the sink is silent again once it is back");
}

// A second idle episode is a second transition: the latch is not a one-shot.
void testIdleCanBeReentered() {
  SinkIdleState s;
  s.observe(false);
  s.observe(true);
  require(s.observe(false), "re-entering idle is announced again");
  require(s.skipped == 2 && s.presented == 1, "counts are cumulative over the sink's whole life");
  require(s.observe(false) == false, "still latched");
}

} // namespace

int main() {
  testSteadyPresentsAreSilent();
  testEnteringIdleIsAnnouncedOnce();
  testLeavingIdleIsAnnouncedOnce();
  testIdleCanBeReentered();
  std::cout << "present_sink_idle: PASS (idle/resume transitions, cumulative counts)\n";
  return 0;
}
