// gte.h — one Core's GTE (COP2): its register file and the Beetle GTE run against it.
#pragma once

#include "gte_preop_observer.h"
#include "gte_registers.h"
#include "gte_state.h"

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
    for (std::uint32_t i = 0; i < gte::kRotationWords; ++i) {
      writeControl(gte::kRotation + i, words[i]);
    }
  }
  gte::RotationWords storeRotation() {
    gte::RotationWords words{};
    for (std::uint32_t i = 0; i < gte::kRotationWords; ++i) {
      words[i] = readControl(gte::kRotation + i);
    }
    return words;
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
  Core &core_;
  GteRegs regs_{};
  GtePreOpObserver observer_;
};

} // namespace psx::cpu
