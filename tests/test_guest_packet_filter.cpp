// Guest packet filtering must suppress only a producer's visual packet contribution.  The guest
// call still performs all of its ABI, scratchpad, GTE, packet-pool, and ordering-table writes; this
// test pins the framework half of that contract by requiring the owner tag to survive from the store
// to the later packet lookup.
#include "testutil.h"

#include "core.h"
#include "game.h"
#include "game_iface.h"
#include "gpu_device.h"
#include "guest_packet_filter.h"
#include "ot_attr.h"

#include <memory>

namespace {
constexpr uint32_t kPoolBase = 0x80020000u;
constexpr uint32_t kPoolStride = 0x1000u;
constexpr uint32_t kProducerA = 0x80012340u;
constexpr uint32_t kProducerB = 0x80056780u;

std::unique_ptr<Core> filter_core() {
  static const GameConfig cfg = {
      .packetPoolBase = kPoolBase,
      .packetPoolStride = kPoolStride,
  };
  static const GameHooks hooks{};
  psxport_install_game(&cfg, &hooks);
  return std::make_unique<Core>();
}

Game &filter_game() {
  static const GameConfig cfg = {
      .packetPoolBase = kPoolBase,
      .packetPoolStride = kPoolStride,
  };
  static const GameHooks hooks{};
  // The legacy runtime is process-global. Construct this fixture once, after installing its
  // runtime, so a later test setup cannot invalidate Core::runtime while this Game is alive.
  static const bool installed = (psxport_install_game(&cfg, &hooks), true);
  (void)installed;
  static Game game;
  return game;
}

uint32_t xy(int x, int y) {
  return (uint32_t)(x & 0xFFFF) | ((uint32_t)(y & 0xFFFF) << 16);
}

void submit_flat_triangle(Game &game, bool suppress) {
  GuestPacketFilter &filter = game.core.rsub.guestPacketFilter;
  filter.setSuppressed(kProducerA, suppress);
  game.core.rsub.otAttr.beginLogicFrame(suppress ? 2 : 1);
  {
    GuestPacketOwnerScope owner(&filter, kProducerA);
    game.core.mem_w32(kPoolBase, (4u << 24) | 0x00FFFFFFu);
    game.core.mem_w32(kPoolBase + 4u, 0x200000FFu);
    game.core.mem_w32(kPoolBase + 8u, xy(4, 4));
    game.core.mem_w32(kPoolBase + 12u, xy(12, 4));
    game.core.mem_w32(kPoolBase + 16u, xy(4, 12));
  }
  gpu_dma2_linked_list(&game.core, kPoolBase);
}
} // namespace

static void test_suppressed_owner_survives_guest_packet_store(void) {
  auto core = filter_core();
  GuestPacketFilter &filter = core->rsub.guestPacketFilter;
  filter.setSuppressed(kProducerA, true);
  const bool previousCensusArm = g_producer_census_armed;
  g_producer_census_armed = false;

  {
    GuestPacketOwnerScope owner(&filter, kProducerA);
    core->mem_w32(kPoolBase + 0x20u, 0x2C000000u);
    core->mem_w32(kPoolBase + 0x24u, 0x00000000u);
  }
  g_producer_census_armed = previousCensusArm;

  OtAttr::Span span{};
  CHECK(core->rsub.otAttr.lookupStore(kPoolBase + 0x20u, &span));
  CHECK_EQ(span.guestProducer, kProducerA);
  CHECK(filter.suppresses(span.guestProducer));
  CHECK(filter.suppressesPacket(core->rsub.otAttr, kPoolBase + 0x20u));
}

static void test_other_or_unattributed_packets_remain_visible(void) {
  auto core = filter_core();
  GuestPacketFilter &filter = core->rsub.guestPacketFilter;
  filter.setSuppressed(kProducerA, true);
  {
    GuestPacketOwnerScope owner(&filter, kProducerB);
    core->mem_w32(kPoolBase + 0x40u, 0x64000000u);
  }

  CHECK(!filter.suppressesPacket(core->rsub.otAttr, kPoolBase + 0x40u));
  CHECK(!filter.suppressesPacket(core->rsub.otAttr, kPoolBase + 0x800u));
}

static void test_nested_owner_scope_restores_the_outer_producer(void) {
  GuestPacketFilter filter;
  {
    GuestPacketOwnerScope outer(&filter, kProducerA);
    CHECK_EQ(filter.currentOwner(), kProducerA);
    {
      GuestPacketOwnerScope inner(&filter, kProducerB);
      CHECK_EQ(filter.currentOwner(), kProducerB);
    }
    CHECK_EQ(filter.currentOwner(), kProducerA);
  }
  CHECK_EQ(filter.currentOwner(), 0u);
}

static uint16_t device_pixel(Game &game, int x, int y) {
  return game.core.gpuDevice.vram()[static_cast<size_t>(y) * psx::gpu::kDeviceVramWidth + static_cast<size_t>(x)];
}

// The filter acts on the native render only; the GPU device the guest observes draws every packet.
static void test_filter_never_reaches_the_device(void) {
  Game &game = filter_game();
  gpu_gp0(&game.core, 0xE3000000u); // drawing area (0,0)-(1023,511)
  gpu_gp0(&game.core, 0xE4000000u | (511u << 10) | 1023u);

  submit_flat_triangle(game, false);
  CHECK(device_pixel(game, 5, 5) != 0u);
  // DMA stamps the command word at node + 4 into s_fifo_addr[0]. The exact lookup key used by
  // gp0_exec must retain the owner tag, not merely another word from the same producer call.
  OtAttr::Span span{};
  CHECK(game.core.rsub.otAttr.lookupStore(kPoolBase + 4u, &span));
  CHECK_EQ(span.guestProducer, kProducerA);
  CHECK(game.core.rsub.guestPacketFilter.suppressesPacket(game.core.rsub.otAttr, kPoolBase + 4u) == false);

  gpu_gp0(&game.core, 0x02000000u); // black fill over the triangle
  gpu_gp0(&game.core, xy(0, 0));
  gpu_gp0(&game.core, xy(16, 16));
  CHECK_EQ(device_pixel(game, 5, 5), 0u);
  submit_flat_triangle(game, true);
  CHECK_EQ(game.core.mem_r32(kPoolBase + 4u), 0x200000FFu);
  CHECK(game.core.rsub.guestPacketFilter.suppressesPacket(game.core.rsub.otAttr, kPoolBase + 4u));
  CHECK(device_pixel(game, 5, 5) != 0u);
}

int main() {
  RUN(suppressed_owner_survives_guest_packet_store);
  RUN(other_or_unattributed_packets_remain_visible);
  RUN(nested_owner_scope_restores_the_outer_producer);
  RUN(filter_never_reaches_the_device);
  return pt_summary();
}
