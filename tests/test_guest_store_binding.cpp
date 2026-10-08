// test_guest_store_binding.cpp — a translated guest store inside a producer scope binds its word.
#include "emission_scope.h"
#include "execution_control.h"

#include "dynarec_test_fixture.h"

#include "testutil.h"

using namespace dynarec_test;

namespace {

constexpr std::uint32_t kProducer = 0x8001F798u;
constexpr std::uint32_t kObject = 0x80150000u;
constexpr std::uint32_t kKnownTarget = 0x80080100u;
constexpr std::uint32_t kRegisterTarget = 0x80080200u;

} // namespace

static void test_known_and_register_addressed_guest_stores_bind(void) {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  installTestImage(core);
  core.mem_w32(kCaller, 0x3c088008u);       // lui t0, 0x8008
  core.mem_w32(kCaller + 4u, 0x24090055u);  // addiu t1, zero, 0x55
  core.mem_w32(kCaller + 8u, 0xad090100u);  // sw t1, 0x100(t0)
  core.mem_w32(kCaller + 12u, 0xac890000u); // sw t1, 0(a0)
  core.mem_w32(kCaller + 16u, 0x03e00008u); // jr ra
  core.mem_w32(kCaller + 20u, 0u);
  core.r[4] = kRegisterTarget;
  core.r[31] = kOuterReturn;
  {
    psx::present::EmissionScope::Guard scope(core.emission, kProducer, kObject, 0u);
    const auto result = psx::cpu::dispatchGuest(core, kCaller, psx::cpu::ExecutionBudget::fromCycles(100));
    CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  }
  CHECK_EQ(core.mem_r32(kKnownTarget), 0x55u);
  CHECK_EQ(core.mem_r32(kRegisterTarget), 0x55u);
  // Each target is the first command word of the packet four bytes before it.
  const auto known = core.emission.keyFor(kKnownTarget - 4u);
  const auto addressed = core.emission.keyFor(kRegisterTarget - 4u);
  CHECK(known.has_value());
  CHECK(addressed.has_value());
  CHECK(known && known->producer == kProducer && known->object == kObject);
  CHECK(addressed && addressed->producer == kProducer && addressed->object == kObject);
}

int main() {
  RUN(known_and_register_addressed_guest_stores_bind);
  return pt_summary();
}
