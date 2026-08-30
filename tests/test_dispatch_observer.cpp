// test_dispatch_observer — dispatch-miss targets are observable before any HLE callee owns them.
//
// Crash 1's first post-syscall boundary is BIOS B(56h): the generated caller dynamically dispatches
// to 0xB0, outside every generated shard. Observing only generated function entries therefore skips
// the real boundary and executes the HLE leaf before the differential harness can compare it.
//
// This hermetic test drives the shipping rec_dispatch -> rec_dispatch_miss -> Hle::dispatchBios path.
// The callback snapshots v0/t1/ra, then the test proves B(56h) subsequently changed v0. A hook after
// HLE would see the changed value and fail. The other regression routes a generated entry whose
// emitted-body stub owns pc_observer_at: the generic router must not count it a second time.
#include "testutil.h"

#include "game.h"
#include "game_iface.h"
#include "recomp_iface.h"

#include <lucent/log.h>
#include <memory>

void rec_dispatch(Core *c, uint32_t addr);

namespace {

constexpr uint32_t kBiosB = 0x000000B0u;
constexpr uint32_t kGetC0Table = 0x56u;
constexpr uint32_t kInitialV0 = 0xA55AA55Au;
constexpr uint32_t kReturnAddress = 0x800431B8u;
constexpr uint32_t kGeneratedTarget = 0x80012340u;

struct Observation {
  int calls = 0;
  uint64_t ordinal = 0;
  uint32_t pc = 0;
  uint32_t v0 = 0;
  uint32_t t1 = 0;
  uint32_t ra = 0;
};

void capture(Core *core, uint64_t ordinal, uint32_t pc, void *user) {
  auto &observation = *static_cast<Observation *>(user);
  ++observation.calls;
  observation.ordinal = ordinal;
  observation.pc = pc;
  observation.v0 = core->r[2];
  observation.t1 = core->r[9];
  observation.ra = core->r[31];
}

void main_dispatch_stub(Core *core, uint32_t address) {
  if (address == kGeneratedTarget) {
    // This is the same ownership rule as an emitted generated wrapper: it observes its own exact PC.
    pc_observer_at(core, address);
  }
}

int index_stub(uint32_t) {
  return -1;
}

const RecompRegistry kRegistry = {
    main_dispatch_stub,
    index_stub,
    nullptr,
    0,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

std::unique_ptr<Game> fresh_game() {
  static GameConfig config{};
  static const GameHooks hooks{};
  config = {};
  config.recMainLo = 0x00010000u;
  config.recMainHi = 0x00100000u;
  psxport_install_game(&config, &hooks);
  psxport_install_recomp(&kRegistry);
  return std::make_unique<Game>();
}

void seed_bios_call(Core &core) {
  core.r[2] = kInitialV0;
  core.r[9] = kGetC0Table;
  core.r[31] = kReturnAddress;
}

void test_bios_boundary_is_observed_before_hle() {
  auto game = fresh_game();
  Core &core = game->core;
  seed_bios_call(core);
  Observation observation;
  CHECK(core.pcObserver.arm(&kBiosB, 1, capture, &observation));

  rec_dispatch(&core, kBiosB);

  CHECK_EQ(observation.calls, 1);
  CHECK_EQ(observation.ordinal, 1u);
  CHECK_EQ(observation.pc, kBiosB);
  CHECK_EQ(observation.v0, kInitialV0);
  CHECK_EQ(observation.t1, kGetC0Table);
  CHECK_EQ(observation.ra, kReturnAddress);
  CHECK(core.r[2] != kInitialV0);
  CHECK_EQ(core.pcObserver.seen(), 1u);
  CHECK_EQ(core.pcObserver.matched(), 1u);
}

void test_unrelated_dynamic_target_does_not_call_observer() {
  auto game = fresh_game();
  Core &core = game->core;
  seed_bios_call(core);
  Observation observation;
  constexpr uint32_t kDifferentTarget = 0x000000A0u;
  CHECK(core.pcObserver.arm(&kDifferentTarget, 1, capture, &observation));

  rec_dispatch(&core, kBiosB);

  CHECK_EQ(observation.calls, 0);
  CHECK_EQ(core.pcObserver.seen(), 1u);
  CHECK_EQ(core.pcObserver.matched(), 0u);
  CHECK(core.r[2] != kInitialV0);
}

void test_generated_entry_is_not_double_observed_by_router() {
  auto game = fresh_game();
  Core &core = game->core;
  Observation observation;
  CHECK(core.pcObserver.arm(&kGeneratedTarget, 1, capture, &observation));

  rec_dispatch(&core, kGeneratedTarget);

  CHECK_EQ(observation.calls, 1);
  CHECK_EQ(observation.ordinal, 1u);
  CHECK_EQ(observation.pc, kGeneratedTarget);
  CHECK_EQ(core.pcObserver.seen(), 1u);
  CHECK_EQ(core.pcObserver.matched(), 1u);
}

void test_recdep_dump_precedes_core_destruction() {
  // This is deliberately the last case and leaves recdep-all enabled through process exit. The old
  // atexit design retained this Game's Core pointer, then dereferenced it after the unique_ptr had
  // destroyed the Game; the test body passed and the executable exited with SIGSEGV.
  lucent::enable_channels("recdep-all");
  auto game = fresh_game();
  rec_dispatch(&game->core, kGeneratedTarget);
  CHECK_EQ(game->core.idiag.recdep.at(kGeneratedTarget), 1u);
  game.reset();
}

} // namespace

int main() {
  RUN(bios_boundary_is_observed_before_hle);
  RUN(unrelated_dynamic_target_does_not_call_observer);
  RUN(generated_entry_is_not_double_observed_by_router);
  RUN(recdep_dump_precedes_core_destruction);
  return pt_summary();
}
