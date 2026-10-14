// psx::host::provisionExecutable against the fixture disc (tests/fixtures/make_provision_disc.py): a
// one-track CHD holding BOOT/SLUS_000.01, the same bytes as tests/fixtures/provision_disc/SLUS_000.01.
#include "testutil.h"

#include "executable_provisioner.h"
#include "fs_util.h"
#include "title_catalog.h"

#include <lucent/content.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

const std::filesystem::path kFixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "provision_disc";
constexpr const char *kDiscKey = "PSXPORT_TEST_PROVISION_DISC";

struct Fixture {
  std::vector<std::uint8_t> executable = Fs::readFile((kFixtures / "SLUS_000.01").string());
  std::string sha256 = lucent::content::sha256_hex(lucent::content::sha256(std::as_bytes(std::span(executable))));
  std::filesystem::path root =
      std::filesystem::current_path() / ("executable_provisioner_" + std::to_string(::getpid()));
  std::filesystem::path target = root / "fixture" / "SLUS_000.01";

  Fixture() {
    setenv(kDiscKey, (kFixtures / "provision.chd").c_str(), 1);
    unsetenv("PSXPORT_DISC");
  }
  ~Fixture() {
    std::filesystem::remove_all(root);
  }

  psx::host::TitleIdentity title(std::string_view discPath) const {
    return psx::host::TitleIdentity{.displayName = "Provision fixture",
                                    .serial = "SLUS_000.01",
                                    .slug = "fixture",
                                    .fileSize = executable.size(),
                                    .sha256 = sha256,
                                    .entry = 0x80010000u,
                                    .globalPointer = 0u,
                                    .textAddress = 0x80010000u,
                                    .textSize = 0x800u,
                                    .stackAddress = 0x801FFFF0u,
                                    .stackOffset = 0u,
                                    .discEnvVar = kDiscKey,
                                    .discExecutablePath = discPath};
  }
};

void test_a_missing_executable_is_extracted_from_the_title_disc() {
  const Fixture fixture;
  CHECK_EQ(fixture.executable.size(), 0x1000u);
  CHECK(!std::filesystem::exists(fixture.target));
  CHECK(psx::host::provisionExecutable(fixture.title("BOOT/SLUS_000.01"), fixture.target) ==
        psx::host::Provisioning::Extracted);
  CHECK(Fs::readFile(fixture.target.string()) == fixture.executable);
}

void test_a_present_executable_is_left_alone() {
  const Fixture fixture;
  const std::uint8_t marker = 0x5A;
  CHECK(Fs::writeFile(fixture.target.string(), &marker, 1));
  CHECK(psx::host::provisionExecutable(fixture.title("BOOT/SLUS_000.01"), fixture.target) ==
        psx::host::Provisioning::Present);
  CHECK(Fs::readFile(fixture.target.string()) == std::vector<std::uint8_t>{marker});
}

void test_bytes_that_are_not_the_title_are_never_written() {
  const Fixture fixture;
  psx::host::TitleIdentity other = fixture.title("BOOT/SLUS_000.01");
  const std::string wrong(64, '0');
  other.sha256 = wrong;
  CHECK(psx::host::provisionExecutable(other, fixture.target) == psx::host::Provisioning::Unavailable);
  CHECK(!std::filesystem::exists(fixture.target));
}

void test_a_path_the_disc_does_not_hold_provisions_nothing() {
  const Fixture fixture;
  CHECK(psx::host::provisionExecutable(fixture.title("BOOT/SLUS_000.02"), fixture.target) ==
        psx::host::Provisioning::Unavailable);
  CHECK(psx::host::provisionExecutable(fixture.title(""), fixture.target) == psx::host::Provisioning::Unavailable);
  CHECK(!std::filesystem::exists(fixture.target));
}

} // namespace

int main() {
  RUN(a_missing_executable_is_extracted_from_the_title_disc);
  RUN(a_present_executable_is_left_alone);
  RUN(bytes_that_are_not_the_title_are_never_written);
  RUN(a_path_the_disc_does_not_hold_provisions_nothing);
  return pt_summary();
}
