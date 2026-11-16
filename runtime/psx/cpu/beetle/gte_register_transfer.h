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

// Copies the GTE's register storage out (diagnostics and snapshots) without executing a register-port
// access: MFC2 aliases and sign extension are instruction semantics, not a state copy.
inline void
exportGteRegisters(const GteRegs &gte, std::span<std::uint32_t, 32> data, std::span<std::uint32_t, 32> control) {
  std::copy_n(gte.REG, data.size(), data.begin());
  std::copy_n(gte.REG + 32, control.size(), control.begin());
}

} // namespace psx::cpu
