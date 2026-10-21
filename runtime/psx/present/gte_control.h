// gte_control.h — the GTE control registers a render runs a body under: read at save time, blended between two
// saved states, written for the render and handed back.
#pragma once

#include "blend.h"
#include "gte.h"
#include "gte_registers.h"

#include <array>
#include <cstdint>

namespace psx::present {

inline constexpr std::uint32_t kGteControlRegisters = 32;

using GteControl = std::array<std::uint32_t, kGteControlRegisters>;

// `gte`'s control registers, without clearing its flags.
GteControl readGteControl(const psx::cpu::Gte &gte);
// Writes every control register but the flags.
void writeGteControl(psx::cpu::Gte &gte, const GteControl &control);
// A rotation matrix `t` of the way from `from` to `to`, element by element.
gte::RotationWords rotationBetween(const gte::RotationWords &from, const gte::RotationWords &to, float t);
// The control registers `t` of the way from `from` to `to`: rotation elements and translation move, every
// other register is `to`'s.
GteControl blendGteControl(const GteControl &from, const GteControl &to, float t);

// The whole GTE as a render found it, handed back when the render is done.
class GteGuard {
public:
  explicit GteGuard(psx::cpu::Gte &gte);
  ~GteGuard();
  GteGuard(const GteGuard &) = delete;
  GteGuard &operator=(const GteGuard &) = delete;
  GteGuard(GteGuard &&) = delete;
  GteGuard &operator=(GteGuard &&) = delete;

private:
  psx::cpu::Gte &gte_;
  GteRawState saved_;
};

} // namespace psx::present
