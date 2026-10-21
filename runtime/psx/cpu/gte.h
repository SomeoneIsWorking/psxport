// gte.h — one Core's GTE (COP2): its register file and the Beetle GTE run against it.
#pragma once

#include "gte_preop_observer.h"
#include "gte_registers.h"
#include "gte_state.h"

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

  std::uint32_t readData(std::uint32_t reg);
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

  // The whole register file and flags, copied without running a read port.
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
  template <std::size_t Count>
  std::array<std::uint32_t, Count> storeControl(std::uint32_t first) {
    std::array<std::uint32_t, Count> words{};
    for (std::uint32_t i = 0; i < Count; ++i) {
      words[i] = readControl(first + i);
    }
    return words;
  }

  Core &core_;
  GteRegs regs_{};
  GtePreOpObserver observer_;
};

} // namespace psx::cpu
