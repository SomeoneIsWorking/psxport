// psx::host::SingleTitleCatalog: a one-title product's name, root, identity and runtime.
#include "game_runtime.h"
#include "single_title_catalog.h"
#include "stub_game_runtime.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr psx::host::TitleIdentity kIdentity{
    .displayName = "Fixture Title",
    .serial = "SLUS_000.01",
    .slug = "fixture",
    .discEnvVar = "PSXPORT_FIXTURE_DISC",
    .discExecutablePath = "SLUS_000.01",
};

void test_the_catalog_is_its_one_title() {
  auto runtime = std::make_unique<psx::test::StubGameRuntime>();
  const psx::test::StubGameRuntime *const owned = runtime.get();
  const psx::host::SingleTitleCatalog catalog(kIdentity, "scratch/bin", std::move(runtime));

  CHECK(catalog.productName() == "Fixture Title");
  CHECK(catalog.provisioningRoot() == "scratch/bin");
  CHECK_EQ(catalog.titles().size(), 1u);
  CHECK(&catalog.titles().front() == &kIdentity);
  CHECK(&catalog.runtime(0) == owned);
}

} // namespace

int main() {
  RUN(the_catalog_is_its_one_title);
  return psx::test::summary();
}
