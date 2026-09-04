// test_coro — unit test for the Coro fiber primitive (runtime/psx/coro.{h,cpp}), the mechanism the
// cooperative scheduler resumes a guest task on. Hermetic: no disc, no GPU, no window, no game.
//
// It was previously an orphan built by a comment-in-the-header command line and asserted with
// assert() — which compiles out under NDEBUG, so it could have "passed" while checking nothing. It now
// runs under ctest through testutil.h, where every check is counted.
#include "testutil.h"

#include "coro.h"

#include <vector>

static std::vector<int> trace;

// A fiber that yields twice mid-body. Each resume() advances it one segment; the C-stack-local
// `phase` MUST survive each yield — that is the whole point of a fiber over a longjmp scheduler.
static void test_midbody_yield_preserves_locals(void) {
  trace.clear();
  Coro co;
  co.start([&] {
    int phase = 10;         // a real C local: only correct on resume if the stack is preserved
    trace.push_back(phase); // 10
    co.yield();
    phase += 1;
    trace.push_back(phase); // 11
    co.yield();
    phase += 1;
    trace.push_back(phase); // 12
  });
  CHECK(co.started());
  CHECK(!co.done()); // nothing runs before the first resume
  CHECK_EQ(trace.size(), 0u);

  co.resume();
  CHECK_EQ(trace.size(), 1u);
  CHECK_EQ(trace[0], 10);
  CHECK(!co.done());
  co.resume();
  CHECK_EQ(trace.size(), 2u);
  CHECK_EQ(trace[1], 11);
  CHECK(!co.done());
  co.resume();
  CHECK_EQ(trace.size(), 3u);
  CHECK_EQ(trace[2], 12);
  CHECK(co.done());
  co.resume();
  CHECK_EQ(trace.size(), 3u); // resume after done is a no-op
}

// Deep nested calls across a yield: the resume point is mid-INNER-function, the exact case the
// longjmp scheduler could not do. The nested frames + their locals must all survive.
static void test_yield_from_nested_frames(void) {
  trace.clear();
  Coro co;
  auto inner = [&](int base) {
    trace.push_back(base + 1);
    co.yield();                // suspend 3 frames deep
    trace.push_back(base + 2); // resume mid-inner-function
  };
  auto mid = [&](int base) {
    trace.push_back(base);
    inner(base + 10);
    trace.push_back(base + 99);
  };
  co.start([&] {
    mid(100);
  });

  co.resume(); // 100, 111
  CHECK_EQ(trace.size(), 2u);
  CHECK_EQ(trace[0], 100);
  CHECK_EQ(trace[1], 111);
  CHECK(!co.done());
  co.resume(); // 112, 199 -> body returns
  CHECK_EQ(trace.size(), 4u);
  CHECK_EQ(trace[2], 112);
  CHECK_EQ(trace[3], 199);
  CHECK(co.done());
}

// Two interleaved fibers driven by one "scheduler" — resuming A must not disturb B's suspended stack.
static void test_two_fibers_interleave(void) {
  trace.clear();
  Coro a, b;
  a.start([&] {
    trace.push_back(1);
    a.yield();
    trace.push_back(3);
    a.yield();
    trace.push_back(5);
  });
  b.start([&] {
    trace.push_back(2);
    b.yield();
    trace.push_back(4);
    b.yield();
    trace.push_back(6);
  });
  a.resume();
  b.resume(); // 1,2
  a.resume();
  b.resume(); // 3,4
  a.resume();
  b.resume(); // 5,6
  const std::vector<int> want{1, 2, 3, 4, 5, 6};
  CHECK_EQ(trace.size(), want.size());
  for (size_t i = 0; i < want.size(); i++) {
    CHECK_EQ(trace[i], want[i]);
  }
  CHECK(a.done());
  CHECK(b.done());
}

// cancel() on a fiber BLOCKED mid-yield must unwind it to done() so it can be destroyed without
// destroying a condvar that still has a waiter (the bug that hung the scheduler at a stage transition).
static void test_cancel_unwinds_blocked_fiber(void) {
  trace.clear();
  int reached_after = 0;
  {
    Coro co;
    co.start([&] {
      trace.push_back(1);
      co.yield();        // blocks here forever unless cancel()'d
      reached_after = 1; // must NOT run after cancel
      trace.push_back(2);
    });
    co.resume(); // runs to the yield
    CHECK_EQ(trace.size(), 1u);
    CHECK(!co.done());
    co.cancel(); // unwinds the blocked fiber
    CHECK(co.done());
  } // ~Coro joins cleanly (no hang/abort)
  CHECK_EQ(reached_after, 0);
  CHECK_EQ(trace.size(), 1u);
}

// ~Coro on a still-blocked fiber must self-cancel (no explicit cancel) — the scheduler relies on
// `delete co` being safe for an abandoned task. Reaching the check below without hanging IS the result;
// if the destructor deadlocks, ctest's per-test TIMEOUT reports it rather than wedging the run.
static void test_destructor_self_cancels(void) {
  int destructed = 0;
  {
    Coro co;
    co.start([&] {
      co.yield();
      co.yield();
    });
    co.resume(); // blocked at first yield
    CHECK(!co.done());
  }
  destructed = 1; // only reached if ~Coro cancelled + joined instead of hanging
  CHECK_EQ(destructed, 1);
}

int main(void) {
  RUN(midbody_yield_preserves_locals);
  RUN(yield_from_nested_frames);
  RUN(two_fibers_interleave);
  RUN(cancel_unwinds_blocked_fiber);
  RUN(destructor_self_cancels);
  return pt_summary();
}
