#pragma once

#include "title_catalog.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace psx::host {

// A product with one title: the framework main boots it directly, with no selector.
class SingleTitleCatalog final : public TitleCatalog {
public:
  // `identity` must outlive the catalog; the product name is its display name.
  SingleTitleCatalog(const TitleIdentity &identity,
                     std::filesystem::path provisioningRoot,
                     std::unique_ptr<GameRuntime> runtime);
  ~SingleTitleCatalog() override;

  std::string_view productName() const override;
  std::filesystem::path provisioningRoot() const override;
  std::span<const TitleIdentity> titles() const override;
  GameRuntime &runtime(std::size_t index) const override;

private:
  std::span<const TitleIdentity, 1> titles_;
  std::filesystem::path provisioningRoot_;
  std::unique_ptr<GameRuntime> runtime_;
};

} // namespace psx::host
