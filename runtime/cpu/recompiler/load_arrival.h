// load_arrival.h — what a guest load still in flight does at the next instruction, decided at translation time the
// way the interpreter's arriving/pending pair decides it at run time. Every backend translates delay slots with it.
#pragma once

#include "r3000_decode.h"

#include <cstdint>

namespace psx::cpu::recompiler {

// What a load still in flight at an instruction does there (the interpreter's arriving/pending pair, resolved here).
enum class Arrival : std::uint8_t {
  None,
  LandBefore, // the instruction neither reads nor writes it: land first
  Hold,       // the instruction reads the old value: land after it, unless it writes the register
  Drop,       // the instruction loads the same register: the first load never lands
  Merge,      // LWL/LWR into the same register merges into the value in flight, which never lands itself
};

inline std::uint32_t bit(std::uint32_t reg) {
  return reg == 0 ? 0u : 1u << reg;
}

// A store instruction (SB/SH/SW/SWL/SWR/SWC2).
inline bool isStoreWord(std::uint32_t word) {
  return isStore(primaryOpcode(word));
}

inline bool leavesEngine(std::uint32_t word) {
  return unknownInstructionFault(word) != nullptr ||
         (primaryOpcode(word) == kSpecial && (functField(word) == kSyscall || functField(word) == kBreak));
}

inline Arrival arrivalAt(std::uint32_t inFlight, std::uint32_t word) {
  if (inFlight == 0) {
    return Arrival::None;
  }
  if (leavesEngine(word)) {
    return Arrival::LandBefore; // the interpreter lands it as the instruction leaves
  }
  if (delayedLoadTarget(word) == inFlight) {
    const std::uint32_t op = primaryOpcode(word);
    return op == kLwl || op == kLwr ? Arrival::Merge : Arrival::Drop;
  }
  if ((gprReads(word) & bit(inFlight)) != 0) {
    return Arrival::Hold;
  }
  return Arrival::LandBefore;
}

} // namespace psx::cpu::recompiler
