// test_guest_call_now.cpp — psx::cpu::callGuestNow is the one owner of "call a guest leaf that must
// return within the current turn". It must deliver every argument to r4..r7 in order, leave the
// argument registers it was not given alone, and reach a native override through the same image-
// scoped dispatch as any other guest call.
#include "game.h"
#include "game_runtime.h"
#include "guest_call.h"
#include "native_dispatch.h"
#include "testutil.h"

#include <cstdint>
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

constexpr std::uint32_t kLeaf = 0x00010100u;
constexpr std::uint32_t kUntouched = 0x5a5a5a5au;

int leafCalls = 0;
std::uint32_t seenArguments[4] = {};

void nativeLeaf(Core *core) {
  ++leafCalls;
  for (int i = 0; i < 4; ++i) {
    seenArguments[i] = core->r[4 + i];
  }
}

std::unique_ptr<Game> makeGame(Runtime &runtime) {
  psxport_install_game(runtime);
  return std::make_unique<Game>();
}

void primeArgumentRegisters(Core &core) {
  for (int i = 4; i < 8; ++i) {
    core.r[i] = kUntouched;
  }
  leafCalls = 0;
}
} // namespace

static void test_arguments_reach_r4_to_r7_in_order() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = core.imageCatalog().activate("guest-call-now", {kLeaf, kLeaf + 16u}, 0x43414c4c4e4f57ull);
  CHECK(core.nativeDispatcher().install({{image, kLeaf}, "native-leaf", nativeLeaf}));

  primeArgumentRegisters(core);
  psx::cpu::callGuestNow(core, "test", kLeaf, 11u, 22u, 33u, 44u);
  CHECK_EQ(leafCalls, 1);
  CHECK_EQ(seenArguments[0], 11u);
  CHECK_EQ(seenArguments[1], 22u);
  CHECK_EQ(seenArguments[2], 33u);
  CHECK_EQ(seenArguments[3], 44u);
}

static void test_unpassed_argument_registers_are_left_alone() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = core.imageCatalog().activate("guest-call-now", {kLeaf, kLeaf + 16u}, 0x43414c4c4e4f57ull);
  CHECK(core.nativeDispatcher().install({{image, kLeaf}, "native-leaf", nativeLeaf}));

  primeArgumentRegisters(core);
  psx::cpu::callGuestNow(core, "test", kLeaf);
  CHECK_EQ(leafCalls, 1);
  CHECK_EQ(seenArguments[0], kUntouched);
  CHECK_EQ(seenArguments[3], kUntouched);

  primeArgumentRegisters(core);
  const std::uint8_t narrow = 7;
  psx::cpu::callGuestNow(core, "test", kLeaf, narrow, 0x80010000u);
  CHECK_EQ(leafCalls, 1);
  CHECK_EQ(seenArguments[0], 7u);
  CHECK_EQ(seenArguments[1], 0x80010000u);
  CHECK_EQ(seenArguments[2], kUntouched);
  CHECK_EQ(seenArguments[3], kUntouched);
}

int main() {
  RUN(arguments_reach_r4_to_r7_in_order);
  RUN(unpassed_argument_registers_are_left_alone);
  return pt_summary();
}
