// test_coro.cpp — unit test for the Coro fiber primitive (no game deps).
// Build+run: c++ -std=c++17 -pthread runtime/recomp/coro.cpp runtime/recomp/test_coro.cpp -o /tmp/x && /tmp/x
// (the repo build has a `test_coro` target; see cmake/tomba2_port.cmake usage in CI/tools).
#include "coro.h"
#include <cassert>
#include <cstdio>
#include <vector>

static std::vector<int> trace;

int main() {
  // 1) A fiber that yields twice mid-body. Each resume() advances it one segment; the C-stack-local
  //    `phase` MUST survive each yield (the whole point — mid-function resume).
  {
    Coro co;
    co.start([&] {
      int phase = 10;          // a real C local: only correct on resume if the stack is preserved
      trace.push_back(phase);  // 10
      co.yield();
      phase += 1;
      trace.push_back(phase);  // 11
      co.yield();
      phase += 1;
      trace.push_back(phase);  // 12
    });
    assert(!co.started() ? false : true);
    assert(!co.done());          // nothing runs before the first resume
    assert(trace.empty());
    co.resume(); assert(trace.size() == 1 && trace[0] == 10 && !co.done());
    co.resume(); assert(trace.size() == 2 && trace[1] == 11 && !co.done());
    co.resume(); assert(trace.size() == 3 && trace[2] == 12 && co.done());
    co.resume(); assert(trace.size() == 3);   // resume after done is a no-op
  }

  // 2) Deep nested calls across a yield: the resume point is mid-INNER-function, the exact case the
  //    longjmp scheduler couldn't do. The nested frames + locals must all survive.
  {
    trace.clear();
    Coro co;
    auto inner = [&](int base) {
      trace.push_back(base + 1);
      co.yield();                 // suspend 3 frames deep
      trace.push_back(base + 2);  // resume mid-inner-function
    };
    auto mid = [&](int base) { trace.push_back(base); inner(base + 10); trace.push_back(base + 99); };
    co.start([&] { mid(100); });
    co.resume();  // 100, 111
    assert(trace.size() == 2 && trace[0] == 100 && trace[1] == 111 && !co.done());
    co.resume();  // 112, 199 -> body returns
    assert(trace.size() == 4 && trace[2] == 112 && trace[3] == 199 && co.done());
  }

  // 3) Two interleaved fibers driven by a single "scheduler" — exercises the per-task-thread isolation
  //    the cooperative scheduler relies on (resuming A must not disturb B's suspended stack).
  {
    trace.clear();
    Coro a, b;
    a.start([&] { trace.push_back(1); a.yield(); trace.push_back(3); a.yield(); trace.push_back(5); });
    b.start([&] { trace.push_back(2); b.yield(); trace.push_back(4); b.yield(); trace.push_back(6); });
    a.resume(); b.resume();   // 1,2
    a.resume(); b.resume();   // 3,4
    a.resume(); b.resume();   // 5,6
    assert((trace == std::vector<int>{1, 2, 3, 4, 5, 6}));
    assert(a.done() && b.done());
  }

  // 4) cancel() a fiber that is BLOCKED mid-yield: it must unwind to done() so it can be destroyed
  //    without destroying a condvar that still has a waiter (the bug that hung the scheduler at a stage
  //    transition). Locals with destructors on the abandoned frames are fine (none here).
  {
    trace.clear();
    int reached_after = 0;
    {
      Coro co;
      co.start([&] {
        trace.push_back(1);
        co.yield();            // blocks here forever unless cancel()'d
        reached_after = 1;     // must NOT run after cancel
        trace.push_back(2);
      });
      co.resume();             // runs to the yield
      assert(trace.size() == 1 && !co.done());
      co.cancel();             // unwinds the blocked fiber
      assert(co.done());
    }                          // ~Coro joins cleanly (no hang/abort)
    assert(reached_after == 0 && trace.size() == 1);
  }

  // 5) ~Coro on a still-blocked fiber must self-cancel (no explicit cancel) — the scheduler relies on
  //    `delete co` being safe for an abandoned task.
  {
    Coro co;
    co.start([&] { co.yield(); co.yield(); });
    co.resume();               // blocked at first yield
    assert(!co.done());
  }                            // destructor must cancel+join without hanging — reaching here = pass

  printf("test_coro: all passed\n");
  return 0;
}
