// A runtime reports its packet pool, so the census attributes its packets; one that declares none
// answers "not owned".
#include "stub_game_runtime.h"
#include "testutil.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "guest_packet_pool_windows.h"
#include "ot_attr.h"

#include <memory>
#include <optional>

namespace {

constexpr uint32_t kBasePtr0 = 0x80001000u;
constexpr uint32_t kEndPtr0 = 0x80001004u;
constexpr uint32_t kBasePtr1 = 0x80001008u;
constexpr uint32_t kEndPtr1 = 0x8000100Cu;

// The fixed-array representation: one contiguous pair of parity pools.
class FixedPoolRuntime final : public psx::test::StubGameRuntime {
public:
  std::optional<psx::gpu::PacketPoolExtent> packetPool(Core &core) const override {
    return windows.extent(core);
  }
  psx::gpu::GuestPacketPoolWindows windows{
      .representation = psx::gpu::GuestPacketPoolWindows::Representation::FixedBaseStride,
      .base = 0x80020000u,
      .stride = 0x1000u,
  };
};

// The heap representation: two independently allocated pools named by guest globals. Crash Bash's
// real declaration, and the one whose bounds move when the guest reallocates.
class DynamicPoolRuntime final : public psx::test::StubGameRuntime {
public:
  std::optional<psx::gpu::PacketPoolExtent> packetPool(Core &core) const override {
    return windows.extent(core);
  }
  psx::gpu::GuestPacketPoolWindows windows{
      .representation = psx::gpu::GuestPacketPoolWindows::Representation::LiveBaseEndPointers,
      .basePointer = {kBasePtr0, kBasePtr1},
      .endPointer = {kEndPtr0, kEndPtr1},
  };
};

// Counts how often the census asks for the pool.
class CountingPoolRuntime final : public psx::test::StubGameRuntime {
public:
  std::optional<psx::gpu::PacketPoolExtent> packetPool(Core &core) const override {
    ++queries;
    return windows.extent(core);
  }
  psx::gpu::GuestPacketPoolWindows windows{
      .representation = psx::gpu::GuestPacketPoolWindows::Representation::FixedBaseStride,
      .base = 0x80020000u,
      .stride = 0x1000u,
  };
  mutable int queries = 0;
};

// Declares no pool at all. This is the DEFAULT every runtime inherits, and the case that must answer
// "not owned" rather than match whatever the last Core left behind.
class NoPoolRuntime final : public psx::test::StubGameRuntime {};

void write_packet(Core &core, uint32_t addr) {
  core.mem_w32(addr, 0x2C000000u);
  core.mem_w32(addr + 4u, 0x00000000u);
}

// A fixed pool attributes the guest's packet.
void test_runtime_fixed_pool_is_attributable() {
  FixedPoolRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  CHECK(psx::test::require(runtime.packetPool(game->core)).contains(0x80020020u));

  OtAttr &attr = game->core.rsub.otAttr;
  write_packet(game->core, 0x80020020u);

  OtAttr::Span span{};
  CHECK(attr.lookupStore(0x80020020u, &span));

  // A packet the runtime's window does NOT cover is still unowned, so the fixed array is not
  // being read as "the whole of RAM".
  write_packet(game->core, 0x80090020u);
  CHECK(!attr.lookupStore(0x80090020u, nullptr));
}

// THE POSITIVE, heap form: both parity pools are tracked and the gap between them is NOT.
void test_runtime_dynamic_pool_tracks_both_parities_without_the_gap() {
  DynamicPoolRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  CHECK(!psx::test::require(runtime.packetPool(game->core)).contains(0x80020020u)); // no pool allocated yet
  game->core.mem_w32(kBasePtr0, 0x80020000u);
  game->core.mem_w32(kEndPtr0, 0x80021000u);
  game->core.mem_w32(kBasePtr1, 0x80100000u);
  game->core.mem_w32(kEndPtr1, 0x80101000u);

  OtAttr &attr = game->core.rsub.otAttr;
  write_packet(game->core, 0x80020020u);
  write_packet(game->core, 0x80100020u);
  game->core.mem_w32(0x80080020u, 0x33333333u);

  CHECK(attr.lookupStore(0x80020020u, nullptr));
  CHECK(attr.lookupStore(0x80100020u, nullptr));
  CHECK(!attr.lookupStore(0x80080020u, nullptr));
}

// The live bounds are re-read when the guest rewrites its own pointer globals, so a reallocated pool
// is tracked without the framework being told.
void test_runtime_dynamic_pool_follows_a_reallocation() {
  DynamicPoolRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  game->core.mem_w32(kBasePtr0, 0x80020000u);
  game->core.mem_w32(kEndPtr0, 0x80021000u);
  game->core.mem_w32(kBasePtr1, 0x80100000u);
  game->core.mem_w32(kEndPtr1, 0x80101000u);

  OtAttr &attr = game->core.rsub.otAttr;
  write_packet(game->core, 0x80020020u);
  CHECK(attr.lookupStore(0x80020020u, nullptr));

  attr.beginLogicFrame(1);
  game->core.mem_w32(kBasePtr0, 0x80030000u);
  game->core.mem_w32(kEndPtr0, 0x80031000u);
  write_packet(game->core, 0x80030020u);

  CHECK(!attr.lookupStore(0x80020020u, nullptr));
  CHECK(attr.lookupStore(0x80030020u, nullptr));
}

// THE NEGATIVE, and the case that makes the zero honest: a runtime that declares nothing answers
// "not owned" for every packet. It must NOT inherit a window, and it must NOT be silenced into
// claiming the guest submitted nothing.
void test_runtime_declaring_no_pool_answers_not_owned() {
  NoPoolRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});

  CHECK(!runtime.packetPool(game->core));

  OtAttr &attr = game->core.rsub.otAttr;
  write_packet(game->core, 0x80020020u);

  CHECK(!attr.lookupStore(0x80020020u, nullptr));
}

// The census reads the pool once per logic frame, not once per guest store.
void test_the_pool_is_read_once_per_logic_frame() {
  CountingPoolRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  const int before = runtime.queries;
  for (uint32_t word = 0; word < 8; ++word) {
    game->core.mem_w32(0x80020020u + (4u * word), word);
  }
  CHECK_EQ(runtime.queries, before + 1);
  game->core.rsub.otAttr.beginLogicFrame(1);
  game->core.mem_w32(0x80020020u, 0u);
  game->core.mem_w32(0x80020024u, 0u);
  CHECK_EQ(runtime.queries, before + 2);
}

} // namespace

int main() {
  RUN(runtime_fixed_pool_is_attributable);
  RUN(runtime_dynamic_pool_tracks_both_parities_without_the_gap);
  RUN(runtime_dynamic_pool_follows_a_reallocation);
  RUN(runtime_declaring_no_pool_answers_not_owned);
  RUN(the_pool_is_read_once_per_logic_frame);
  return psx::test::summary();
}
