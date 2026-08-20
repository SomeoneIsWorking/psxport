// test_platform_hle_registration — repeated platform-service setup is idempotent per Game.
//
// Native boot and the SBS oracle both initialise the same hardware-service table. Some consumers
// also initialise their primary Game before calling the shared boot path. Re-registering a service
// must therefore replace its handler without filling the fixed table, while reinstalling the
// process-global recompiled override in case another registrar displaced it.
#include "testutil.h"

#include "game.h"
#include "game_iface.h"
#include "recomp_iface.h"

#include <memory>

namespace {

constexpr uint32_t kFirst = 0x80080010u;
constexpr uint32_t kSecond = 0x80080020u;

int g_override_calls = 0;
uint32_t g_last_address = 0;
RecOverrideFn g_last_handler = nullptr;

void first_handler(Core *) {}
void replacement_handler(Core *) {}
void second_handler(Core *) {}

void set_override_stub(uint32_t address, RecOverrideFn handler) {
  ++g_override_calls;
  g_last_address = address;
  g_last_handler = handler;
}

const RecompRegistry kRegistry = {
    nullptr,
    nullptr,
    nullptr,
    0,
    set_override_stub,
    nullptr,
    nullptr,
    nullptr,
};

std::unique_ptr<Game> fresh_game(uint32_t builtin = 0) {
  static GameConfig cfg{};
  static const GameHooks hooks{};
  cfg = {};
  cfg.hle.windowLo[0] = 0x80080000u;
  cfg.hle.windowHi[0] = 0x80090000u;
  cfg.hle.cdReadSync = builtin;
  psxport_install_game(&cfg, &hooks);
  psxport_install_recomp(&kRegistry);
  g_override_calls = 0;
  g_last_address = 0;
  g_last_handler = nullptr;
  return std::make_unique<Game>();
}

} // namespace

static void test_duplicate_replaces_and_reinstalls(void) {
  auto game = fresh_game();
  game->platform_hle.register_(kFirst, first_handler);
  game->platform_hle.register_(kFirst, replacement_handler);

  CHECK(game->platform_hle.lookup(kFirst) == replacement_handler);
  CHECK_EQ(g_override_calls, 2);
  CHECK_EQ(g_last_address, kFirst);
  CHECK(g_last_handler == replacement_handler);
}

static void test_duplicates_do_not_fill_the_table(void) {
  auto game = fresh_game(kFirst);
  for (int i = 0; i < 64; ++i) {
    game->platform_hle.initBuiltins();
  }
  game->platform_hle.register_(kSecond, second_handler);

  CHECK(game->platform_hle.lookup(kFirst) != nullptr);
  CHECK(game->platform_hle.lookup(kSecond) == second_handler);
  CHECK_EQ(g_override_calls, 65);
}

int main() {
  RUN(duplicate_replaces_and_reinstalls);
  RUN(duplicates_do_not_fill_the_table);
  return pt_summary();
}
