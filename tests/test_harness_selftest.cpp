// test_harness_selftest — does the harness itself actually FAIL when it should?
//
// A diagnostic that can only print one answer is not a diagnostic. testutil.h's whole claim is that a
// broken or empty test goes RED, so that claim is asserted here, in the shipping artifact, on every
// ctest run — not just once by hand at the time it was written.
//
// The mechanism: `pt_expect_fail` silences the FAIL text for a deliberately-failing probe (so this
// suite's output is not littered with scary lines that are actually expected), the probe runs, the
// counters it moved are captured, and then the counters are RESTORED so the deliberate failure does
// not poison this run's real tally. If any of the harness's failure paths ever stops firing, these
// cases go red.
#include "testutil.h"

// ---- probes: each is a test body that is WRONG on purpose ---------------------------------------
static void test_probe_failing_check(void) {
  CHECK(1 == 2);                 // must record a failure and return immediately
  CHECK(1 == 1);                 // must NOT be reached (a failing check aborts the case)
}

static void test_probe_asserts_nothing(void) {
  // deliberately empty: RUN() must call this a FAILURE, not a pass
}

// ---- the actual tests ---------------------------------------------------------------------------

// A failing CHECK must (a) be counted as a failure, (b) abort the rest of the test body, and
// (c) make RUN() mark the case failed.
static void test_failing_check_goes_red(void) {
  int s_checks = pt_checks, s_fails = pt_fails, s_tests = pt_tests, s_tf = pt_tests_failed,
      s_cc = pt_case_checks;
  pt_expect_fail = 1;
  RUN(probe_failing_check);
  pt_expect_fail = 0;
  int observed_checks = pt_checks - s_checks;   // 1: the failing one; the second must not run
  int observed_fails  = pt_fails - s_fails;
  int observed_tf     = pt_tests_failed - s_tf;
  pt_checks = s_checks; pt_fails = s_fails; pt_tests = s_tests; pt_tests_failed = s_tf;
  pt_case_checks = s_cc;

  CHECK_EQ(observed_fails, 1);    // the bad check was counted
  CHECK_EQ(observed_checks, 1);   // and it short-circuited the rest of the body
  CHECK_EQ(observed_tf, 1);       // and RUN() marked the case failed
}

// A test body that asserts NOTHING must go red. This is the failure mode `assert()`-based tests
// cannot catch: an empty body exits 0 and reads as a pass.
static void test_empty_case_goes_red(void) {
  int s_checks = pt_checks, s_fails = pt_fails, s_tests = pt_tests, s_tf = pt_tests_failed,
      s_cc = pt_case_checks;
  pt_expect_fail = 1;
  RUN(probe_asserts_nothing);
  pt_expect_fail = 0;
  int observed_fails = pt_fails - s_fails;
  int observed_tf    = pt_tests_failed - s_tf;
  pt_checks = s_checks; pt_fails = s_fails; pt_tests = s_tests; pt_tests_failed = s_tf;
  pt_case_checks = s_cc;

  CHECK_EQ(observed_fails, 1);
  CHECK_EQ(observed_tf, 1);
}

// The value-printing comparators must agree with plain CHECK on the positive side; their negative
// side is exercised by the probe above (CHECK_EQ funnels through the same PT_FAILED path).
static void test_comparators_pass_on_equal(void) {
  CHECK_EQ(2 + 2, 4);
  CHECK_EQ(0xDEADBEEFu, 0xDEADBEEFu);
  CHECK_STREQ("psxport", "psxport");
  const unsigned char a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 4};
  CHECK_MEM_EQ(a, b, sizeof a);
}

// pt_verdict() is what pt_summary() returns to the shell, i.e. what ctest reads as red/green.
// Observations are taken first and the counters restored BEFORE asserting, so a failure of this
// case cannot be swallowed by the restore.
static void test_verdict_exit_code(void) {
  int s_fails = pt_fails, s_tests = pt_tests;
  pt_fails = 0;             int v_clean   = pt_verdict();   // clean run          -> 0
  pt_fails = 3;             int v_failed  = pt_verdict();   // any failed check   -> 1
  pt_tests = 0; pt_fails = 0; int v_notests = pt_verdict(); // no tests run at all -> 1
  pt_fails = s_fails; pt_tests = s_tests;

  CHECK_EQ(v_clean, 0);
  CHECK_EQ(v_failed, 1);
  CHECK_EQ(v_notests, 1);
}

int main(void) {
  RUN(failing_check_goes_red);
  RUN(empty_case_goes_red);
  RUN(comparators_pass_on_equal);
  RUN(verdict_exit_code);
  return pt_summary();
}
