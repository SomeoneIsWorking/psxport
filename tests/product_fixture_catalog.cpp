#include "product_fixture_catalog.h"

#include <cstdlib>
#include <lucent/log.h>

namespace psx::host::fixture {

std::string_view EmptyCatalog::productName() const {
  return "Product fixture";
}

std::filesystem::path EmptyCatalog::provisioningRoot() const {
  return "scratch/assets";
}

std::span<const TitleIdentity> EmptyCatalog::titles() const {
  return {};
}

psx::game::GameRuntime &EmptyCatalog::runtime(std::size_t index) const {
  lucent::error("fixture", "the empty catalog has no runtime {}", index);
  std::abort();
}

std::unique_ptr<TitleCatalog> createCatalog() {
  return std::make_unique<EmptyCatalog>();
}

} // namespace psx::host::fixture
