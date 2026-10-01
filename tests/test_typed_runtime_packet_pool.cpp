// A typed GameRuntime declares its own packet-pool windows, so its GuestPacketFilter is not blind.
//
// The bug this pins is the shape this project keeps finding: a title that HAS reverse-engineered its
// packet pool still had nowhere to say so, because the window came only from a legacy GameConfig.
// The consequence was a confident zero — the filter matched no span and answered "not owned" for
// every packet, which reads exactly like a guest that submitted nothing there.
//
// Both representations are covered, because a title declares whichever one its guest actually uses and
// collapsing them into a single code path is how they drift. The negative is covered too, and it is the
// half that matters: a runtime that declares nothing must answer "not owned", NOT accidentally match
// a leftover span from another Core.
#include "testutil.h"

#include "core.h"
#include "game.h"
#include "game_iface.h"
#include "game_runtime.h"
#include "guest_packet_filter.h"
#include "guest_packet_pool_windows.h"
#include "ot_attr.h"

#include <memory>

namespace {

constexpr uint32_t kProducerA = 0x80012340u;
constexpr uint32_t kProducerB = 0x80056780u;

constexpr uint32_t kBasePtr0 = 0x80001000u;
constexpr uint32_t kEndPtr0 = 0x80001004u;
constexpr uint32_t kBasePtr1 = 0x80001008u;
constexpr uint32_t kEndPtr1 = 0x8000100Cu;

class TypedRuntime : public GameRuntime {
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

// The fixed-array representation: one contiguous pair of parity pools.
class FixedPoolRuntime final : public TypedRuntime {
public:
  const GuestPacketPoolWindows *guestPacketPoolWindows() const override {
    return &windows;
  }
  GuestPacketPoolWindows windows{
      .representation = GuestPacketPoolWindows::Representation::FixedBaseStride,
      .base = 0x80020000u,
      .stride = 0x1000u,
  };
};

// The heap representation: two independently allocated pools named by guest globals. Crash Bash's
// real declaration, and the one whose bounds move when the guest reallocates.
class DynamicPoolRuntime final : public TypedRuntime {
public:
  const GuestPacketPoolWindows *guestPacketPoolWindows() const override {
    return &windows;
  }
  GuestPacketPoolWindows windows{
      .representation = GuestPacketPoolWindows::Representation::LiveBaseEndPointers,
      .basePointer = {kBasePtr0, kBasePtr1},
      .endPointer = {kEndPtr0, kEndPtr1},
  };
};

// Declares no pool at all. This is the DEFAULT every runtime inherits, and the case that must answer
// "not owned" rather than match whatever the last Core left behind.
class NoPoolRuntime final : public TypedRuntime {};

void write_owned_packet(Core &core, GuestPacketFilter &filter, uint32_t addr, uint32_t producer) {
  filter.setSuppressed(producer, true);
  {
    GuestPacketOwnerScope owner(&filter, producer);
    core.mem_w32(addr, 0x2C000000u);
    core.mem_w32(addr + 4u, 0x00000000u);
  }
}

} // namespace

// THE POSITIVE: a typed runtime that declares a fixed pool attributes the guest's packet and the
// filter suppresses it — the exact pair of answers that were impossible before.
static void test_typed_runtime_fixed_pool_is_attributable_and_suppressible() {
  static FixedPoolRuntime runtime;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  CHECK(game->core.cfg == nullptr);
  CHECK(declaredGuestPacketPoolWindows(game->core) == &runtime.windows);

  OtAttr &attr = game->core.rsub.otAttr;
  GuestPacketFilter &filter = game->core.rsub.guestPacketFilter;
  write_owned_packet(game->core, filter, 0x80020020u, kProducerA);

  OtAttr::Span span{};
  CHECK(attr.lookupStore(0x80020020u, &span));
  CHECK_EQ(span.guestProducer, kProducerA);
  CHECK(filter.suppressesPacket(attr, 0x80020020u));

  // A packet the typed runtime's window does NOT cover is still unowned, so the fixed array is not
  // being read as "the whole of RAM".
  write_owned_packet(game->core, filter, 0x80090020u, kProducerB);
  CHECK(!attr.lookupStore(0x80090020u, nullptr));
  CHECK(!filter.suppressesPacket(attr, 0x80090020u));
}

// THE POSITIVE, heap form: both parity pools are tracked and the gap between them is NOT.
static void test_typed_runtime_dynamic_pool_tracks_both_parities_without_the_gap() {
  static DynamicPoolRuntime runtime;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  CHECK(declaredGuestPacketPoolWindows(game->core) == &runtime.windows);
  game->core.mem_w32(kBasePtr0, 0x80020000u);
  game->core.mem_w32(kEndPtr0, 0x80021000u);
  game->core.mem_w32(kBasePtr1, 0x80100000u);
  game->core.mem_w32(kEndPtr1, 0x80101000u);

  OtAttr &attr = game->core.rsub.otAttr;
  GuestPacketFilter &filter = game->core.rsub.guestPacketFilter;
  write_owned_packet(game->core, filter, 0x80020020u, kProducerA);
  write_owned_packet(game->core, filter, 0x80100020u, kProducerB);
  game->core.mem_w32(0x80080020u, 0x33333333u);

  CHECK(attr.lookupStore(0x80020020u, nullptr));
  CHECK(attr.lookupStore(0x80100020u, nullptr));
  CHECK(!attr.lookupStore(0x80080020u, nullptr));
  CHECK(filter.suppressesPacket(attr, 0x80020020u));
  CHECK(filter.suppressesPacket(attr, 0x80100020u));
}

// The live bounds are re-read when the guest rewrites its own pointer globals, so a reallocated pool
// is tracked without the framework being told. Before this seam a typed runtime had no bounds at all,
// so this case had nothing to re-read.
static void test_typed_runtime_dynamic_pool_follows_a_reallocation() {
  static DynamicPoolRuntime runtime;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  game->core.mem_w32(kBasePtr0, 0x80020000u);
  game->core.mem_w32(kEndPtr0, 0x80021000u);
  game->core.mem_w32(kBasePtr1, 0x80100000u);
  game->core.mem_w32(kEndPtr1, 0x80101000u);

  OtAttr &attr = game->core.rsub.otAttr;
  write_owned_packet(game->core, game->core.rsub.guestPacketFilter, 0x80020020u, kProducerA);
  CHECK(attr.lookupStore(0x80020020u, nullptr));

  attr.beginLogicFrame(1);
  game->core.mem_w32(kBasePtr0, 0x80030000u);
  game->core.mem_w32(kEndPtr0, 0x80031000u);
  write_owned_packet(game->core, game->core.rsub.guestPacketFilter, 0x80030020u, kProducerA);

  CHECK(!attr.lookupStore(0x80020020u, nullptr));
  CHECK(attr.lookupStore(0x80030020u, nullptr));
}

// THE NEGATIVE, and the case that makes the zero honest: a runtime that declares nothing answers
// "not owned" for every packet. It must NOT inherit a window, and it must NOT be silenced into
// claiming the guest submitted nothing.
static void test_runtime_declaring_no_pool_answers_not_owned() {
  static NoPoolRuntime runtime;
  psxport_install_game(runtime);
  auto game = std::make_unique<Game>();

  CHECK(game->core.cfg == nullptr);
  CHECK(declaredGuestPacketPoolWindows(game->core) == nullptr);

  OtAttr &attr = game->core.rsub.otAttr;
  GuestPacketFilter &filter = game->core.rsub.guestPacketFilter;
  write_owned_packet(game->core, filter, 0x80020020u, kProducerA);

  CHECK(!attr.lookupStore(0x80020020u, nullptr));
  CHECK(!filter.suppressesPacket(attr, 0x80020020u));
}

// A LEGACY GameConfig keeps its own owner: the typed accessor refuses it, so the config's window is
// still read where it always was and cannot be displaced by a declaration.
static void test_legacy_config_is_never_consulted_as_a_typed_declaration() {
  static const GameConfig cfg = {
      .packetPoolBase = 0x80020000u,
      .packetPoolStride = 0x1000u,
  };
  static const GameHooks hooks{};
  psxport_install_game(&cfg, &hooks);
  auto game = std::make_unique<Game>();

  CHECK_EQ(game->core.cfg, &cfg);
  CHECK(declaredGuestPacketPoolWindows(game->core) == nullptr);

  OtAttr &attr = game->core.rsub.otAttr;
  GuestPacketFilter &filter = game->core.rsub.guestPacketFilter;
  write_owned_packet(game->core, filter, 0x80020020u, kProducerA);

  OtAttr::Span span{};
  CHECK(attr.lookupStore(0x80020020u, &span));
  CHECK_EQ(span.guestProducer, kProducerA);
  CHECK(filter.suppressesPacket(attr, 0x80020020u));
}

int main() {
  RUN(typed_runtime_fixed_pool_is_attributable_and_suppressible);
  RUN(typed_runtime_dynamic_pool_tracks_both_parities_without_the_gap);
  RUN(typed_runtime_dynamic_pool_follows_a_reallocation);
  RUN(runtime_declaring_no_pool_answers_not_owned);
  RUN(legacy_config_is_never_consulted_as_a_typed_declaration);
  return pt_summary();
}
