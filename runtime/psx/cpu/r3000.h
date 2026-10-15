// R3000A register file — the CPU state of one emulator instance.
//
// This header is the register portion of the runtime ABI. `Core` publicly inherits the register
// file and owns memory plus every subsystem as members; the rest of the ABI lives on Core in
// core.h.
#pragma once
#include <cstdint>

struct R3000 {
  std::uint32_t r[32];  // GPRs; r[0] is hardwired 0
  std::uint32_t hi, lo; // mult/div result registers
  std::uint32_t pc;     // program counter — PER-CORE and synchronized at every runtime executor exit.
                        // Diagnostics therefore report the architectural PC for this Core; there is no
                        // process-global "current PC", and two Cores retain independent state.
};

namespace psx::cpu {

// MIPS o32 register numbers, the index into R3000::r.
enum Gpr : std::uint8_t {
  kZero = 0,
  kAt = 1,
  kV0 = 2,
  kV1 = 3,
  kA0 = 4,
  kA1 = 5,
  kA2 = 6,
  kA3 = 7,
  kT0 = 8,
  kT1 = 9,
  kS0 = 16,
  kGp = 28,
  kSp = 29,
  kFp = 30,
  kRa = 31,
};

} // namespace psx::cpu
