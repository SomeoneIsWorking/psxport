// test_host_turn_guest_clock.cpp — the host field clock owes fields by EMULATED guest time.
//
// One field period of Timing::emulatedCpuTicks since the last delivered field raises the pending
// host turn; a delivered field or a served turn starts the next complete period. No wall clock,
// thread, or sleep takes part, so a slow host cannot change how many fields a guest update sees.
#include "emulated_time.h"
#include "execution_services.h"
#include "field_rate.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_call.h"
#include "host_turn.h"
#include "testutil.h"

#include <cstdio>
#include <memory>

namespace {

class Runtime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
};

int deliveredTurns;
uint32_t turnFlagAddress;

void deliverTurn(Core *core) {
  ++deliveredTurns;
  if (turnFlagAddress != 0) {
    core->mem_w32(turnFlagAddress, 1u);
  }
}

class Fixture {
public:
  Fixture() {
    deliveredTurns = 0;
    turnFlagAddress = 0;
    game->hle.irq_enabled = 1;
    psx::cpu::registerHostTurn(game->core, deliverTurn, FIELD_RATE_NTSC_MILLIHZ);
  }
  ~Fixture() {
    psx::cpu::shutdownHostTurn();
  }
  bool hostOwed() const {
    return (game->core.pending_work & Core::PW_HOST) != 0;
  }
  std::unique_ptr<Game> game = std::make_unique<Game>();
};

const uint32_t kPeriod = static_cast<uint32_t>(display_field_cpu_ticks(1, 1, FIELD_RATE_NTSC_MILLIHZ));

} // namespace

static void test_field_is_owed_after_one_period_of_guest_time(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  psx::cpu::accountGuestInstructions(core, kPeriod - 1);
  CHECK(!fixture.hostOwed());
  psx::cpu::accountGuestInstructions(core, 1);
  CHECK(fixture.hostOwed());
}

static void test_served_turn_starts_the_next_complete_period(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  psx::cpu::accountGuestInstructions(core, kPeriod + 10);
  CHECK(fixture.hostOwed());
  psx::cpu::serviceHostTurn(core);
  CHECK_EQ(deliveredTurns, 1);
  CHECK(!fixture.hostOwed());
  // The period restarts at the served turn, not at the stale deadline: 10 ticks of overrun do not
  // bring the next field closer.
  psx::cpu::accountGuestInstructions(core, kPeriod - 1);
  CHECK(!fixture.hostOwed());
  psx::cpu::accountGuestInstructions(core, 1);
  CHECK(fixture.hostOwed());
}

static void test_delivered_field_cancels_and_restarts_the_period(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  psx::cpu::accountGuestInstructions(core, kPeriod);
  CHECK(fixture.hostOwed());
  // The native frame loop delivered the field itself: the latched turn is the same event, and
  // the timing owner re-anchors the clock as part of advancing display fields.
  CHECK(fixture.game->timing.advanceDisplayFields(1, 1, FIELD_RATE_NTSC_MILLIHZ));
  CHECK(!fixture.hostOwed());
  psx::cpu::accountGuestInstructions(core, kPeriod - 1);
  CHECK(!fixture.hostOwed());
  psx::cpu::accountGuestInstructions(core, 1);
  CHECK(fixture.hostOwed());
}

static void test_a_delivered_field_does_not_request_itself(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  // Delivered fields advance the clock to exactly one period past the previous boundary, i.e. onto
  // the deadline. Guest work accounted inside that field's callbacks (PadVSync and friends) must
  // not see a due clock: that request would be for the field already in flight, and serving it at
  // the next boundary chained one unrequested field after another.
  for (int field = 0; field < 8; ++field) {
    CHECK(fixture.game->timing.advanceDisplayFields(1, 1, FIELD_RATE_NTSC_MILLIHZ));
    psx::cpu::accountGuestInstructions(core, 200);
    CHECK(!fixture.hostOwed());
  }
}

static void test_long_update_owes_one_field_per_period(void) {
  Fixture fixture;
  Core &core = fixture.game->core;
  int owedTurns = 0;
  // Four periods of guest work with a boundary check after every chunk: exactly four fields, none
  // early, none doubled.
  for (uint32_t tick = 0; tick < 4u * kPeriod; tick += 64) {
    psx::cpu::accountGuestInstructions(core, 64);
    if (fixture.hostOwed()) {
      psx::cpu::serviceHostTurn(core);
      ++owedTurns;
    }
  }
  CHECK_EQ(owedTurns, 4);
  CHECK_EQ(deliveredTurns, 4);
}

static void test_unregistered_core_is_never_owed_a_field(void) {
  Fixture fixture;
  auto foreign = std::make_unique<Game>();
  psx::cpu::accountGuestInstructions(foreign->core, 3u * kPeriod);
  CHECK_EQ(foreign->core.pending_work & Core::PW_HOST, 0);
  psx::cpu::notifyDisplayField(foreign->core);
  CHECK(!fixture.hostOwed());
}

static void test_host_time_does_not_owe_a_field(void) {
  Fixture fixture;
  // Negative arm: without guest time nothing is owed however the host clock advances. The old
  // wall-clock timer would have raised the request here after one 16.7 ms period.
  CHECK(!fixture.hostOwed());
  psx::cpu::accountGuestInstructions(fixture.game->core, 0);
  CHECK(!fixture.hostOwed());
}

// The guest loop the hand-off crash reproduced: a libetc-style `lw; beqz` wait on a word that only
// the field's work sets. The executor must end its translated segment at the clock's deadline so
// the turn is taken after one field period; the whole budget is the failure the crash reported.
static void test_guest_wait_on_field_work_returns_after_one_period(void) {
  constexpr uint32_t kWait = 0x00010000u;
  constexpr uint32_t kFlag = 0x00010100u;
  const uint64_t kBudget = 200ull * kPeriod;
  static Runtime runtime;
  psxport_install_game(runtime);
  Fixture fixture;
  Core &core = fixture.game->core;
  core.imageCatalog().activate("host-turn-guest-clock", {kWait, kFlag + 4u}, 0x484f5354ull);
  core.mem_w32(kWait + 0u, 0x3c040001u);  // lui   a0, 0x0001
  core.mem_w32(kWait + 4u, 0x8c820100u);  // lw    v0, 0x100(a0)      <- .loop
  core.mem_w32(kWait + 8u, 0u);           // nop
  core.mem_w32(kWait + 12u, 0x1040fffdu); // beqz  v0, .loop
  core.mem_w32(kWait + 16u, 0u);          // nop
  core.mem_w32(kWait + 20u, 0x03e00008u); // jr    ra
  core.mem_w32(kWait + 24u, 0u);          // nop
  core.mem_w32(kFlag, 0u);
  turnFlagAddress = kFlag;

  const uint64_t ticksBefore = fixture.game->timing.emulatedCpuTicks();
  const auto result = psx::cpu::dispatchGuest0(core, kWait, psx::cpu::ExecutionBudget::fromCycles(kBudget));
  const uint64_t ticksElapsed = fixture.game->timing.emulatedCpuTicks() - ticksBefore;
  if (!result.returned()) {
    std::printf("  dispatch exited %s at 0x%08X after %llu cycles: %s\n",
                psx::cpu::executionExitName(result.reason),
                result.guestPc,
                static_cast<unsigned long long>(result.cycles),
                result.detail.c_str());
  }
  CHECK(result.returned());
  CHECK_EQ(deliveredTurns, 1);
  CHECK_EQ(core.mem_r32(kFlag), 1u);
  // One period of GUEST time plus at most the loop iteration that observes the flag; never the
  // budget. Lightrec's cycle count is a different unit (it charges more than one cycle per
  // instruction), so the bound is checked on the emulated clock that owes the field.
  CHECK(ticksElapsed >= kPeriod);
  CHECK(ticksElapsed < kPeriod + 64u);
  CHECK(result.cycles < kBudget);
}

int main() {
  RUN(guest_wait_on_field_work_returns_after_one_period);
  RUN(field_is_owed_after_one_period_of_guest_time);
  RUN(served_turn_starts_the_next_complete_period);
  RUN(delivered_field_cancels_and_restarts_the_period);
  RUN(a_delivered_field_does_not_request_itself);
  RUN(long_update_owes_one_field_per_period);
  RUN(unregistered_core_is_never_owed_a_field);
  RUN(host_time_does_not_owe_a_field);
  return pt_summary();
}
