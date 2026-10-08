// THE SHARED DYNArec-CONTRACT FIXTURE.
//
// This exists because `test_dynarec_contract.cpp` reached the project's 1,200-line cap while the
// budget-exit census was being added, and the fix for that is to EXTRACT, not to raise the cap or to
// copy the fixture into a second translation unit. A duplicated fixture is two fixtures that drift,
// which is the same "one fact, one home" rule the rest of the project is built on.
//
// Everything here is a test double for the guest image and the native dispatcher. The variables are
// `inline` so the header may be included by more than one test binary without an ODR violation.
#pragma once

#include "execution_control.h"
#include "game.h"
#include "game_runtime.h"
#include "lightrec_executor.h"
#include "native_dispatch.h"

#include <cstdint>
#include <memory>

namespace dynarec_test {

class Runtime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
};

std::unique_ptr<Game> makeGame(Runtime &runtime) {
  psxport_install_game(runtime);
  return std::make_unique<Game>();
}

constexpr std::uint32_t encodeJal(std::uint32_t target) {
  return 0x0c000000u | ((target >> 2u) & 0x03ffffffu);
}

constexpr std::uint32_t encodeJ(std::uint32_t target) {
  return 0x08000000u | ((target >> 2u) & 0x03ffffffu);
}

constexpr std::uint32_t kCaller = 0x00010000u;
constexpr std::uint32_t kCallee = 0x00010100u;
constexpr std::uint32_t kInnerCallee = 0x00010140u;
constexpr std::uint32_t kNestedReturn = 0x00010180u;
constexpr std::uint32_t kWriter = 0x00010200u;
constexpr std::uint32_t kMainFallback = 0x00010300u;
constexpr std::uint32_t kOuterReturn = 0x00010f00u;
constexpr std::uint32_t kObservedWriter = 0x80010500u;
constexpr std::uint32_t kMainRegisterValue = 0x13579bdfu;
constexpr std::uint32_t kTaskRegisterValue = 0x2468ace0u;

psx::cpu::ImageIdentity installTestImage(Core &core) {
  return core.imageCatalog().activate("dynarec-contract", {kCaller, kOuterReturn + 12u}, 0x44594e41524543ull);
}

void writeReturningCaller(Core &core) {
  core.mem_w32(kCaller, 0x03e08021u); // addu s0, ra, zero
  core.mem_w32(kCaller + 4u, encodeJal(kCallee));
  core.mem_w32(kCaller + 8u, 0u);           // delay-slot nop
  core.mem_w32(kCaller + 12u, 0x24420002u); // addiu v0, v0, 2
  core.mem_w32(kCaller + 16u, 0x02000008u); // jr s0
  core.mem_w32(kCaller + 20u, 0u);          // delay-slot nop

  core.mem_w32(kOuterReturn, 0x24177badu);      // must not execute: addiu s7, zero, 0x7bad
  core.mem_w32(kOuterReturn + 4u, 0x1000ffffu); // stable self-loop if the boundary is missed
  core.mem_w32(kOuterReturn + 8u, 0u);
}

inline int nativeOverrideCalls = 0;
inline std::uint32_t nativeOverrideActiveAddress = 0;

void nativeCallee(Core *core) {
  ++nativeOverrideCalls;
  nativeOverrideActiveAddress = core->active_native_address;
  core->r[2] = 40u;
}

void nativeFrameExit(Core *core) {
  core->r[17] = 7u;
  psx::cpu::requestExecutionExit(*core, psx::cpu::ExecutionExitReason::FrameBoundary);
}

} // namespace dynarec_test
