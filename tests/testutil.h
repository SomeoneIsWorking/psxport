// testutil.h — the tiny assertion harness every psxport test uses.
//
// WHY IT EXISTS: a test that asserts nothing must not be able to look green. `assert()` cannot give
// you that — it is compiled out under NDEBUG, it prints nothing on success, and a test whose checks
// never executed exits 0 exactly like a test that passed 40 of them. So every check here is COUNTED,
// the count is printed, and a test function that performed ZERO checks is reported as a FAILURE.
//
// Style follows tools/fmv_export/test_fmv_decode.cpp (the pre-existing hermetic suite in this repo):
// one `static void test_<name>(void)` per case, `RUN(name)` in main, a summary line, exit 1 on any
// failure.
//
// USAGE
//   #include "testutil.h"
//   static void test_thing(void) {
//     CHECK(setup_ok());
//     CHECK_EQ(compute(2), 4);
//   }
//   int main(void) { RUN(thing); return pt_summary(); }
//
// RULES FOR TESTS IN psxport/tests/
//   - HERMETIC ONLY. No disc image, no GPU, no window, no network, no wall-clock sleeps. The three
//     consuming games each need a disc to run at all, so a disc-gated test is useless as a shared
//     gate — and there is deliberately NO skip() here, because a suite that reports "skipped" reads
//     as green while covering nothing. If your check needs a disc, it does not belong in tests/.
//   - Feed the unit its inputs directly and assert on its outputs.
//   - A negative result must carry its denominator: assert the count you scanned, not just "no hits".
//
// The counters are `static` (one copy per translation unit) — a test is expected to be a single .c/
// .cpp file. That is also why this is header-only: no library to link, no build file to edit.
#ifndef PSXPORT_TESTUTIL_H
#define PSXPORT_TESTUTIL_H

#include <stdio.h>
#include <string.h>

/* ---- counters -------------------------------------------------------------------------------- */
static int pt_checks = 0;       /* checks executed across the whole run                            */
static int pt_fails = 0;        /* checks that failed                                              */
static int pt_tests = 0;        /* test functions RUN()                                            */
static int pt_tests_failed = 0; /* test functions that failed or checked nothing                   */
static int pt_case_checks = 0;  /* checks executed inside the CURRENT test function                */
static int pt_expect_fail = 0;  /* self-test only: suppress the FAIL print for a deliberate failure */

/* ---- checks ---------------------------------------------------------------------------------- */
/* Every check increments pt_checks whether it passes or fails — that count is the evidence the test
 * actually ran. A failing check aborts the enclosing test function (like the fmv suite's CHECK), so
 * the test bodies below need no error plumbing. */
#define PT_FAILED(fmt, ...)                                                                                            \
  do {                                                                                                                 \
    ++pt_fails;                                                                                                        \
    if (!pt_expect_fail)                                                                                               \
      fprintf(stderr, "    FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__);                                   \
  } while (0)

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    ++pt_checks;                                                                                                       \
    ++pt_case_checks;                                                                                                  \
    if (!(cond)) {                                                                                                     \
      PT_FAILED("%s", #cond);                                                                                          \
      return;                                                                                                          \
    }                                                                                                                  \
  } while (0)

/* Integer equality. Prints BOTH values (decimal + hex) — "CHECK(a == b)" alone tells you nothing
 * about what a actually was, which is most of the debugging time on a red test. */
#define CHECK_EQ(got, want)                                                                                            \
  do {                                                                                                                 \
    long long pt_g = (long long)(got), pt_w = (long long)(want);                                                       \
    ++pt_checks;                                                                                                       \
    ++pt_case_checks;                                                                                                  \
    if (pt_g != pt_w) {                                                                                                \
      PT_FAILED("%s == %s: got %lld (0x%llx) want %lld (0x%llx)",                                                      \
                #got,                                                                                                  \
                #want,                                                                                                 \
                pt_g,                                                                                                  \
                (unsigned long long)pt_g,                                                                              \
                pt_w,                                                                                                  \
                (unsigned long long)pt_w);                                                                             \
      return;                                                                                                          \
    }                                                                                                                  \
  } while (0)

/* String equality.
 *
 * THE LIFETIME TRAP THIS IS SHAPED AROUND — do not "simplify" it back. The obvious spelling
 *
 *     const char *pt_g = (got), *pt_w = (want);      // statement 1
 *     if (strcmp(pt_g, pt_w) != 0) ...               // statement 2  <-- pt_g is DANGLING
 *
 * is wrong for the most common C++ argument there is, `some_call().c_str()`: the temporary
 * std::string is destroyed at the end of statement 1, so statement 2 compares freed memory. And it
 * fails in the worst possible way — SILENTLY PASSING. A result short enough for the small-string
 * optimisation still lives in the (dead but not yet reused) stack slot and compares equal, while a
 * longer one reads a freed heap block. Measured 2026-08-06 in this very header: the same test
 * passed `"a &amp; b"` (9 chars, inline) and garbled `"say &quot;hi&quot;"` (18 chars, heap) —
 * so the macro's verdict depended on the LENGTH of the answer, not its correctness.
 *
 * The fix is to do the whole comparison inside ONE full-expression, so every temporary in `got` and
 * `want` is still alive: the call below is that expression. Kept as a plain function (not a lambda,
 * not a statement expression) so C tests can use this header too. */
static int pt_streq_failed(
    const char *got_expr, const char *want_expr, const char *got, const char *want, const char *file, int line) {
  if (got && want && strcmp(got, want) == 0) {
    return 0;
  }
  ++pt_fails;
  if (!pt_expect_fail) {
    fprintf(stderr,
            "    FAIL %s:%d: %s == %s: got \"%s\" want \"%s\"\n",
            file,
            line,
            got_expr,
            want_expr,
            got ? got : "(null)",
            want ? want : "(null)");
  }
  return 1;
}

#define CHECK_STREQ(got, want)                                                                                         \
  do {                                                                                                                 \
    ++pt_checks;                                                                                                       \
    ++pt_case_checks;                                                                                                  \
    if (pt_streq_failed(#got, #want, (got), (want), __FILE__, __LINE__))                                               \
      return;                                                                                                          \
  } while (0)

/* Buffer equality; on mismatch names the FIRST differing byte index and both bytes. */
#define CHECK_MEM_EQ(got, want, n)                                                                                     \
  do {                                                                                                                 \
    const unsigned char *pt_g = (const unsigned char *)(got);                                                          \
    const unsigned char *pt_w = (const unsigned char *)(want);                                                         \
    size_t pt_n = (size_t)(n), pt_i;                                                                                   \
    ++pt_checks;                                                                                                       \
    ++pt_case_checks;                                                                                                  \
    for (pt_i = 0; pt_i < pt_n; ++pt_i)                                                                                \
      if (pt_g[pt_i] != pt_w[pt_i]) {                                                                                  \
        PT_FAILED("%s == %s: byte %zu: got 0x%02x want 0x%02x (%zu bytes compared)",                                   \
                  #got,                                                                                                \
                  #want,                                                                                               \
                  pt_i,                                                                                                \
                  pt_g[pt_i],                                                                                          \
                  pt_w[pt_i],                                                                                          \
                  pt_n);                                                                                               \
        break;                                                                                                         \
      }                                                                                                                \
    if (pt_i != pt_n)                                                                                                  \
      return;                                                                                                          \
  } while (0)

/* ---- runner ---------------------------------------------------------------------------------- */
/* RUN(x) calls test_x(). A case that checked nothing is a FAILURE, not a pass: that is the whole
 * point of counting. It catches the empty stub, the case whose body was `#if 0`'d, and the case that
 * returned early before reaching a single assertion. */
#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    int pt_f0 = pt_fails;                                                                                              \
    pt_case_checks = 0;                                                                                                \
    ++pt_tests;                                                                                                        \
    if (!pt_expect_fail)                                                                                               \
      fprintf(stderr, "test %s\n", #name);                                                                             \
    test_##name();                                                                                                     \
    if (pt_fails != pt_f0) {                                                                                           \
      ++pt_tests_failed;                                                                                               \
      if (!pt_expect_fail)                                                                                             \
        fprintf(stderr, "  FAIL (%d checks, %d failed)\n", pt_case_checks, pt_fails - pt_f0);                          \
    } else if (pt_case_checks == 0) {                                                                                  \
      ++pt_tests_failed;                                                                                               \
      ++pt_fails;                                                                                                      \
      if (!pt_expect_fail)                                                                                             \
        fprintf(stderr, "  FAIL: test %s asserted NOTHING (0 checks) - an empty test is not a pass\n", #name);         \
    } else if (!pt_expect_fail) {                                                                                      \
      fprintf(stderr, "  PASS (%d checks)\n", pt_case_checks);                                                         \
    }                                                                                                                  \
  } while (0)

/* The verdict, without printing: 0 = green. A run with NO tests at all is a failure too — an
 * executable that RUN()s nothing would otherwise be a green ctest entry covering zero code. */
static int pt_verdict(void) {
  return (pt_tests == 0 || pt_fails) ? 1 : 0;
}

/* Final line + process exit code; `return pt_summary();` from main. */
static int pt_summary(void) {
  if (pt_tests == 0) {
    fprintf(stderr, "\nFAIL: no tests were run (0 RUN() calls) - this binary gates nothing\n");
  } else {
    /* checks-run and failures are reported side by side rather than subtracted: an empty test case
     * is a failure with no check behind it, so "passed = run - failed" would go negative. */
    fprintf(stderr,
            "\n%d/%d tests passed, %d checks run, %d failed\n",
            pt_tests - pt_tests_failed,
            pt_tests,
            pt_checks,
            pt_fails);
  }
  return pt_verdict();
}

#endif /* PSXPORT_TESTUTIL_H */
