// gte.h — one Core's GTE (COP2): its register file and the Beetle GTE run against it.
#pragma once

#include "gte_preop_observer.h"
#include "gte_registers.h"
#include "gte_state.h"
#include "projected_vertex.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

class Core;

namespace psx::cpu {

// Every call binds this register file first, so a title's reads and writes land on its own Core's
// GTE whichever Core ran last.
class Gte {
public:
  explicit Gte(Core &core);
  Gte(const Gte &) = delete;
  Gte &operator=(const Gte &) = delete;
  Gte(Gte &&) = delete;
  Gte &operator=(Gte &&) = delete;
  ~Gte() = default;

  // Aborts on SXY0..SXYP: a projected vertex is read whole, place and all, through `projected`.
  std::uint32_t readData(std::uint32_t reg);
  // A write to the SXY FIFO puts that entry on the pixel its word names.
  void writeData(std::uint32_t reg, std::uint32_t value);
  std::uint32_t readControl(std::uint32_t reg);
  void writeControl(std::uint32_t reg, std::uint32_t value);
  // One COP2 command at the Core's current PC, with the Core's GTE observer.
  void execute(std::uint32_t instruction);

  // The rotation matrix, CR0..CR4.
  void loadRotation(std::span<const std::uint32_t, gte::kRotationWords> words) {
    loadControl(gte::kRotation, words);
  }
  gte::RotationWords storeRotation() {
    return storeControl<gte::kRotationWords>(gte::kRotation);
  }
  // The light matrix, CR8..CR12, packed as the rotation is.
  void loadLight(std::span<const std::uint32_t, gte::kRotationWords> words) {
    loadControl(gte::kLight0, words);
  }
  gte::RotationWords storeLight() {
    return storeControl<gte::kRotationWords>(gte::kLight0);
  }
  // The translation vector, CR5..CR7.
  void loadTranslation(const gte::TranslationWords &words) {
    loadControl(gte::kTranslationX, words);
  }

  // SXY entry 0..2 with its place: from the projection that pushed it, or its word's pixel once a write replaced it.
  // Its depth is in the title's common view units: SZ times the open DepthUnit's scale less its nearer, and times the
  // carried translation's scale less its nearer, 0 when that is not in front of the eye.
  gte::ProjectedVertex projected(std::uint32_t entry);
  // The guest loaded TRZ from a word carrying `depth` (carried 0: from an untagged one). A host write of TRZ drops it.
  void carryTranslation(const GteTranslationDepth &depth) {
    regs_.translationDepth = depth;
  }

  // The whole register file, flags and SXY places, copied without running a read port.
  GteRawState saveRaw() const;
  void restoreRaw(const GteRawState &state);

  // Makes this the Beetle GTE instance and its Core's projection the current one.
  void bind();
  // Returns both bind points to their defaults if they still name this Core.
  void release();
  GteRegs &registers() {
    return regs_;
  }
  const GteRegs &registers() const {
    return regs_;
  }
  // The explicitly armed observer around each guest GTE command.
  GtePreOpObserver &observer() {
    return observer_;
  }

private:
  void loadControl(std::uint32_t first, std::span<const std::uint32_t> words) {
    for (std::uint32_t i = 0; i < words.size(); ++i) {
      writeControl(first + i, words[i]);
    }
  }
  template <std::size_t Count> std::array<std::uint32_t, Count> storeControl(std::uint32_t first) {
    std::array<std::uint32_t, Count> words{};
    for (std::uint32_t i = 0; i < Count; ++i) {
      words[i] = readControl(first + i);
    }
    return words;
  }

  friend class DepthUnit;

  Core &core_;
  GteRegs regs_{};
  GtePreOpObserver observer_;
  float depthScale_ = 1.0f;
  float depthNearer_ = 0.0f;
};

// While open, `projected` depths are SZ times `scale`, less `nearer`: a draw site whose translation and model are scaled
// from the title's common view units (a moby drawn at its own position shift) names that scale, so depths compare
// across draws, and one the guest sorts nearer than it lies (a bias in ordering-table bins) names that distance.
class DepthUnit {
public:
  DepthUnit(Gte &gte, float scale, float nearer = 0.0f)
      : gte_(gte), previousScale_(gte.depthScale_), previousNearer_(gte.depthNearer_) {
    gte_.depthScale_ = scale;
    gte_.depthNearer_ = nearer;
  }
  DepthUnit(const DepthUnit &) = delete;
  DepthUnit &operator=(const DepthUnit &) = delete;
  DepthUnit(DepthUnit &&) = delete;
  DepthUnit &operator=(DepthUnit &&) = delete;
  ~DepthUnit() {
    gte_.depthScale_ = previousScale_;
    gte_.depthNearer_ = previousNearer_;
  }

private:
  Gte &gte_;
  float previousScale_;
  float previousNearer_;
};

} // namespace psx::cpu
