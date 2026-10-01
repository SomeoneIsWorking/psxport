// test_session_control.cpp — a Game asks its host to end it and show the title selector again.
// A product without a selector must be unable to make that request, so the ESC row and the control
// channel's `session return` refuse there instead of ending a session nobody can resume.
#include "../runtime/psx/session_control.h"
#include "testutil.h"

static void test_refused_without_a_selector(void) {
  SessionControl session;
  CHECK(!session.returnAvailable());
  CHECK(!session.requestReturn());
  CHECK(!session.returnRequested());
}

static void test_recorded_with_a_selector(void) {
  SessionControl session;
  session.setReturnAvailable(true);
  CHECK(!session.returnRequested());
  CHECK(session.requestReturn());
  CHECK(session.returnRequested());
}

int main(void) {
  RUN(refused_without_a_selector);
  RUN(recorded_with_a_selector);
  return pt_summary();
}
