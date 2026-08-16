// test_gte_cross_thread — the GTE binding must follow the GUEST, not the host thread.
//
// Guest code migrates across host threads by design: the scheduler gives each PSX task a Coro and
// Coro::start spawns a real std::thread (scheduler.cpp / pc_scheduler.cpp). So a guest GTE access can
// legitimately arrive on a thread that never called GTE_BindState itself.
//
// This is the gate for a measured incident. beetle-psx 190021cc made gte_cur/gte_default_regs
// _Thread_local; a task fiber then saw gte_cur == NULL, fell back to its own zeroed gte_default_regs
// whose CR/DR are NULL, and the first guest GTE write on that fiber stored through NULL. Effect:
// PSXPORT_ORACLE=1 segfaulted in every 3D scene (8 of 9 replays in Tomba2Engine's library), because
// ORACLE's GATE component runs the recompiled guest bodies inside the fiber rather than intercepting
// them with natives on the scheduler thread. tests/test_gte_isolated.cpp is entirely single-threaded,
// which is exactly why that shipped green.
//
// Note what this asserts: not merely "it does not crash", but that the write LANDS IN THE BOUND STATE.
// Per-thread register files would not crash once initialised — they would silently run guest geometry
// against a private file while the Core's real GTE state sat on another thread, and Coro allocates a
// fresh thread per task start, so that private file would be re-zeroed on every restart. A test that
// only checked for a crash would pass on that quieter, worse bug.
#include "gte_state.h"
#include "testutil.h"

#include <cstdint>
#include <cstring>
#include <thread>

extern "C" {
void     GTE_WriteCR(unsigned int which, uint32_t value);
uint32_t GTE_ReadCR(unsigned int which);
}

namespace {

// A value with bits in both halves, so a half-written or zeroed register is distinguishable from a
// correctly written one.
constexpr uint32_t kSentinel = 0xA5C31007u;
constexpr unsigned kCtrlReg  = 0;   // RT11/RT12 — the first register a libgte MulMatrix touches, and
                                    // the exact one the incident faulted on.

// Everything the worker observed, so a failure says WHICH half broke rather than just "not equal".
struct WorkerResult {
  uint32_t readBack   = 0;      // GTE_ReadCR seen from the worker thread
  const GteRegs* seen = nullptr;  // GTE_CurState() as the worker saw it
};

void writeFromAnotherThread(WorkerResult* out) {
  std::thread worker([out] {
    // Deliberately NO GTE_BindState here: this models a guest task fiber, which never binds.
    GTE_WriteCR(kCtrlReg, kSentinel);
    out->readBack = GTE_ReadCR(kCtrlReg);
    out->seen     = GTE_CurState();
  });
  worker.join();
}

void test_a_write_from_an_unbound_thread_lands_in_the_bound_state(void) {
  GteRegs bound;
  std::memset(&bound, 0, sizeof(bound));
  GTE_BindState(&bound);          // the main/scheduler thread binds, as native_boot.cpp does

  WorkerResult got;
  writeFromAnotherThread(&got);

  // The binding the worker resolved must be the one this thread installed.
  CHECK(got.seen == &bound);
  // The write must be visible in the bound state from THIS thread — the whole point.
  CHECK_EQ((int)bound.CR[kCtrlReg], (int)kSentinel);
  // And the worker's own read-back must agree, i.e. it was not writing to a private file.
  CHECK_EQ((int)got.readBack, (int)kSentinel);
}

// The other half of the contract: a thread that touches the GTE when NOTHING has been bound must still
// get a usable register file. This is the NULL-CR deref itself, isolated — the fallback has to hand
// back a fully initialised file or not exist at all.
void test_the_unbound_fallback_is_never_half_initialised(void) {
  GTE_BindState(nullptr);         // fall back to the default register file

  GteRegs* cur = GTE_CurState();
  CHECK(cur != nullptr);
  CHECK(cur->CR != nullptr);
  CHECK(cur->DR != nullptr);
  CHECK(cur->CR == cur->REG + 32);
  CHECK(cur->DR == cur->REG);

  WorkerResult got;
  writeFromAnotherThread(&got);   // must not fault, and must reach the same default file
  CHECK(got.seen == cur);
  CHECK_EQ((int)got.readBack, (int)kSentinel);
}

}  // namespace

int main(void) {
  RUN(a_write_from_an_unbound_thread_lands_in_the_bound_state);
  RUN(the_unbound_fallback_is_never_half_initialised);
  return pt_summary();
}
