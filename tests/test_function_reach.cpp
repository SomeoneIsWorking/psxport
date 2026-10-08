#include "function_reach.h"
#include "image_identity.h"
#include "testutil.h"

#include <fstream>
#include <iterator>
#include <string>

namespace {

// Relative to the test's working directory, which ctest sets inside the build tree.
std::string reportPath(const char *name) {
  return name;
}

void test_a_pc_is_recorded_once_under_the_image_that_owned_it() {
  psx::cpu::ImageCatalog catalog;
  catalog.activate("exe", {0x10000u, 0x20000u}, 0xAAu);
  psx::cpu::FunctionReach reach(catalog, reportPath("psxport_reach_once.json"));
  reach.observe(0x80010000u);
  reach.observe(0x80010000u);
  reach.observe(0x80010040u);
  CHECK_EQ(reach.reached().size(), 1u);
  const auto &pcs = reach.reached().at({"exe", 0xAAu});
  CHECK_EQ(pcs.size(), 2u);
  CHECK(pcs.count(0x80010000u) == 1u);
  CHECK_EQ(reach.unownedDispatches(), 0u);
}

// Two overlays that reuse one load address must never merge, and a pc seen under the first residency
// must be recorded again once the second overlay replaces it.
void test_address_reusing_overlays_stay_separate() {
  psx::cpu::ImageCatalog catalog;
  const auto first = catalog.activate("overlay-a", {0x30000u, 0x40000u}, 1u);
  psx::cpu::FunctionReach reach(catalog, reportPath("psxport_reach_overlay.json"));
  reach.observe(0x80030100u);
  CHECK(catalog.deactivate(first));
  catalog.activate("overlay-b", {0x30000u, 0x40000u}, 2u);
  reach.observe(0x80030100u);
  CHECK_EQ(reach.reached().size(), 2u);
  CHECK(reach.reached().at({"overlay-a", 1u}).count(0x80030100u) == 1u);
  CHECK(reach.reached().at({"overlay-b", 2u}).count(0x80030100u) == 1u);
}

void test_pcs_outside_every_image_and_outside_ram_are_counted_not_recorded() {
  psx::cpu::ImageCatalog catalog;
  catalog.activate("exe", {0x10000u, 0x20000u}, 0xAAu);
  psx::cpu::FunctionReach reach(catalog, reportPath("psxport_reach_unowned.json"));
  reach.observe(0x80050000u); // RAM, but no image owns it
  reach.observe(0xBFC00000u); // BIOS ROM
  CHECK(reach.reached().empty());
  CHECK_EQ(reach.unownedDispatches(), 2u);
}

void test_the_report_names_each_image_and_its_pcs() {
  psx::cpu::ImageCatalog catalog;
  catalog.activate("exe", {0x10000u, 0x20000u}, 0xABCDu);
  psx::cpu::FunctionReach reach(catalog, reportPath("psxport_reach_json.json"));
  CHECK(reach.reportJson(false).find("\"images\": [\n]") != std::string::npos);
  reach.observe(0x80010010u);
  const std::string json = reach.reportJson(true);
  CHECK(json.find("\"complete\": true") != std::string::npos);
  CHECK(json.find("\"name\": \"exe\", \"content\": \"0x000000000000ABCD\", \"pcs\": [\"0x80010010\"]") !=
        std::string::npos);
}

// A run killed before the destructor must still leave its entries on disk, marked incomplete.
void test_a_growing_report_is_flushed_before_the_end() {
  psx::cpu::ImageCatalog catalog;
  catalog.activate("exe", {0x10000u, 0x20000u}, 0xAAu);
  const std::string path = reportPath("psxport_reach_flush.json");
  psx::cpu::FunctionReach reach(catalog, path);
  for (std::uint32_t i = 0; i < 64u; ++i) {
    reach.observe(0x80010000u + i * 4u);
  }
  std::ifstream file(path);
  const std::string written((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  CHECK(written.find("\"complete\": false") != std::string::npos);
  CHECK(written.find("\"0x800100FC\"") != std::string::npos);
}

} // namespace

int main() {
  RUN(a_pc_is_recorded_once_under_the_image_that_owned_it);
  RUN(address_reusing_overlays_stay_separate);
  RUN(pcs_outside_every_image_and_outside_ram_are_counted_not_recorded);
  RUN(the_report_names_each_image_and_its_pcs);
  RUN(a_growing_report_is_flushed_before_the_end);
  return pt_summary();
}
