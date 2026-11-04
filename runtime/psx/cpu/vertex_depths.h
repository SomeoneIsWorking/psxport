// vertex_depths.h — the view depth of each projected vertex a draw site stored into guest RAM or the scratchpad,
// kept by the word it was stored to until any store touches that word, so the record tap gives the guest's own
// packets the depth their producer saw.
#pragma once

#include "guest_memory_map.h"

#include <cstdint>
#include <vector>

namespace psx::cpu {

// A stored vertex's view depth (0 unknown) and how far its draw site sorts it back (GtePlace::back).
struct NotedDepth {
  float depth = 0.0f;
  float back = 0.0f;
};

class VertexDepths {
public:
  // Off (the native depth setting) frees the table, so no store or read pays for it.
  void track(bool on);
  // `word`, a vertex of `depth`, was stored at `address`.
  void note(std::uint32_t address, std::uint32_t word, NotedDepth depth);
  // A store of `bytes` at `address` ends the depth of every word it touches.
  void forget(std::uint32_t address, std::uint32_t bytes) {
    const std::uint32_t p = physical(address);
    if (p < kRamMirrorSpan) {
      const std::uint32_t offset = p & (kMainRamBytes - 1u);
      if ((offset + bytes + 3u) / 4u > ramFirst_ && offset / 4u < ramEnd_) {
        forgetNoted(address, bytes);
      }
    } else if (scratchpadNoted_ && p - kScratchpadPhysicalBase < kScratchpadBytes) {
      forgetNoted(address, bytes);
    }
  }
  // The depth noted at `address` while it still holds that word, else unknown.
  [[nodiscard]] NotedDepth at(std::uint32_t address, std::uint32_t word) const;

private:
  struct Entry {
    std::uint32_t word = 0;
    NotedDepth depth;
  };
  // Main RAM's words, then the scratchpad's; allocated on the first note.
  [[nodiscard]] static std::uint32_t slot(std::uint32_t address);
  void forgetNoted(std::uint32_t address, std::uint32_t bytes);

  bool tracking_ = false;
  std::vector<Entry> entries_;
  // The RAM words and whether the scratchpad ever held a note, so a store elsewhere checks nothing.
  std::uint32_t ramFirst_ = kMainRamBytes / 4u;
  std::uint32_t ramEnd_ = 0;
  bool scratchpadNoted_ = false;
};

} // namespace psx::cpu
