// gte_control.cpp — see gte_control.h.
#include "gte_control.h"

#include <cstring>

namespace psx::present {
namespace {

constexpr std::uint32_t kControlBase = 32u;

} // namespace

GteGuard::GteGuard(psx::cpu::Gte &gte) : gte_(gte), saved_(gte.saveRaw()) {}

GteGuard::~GteGuard() {
  gte_.restoreRaw(saved_);
}

GteControl readGteControl(const psx::cpu::Gte &gte) {
  const GteRawState raw = gte.saveRaw();
  GteControl control;
  std::memcpy(control.data(), raw.reg + kControlBase, sizeof(control));
  return control;
}

void writeGteControl(psx::cpu::Gte &gte, const GteControl &control) {
  for (std::uint32_t reg = 0; reg < kGteControlRegisters; ++reg) {
    if (reg != gte::kFlag) {
      gte.writeControl(reg, control[reg]);
    }
  }
}

GteControl blendGteControl(const GteControl &from, const GteControl &to, float t) {
  GteControl blended = to;
  for (std::uint32_t reg = 0; reg < kGteTranslationEnd; ++reg) {
    if (reg >= kGteRotationWords) {
      blended[reg] = static_cast<std::uint32_t>(
          lerpInt(static_cast<std::int32_t>(from[reg]), static_cast<std::int32_t>(to[reg]), t));
    } else {
      blended[reg] = lerpHalves(from[reg], to[reg], t);
    }
  }
  return blended;
}

} // namespace psx::present
