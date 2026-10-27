#include "catalog.h"

#include "runtime.h"
#include "single_title_catalog.h"
#include "testprog_identity.h"

namespace psx::testprog {
namespace {

constexpr psx::host::TitleIdentity kTitleIdentity{
    .displayName = "psxport test program",
    .serial = identity::kSerial,
    .slug = "psxtest",
    .fileSize = identity::kExecutableBytes,
    .sha256 = identity::kExecutableSha256,
    .entry = identity::kEntry,
    .textAddress = identity::kTextAddress,
    .textSize = identity::kTextBytes,
    .stackAddress = identity::kStackAddress,
};

} // namespace

std::unique_ptr<psx::host::TitleCatalog> createCatalog() {
  return std::make_unique<psx::host::SingleTitleCatalog>(
      kTitleIdentity, std::string(identity::kProvisioningRoot), std::make_unique<Runtime>());
}

} // namespace psx::testprog
