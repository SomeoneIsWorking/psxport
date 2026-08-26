// test_platform_hle_direct_runtime.cpp — direct runtimes (core.cfg == nullptr) must be able to
// declare their own hardware-sync primitives without instantiating the legacy GameConfig bag.
//
// WHY THIS EXISTS: Tekken 3 derives directly from GameRuntime (its repo rules forbid the adapter),
// and its whole-program boot spins forever inside libetc's VSync because PlatformHle::initBuiltins()
// reads ONLY GameConfig::hle — which does not exist for a direct runtime. native_boot.cpp:391 never
// runs on a direct-boot path either, so nothing announces the gap; the guest just hangs. The seam
// under test: GameRuntime::platformHlePlan(), a consumer-owned fact slice per
// docs/plans/game-seam-redesign.md ("platform-library entry tables"), consumed by initBuiltins()
// when core.cfg is null.
//
// HERMETIC: no disc, no GPU, no window. The VSync HLE case drives EmulatedTime directly through
// advanceGuestInstructionTicks / advanceDisplayFields.

#include "emulated_time.h"
#include "field_rate.h"
#include "game.h"
#include "game_runtime.h"
#include "hw_bind.h"
#include "platform_hle.h"
#include "testutil.h"
#include "timing.h"

#include <memory>

namespace {

// Measured Tekken 3 (SLUS_004.02) facts, used here as opaque stand-in constants: libetc VSync at
// 0x800859A8 with body extent [0x800859A8, 0x80085B20). The GAME owns these numbers; this test only
// needs addresses that are inside/outside a declared window.
constexpr uint32_t kVSyncAddr = 0x800859A8u;
constexpr uint32_t kVSyncEnd = 0x80085B20u;

// Measured CTR (SCUS_944.26) libgte projection leaves. They share one verified SCEI-library
// window: SetGeomScreen [0x8007781C,0x8007782C), SetGeomOffset [0x8007782C,0x80077844).
constexpr uint32_t kSetGeomScreenAddr = 0x8007781Cu;
constexpr uint32_t kSetGeomOffsetAddr = 0x8007782Cu;
constexpr uint32_t kSetGeomLeavesEnd = 0x80077844u;

int g_handlerCalls = 0;
void recorded_handler(Core *) {
  ++g_handlerCalls;
}

class SilentRuntime : public GameRuntime {
public:
  const GuestProgramImage *guestProgramImage() const override {
    return &image_;
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  void *createContext(Core &) override {
    return &ctx_;
  }
  void destroyContext(void *c) override {
    CHECK_EQ(c, &ctx_);
  }
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}

  GuestProgramImage image_{};
  int ctx_ = 0;
};

class PlannedRuntime final : public SilentRuntime {
public:
  const PlatformHlePlan *platformHlePlan() const override {
    return &plan_;
  }

  PlatformHlePlan plan_{};
};

GuestProgramImage makeImage() {
  GuestProgramImage image{};
  image.residentText = {0x00010000u, 0x00120000u};
  image.backtraceText = {0x00010000u, 0x00130000u};
  return image;
}

} // namespace

static void test_direct_runtime_without_plan_installs_nothing_and_says_so() {
  SilentRuntime runtime;
  runtime.image_ = makeImage();
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  // The honest default: a direct runtime that declares no sync primitives installs none. Any
  // address then misses — including the one that would have been bound — and initBuiltins()
  // must not dereference the absent legacy config to get there.
  game->platform_hle.initBuiltins();
  CHECK_EQ(g_handlerCalls, 0);
  CHECK(game->platform_hle.lookup(kVSyncAddr | 0x80000000u) == nullptr);
  CHECK(game->platform_hle.lookup((kVSyncAddr + 0x100) | 0x80000000u) == nullptr);
}

static void test_direct_runtime_plan_binds_entries_inside_the_declared_window() {
  PlannedRuntime runtime;
  runtime.image_ = makeImage();
  runtime.plan_.bindingCount = 1;
  runtime.plan_.bindings[0] = {kVSyncAddr, recorded_handler};
  runtime.plan_.windowLo[0] = kVSyncAddr;
  runtime.plan_.windowHi[0] = kVSyncEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();

  // Inside the declared window AND exactly at the bound address: the handler resolves and runs.
  g_handlerCalls = 0;
  OverrideFn fn = game->platform_hle.lookup(kVSyncAddr | 0x80000000u);
  CHECK(fn != nullptr);
  if (fn) {
    fn(&game->core);
  }
  CHECK_EQ(g_handlerCalls, 1);

  // The window guard still protects the table: the byte past the measured body end is refused,
  // and so is engine text far below the library window.
  CHECK(game->platform_hle.lookup(kVSyncEnd | 0x80000000u) == nullptr);
  CHECK(game->platform_hle.lookup(0x80050000u | 0x80000000u) == nullptr);
}

static void test_direct_runtime_plan_binds_standard_libgte_projection_leaves() {
  PlannedRuntime runtime;
  runtime.image_ = makeImage();
  runtime.plan_.setGeomOffset = kSetGeomOffsetAddr;
  runtime.plan_.setGeomScreen = kSetGeomScreenAddr;
  runtime.plan_.windowLo[0] = kSetGeomScreenAddr;
  runtime.plan_.windowHi[0] = kSetGeomLeavesEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();

  OverrideFn setScreen = game->platform_hle.lookup(kSetGeomScreenAddr);
  OverrideFn setOffset = game->platform_hle.lookup(kSetGeomOffsetAddr);
  CHECK(setScreen != nullptr);
  CHECK(setOffset != nullptr);

  Core &core = game->core;
  gte_bind(&core);
  core.r[4] = 401u;
  if (setScreen) {
    setScreen(&core);
  }
  CHECK_EQ(core.rsub.projParams.geomH(), 401.0f);
  CHECK_EQ(core.r[4], 401u); // SetGeomScreen does not mutate its argument register.
  CHECK(!core.rsub.projParams.geomValid());

  core.r[4] = 172u;
  core.r[5] = 119u;
  if (setOffset) {
    setOffset(&core);
  }
  CHECK_EQ(core.rsub.projParams.geomOfx(), 172.0f);
  CHECK_EQ(core.rsub.projParams.geomOfy(), 119.0f);
  CHECK_EQ(core.r[4], 172u << 16);
  CHECK_EQ(core.r[5], 119u << 16);
  CHECK(core.rsub.projParams.geomValid());
}

static void test_direct_runtime_standard_libgte_leaves_still_require_the_declared_window() {
  PlannedRuntime runtime;
  runtime.image_ = makeImage();
  runtime.plan_.setGeomOffset = kSetGeomOffsetAddr;
  runtime.plan_.setGeomScreen = kSetGeomScreenAddr;
  // Admit only SetGeomOffset. The typed SetGeomScreen field must not bypass the standard guard.
  runtime.plan_.windowLo[0] = kSetGeomOffsetAddr;
  runtime.plan_.windowHi[0] = kSetGeomLeavesEnd;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->platform_hle.initBuiltins();

  CHECK(game->platform_hle.lookup(kSetGeomOffsetAddr) != nullptr);
  CHECK(game->platform_hle.lookup(kSetGeomScreenAddr) == nullptr);
}

static void test_vsync_hle_query_returns_emulated_display_fields() {
  PlannedRuntime runtime; // any runtime; the VSync HLE is generic framework behavior
  runtime.image_ = makeImage();
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  Core &core = game->core;
  Timing &timing = game->timing;

  // Two whole display fields of emulated time, advanced through the public clock API.
  CHECK(timing.advanceDisplayFields(2, 1, FIELD_RATE_NTSC_MILLIHZ));
  core.r[4] = 0xFFFFFFFFu; // VSync(-1): QUERY the current vblank count
  Timing::vsyncHle(&core);
  CHECK_EQ(core.r[2], 2u); // $v0: two display fields have elapsed
  CHECK_EQ(timing.vblank, 2u);

  // One more field moves the answer with it.
  CHECK(timing.advanceDisplayFields(1, 1, FIELD_RATE_NTSC_MILLIHZ));
  Timing::vsyncHle(&core);
  CHECK_EQ(core.r[2], 3u);
}

static void test_vsync_hle_wait_consumes_the_field_interval() {
  PlannedRuntime runtime;
  runtime.image_ = makeImage();
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();
  Core &core = game->core;

  const uint64_t before = game->timing.emulatedCpuTicks();
  core.r[4] = 3u; // VSync(3): wait three vblanks
  Timing::vsyncHle(&core);
  CHECK_EQ(core.r[2], 3u);
  const uint64_t consumed = game->timing.emulatedCpuTicks() - before;
  CHECK_EQ(consumed, display_field_cpu_ticks(3, 1, FIELD_RATE_NTSC_MILLIHZ));

  // The counter mirror the recomp code reads (DAT_…abde0) moved with the wait.
  CHECK_EQ(game->core.mem_r32(0x800ABDE0u), 3u);
}

int main(void) {
  RUN(direct_runtime_without_plan_installs_nothing_and_says_so);
  RUN(direct_runtime_plan_binds_entries_inside_the_declared_window);
  RUN(direct_runtime_plan_binds_standard_libgte_projection_leaves);
  RUN(direct_runtime_standard_libgte_leaves_still_require_the_declared_window);
  RUN(vsync_hle_query_returns_emulated_display_fields);
  RUN(vsync_hle_wait_consumes_the_field_interval);
  return pt_summary();
}
