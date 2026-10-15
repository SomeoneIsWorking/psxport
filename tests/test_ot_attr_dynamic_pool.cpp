#include "testutil.h"

#include "core.h"
#include "guest_packet_pool_windows.h"
#include "ot_attr.h"
#include "render_substrate.h"
#include "stub_game_runtime.h"

#include <memory>
#include <optional>

namespace {

constexpr uint32_t kBasePtr0 = 0x80001000u;
constexpr uint32_t kEndPtr0 = 0x80001004u;
constexpr uint32_t kBasePtr1 = 0x80001008u;
constexpr uint32_t kEndPtr1 = 0x8000100Cu;

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
std::unique_ptr<DynamicPoolRuntime> dynamicPoolRuntime;

std::unique_ptr<Core> dynamic_pool_core() {
  return std::make_unique<Core>(*dynamicPoolRuntime);
}

void set_bounds(Core &core, uint32_t lo0, uint32_t hi0, uint32_t lo1, uint32_t hi1) {
  core.mem_w32(kBasePtr0, lo0);
  core.mem_w32(kEndPtr0, hi0);
  core.mem_w32(kBasePtr1, lo1);
  core.mem_w32(kEndPtr1, hi1);
}

void test_tracks_both_live_pools_without_covering_the_gap() {
  auto core = dynamic_pool_core();
  OtAttr &attr = core->rsub.otAttr;
  set_bounds(*core, 0x80020000u, 0x80021000u, 0x80100000u, 0x80101000u);

  core->mem_w32(0x80020020u, 0x11111111u);
  core->mem_w32(0x80100020u, 0x22222222u);
  core->mem_w32(0x80080020u, 0x33333333u);

  CHECK(attr.lookupStore(0x80020020u, nullptr));
  CHECK(attr.lookupStore(0x80100020u, nullptr));
  CHECK(!attr.lookupStore(0x80080020u, nullptr));
}

void test_bound_pointer_writes_invalidate_the_cached_windows() {
  auto core = dynamic_pool_core();
  OtAttr &attr = core->rsub.otAttr;
  set_bounds(*core, 0x80020000u, 0x80021000u, 0x80100000u, 0x80101000u);
  core->mem_w32(0x80020020u, 0x11111111u);
  CHECK(attr.lookupStore(0x80020020u, nullptr));

  attr.beginLogicFrame(1);
  set_bounds(*core, 0x80030000u, 0x80031000u, 0x80110000u, 0x80111000u);
  core->mem_w32(0x80020020u, 0x22222222u);
  core->mem_w32(0x80030020u, 0x33333333u);

  CHECK(!attr.lookupStore(0x80020020u, nullptr));
  CHECK(attr.lookupStore(0x80030020u, nullptr));
}

} // namespace

int main() {
  dynamicPoolRuntime = std::make_unique<DynamicPoolRuntime>();
  RUN(tracks_both_live_pools_without_covering_the_gap);
  RUN(bound_pointer_writes_invalidate_the_cached_windows);
  return psx::test::summary();
}
