// The first-run disc screen's judge, the saved choice and the hidden-run refusal, against the fixture
// disc (tests/fixtures/make_provision_disc.py).
#include "source_tree.h"
#include "testutil.h"

#include "disc.h"
#include "executable_provisioner.h"
#include "first_run_setup.h"
#include "fs_util.h"
#include "saved_disc.h"
#include "title_catalog.h"
#include "title_disc.h"

#include <lucent/config.h>
#include <lucent/content.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::filesystem::path fixtures() {
  return psx::test::sourcePath("tests/fixtures/provision_disc");
}

std::filesystem::path fixtureDisc() {
  return fixtures() / "provision.chd";
}
constexpr const char *kDiscKey = "PSXPORT_TEST_FIRST_RUN_DISC";

struct Fixture {
  std::vector<std::uint8_t> executable = Fs::readFile((fixtures() / "SLUS_000.01").string());
  std::string sha256 = lucent::content::sha256_hex(lucent::content::sha256(std::as_bytes(std::span(executable))));
  std::filesystem::path root = std::filesystem::current_path() / ("first_run_setup_" + std::to_string(::getpid()));
  psx::host::TitleIdentity title{.displayName = "Provision fixture",
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
                                 .discExecutablePath = "BOOT/SLUS_000.01"};

  Fixture() {
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    unsetenv(kDiscKey);
    unsetenv("PSXPORT_DISC");
    lucent::config::reset_cache();
  }
  ~Fixture() {
    std::filesystem::remove_all(root);
  }
};

void test_the_title_disc_is_accepted() {
  const Fixture fixture;
  CHECK(psx::host::FirstRunSetup(fixture.title).judge(fixtureDisc()).empty());
}

void test_a_disc_of_another_title_is_refused() {
  Fixture fixture;
  const std::string wrong(64, '0');
  fixture.title.sha256 = wrong;
  const std::string reason = psx::host::FirstRunSetup(fixture.title).judge(fixtureDisc());
  CHECK(reason.find("is not Provision fixture (SLUS_000.01)") != std::string::npos);
}

void test_a_file_that_is_not_a_disc_image_is_refused() {
  const Fixture fixture;
  const std::string reason = psx::host::FirstRunSetup(fixture.title).judge(fixtures() / "SLUS_000.01");
  CHECK_STREQ(reason.c_str(), "SLUS_000.01 is not a CHD disc image");
}

void test_a_saved_choice_is_the_disc_a_title_opens() {
  const Fixture fixture;
  const psx::host::SavedDisc saved(fixture.title);
  CHECK(!saved.load().has_value());
  CHECK(!psx::host::TitleDisc(fixture.title).opened());
  CHECK(saved.save(fixtureDisc()));
  CHECK(saved.load() == fixtureDisc());
  psx::cd::Disc disc{};
  saved.bind(disc);
  CHECK_STREQ(disc.env_key, kDiscKey);
  CHECK_STREQ(disc.saved_path, fixtureDisc().c_str());
  const std::filesystem::path target = fixture.root / "SLUS_000.01";
  CHECK(psx::host::provisionExecutable(fixture.title, target) == psx::host::Provisioning::Extracted);
  CHECK(Fs::readFile(target.string()) == fixture.executable);
}

// The title's own key still wins over the saved choice.
void test_the_title_key_wins_over_the_saved_choice() {
  const Fixture fixture;
  CHECK(psx::host::SavedDisc(fixture.title).save(fixtureDisc()));
  setenv(kDiscKey, (fixture.root / "missing.chd").c_str(), 1);
  lucent::config::reset_cache();
  CHECK(!psx::host::TitleDisc(fixture.title).opened());
  unsetenv(kDiscKey);
  lucent::config::reset_cache();
}

void test_a_hidden_run_refuses_without_saving() {
  const Fixture fixture;
  CHECK(psx::host::FirstRunSetup(fixture.title).run(false) == psx::host::FirstRunSetup::Outcome::Unavailable);
  CHECK(!psx::host::SavedDisc(fixture.title).load().has_value());
}

} // namespace

int main() {
  RUN(the_title_disc_is_accepted);
  RUN(a_disc_of_another_title_is_refused);
  RUN(a_file_that_is_not_a_disc_image_is_refused);
  RUN(a_saved_choice_is_the_disc_a_title_opens);
  RUN(the_title_key_wins_over_the_saved_choice);
  RUN(a_hidden_run_refuses_without_saving);
  return psx::test::summary();
}
