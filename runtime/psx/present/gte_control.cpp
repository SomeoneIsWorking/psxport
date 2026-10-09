// gte_control.cpp — see gte_control.h.
#include "gte_control.h"

#include "core.h"

#include <cstring>

namespace psx::present {
namespace {

constexpr std::uint32_t kControlBase = 32u;

} // namespace

GteGuard::GteGuard() {
  GTE_SaveRawState(&saved_);
}

GteGuard::~GteGuard() {
  GTE_RestoreRawState(&saved_);
}

GteControl readGteControl() {
  GteRawState raw;
  GTE_SaveRawState(&raw);
  GteControl control;
  std::memcpy(control.data(), raw.reg + kControlBase, sizeof(control));
  return control;
}

void writeGteControl(const GteControl &control) {
  for (std::uint32_t reg = 0; reg < kGteControlRegisters; ++reg) {
    if (reg != gte::kFlag) {
      gte_write_ctrl(reg, control[reg]);
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
