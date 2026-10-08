// A DIRECT runtime (no GameConfig) names its window and its memory card through
// GameRuntime::hostIdentity(). Game::hostIdentity() is the one place that chooses between the legacy
// GameConfig fields and that declaration, and Memcard::init() resolves the backing file from it:
// the title's own variable first, then the generic PSXPORT_CARD, then the title's default path.
#include "game.h"
#include "game_runtime.h"
#include "memcard.h"
#include "testutil.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace {

// Two knobs, because the configuration owner remembers the first answer it gave for a name: one the
// test sets, and one it never does.
constexpr const char *kCardVar = "PSXPORT_TEST_HOST_IDENTITY_CARD";
constexpr const char *kUnsetCardVar = "PSXPORT_TEST_HOST_IDENTITY_UNSET_CARD";

class IdentifiedRuntime final : public GameRuntime {
public:
  explicit IdentifiedRuntime(const HostIdentity *declared) : declared_(declared) {}
  const HostIdentity *hostIdentity() const override {
    return declared_;
  }
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}

private:
  const HostIdentity *declared_;
};

// The card variable is an undeclared knob, so it comes from the real environment, as in a player's shell.
void setEnvironment(const char *name, const char *value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

std::string scratchPath(const char *name) {
  const auto directory = std::filesystem::current_path() / "host_identity_test";
  std::filesystem::create_directories(directory);
  return (directory / name).string();
}

// Constructs a Game for `runtime`, initialises its card, and returns the file it chose.
std::string cardChosenBy(IdentifiedRuntime &runtime) {
  psxport_install_game(runtime);
  const auto game = std::make_unique<Game>();
  game->memcard.init();
  return game->memcard.path();
}

void test_a_declared_identity_reaches_the_game() {
  const HostIdentity declared{"Declared Title", kCardVar, "ignored-default.mcr"};
  IdentifiedRuntime runtime(&declared);
  psxport_install_game(runtime);
  const auto game = std::make_unique<Game>();
  const HostIdentity seen = game->hostIdentity();
  CHECK(std::string(seen.windowTitle) == std::string("Declared Title"));
  CHECK(std::string(seen.cardEnvVar) == std::string(kCardVar));
  CHECK(std::string(seen.cardDefaultPath) == std::string("ignored-default.mcr"));
}

void test_no_declaration_is_an_honest_null_identity() {
  IdentifiedRuntime runtime(nullptr);
  psxport_install_game(runtime);
  const auto game = std::make_unique<Game>();
  const HostIdentity seen = game->hostIdentity();
  CHECK(seen.windowTitle == nullptr);
  CHECK(seen.cardEnvVar == nullptr);
  CHECK(seen.cardDefaultPath == nullptr);
}

void test_the_title_card_variable_outranks_the_default_path() {
  const std::string fromVariable = scratchPath("variable.mcr");
  const std::string fromDefault = scratchPath("default.mcr");
  const HostIdentity declared{"T", kCardVar, fromDefault.c_str()};
  IdentifiedRuntime runtime(&declared);
  setEnvironment(kCardVar, fromVariable.c_str());
  CHECK(cardChosenBy(runtime) == fromVariable);
  std::remove(fromVariable.c_str());
}

void test_the_default_path_applies_when_the_variable_is_unset() {
  const std::string fromDefault = scratchPath("default.mcr");
  const HostIdentity declared{"T", kUnsetCardVar, fromDefault.c_str()};
  IdentifiedRuntime runtime(&declared);
  CHECK(cardChosenBy(runtime) == fromDefault);
  std::remove(fromDefault.c_str());
}

} // namespace

int main() {
  RUN(a_declared_identity_reaches_the_game);
  RUN(no_declaration_is_an_honest_null_identity);
  RUN(the_default_path_applies_when_the_variable_is_unset);
  RUN(the_title_card_variable_outranks_the_default_path);
  std::filesystem::remove_all(std::filesystem::current_path() / "host_identity_test");
  return pt_summary();
}
