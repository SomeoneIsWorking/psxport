// bios_call.h — one BIOS call through Hle::dispatchBios, the entry the guest's A0/B0/C0 stubs reach.
#pragma once

#include "game.h"
#include "r3000.h"

#include <array>
#include <cstdint>

namespace psx::test {

// v0 is poisoned with this before a call, so a handler that never writes it cannot read as an answer.
inline constexpr std::uint32_t kUnwrittenV0 = 0xDEADBEEFu;

struct BiosCall {
  char table = 'A';
  std::uint32_t function = 0;
  std::array<std::uint32_t, 3> args{}; // a0..a2
};

// Whether a handler claimed the call; its result is left in v0.
inline bool dispatchBios(Game &game, const BiosCall &call) {
  game.core.r[psx::cpu::kA0] = call.args[0];
  game.core.r[psx::cpu::kA1] = call.args[1];
  game.core.r[psx::cpu::kA2] = call.args[2];
  game.core.r[psx::cpu::kV0] = kUnwrittenV0;
  return game.hle.dispatchBios(call.table, call.function);
}

// The call's v0, or kUnwrittenV0 when no handler claimed it.
inline std::uint32_t biosResult(Game &game, const BiosCall &call) {
  return dispatchBios(game, call) ? game.core.r[psx::cpu::kV0] : kUnwrittenV0;
}

} // namespace psx::test
