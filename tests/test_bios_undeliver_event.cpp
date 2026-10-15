// test_bios_undeliver_event.cpp — B0:0x20 UnDeliverEvent returns a delivered, handler-less event to not-fired.
#include "bios_call.h"
#include "game.h"
#include "hle.h"
#include "testutil.h"

#include "stub_game_runtime.h"
#include <memory>

namespace {

constexpr uint32_t kClass = 0xF0000009u;
constexpr uint32_t kSpec = 0x0020u;

uint32_t openEnabled(Game &g, uint32_t mode) {
  g.core.r[7] = 0;
  const uint32_t handle = psx::test::biosResult(g, {'B', 0x08, {kClass, kSpec, mode}});
  psx::test::biosResult(g, {'B', 0x0C, {handle, 0, 0}});
  return handle;
}

bool testEvent(Game &g, uint32_t handle) {
  return psx::test::biosResult(g, {'B', 0x0B, {handle, 0, 0}}) != 0;
}

void test_undeliver_clears_a_delivered_nointr_event() {
  psx::test::StubGameRuntime stubRuntime;
  auto g = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  const uint32_t handle = openEnabled(*g, EV_MD_NOINTR);
  g->hle.deliverEvent(kClass, kSpec);
  CHECK_EQ(psx::test::biosResult(*g, {'B', 0x20, {kClass, kSpec, 0}}), 0u);
  CHECK(!testEvent(*g, handle));
}

void test_undeliver_leaves_another_class_fired() {
  psx::test::StubGameRuntime stubRuntime;
  auto g = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  const uint32_t handle = openEnabled(*g, EV_MD_NOINTR);
  g->hle.deliverEvent(kClass, kSpec);
  psx::test::biosResult(*g, {'B', 0x20, {kClass + 1u, kSpec, 0}});
  CHECK(testEvent(*g, handle));
}

} // namespace

int main() {
  RUN(undeliver_clears_a_delivered_nointr_event);
  RUN(undeliver_leaves_another_class_fired);
  return psx::test::summary();
}
