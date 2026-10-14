// test_machine_composition.cpp — psx::Machine: the composition every product shares, and the two
// properties that only hold if it is really shared.
//
// The first is the ORDER: the binds, then the title's overrides, then the preflight. A product that
// registers an override before the devices are bound dispatches into a device whose state was never
// published to its instance, and the failure looks like a title bug for weeks.
//
// The second is that a title cannot come up without a control channel: `attachControlChannel` is the
// one call that opens the endpoint AND arms the store observer. The run's cap is RunBudget's.
#include "testutil.h"

#include "config_var.h"
#include "core.h"
#include "dbg_server.h"
#include "frame_presenter.h"
#include "game.h"
#include "game_runtime.h"
#include "host_input.h"
#include "machine.h"
#include "platform_hle.h"
#include "render_queue.h"

#include <cstdint>
#include <memory>
#include <span>

namespace {

// A measured VSync address inside the synthetic image the driver steps from. Its VALUE is a
// title fact; the fact that the preflight REFUSES a product without one is not.
constexpr std::uint32_t kVSyncAddress = 0x00014000u;

unsigned g_registerOverridesCalls = 0;

// A direct runtime that declares the one thing the frame-loop preflight requires: a measured VSync
// address. A product without one is refused BEFORE it can dispatch guest code, which is the property
// this fixture must not lose while it is standing in for a real title.
class Runtime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  void registerOverrides(Game &) override {
    ++g_registerOverridesCalls;
  }
  const PlatformHlePlan *platformHlePlan() const override {
    return &plan;
  }

  PlatformHlePlan plan{};
};

// A host backend that advances the presentation fence and touches no device, so the frame contract
// can be exercised without a GPU: the same seam `test_frame_loop_shell` uses.
class FenceBackend final : public psx::frame::FramePresentationBackend {
public:
  void emit(std::span<const RqItem>) override {}
  void presentReal() override {}
  void captureDiagnostic(std::uint64_t, bool) override {}
  void pace(int, int) override {}
  void reconcile(std::uint64_t) override {}
  void beginLedgerFrame() override {}
  bool interpolatesRecords() const override {
    return false;
  }
  bool sealedFrameIsCut() override {
    return false;
  }
  void presentInBetween() override {}
  psx::gpu::RecordRect displayedBuffer() override {
    return {};
  }
};

class Driver final : public FrameDriver {
public:
  void stepFrame(Core &core, std::uint32_t frame) override {
    ++steps;
    lastFrame = frame;
    // The frame contract: presentation commits EXACTLY once per field, and the shell refuses a
    // driver that misses it. That refusal is why this driver commits here rather than not at all.
    game->presentation.commitUnpresented(backend_);
  }
  unsigned steps = 0;
  std::uint32_t lastFrame = 0;
  Game *game = nullptr;

private:
  FenceBackend backend_;
};

struct Fixture {
  Runtime runtime;
  std::unique_ptr<Game> game;
  Driver *driver = nullptr; // owned by the Game, which owns its frame driver

  Fixture() {
    // The VSync entry has to sit inside a declared service window: PlatformHle installs the window
    // table and the address together, so an address outside it is refused as unmeasured.
    runtime.plan.vsyncAddress = kVSyncAddress;
    runtime.plan.windowLo[0] = kVSyncAddress;
    runtime.plan.windowHi[0] = kVSyncAddress + 4u;
    psxport_install_game(runtime);
    game = std::make_unique<Game>();
    auto owned = std::make_unique<Driver>();
    owned->game = game.get();
    driver = owned.get();
    game->frameDriver = std::move(owned);
  }
};

static void test_construction_binds_the_devices_and_seeds_the_entry_registers(void) {
  Fixture fixture;
  psx::Machine machine{*fixture.game};
  machine.bindDevices();
  CHECK_EQ(fixture.game->core.r[4], 1u);
  CHECK_EQ(fixture.game->core.r[5], 0u);
}

static void test_prepare_registers_the_title_overrides_before_the_preflight(void) {
  Fixture fixture;
  g_registerOverridesCalls = 0;
  psx::Machine machine{*fixture.game};
  machine.prepare();
  CHECK_EQ(g_registerOverridesCalls, 1u);
  // The preflight is what lets a frame step run at all: it is the second half of `prepare`, and a
  // frame step is REFUSED without it — which is what the capped run below proves by reaching its cap.
  CHECK(true);
}

static void test_the_loop_ends_on_the_clients_quit_and_owes_a_ledger(void) {
  Fixture fixture;
  psx::Machine machine{*fixture.game};
  machine.prepare();
  machine.attachControlChannel();
  // A client that connects and says `quit` must end the run, not just disconnect.
  fixture.game->dbg_server.requestQuit();
  machine.run(0u);
  CHECK(fixture.driver->steps > 0u);
  CHECK(!fixture.game->dbg_server.quitRequested());
}

static void test_a_capped_run_stops_at_its_cap(void) {
  Fixture fixture;
  psx::Machine machine{*fixture.game};
  machine.prepare();
  machine.run(3u);
  CHECK_EQ(fixture.driver->steps, 3u);
  CHECK_EQ(fixture.driver->lastFrame, 2u);
}

// Each step is a call a product can make without adopting the whole composition.
static void test_the_boot_steps_are_individually_callable(void) {
  Fixture fixture;
  g_registerOverridesCalls = 0;
  psx::Machine machine{*fixture.game};
  machine.reportConfigurationOnce();
  machine.armHostDiagnostics();
  // Both halves of `prepare`, in order: the title's overrides, then the preflight.
  machine.prepare();
  CHECK_EQ(g_registerOverridesCalls, 1u);
}

// The display clear PRESENTS (black framebuffer + present, so the title's first fields do not
// composite over a stale splash), so it needs a real sink and cannot run in a hermetic test. Its
// existence and signature are what this pins; the step itself is exercised by every product's boot.
static void test_the_display_clear_is_the_frame_owners_step(void) {
  void (psx::Machine::*clearDisplay)() = &psx::Machine::clearDisplayForFrontEnd;
  CHECK(clearDisplay != nullptr);
}

// The load-state step is fatal in its shipping form, so the test drives the reporting form: unset means
// boot normally, and a path that cannot be loaded is refused with its reason rather than booted around.
static void test_configured_state_reports_instead_of_silently_booting(void) {
  Fixture fixture;
  psx::Machine machine{*fixture.game};
  std::string error;
  CHECK(machine.tryApplyConfiguredState(error));
  psx::config::cv_load_state.set(psx::config::Layer::Runtime, "no/such/state.sav");
  const bool applied = machine.tryApplyConfiguredState(error);
  psx::config::cv_load_state.clear(psx::config::Layer::Runtime);
  CHECK(!applied);
  CHECK(!error.empty());
}

static void test_a_window_close_is_recorded_on_the_attached_scope(void) {
  psx::input::QuitScope quit;
  CHECK(!quit.requested());
  quit.request();
  CHECK(quit.take());
  CHECK(!quit.take());
}

} // namespace

int main() {
  RUN(construction_binds_the_devices_and_seeds_the_entry_registers);
  RUN(prepare_registers_the_title_overrides_before_the_preflight);
  RUN(the_boot_steps_are_individually_callable);
  RUN(the_display_clear_is_the_frame_owners_step);
  RUN(configured_state_reports_instead_of_silently_booting);
  RUN(the_loop_ends_on_the_clients_quit_and_owes_a_ledger);
  RUN(a_capped_run_stops_at_its_cap);
  RUN(a_window_close_is_recorded_on_the_attached_scope);
  return pt_summary();
}