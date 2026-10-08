// The comparison policy of the override differential: what an original and a native override must agree
// on after one call, under the MIPS O32 contract. Pure — it reads two captured states and two journals and
// never touches a Core — so the policy is testable on constructed states as well as through a real call.
#pragma once

#include "execution_exit.h"
#include "machine_snapshot.h"
#include "override_differential.h"
#include "side_effect_journal.h"

#include <array>
#include <cstdint>
#include <span>

namespace psx::cpu {

// The registers a caller may rely on after `jr $ra`: results v0/v1, callee-saved s0-s7 and s8/fp, and
// gp/sp/ra. Everything else is caller-saved or kernel-reserved and is not compared.
inline constexpr std::array<std::uint32_t, 14> kO32PreservedOrResultRegisters = {
    2, 3, 16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
inline constexpr std::uint32_t kCop0Status = 12;

const char *mipsRegisterName(std::uint32_t index);

struct CallObservation {
  const MachineSnapshot &state;
  const ExecutionResult &result;
  std::span<const SideEffect> effects;
};

// Judges a call both paths COMPLETED (both returned, neither performed an unreplayable effect): Match, or
// Mismatch with the first difference and the difference counts. `entrySp` and `deadStackBytes` bound the
// callee-frame window whose byte differences are counted in `deadStackBytesIgnored` instead of judged.
DifferentialOutcome judgeCompletedCall(const CallObservation &original,
                                       const CallObservation &native,
                                       std::uint32_t entrySp,
                                       std::uint32_t deadStackBytes);

} // namespace psx::cpu
