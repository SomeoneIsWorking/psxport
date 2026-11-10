// vertex_places.h — where each projected vertex stored into guest RAM or the scratchpad lay before its SXY word
// rounded it, kept by the word it was stored to while that word holds it (PGXP's memory shadow). A guest SWC2 of an
// SXY entry notes the GTE's place, a native draw site its own; a word loaded into a GPR keeps its place through the
// next store of that GPR while the GPR still holds the word.
#pragma once

#include "gte_state.h"
#include "guest_memory_map.h"
#include "r3000.h"
#include "write_rollback.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace psx::cpu {

// A stored vertex's place and the guest function whose SWC2 stored it, 0 for a native draw site.
struct NotedPlace {
  GtePlace place{};
  std::uint32_t site = 0;
};

class VertexPlaces {
public:
  // Notes made while `rollback` is armed are put back with the guest's stores.
  explicit VertexPlaces(WriteRollback &rollback) : rollback_(rollback) {}

  // Off frees the table, so no store or load pays for it.
  void track(bool on);
  [[nodiscard]] bool tracking() const {
    return tracking_;
  }
  // `word`, a vertex at `noted`, was stored at `address`.
  void note(std::uint32_t address, std::uint32_t word, const NotedPlace &noted);
  // A store of `bytes` at `address` ends the place of every word it touches.
  void forget(std::uint32_t address, std::uint32_t bytes) {
    if (mayHoldNote(address, bytes)) {
      forgetNoted(address, bytes);
    }
  }
  // The place noted at `address` while it still holds `word`.
  [[nodiscard]] std::optional<NotedPlace> at(std::uint32_t address, std::uint32_t word) const;

  // GPR `reg` was loaded with `word` from `address`.
  void loaded(std::uint32_t reg, std::uint32_t address, std::uint32_t word);
  // GPR `reg` now holds `word`, a vertex at `noted` (an MFC2 of an SXY entry).
  void carry(std::uint32_t reg, std::uint32_t word, const NotedPlace &noted);
  // GPR `reg` was loaded by anything but a whole-word load or a placed MFC2.
  void overwrote(std::uint32_t reg);
  // GPR `reg`, holding `word`, was stored at `address`.
  void stored(std::uint32_t reg, std::uint32_t address, std::uint32_t word);

private:
  struct Entry {
    std::uint32_t word = 0;
    bool placed = false;
    NotedPlace noted;
  };
  // Whether any of the `bytes` at `address` lie where a note was ever made.
  [[nodiscard]] bool mayHoldNote(std::uint32_t address, std::uint32_t bytes) const {
    const std::uint32_t p = physical(address);
    if (p < kRamMirrorSpan) {
      const std::uint32_t offset = p & (kMainRamBytes - 1u);
      return (offset + bytes + 3u) / 4u > ramFirst_ && offset / 4u < ramEnd_;
    }
    return scratchpadNoted_ && p - kScratchpadPhysicalBase < kScratchpadBytes;
  }
  // Main RAM's words, then the scratchpad's; allocated on the first note.
  [[nodiscard]] static std::uint32_t slot(std::uint32_t address);
  void forgetNoted(std::uint32_t address, std::uint32_t bytes);
  // Before `entry` changes inside a rolled-back run.
  void keep(Entry &entry);

  WriteRollback &rollback_;
  bool tracking_ = false;
  std::vector<Entry> entries_;
  std::array<Entry, kGprCount> carried_{};
  // The RAM words and whether the scratchpad ever held a note, so a store elsewhere checks nothing.
  std::uint32_t ramFirst_ = kMainRamBytes / 4u;
  std::uint32_t ramEnd_ = 0;
  bool scratchpadNoted_ = false;
};

} // namespace psx::cpu
