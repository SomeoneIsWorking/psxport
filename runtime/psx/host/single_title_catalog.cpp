#include "single_title_catalog.h"

#include "game_runtime.h"

#include <cstdlib>
#include <lucent/log.h>
#include <utility>

namespace psx::host {

SingleTitleCatalog::SingleTitleCatalog(const TitleIdentity &identity,
                                       std::filesystem::path provisioningRoot,
                                       std::unique_ptr<psx::game::GameRuntime> runtime)
    : titles_(&identity, 1), provisioningRoot_(std::move(provisioningRoot)), runtime_(std::move(runtime)) {
  if (!runtime_) {
    lucent::error("title-catalog", "{} has no runtime", identity.displayName);
    std::abort();
  }
}

SingleTitleCatalog::~SingleTitleCatalog() = default;

std::string_view SingleTitleCatalog::productName() const {
  return titles_.front().displayName;
}

std::filesystem::path SingleTitleCatalog::provisioningRoot() const {
  return provisioningRoot_;
}

std::span<const TitleIdentity> SingleTitleCatalog::titles() const {
  return titles_;
}

psx::game::GameRuntime &SingleTitleCatalog::runtime(std::size_t index) const {
  if (index != 0) {
    lucent::error("title-catalog", "{} has no runtime for catalog index {}", titles_.front().displayName, index);
    std::abort();
  }
  return *runtime_;
}

} // namespace psx::host
