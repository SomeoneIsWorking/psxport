// libetc VSync is never a shipping source of fields. A measured negative query reads the title's
// libetc field counter; nonnegative calls remain protected typed frame boundaries.
#include "execution_control.h"
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "native_dispatch.h"
#include "platform_hle.h"
#include "testutil.h"

#include <csignal>
#include <cstdint>
#include <memory>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr uint32_t kVSyncAddress = 0x800859A8u;
constexpr uint32_t kWindowEnd = 0x80085B20u;
constexpr uint32_t kSyntheticVBlankCounter = 0x80018000u;

class DirectRuntime final : public GameRuntime {
public:
  const PlatformHlePlan *platformHlePlan() const override {
    return &plan;
  }
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

  PlatformHlePlan plan{};
};

void harmless_handler(Core *) {}

void assert_wait_modes_request_frame_boundary(Game &game) {
  const OverrideFn handler = game.platform_hle.lookup(kVSyncAddress);
  CHECK(handler != nullptr);
  for (const int32_t mode : {0, 1, 4}) {
    game.core.r[4] = static_cast<uint32_t>(mode);
    handler(&game.core);
    const auto result = game.core.executionControl().consume();
    CHECK(result.has_value());
    if (result) {
      CHECK_EQ(result->reason, psx::cpu::ExecutionExitReason::FrameBoundary);
    }
  }
}

void assert_missing_counter_aborts(Game &game) {
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    game.core.r[4] = static_cast<uint32_t>(-1);
    game.platform_hle.lookup(kVSyncAddress)(&game.core);
    _exit(0);
  }
  int status = 0;
  CHECK_EQ(waitpid(child, &status, 0), child);
  CHECK(WIFSIGNALED(status));
  CHECK_EQ(WTERMSIG(status), SIGABRT);
}

} // namespace

static void test_direct_runtime_installs_one_wait_boundary_and_measured_query() {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.vsyncQueryCounterAddress = kSyntheticVBlankCounter;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();
  game->platform_hle.initBuiltins(); // repeatable even when no title override is installed
  CHECK(game->platform_hle.hasNativeFrameLoopContract());
  game->platform_hle.requireNativeFrameLoopContract();
  game->core.mem_w32(kSyntheticVBlankCounter, 73u);
  const OverrideFn handler = game->platform_hle.lookup(kVSyncAddress);
  CHECK(handler != nullptr);
  game->core.r[4] = static_cast<uint32_t>(-1);
  game->core.r[2] = 0xDEADBEEFu;
  handler(&game->core);
  CHECK_EQ(game->core.r[2], 73u);
  CHECK(!game->core.executionControl().pending());
  CHECK_EQ(game->core.mem_r32(kSyntheticVBlankCounter), 73u);
  game->core.mem_w32(kSyntheticVBlankCounter, 74u);
  handler(&game->core);
  CHECK_EQ(game->core.r[2], 74u);
  CHECK(!game->core.executionControl().pending());
  assert_wait_modes_request_frame_boundary(*game);
}

static void test_legacy_adapter_installs_the_same_wait_boundary() {
  static GameConfig config{};
  static const GameHooks hooks{};
  config = {};
  config.hle.windowLo[0] = kVSyncAddress;
  config.hle.windowHi[0] = kWindowEnd;
  config.hle.vsyncTrap = kVSyncAddress;
  psxport_install_game(&config, &hooks);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();
  CHECK(game->platform_hle.hasNativeFrameLoopContract());
  game->platform_hle.requireNativeFrameLoopContract();
  assert_wait_modes_request_frame_boundary(*game);
}

static void test_vsync_boundary_cannot_be_replaced() {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();

  CHECK(!game->platform_hle.register_(kVSyncAddress, harmless_handler));
  assert_wait_modes_request_frame_boundary(*game);
}

static void test_negative_query_without_measured_counter_refuses() {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();

  assert_missing_counter_aborts(*game);
  CHECK(!game->core.executionControl().pending());
}

static void test_missing_direct_vsync_address_has_no_product_contract() {
  DirectRuntime runtime;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();

  CHECK(!game->platform_hle.hasNativeFrameLoopContract());
}

static void test_vsync_address_outside_the_declared_window_is_refused() {
  DirectRuntime runtime;
  runtime.plan.windowLo[0] = kVSyncAddress + 4u;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  CHECK(!game->platform_hle.register_(kVSyncAddress, harmless_handler));
  CHECK(game->platform_hle.lookup(kVSyncAddress) == nullptr);
}

// A TITLE'S OWN OVERRIDE WINS OVER THE HOST-SERVICE TABLE. This is the case that was silently
// impossible before, and it is the whole reason `resolveHostDispatch` consults the image-scoped native
// override table FIRST.
//
// `vsync_boundary_cannot_be_replaced` above pins that the *platform_hle table's own* entry cannot be
// replaced by `platform_hle.register_` — a different mechanism. This case is about a different table
// entirely: the title installs a native override for the same guest address through the image-scoped
// seam, and the dispatch must reach IT rather than the framework's VSync builtin.
//
// Measured consequence of getting it backwards, on Mega Man X4: the title owns the movie VSync boundary
// at 0x800E4DB0 (`x4::movie::fieldBoundary`) while the legacy HLE window's `.vsyncTrap` claims the same
// address for `PlatformHle::vsync`. The builtin won, requested the bounded exit with `core.pc` (the
// VSync ENTRY) rather than `core.r[31]` (the call's return address), and the movie task resumed at the
// VSync entry forever — so the guest's STR pull never advanced, `StGetNext` was never re-entered, and
// 200 fields produced 0 prims and 7 of 7 all-black presents.
static int g_titleOverrideCalls = 0;
static int g_builtinCalls = 0;

// Returns cleanly, so the dispatch is a GuestReturn. That is what makes the ORDER observable: the
// framework's `PlatformHle::vsync` builtin requests a FrameBoundary, so if the builtin were reached
// instead the result would be FrameBoundary and the counts below would both be zero. A title-owned
// boundary is free to return cleanly or to request an exit of its own choosing; which FUNCTION runs is
// the point, not what it requests.
void titleOwnedBoundary(Core *) {
  ++g_titleOverrideCalls;
}

static void test_a_title_override_is_dispatched_before_a_host_service_leaf(void) {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();

  // Both tables now claim this address: the framework's VSync builtin through the HLE plan, and the
  // title's own boundary through the image-scoped seam.
  const OverrideFn builtin = game->platform_hle.lookup(kVSyncAddress);
  CHECK(builtin != nullptr);
  // The catalog's ranges are PHYSICAL: `ImageCatalog::resolve` masks the guest address with
  // 0x1fffffff before matching. Registering the KSEG0 form would silently cover nothing, and the
  // resulting dispatch would fall through to the builtin — looking exactly like the bug under test.
  constexpr uint32_t kPhysicalLo = kVSyncAddress & 0x1fffffffu;
  constexpr uint32_t kPhysicalHi = kWindowEnd & 0x1fffffffu;
  const psx::cpu::ImageIdentity identity =
      game->core.imageCatalog().activate("vsync-ownership", {kPhysicalLo, kPhysicalHi}, 0x564f574eull);
  g_titleOverrideCalls = 0;
  g_builtinCalls = 0;
  CHECK(game->core.nativeDispatcher().install({{identity, kVSyncAddress}, "title-movie-boundary", titleOwnedBoundary}));
  // The dispatch resolves the OVERRIDE through the image identity, so the test asserts its own premise
  // rather than discovering it as an abort. Without this, a range that did not cover the address would
  // look exactly like the ordering bug this case exists to catch.
  CHECK(game->core.currentImageIdentity(kVSyncAddress).has_value());
  CHECK(game->core.nativeDispatcher().intercepts({identity, kVSyncAddress}));

  // The RETURNING form, deliberately. The void `dispatchGuestToReturn` aborts on anything but
  // GuestReturn, so with the old ordering this case would die on an abort rather than report a failed
  // check — technically a catch, but a test that dies instead of reporting is a bad test. With the
  // builtin reached instead, the result is a typed FrameBoundary and the assertions below name it.
  const psx::cpu::ExecutionResult result =
      psx::cpu::dispatchGuest(game->core, kVSyncAddress, psx::cpu::ExecutionBudget::fromCycles(1000));
  CHECK_EQ(result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  // The title's function ran, and the framework's builtin did not. One call, not a blend of the two.
  CHECK_EQ(g_titleOverrideCalls, 1);
  CHECK_EQ(g_builtinCalls, 0);
}

// ...and the fallback still works where no title has claimed the address, which is what the HLE table is
// FOR. Without this the ordering change would look like it had simply disabled the builtins.
static void test_a_host_service_leaf_still_answers_an_unclaimed_address(void) {
  DirectRuntime runtime;
  runtime.plan.vsyncAddress = kVSyncAddress;
  runtime.plan.windowLo[0] = kVSyncAddress;
  runtime.plan.windowHi[0] = kWindowEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  game->platform_hle.initBuiltins();
  // An image is active but NO override is installed for this address.
  (void)game->core.imageCatalog().activate(
      "vsync-ownership", {kVSyncAddress & 0x1fffffffu, kWindowEnd & 0x1fffffffu}, 0x564f574eull);
  g_titleOverrideCalls = 0;
  CHECK(game->platform_hle.lookup(kVSyncAddress) != nullptr);

  game->core.r[4] = 0u; // VSync(0): a protected typed frame boundary
  const OverrideFn handler = game->platform_hle.lookup(kVSyncAddress);
  handler(&game->core);
  CHECK_EQ(g_titleOverrideCalls, 0);
  CHECK(game->core.executionControl().pending());
}

int main() {
  RUN(direct_runtime_installs_one_wait_boundary_and_measured_query);
  RUN(legacy_adapter_installs_the_same_wait_boundary);
  RUN(vsync_boundary_cannot_be_replaced);
  RUN(negative_query_without_measured_counter_refuses);
  RUN(missing_direct_vsync_address_has_no_product_contract);
  RUN(vsync_address_outside_the_declared_window_is_refused);
  RUN(a_title_override_is_dispatched_before_a_host_service_leaf);
  RUN(a_host_service_leaf_still_answers_an_unclaimed_address);
  return pt_summary();
}
