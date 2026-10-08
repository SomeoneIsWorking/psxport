// test_producer_census_arm.cpp — THE PSXPORT_PRODUCERS KNOB MUST ARM ON EVERY RUNTIME'S ROUTE.
//
// WHAT THIS GATES (the defect it was written RED against, 2026-08-25): the producer-census arm
// `g_producer_census_armed` (ot_attr.h, default true) was assigned ONLY inside native_boot_run
// (native_boot.cpp), a path that ports booting through GameRuntime::bootInit — Tekken 3 is the
// measured case — NEVER execute. There, PSXPORT_PRODUCERS=0 silently changed nothing: the knob's own
// help text says it exists "to price the census", so an A/B that cannot reach the arm prices nothing.
//
// THE FIX UNDER TEST: the arm moves into `Game`'s constructor (game.cpp) — a path EVERY runtime
// executes exactly once per Game. native_boot_run itself dereferences c->game, so no runtime can
// reach guest execution before constructing a Game; arming there preserves the old site's
// before-guest-execution guarantee (and is strictly earlier). The assignment is idempotent, so SBS's
// two Games each re-arm identically, and case 3 proves the value REFRESHES per construction rather
// than being a one-shot at process start.
//
// ENV RESOLUTION (verified in config_var.h/config.cpp): CVars self-register in their constructor and
// bind their Override layer LAZILY on first get() — ensure_env_bound has "no init call to forget"
// (config_var.h) — so by the time Game constructs, resolution needs no explicit initialisation and
// honours whatever ::setenv installed beforehand. The one trap for THIS test: that binding is
// once-per-CVar, so between cases the environment is re-opened with psx::config::reset_for_test(),
// the same mechanism tests/test_repl_unserviced_refusal.cpp documents ("a CVar binds its env
// Override once").
#include "config.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_program_image.h"
#include "ot_attr.h"
#include "testutil.h"

#include <lucent/config.h>

#include <cstdlib>
#include <memory>

namespace {

class ArmRuntime final : public GameRuntime {
public:
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }

  const GuestProgramImage *guestProgramImage() const override {
    return &programImage;
  }
  void *createContext(Core &) override {
    return &contextToken;
  }
  void destroyContext(void *context) override {
    CHECK_EQ(context, &contextToken);
  }
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }

  GuestProgramImage programImage{
      .bss = {0x800BE0D8u, 0x80106228u},
      .stackTopWordAddress = 0x800A3F88u,
      .stackReserveWordAddress = 0x800A3F8Cu,
      .heapBase = 0x80106228u,
      .heapSizeStoreAddress = 0x800ABEF8u,
      .heapBaseStoreAddress = 0x800ABEF4u,
      .globalPointer = 0x800BE0D4u,
      .libcInitEntry = 0x80089860u,
      .gameMainEntry = 0x80050B08u,
      .crt0Entry = 0x800896E0u,
      .residentText = {0x00010000u, 0x00100000u},
      .backtraceText = {0x00010000u, 0x00120000u},
      .stackBias = {true, -8},
  };
  int contextToken = 0;
};

std::unique_ptr<Game> make_game(ArmRuntime &runtime) {
  psxport_install_game(runtime);
  return std::make_unique<Game>();
}

void clear_override(void) {
  unsetenv("PSXPORT_PRODUCERS");
  // Both caches must forget (the pairing tests/test_config_cvar.cpp set_env documents): lucent's
  // per-name memo of the environment, and each CVar's once-only Override binding.
  lucent::config::reset_cache();
  psx::config::reset_for_test();
}

// (1) DEFAULT STATE: with no PSXPORT_PRODUCERS in the environment, constructing a Game leaves the
// census ARMED (default true, behaviour unchanged).
void test_default_state_arms_census_without_env_override(void) {
  clear_override();
  ArmRuntime runtime;
  auto game = make_game(runtime);
  CHECK_EQ(g_producer_census_armed, true);
}

// (2) THE DEFECT: PSXPORT_PRODUCERS=0 set before construction must DISARM at that construction.
// RED before the fix — nothing on the construction route read the CVar, so the global stayed at its
// default true and this check failed.
void test_producers_zero_disarms_at_construction(void) {
  CHECK_EQ(setenv("PSXPORT_PRODUCERS", "0", 1), 0);
  // The CVar binds its env Override once AND lucent memoises the env read — forget both, or this
  // case measures case 1's already-bound answer.
  lucent::config::reset_cache();
  psx::config::reset_for_test();
  ArmRuntime runtime;
  auto game = make_game(runtime);
  CHECK_EQ(g_producer_census_armed, false);
}

// (3) PER-CONSTRUCTION REFRESH, not a one-shot: after removing the override, ANOTHER constructed
// Game arms again. This is what makes SBS's two Games safe — each construction re-reads the knob.
void test_removing_override_rearms_next_construction(void) {
  clear_override();
  ArmRuntime runtime;
  auto game = make_game(runtime);
  CHECK_EQ(g_producer_census_armed, true);
}

} // namespace

int main() {
  RUN(default_state_arms_census_without_env_override);
  RUN(producers_zero_disarms_at_construction);
  RUN(removing_override_rearms_next_construction);
  return pt_summary();
}
