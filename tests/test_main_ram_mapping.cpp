#include "core.h"
#include "game_iface.h"
#include "testutil.h"

#include <memory>

namespace {

std::unique_ptr<Core> bareCore() {
  static const GameConfig config{};
  static const GameHooks hooks{};
  psxport_install_game(&config, &hooks);
  return std::make_unique<Core>();
}

void test_main_ram_aliases_share_the_store_mapping() {
  auto core = bareCore();
  constexpr GuestAddressRange expected{0x10000u, 0x10800u};
  for (const std::uint32_t address : {0x00010000u, 0x80010000u, 0xA0010000u, 0x80210000u, 0x80410000u, 0x80610000u}) {
    const auto mapped = core->mappedMainRamRange(address, 2048u);
    CHECK(mapped.has_value());
    CHECK_EQ(mapped->begin, expected.begin);
    CHECK_EQ(mapped->end, expected.end);
    core->mem_w8(address, 0x5Au);
    CHECK_EQ(core->ram[expected.begin], 0x5Au);
  }
}

void test_non_main_ram_and_straddling_spans_refuse() {
  auto core = bareCore();
  const auto lastByte = core->mappedMainRamRange(0x807FFFFFu, 1u);
  CHECK(lastByte.has_value());
  CHECK_EQ(lastByte->begin, 0x1FFFFFu);
  CHECK_EQ(lastByte->end, 0x200000u);
  CHECK(!core->mappedMainRamRange(0x807FFFFFu, 2u));
  CHECK(!core->mappedMainRamRange(0x9F800100u, 1u));
  CHECK(!core->mappedMainRamRange(0x1F801800u, 1u));
  CHECK(!core->mappedMainRamRange(0x80800000u, 1u));
  CHECK(!core->mappedMainRamRange(0x80010000u, 0u));
  CHECK(!core->mappedMainRamRange(0x80010000u, 0x200001u));
}

} // namespace

int main() {
  RUN(main_ram_aliases_share_the_store_mapping);
  RUN(non_main_ram_and_straddling_spans_refuse);
  return pt_summary();
}
