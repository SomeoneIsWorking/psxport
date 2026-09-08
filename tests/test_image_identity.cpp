#include "core.h"
#include "image_identity.h"
#include "testutil.h"

#include <memory>

namespace {

void test_address_aliases_and_unload_reveal_are_preserved() {
  psx::cpu::ImageCatalog catalog;
  const auto base = catalog.activate("base", {0x1000u, 0x2000u}, 11u);
  const auto overlay = catalog.activate("overlay", {0x1400u, 0x1600u}, 22u);
  CHECK(catalog.resolve(0x1000u) == base);
  CHECK(catalog.resolve(0x1400u) == overlay);
  CHECK(catalog.resolve(0x80001400u) == overlay);
  CHECK(catalog.resolve(0xa0001400u) == overlay);
  CHECK(catalog.resolve(0x1600u) == base);
  CHECK(!catalog.resolve(0x2000u));
  CHECK_EQ(catalog.activeCount(), 2u);
  CHECK(catalog.deactivate(overlay));
  CHECK(!catalog.deactivate(overlay));
  CHECK(catalog.resolve(0x80001400u) == base);
  CHECK_EQ(catalog.activeCount(), 1u);
}

void test_range_requires_one_complete_active_residency() {
  psx::cpu::ImageCatalog catalog;
  const GuestAddressRange whole{0x1000u, 0x2000u};
  CHECK(!catalog.resolve(whole));
  const auto base = catalog.activate("base", whole, 11u);
  CHECK(catalog.resolve(whole) == base);
  CHECK(catalog.resolve(GuestAddressRange{0x1101u, 0x1102u}) == base);
  CHECK(!catalog.resolve(GuestAddressRange{0x0fffu, 0x1001u}));
  CHECK(!catalog.resolve(GuestAddressRange{0x1fffu, 0x2001u}));

  // The starting address still names base, but one interior byte now has another residency.
  const auto interior = catalog.activate("interior", {0x1800u, 0x1801u}, 22u);
  CHECK(catalog.resolve(whole.begin) == base);
  CHECK(!catalog.resolve(whole));
  CHECK(catalog.resolve(GuestAddressRange{0x1000u, 0x1800u}) == base);
  CHECK(catalog.resolve(GuestAddressRange{0x1800u, 0x1801u}) == interior);
  CHECK(catalog.resolve(GuestAddressRange{0x1801u, 0x2000u}) == base);
  CHECK(!catalog.resolve(GuestAddressRange{0x1800u, 0x1802u}));
  CHECK(catalog.deactivate(interior));
  CHECK(catalog.resolve(whole) == base);
}

void test_reload_and_adjacent_generations_do_not_merge() {
  psx::cpu::ImageCatalog catalog;
  const GuestAddressRange whole{0x1000u, 0x2000u};
  const auto original = catalog.activate("world", whole, 11u);
  const auto reload = catalog.activate("world", whole, 11u);
  CHECK(original != reload);
  CHECK(reload.generation > original.generation);
  CHECK(catalog.resolve(whole) == reload);
  const auto first = catalog.activate("world", {0x1000u, 0x1800u}, 11u);
  const auto second = catalog.activate("world", {0x1800u, 0x2000u}, 11u);
  CHECK(first != second);
  CHECK(!catalog.resolve(whole));
  CHECK(catalog.resolve(GuestAddressRange{0x1000u, 0x1800u}) == first);
  CHECK(catalog.resolve(GuestAddressRange{0x1800u, 0x2000u}) == second);
  const auto covering = catalog.activate("replacement", {0x0800u, 0x2800u}, 33u);
  CHECK(catalog.resolve(whole) == covering);
  CHECK(catalog.deactivate(covering));
  CHECK(!catalog.resolve(whole));
  CHECK(catalog.deactivate(second));
  CHECK(!catalog.resolve(whole));
  CHECK(catalog.deactivate(first));
  CHECK(catalog.resolve(whole) == reload);
  CHECK(catalog.deactivate(reload));
  CHECK(catalog.resolve(whole) == original);
  CHECK(catalog.deactivate(original));
  CHECK(!catalog.resolve(whole));
}

void test_gaps_empty_invalid_and_nonphysical_ranges_refuse() {
  psx::cpu::ImageCatalog catalog;
  catalog.activate("left", {0x1000u, 0x1100u}, 11u);
  catalog.activate("right", {0x1200u, 0x1300u}, 11u);
  CHECK(!catalog.resolve(GuestAddressRange{0x1000u, 0x1300u}));
  for (GuestAddressRange range : {GuestAddressRange{},
                                  {0x1000u, 0x1000u},
                                  {0x1100u, 0x1000u},
                                  {0x1000u, 0u},
                                  {0u, 0xffffffffu},
                                  {0x80001000u, 0x80001100u},
                                  {0xa0001000u, 0xa0001100u}}) {
    CHECK(!catalog.resolve(range));
  }
  const auto top = catalog.activate("top", {0x1fffffffu, 0x20000000u}, 33u);
  CHECK(catalog.resolve(GuestAddressRange{0x1fffffffu, 0x20000000u}) == top);
  CHECK(!catalog.resolve(GuestAddressRange{0x1fffffffu, 0x20000001u}));
}

void test_core_forwards_range_resolution_to_its_own_catalog() {
  auto core = std::make_unique<Core>();
  const GuestAddressRange whole{0x1000u, 0x2000u};
  const auto base = core->imageCatalog().activate("world", whole, 11u);
  CHECK(core->currentImageIdentity(whole) == base);
  CHECK(core->currentImageIdentity(0x80001000u) == base);
  const auto partial = core->imageCatalog().activate("overlap", {0x1400u, 0x1500u}, 22u);
  CHECK(!core->currentImageIdentity(whole));
  CHECK(core->currentImageIdentity(GuestAddressRange{0x1400u, 0x1500u}) == partial);
  CHECK(core->imageCatalog().deactivate(partial));
  CHECK(core->currentImageIdentity(whole) == base);
}

} // namespace

int main() {
  RUN(address_aliases_and_unload_reveal_are_preserved);
  RUN(range_requires_one_complete_active_residency);
  RUN(reload_and_adjacent_generations_do_not_merge);
  RUN(gaps_empty_invalid_and_nonphysical_ranges_refuse);
  RUN(core_forwards_range_resolution_to_its_own_catalog);
  return pt_summary();
}
