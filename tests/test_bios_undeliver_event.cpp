// test_bios_undeliver_event.cpp — B0:0x20 UnDeliverEvent returns a delivered, handler-less event to not-fired.
#include "game.h"
#include "hle.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kClass = 0xF0000009u;
constexpr uint32_t kSpec = 0x0020u;

uint32_t biosB0(Game &g, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  Core &c = g.core;
  c.r[4] = a0;
  c.r[5] = a1;
  c.r[6] = a2;
  c.r[2] = 0xDEADBEEFu;
  return g.hle.dispatchBios('B', fn) ? c.r[2] : 0xDEADBEEFu;
}

uint32_t openEnabled(Game &g, uint32_t mode) {
  g.core.r[7] = 0;
  const uint32_t handle = biosB0(g, 0x08, kClass, kSpec, mode);
  biosB0(g, 0x0C, handle, 0, 0);
  return handle;
}

bool testEvent(Game &g, uint32_t handle) {
  return biosB0(g, 0x0B, handle, 0, 0) != 0;
}

void test_undeliver_clears_a_delivered_nointr_event() {
  auto g = std::make_unique<Game>();
  const uint32_t handle = openEnabled(*g, EV_MD_NOINTR);
  g->hle.deliverEvent(kClass, kSpec);
  CHECK_EQ(biosB0(*g, 0x20, kClass, kSpec, 0), 0u);
  CHECK(!testEvent(*g, handle));
}

void test_undeliver_leaves_another_class_fired() {
  auto g = std::make_unique<Game>();
  const uint32_t handle = openEnabled(*g, EV_MD_NOINTR);
  g->hle.deliverEvent(kClass, kSpec);
  biosB0(*g, 0x20, kClass + 1u, kSpec, 0);
  CHECK(testEvent(*g, handle));
}

} // namespace

int main() {
  RUN(undeliver_clears_a_delivered_nointr_event);
  RUN(undeliver_leaves_another_class_fired);
  return pt_summary();
}
