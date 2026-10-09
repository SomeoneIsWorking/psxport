// gte_control.h — the GTE control registers a render runs a body under: read at save time, blended between two
// saved states, written for the render and handed back.
#pragma once

#include "blend.h"
#include "gte_registers.h"
#include "gte_state.h"

#include <array>
#include <cstdint>

namespace psx::present {

inline constexpr std::uint32_t kGteControlRegisters = 32;
// Control registers 0..4 hold the rotation matrix as s16 pairs, 5..7 the translation as s32.
inline constexpr std::uint32_t kGteRotationWords = 5;
inline constexpr std::uint32_t kGteTranslationEnd = 8;

using GteControl = std::array<std::uint32_t, kGteControlRegisters>;

// The bound GTE's control registers, without clearing its flags.
GteControl readGteControl();
// Writes every control register but the flags.
void writeGteControl(const GteControl &control);
// The control registers `t` of the way from `from` to `to`: rotation elements and translation move, every
// other register is `to`'s.
GteControl blendGteControl(const GteControl &from, const GteControl &to, float t);

// The whole GTE as a render found it, handed back when the render is done.
class GteGuard {
public:
  GteGuard();
  ~GteGuard();
  GteGuard(const GteGuard &) = delete;
  GteGuard &operator=(const GteGuard &) = delete;
  GteGuard(GteGuard &&) = delete;
  GteGuard &operator=(GteGuard &&) = delete;

private:
  GteRawState saved_{};
};

} // namespace psx::present
