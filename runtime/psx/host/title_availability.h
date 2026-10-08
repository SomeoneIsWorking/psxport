#pragma once

#include "title_catalog.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace psx::host {

enum class AvailabilityStatus : std::uint8_t {
  Available,
  NotProvisioned,
  IdentityMismatch,
};

// One catalog title as the selector may offer it. `identity` is the catalog entry (serial and
// hashes): that, not the label, is what a selection means.
struct TitleAvailability {
  const TitleIdentity *identity = nullptr;
  std::size_t index = 0; // catalog position, which is also the TitleCatalog::runtime index
  std::filesystem::path executable;
  AvailabilityStatus status = AvailabilityStatus::NotProvisioned;
  std::string reason; // empty when Available

  bool available() const {
    return status == AvailabilityStatus::Available;
  }
};

// Decides which catalog titles can be started: the executable must be provisioned at
// `<root>/<slug>/<serial>` AND authenticate through the same `selectExecutableFile` the boot path
// uses. A title that is not provisioned never blocks one that is.
class TitleAvailabilityProbe {
public:
  TitleAvailabilityProbe(std::filesystem::path provisioningRoot, std::span<const TitleIdentity> catalog);

  // One entry per catalog title, in catalog order.
  std::vector<TitleAvailability> probe() const;

private:
  std::filesystem::path root_;
  std::span<const TitleIdentity> catalog_;
};

} // namespace psx::host
