// vertex_depths.h — the view depth of each projected vertex a draw site stored into guest RAM or the scratchpad,
// kept by the word it was stored to until any store touches that word, so the record tap gives the guest's own
// packets the depth their producer saw.
#pragma once

#include <cstdint>
#include <vector>

namespace psx::cpu {

class VertexDepths {
public:
  // `word`, a vertex of view depth `depth` (0 unknown), was stored at `address`.
  void note(std::uint32_t address, std::uint32_t word, float depth);
  // A store of `bytes` at `address` ends the depth of every word it touches.
  void forget(std::uint32_t address, std::uint32_t bytes);
  // The depth noted at `address` while it still holds that word, else 0.
  [[nodiscard]] float at(std::uint32_t address, std::uint32_t word) const;

private:
  struct Entry {
    std::uint32_t word = 0;
    float depth = 0.0f;
  };
  // Main RAM's words, then the scratchpad's; allocated on the first note.
  [[nodiscard]] static std::uint32_t slot(std::uint32_t address);

  std::vector<Entry> entries_;
};

} // namespace psx::cpu
