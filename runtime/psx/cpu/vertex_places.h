// vertex_places.h — where each projected vertex stored into guest RAM or the scratchpad lay before its SXY word
// rounded it, kept by the word it was stored to while that word holds it (PGXP's memory shadow). A guest SWC2 of an
// SXY entry notes the GTE's place, a native draw site its own; a word loaded into a GPR keeps its place through the
// next store of that GPR while the GPR still holds the word.
#pragma once

#include "gte_state.h"
#include "guest_memory_map.h"
#include "r3000.h"
#include "write_rollback.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace psx::cpu {

// A stored vertex's place and the guest function whose SWC2 stored it, 0 for a native draw site; or a translation
// word's depth unit, which a guest CTC2 of TRZ hands the GTE (Gte::carryTranslation).
struct NotedPlace {
  GtePlace place{};
  std::uint32_t site = 0;
  GteTranslationDepth translation{};
};

// A drawer that buckets each face by summed depth less its face word's bias: `faceLoad` (an LW in `function`) reads
// that word, and the face's vertices then test `slots(word)` slots of `bin` plus their `back` nearer.
struct FaceSort {
  std::uint32_t function = 0;
  std::uint32_t faceLoad = 0;
  float bin = 0.0f;
  std::int32_t (*slots)(std::uint32_t faceWord) = nullptr;
};

// One whole-word load: the function that ran it, its opcode and the word it read.
struct LoadedWord {
  std::uint32_t function = 0;
  std::uint32_t opcode = 0;
  std::uint32_t word = 0;
};

// Every change to the table while `rollback` is armed is put back with the guest's stores, so a replay leaves
// the places as it found them.
class VertexPlaces {
public:
  explicit VertexPlaces(WriteRollback &rollback) : rollback_(rollback) {}

  // Moves the vertices `sort.function` stores after each face word by that face's bias.
  void sortFaces(const FaceSort &sort);
  // False for every load that is no sorted drawer's face load, so only the others look up the enclosing function.
  [[nodiscard]] bool mayLoadFace(std::uint32_t opcode) const {
    return (faceLoadMask_ & opcodeBit(opcode)) != 0;
  }
  // A sorted drawer's face load opens that face.
  void faceLoaded(const LoadedWord &load);
  // A new translation starts another model, so no face is open.
  void endFace();

  // Off frees the table, so no store or load pays for it.
  void track(bool on);
  [[nodiscard]] bool tracking() const {
    return tracking_;
  }
  // `word`, a vertex at `noted`, was stored at `address`.
  void note(std::uint32_t address, std::uint32_t word, const NotedPlace &noted);
  // A store of `bytes` at `address` ends the place of every word it touches.
  void forget(std::uint32_t address, std::uint32_t bytes) {
    if (placed_.empty()) {
      return;
    }
    if ((address & 3u) + bytes > 4u) {
      forgetSpan(address, bytes);
      return;
    }
    if (const std::uint32_t at = slot(address); at != kNoSlot && placed(at)) {
      unplace(at);
    }
  }
  // The place noted at `address` while it still holds `word`.
  [[nodiscard]] std::optional<NotedPlace> at(std::uint32_t address, std::uint32_t word) const;

  // GPR `reg` was loaded with `word` from `address`.
  void loaded(std::uint32_t reg, std::uint32_t address, std::uint32_t word) {
    if (reg == kZero || (!carries(reg) && !placedAt(address))) {
      return;
    }
    loadedPlaced(reg, at(address, word), word);
  }
  // GPR `reg` now holds `word`, a vertex at `noted` (an MFC2 of an SXY entry).
  void carry(std::uint32_t reg, std::uint32_t word, const NotedPlace &noted);
  // GPR `reg` was loaded by anything but a whole-word load or a placed MFC2.
  void overwrote(std::uint32_t reg) {
    if (carries(reg)) {
      drop(reg);
    }
  }
  // What GPR `reg` carries while it still holds `word`.
  [[nodiscard]] std::optional<NotedPlace> carried(std::uint32_t reg, std::uint32_t word) const {
    if (!carries(reg) || carried_[reg].word != word) {
      return std::nullopt;
    }
    return carried_[reg].noted;
  }
  // GPR `reg`, holding `word`, was stored at `address`.
  void stored(std::uint32_t reg, std::uint32_t address, std::uint32_t word) {
    if (carries(reg) && carried_[reg].word == word) {
      note(address, word, faced(reg) ? inFace(carried_[reg].noted) : carried_[reg].noted);
    }
  }

private:
  struct Stored {
    std::uint32_t word = 0;
    NotedPlace noted;
  };
  static constexpr std::uint32_t kRamWords = kMainRamBytes / 4u;
  static constexpr std::uint32_t kSlots = kRamWords + kScratchpadBytes / 4u;
  static constexpr std::uint32_t kNoSlot = kSlots;

  // Main RAM's words, then the scratchpad's.
  [[nodiscard]] static constexpr std::uint32_t slot(std::uint32_t address) {
    if (const std::optional<std::uint32_t> offset = mainRamOffset(address)) {
      return *offset / 4u;
    }
    const std::uint32_t p = physical(address);
    if (p >= kScratchpadPhysicalBase && p < kScratchpadPhysicalBase + kScratchpadBytes) {
      return kRamWords + (p - kScratchpadPhysicalBase) / 4u;
    }
    return kNoSlot;
  }
  [[nodiscard]] bool placed(std::uint32_t at) const {
    return ((placed_[at / 64u] >> (at % 64u)) & 1u) != 0;
  }
  [[nodiscard]] bool placedAt(std::uint32_t address) const {
    if (placed_.empty()) {
      return false;
    }
    const std::uint32_t at = slot(address);
    return at != kNoSlot && placed(at);
  }
  [[nodiscard]] bool carries(std::uint32_t reg) const {
    return ((carriedMask_ >> reg) & 1u) != 0;
  }
  void unplace(std::uint32_t at);
  void forgetSpan(std::uint32_t address, std::uint32_t bytes);
  // GPR `reg` was loaded with `word`, placed at `noted` or not at all.
  void loadedPlaced(std::uint32_t reg, const std::optional<NotedPlace> &noted, std::uint32_t word);
  // Whether GPR `reg` holds a vertex its sorted drawer loaded back, so its store belongs to the open face.
  [[nodiscard]] bool faced(std::uint32_t reg) const {
    return ((loadedMask_ >> reg) & 1u) != 0 && face_.function != 0 && carried_[reg].noted.site == face_.function;
  }
  // `noted` moved by the open face's bias.
  [[nodiscard]] NotedPlace inFace(const NotedPlace &noted) const;
  [[nodiscard]] static constexpr std::uint32_t opcodeBit(std::uint32_t opcode) {
    return 1u << ((opcode ^ (opcode >> 16)) & 31u);
  }
  // Before the open face changes; once per run.
  void keepFace();
  void drop(std::uint32_t reg);
  // Before `value` changes inside a rolled-back run.
  template <class Value> void keep(Value &value) {
    if (rollback_.armed()) {
      rollback_.note(reinterpret_cast<std::uint8_t *>(&value), sizeof value);
    }
  }
  // Before a bit of placed_[index] changes; once per run, as the oldest copy is the one put back.
  void keepBits(std::uint32_t index);
  // Before the carried registers change; once per run.
  void keepCarried();

  WriteRollback &rollback_;
  bool tracking_ = false;
  // One bit per slot, so a load or store of an unplaced word tests only its bit; allocated by the first note.
  std::vector<std::uint64_t> placed_;
  std::vector<Stored> stored_;
  std::vector<std::uint32_t> keptRun_; // per placed_ word, the rollback run that last kept it
  std::uint32_t carriedMask_ = 0;      // the GPRs carrying a place
  std::uint32_t loadedMask_ = 0;       // those whose place a whole-word load gave
  std::array<Stored, kGprCount> carried_{};
  std::uint32_t carriedKeptRun_ = 0;
  std::vector<FaceSort> faceSorts_;
  std::uint32_t faceLoadMask_ = 0;
  struct OpenFace {
    std::uint32_t function = 0;
    float nearerSlots = 0.0f;
    float bin = 0.0f;
  };
  OpenFace face_;
  std::uint32_t faceKeptRun_ = 0;
};

} // namespace psx::cpu
