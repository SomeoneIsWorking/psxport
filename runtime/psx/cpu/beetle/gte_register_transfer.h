#pragma once

#include "gte_registers.h"
#include "gte_state.h"
#include "projected_vertex.h"

#include <algorithm>
#include <cstdint>
#include <span>

namespace psx::cpu {

// An SXY entry whose word `data` replaces lies on its new word's pixel; an entry it leaves keeps its place.
inline void placeReplacedScreenEntries(GteRegs &gte, std::span<const std::uint32_t, 32> data) {
  for (std::uint32_t entry = 0; entry < 3u; ++entry) {
    const std::uint32_t word = data[gte::kSxy0 + entry];
    if (word != gte.REG[gte::kSxy0 + entry]) {
      gte.places[entry] = gte::ProjectedVertex::whole(word).place();
    }
  }
}

// Transfer register storage between the CPU backend and the per-Game GTE without executing CPU
// register-port accesses. MTC2(SXYP/IRGB/LZCS) and CTC2(FLAG) have side effects; MFC2 aliases and
// sign extension are likewise instruction semantics, not a state-copy operation.
inline void
importGteRegisters(GteRegs &gte, std::span<const std::uint32_t, 32> data, std::span<const std::uint32_t, 32> control) {
  // Guest code wrote SXY directly (MTC2): the place of the word it replaced no longer belongs to it.
  placeReplacedScreenEntries(gte, data);
  std::copy(data.begin(), data.end(), gte.REG);
  std::copy(control.begin(), control.end(), gte.REG + 32);
  // Lightrec reads SXYP (15) from SXY2 (14) and does not maintain slot15 after MTC2. Beetle's
  // GTE_ReadDR(15) reads a stored alias, so materialize it without advancing SXY0..2.
  gte.REG[15] = gte.REG[14];
  // Beetle uses FLAGS as a command-local accumulator, clears it at command entry, and publishes
  // it to CR31 at completion. Outside a command, import the last published FLAG without CTC2's mask.
  gte.FLAGS = control[31];
}

inline void
exportGteRegisters(const GteRegs &gte, std::span<std::uint32_t, 32> data, std::span<std::uint32_t, 32> control) {
  std::copy_n(gte.REG, data.size(), data.begin());
  std::copy_n(gte.REG + 32, control.size(), control.begin());
}

} // namespace psx::cpu
