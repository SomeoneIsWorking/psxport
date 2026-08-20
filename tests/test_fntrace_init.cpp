// test_fntrace_init — function tracing is installed after game overrides and
// restored per Core.
//
// The tracer used to have a complete implementation and documented environment
// variables, but no boot path called fntrace_init(). It therefore produced the
// most dangerous diagnostic answer: silence that looked like an unreached
// function. This test drives the same public initializer the shipping boot
// paths call through a synthetic RecompRegistry.
#include "testutil.h"

#include "fntrace.h"
#include "recomp_iface.h"

#include <cstdlib>

namespace {

constexpr uint32_t kFirst = 0x80001000u;
constexpr uint32_t kSecond = 0x80002000u;

struct Install {
  uint32_t address;
  RecOverrideFn handler;
};

Install g_installs[8] = {};
int g_install_count = 0;

void dispatch_stub(Core *, uint32_t) {}

int index_stub(uint32_t address) {
  return address == kFirst || address == kSecond ? 0 : -1;
}

void set_override_stub(uint32_t address, RecOverrideFn handler) {
  if (g_install_count < static_cast<int>(sizeof g_installs / sizeof g_installs[0])) {
    g_installs[g_install_count] = {address, handler};
  }
  ++g_install_count;
}

void game_override_stub(Core *) {}

const RecompRegistry kRegistry = {
    dispatch_stub,
    index_stub,
    nullptr,
    0,
    set_override_stub,
    nullptr,
    nullptr,
    nullptr,
};

} // namespace

static void test_installs_and_reinstalls_after_game_overrides(void) {
  CHECK_EQ(setenv("PSXPORT_FNTRACE", "80001000,80002000", 1), 0);
  psxport_install_recomp(&kRegistry);

  fntrace_init();
  CHECK_EQ(g_install_count, 2);
  CHECK_EQ(g_installs[0].address, kFirst);
  CHECK_EQ(g_installs[1].address, kSecond);
  CHECK(g_installs[0].handler != nullptr);
  CHECK(g_installs[1].handler != nullptr);

  // Model a later Core's game registration displacing one traced address. The
  // next initializer call must restore both tracer hooks; merely remembering
  // that initialization happened is insufficient.
  set_override_stub(kFirst, game_override_stub);
  CHECK_EQ(g_install_count, 3);
  CHECK(g_installs[2].handler == game_override_stub);

  fntrace_init();
  CHECK_EQ(g_install_count, 5);
  CHECK_EQ(g_installs[3].address, kFirst);
  CHECK_EQ(g_installs[4].address, kSecond);
  CHECK(g_installs[3].handler == g_installs[0].handler);
  CHECK(g_installs[4].handler == g_installs[1].handler);
}

int main(void) {
  RUN(installs_and_reinstalls_after_game_overrides);
  return pt_summary();
}
