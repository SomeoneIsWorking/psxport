// test_fntrace_init — function tracing preserves the exact handler already installed in the
// generated MAIN override slot, then restores its own hook after later title registration.
#include "testutil.h"

#include "core.h"
#include "fntrace.h"
#include "game.h"
#include "override_registry.h"
#include "recomp_iface.h"

#include <cstdlib>
#include <memory>

namespace {

constexpr uint32_t kRegistryOwned = 0x80001000u;
constexpr uint32_t kRawOwned = 0x80002000u;

RecOverrideFn g_slots[2] = {};
int gSetCount = 0;
int gDispatchCalls = 0;
int gRegistryNativeCalls = 0;
int gRegistryGenCalls = 0;
int gRawOwnerCalls = 0;
int gReplacementOwnerCalls = 0;

int slotFor(uint32_t address) {
  if (address == kRegistryOwned) {
    return 0;
  }
  if (address == kRawOwned) {
    return 1;
  }
  return -1;
}

void dispatchStub(Core *, uint32_t) {
  ++gDispatchCalls;
}

int indexStub(uint32_t address) {
  return slotFor(address);
}

void setOverrideStub(uint32_t address, RecOverrideFn handler) {
  const int slot = slotFor(address);
  if (slot >= 0) {
    g_slots[slot] = handler;
  }
  ++gSetCount;
}

RecOverrideFn getOverrideStub(uint32_t address) {
  const int slot = slotFor(address);
  return slot >= 0 ? g_slots[slot] : nullptr;
}

void registryNative(Core *) {
  ++gRegistryNativeCalls;
}

void registryGen(Core *) {
  ++gRegistryGenCalls;
}

void rawOwner(Core *) {
  ++gRawOwnerCalls;
}

void replacementOwner(Core *) {
  ++gReplacementOwnerCalls;
}

const RecompRegistry kRegistry = {
    dispatchStub,
    indexStub,
    nullptr,
    0,
    setOverrideStub,
    nullptr,
    nullptr,
    nullptr,
    getOverrideStub,
};

const RecompRegistry kMissingGetterRegistry = {
    dispatchStub,
    indexStub,
    nullptr,
    0,
    setOverrideStub,
    nullptr,
    nullptr,
    nullptr,
};

void invokeSlot(Game &game, uint32_t address) {
  const int slot = slotFor(address);
  CHECK(slot >= 0);
  CHECK(g_slots[slot] != nullptr);
  game.core.pc = address;
  g_slots[slot](&game.core);
}

} // namespace

static void test_preserves_registry_and_raw_owners(void) {
  CHECK_EQ(setenv("PSXPORT_FNTRACE", "80001000,80002000,80003000", 1), 0);
  psxport_install_recomp(&kRegistry);

  overrides::install(kRegistryOwned, "test registry owner", registryNative, registryGen, setOverrideStub);
  setOverrideStub(kRawOwned, rawOwner);
  const int ownerInstallCount = gSetCount;

  fntrace_init();
  CHECK_EQ(gSetCount, ownerInstallCount + 2);
  CHECK(g_slots[0] != nullptr);
  CHECK(g_slots[1] != nullptr);

  auto game = std::make_unique<Game>();
  invokeSlot(*game, kRegistryOwned);
  invokeSlot(*game, kRawOwned);
  CHECK_EQ(gRegistryNativeCalls, 1);
  CHECK_EQ(gRegistryGenCalls, 0);
  CHECK_EQ(gRawOwnerCalls, 1);
  CHECK_EQ(gDispatchCalls, 0);

  // A later Core may reinstall a title/HLE owner. Repeat initialization must capture that new exact
  // owner before restoring the tracer; an already-installed tracer on the other site keeps its
  // original chained owner.
  setOverrideStub(kRegistryOwned, replacementOwner);
  const int replacementInstallCount = gSetCount;
  CHECK_EQ(setenv("PSXPORT_FNTRACE", "80003000", 1), 0); // parsing is one-shot

  fntrace_init();
  CHECK_EQ(gSetCount, replacementInstallCount + 2);
  invokeSlot(*game, kRegistryOwned);
  invokeSlot(*game, kRawOwned);
  CHECK_EQ(gReplacementOwnerCalls, 1);
  CHECK_EQ(gRawOwnerCalls, 2);
  CHECK_EQ(gDispatchCalls, 0);

  // Old generated substrates have no getter. The initialized tracer must refuse to touch either
  // raw slot rather than guessing that generated redispatch is equivalent to its current owner.
  psxport_install_recomp(&kMissingGetterRegistry);
  const int beforeRefusal = gSetCount;
  fntrace_init();
  CHECK_EQ(gSetCount, beforeRefusal);
}

int main(void) {
  RUN(preserves_registry_and_raw_owners);
  return pt_summary();
}
