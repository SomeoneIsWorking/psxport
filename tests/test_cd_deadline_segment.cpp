// A guest that spins on plain RAM must still receive the CD interrupt a controller deadline raises.
//
// ROOT CAUSE THIS PINS (Toy Story 2's STR player, issue 0145). Device time is folded into the guest
// clock on a device register access and at a segment end. The FMV's bounded ring pop reads only RAM,
// so a segment spanning the whole call budget never advanced the controller's clock: the INT1 the
// drive owed appeared at the very instant the budget ran out, and the call was reported as an
// undelivered event. The executor now ends a segment at the controller's next armed deadline.
//
// The control is the same guest loop with NO armed deadline: it must still run its whole budget (the
// cap must not shorten a segment for a device that owes nothing), and the delivery must not happen.
#include "cd_control.h"
#include "cd_stock_read_completion.h"
#include "cdc_state.h"
#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_call.h"
#include "guest_cd_stream_callback_layout.h"
#include "image_identity.h"
#include "native_dispatch.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kSpin = 0x80010000u;
constexpr uint32_t kFlag = 0x80010800u;
constexpr uint32_t kSlot = 0x80010900u;
constexpr uint32_t kCallback = 0x80010400u;
constexpr uint32_t kBudget = 4'000'000u;

int callbackCalls = 0;

void readyCallback(Core *core) {
  ++callbackCalls;
  core->mem_w32(kFlag, 1u);
}

int fakeSector(DiscState *, uint32_t lba, uint8_t *out, uint32_t) {
  for (uint32_t i = 0; i < 2352u; i++) {
    out[i] = static_cast<uint8_t>(lba + i);
  }
  return 1;
}

int fakeSector2048(DiscState *, uint32_t lba, uint8_t *out) {
  for (uint32_t i = 0; i < 2048u; i++) {
    out[i] = static_cast<uint8_t>(lba + i);
  }
  return 1;
}

class StreamRuntime final : public GameRuntime {
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
  const GuestCdStreamCallbackLayout *guestCdStreamCallbackLayout() const override {
    return &layout;
  }
  GuestCdStreamCallbackLayout layout{kSlot, GuestCdStreamCallbackLayout::DeliveryOwner::GuestInterrupt};
};

// lui t1,0x8001 ; loop: lw t0,0x800(t1) ; beq t0,zero,loop ; nop ; jr ra ; nop
// Reads ONLY main RAM: no device register is touched while it spins.
void writeSpinOnRamFlag(Core &core) {
  core.mem_w32(kSpin, 0x3C098001u);
  core.mem_w32(kSpin + 4u, 0x8D280800u);
  core.mem_w32(kSpin + 8u, 0x1100FFFEu);
  core.mem_w32(kSpin + 12u, 0u);
  core.mem_w32(kSpin + 16u, 0x03E00008u);
  core.mem_w32(kSpin + 20u, 0u);
}

struct Fixture {
  Fixture() {
    callbackCalls = 0;
    psxport_install_game(runtime);
    game = std::make_unique<Game>();
    Core &core = game->core;
    const auto image = core.imageCatalog().activate("deadline-segment", {kSpin & 0x1FFFFFFFu, 0x00011000u}, 7u);
    CHECK(core.nativeDispatcher().install({{image, kCallback}, "ready-callback", readyCallback}));
    core.mem_w32(kSlot, kCallback);
    core.mem_w32(kFlag, 0u);
    writeSpinOnRamFlag(core);
    game->hle.irq_enabled = 1;
    game->cdc.disc_read_raw_fn = fakeSector;
    game->cdc.disc_read_sector_fn = fakeSector2048;
    psx::cd::armCdInterrupt(core);
  }

  psx::cpu::ExecutionResult spin() {
    return psx::cpu::dispatchGuest0(game->core, kSpin, psx::cpu::ExecutionBudget::fromCycles(kBudget));
  }

  StreamRuntime runtime;
  std::unique_ptr<Game> game;
};

} // namespace

// THE POSITIVE: a ReadN arms the drive's first-sector deadline; the spin that touches no device
// register receives the data-ready callback mid-call, returns, and spends far less than its budget.
static void test_a_ram_spin_receives_the_interrupt_a_controller_deadline_raises() {
  Fixture fixture;
  Core &core = fixture.game->core;
  cdc_begin_read(&fixture.game->cdc, 100);
  CHECK(fixture.game->cdc.drive_event_armed != 0);

  const auto result = fixture.spin();

  CHECK(result.returned());
  CHECK_EQ(callbackCalls, 1);
  CHECK_EQ(core.mem_r32(kFlag), 1u);
  CHECK_EQ(fixture.game->hle.cd_ready_delivered, 1u);
  CHECK(result.cycles < kBudget);
}

// THE CONTROL: nothing is armed, so nothing is delivered and the loop runs its whole budget. A cap
// that fired for an idle controller would shorten every guest call; a delivery without a deadline
// would be invented.
static void test_with_no_armed_deadline_the_spin_runs_its_whole_budget_and_delivers_nothing() {
  Fixture fixture;
  Core &core = fixture.game->core;
  CHECK(!fixture.game->cdc.drive_event_armed);
  CHECK(!fixture.game->cdc.command_event_armed);

  const auto result = fixture.spin();

  CHECK(!result.returned());
  CHECK_EQ(callbackCalls, 0);
  CHECK_EQ(core.mem_r32(kFlag), 0u);
  CHECK_EQ(fixture.game->hle.cd_ready_delivered, 0u);
  CHECK(result.cycles >= kBudget);
}

// The accessor itself: absent when nothing is armed (the executor must not cap), the exact remaining
// ticks when armed, zero once due.
static void test_the_device_event_horizon_is_absent_exact_and_zero_when_due() {
  Fixture fixture;
  CHECK(!fixture.game->timing.ticksUntilDeviceEvent().has_value());
  cdc_begin_read(&fixture.game->cdc, 100);
  const auto remaining = fixture.game->timing.ticksUntilDeviceEvent();
  CHECK(remaining.has_value());
  CHECK(*remaining > 0u);
  fixture.game->timing.advanceGuestInstructionTicks(static_cast<uint32_t>(*remaining) - 1u);
  CHECK_EQ(*fixture.game->timing.ticksUntilDeviceEvent(), 1u);
}

int main() {
  RUN(a_ram_spin_receives_the_interrupt_a_controller_deadline_raises);
  RUN(with_no_armed_deadline_the_spin_runs_its_whole_budget_and_delivers_nothing);
  RUN(the_device_event_horizon_is_absent_exact_and_zero_when_due);
  return pt_summary();
}
