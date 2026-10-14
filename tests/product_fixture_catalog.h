// A catalog with no titles, so psxport builds and runs its own product main (psxport_add_product).
#pragma once

#include "title_catalog.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace psx::host::fixture {

class EmptyCatalog final : public TitleCatalog {
public:
  std::string_view productName() const override;
  std::filesystem::path provisioningRoot() const override;
  std::span<const TitleIdentity> titles() const override;
  GameRuntime &runtime(std::size_t index) const override;
};

std::unique_ptr<TitleCatalog> createCatalog();

} // namespace psx::host::fixture
