#include "gte_control.h"

#include <algorithm>
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

gte::RotationWords rotationBetween(const gte::RotationWords &from, const gte::RotationWords &to, float t) {
  gte::RotationWords shown{};
  // s16 pairs.
  for (std::uint32_t word = 0; word < gte::kRotationWords; ++word) {
    shown[word] = lerpHalves(from[word], to[word], t);
  }
  return shown;
}

GteControl blendGteControl(const GteControl &from, const GteControl &to, float t) {
  GteControl blended = to;
  const auto rotationOf = [](const GteControl &control) {
    gte::RotationWords words{};
    std::copy_n(control.begin() + gte::kRotation, gte::kRotationWords, words.begin());
    return words;
  };
  const gte::RotationWords rotation = rotationBetween(rotationOf(from), rotationOf(to), t);
  std::copy(rotation.begin(), rotation.end(), blended.begin() + gte::kRotation);
  for (std::uint32_t reg = gte::kTranslationX; reg <= gte::kTranslationZ; ++reg) {
    blended[reg] = lerpWord(from[reg], to[reg], t);
  }
  return blended;
}

} // namespace psx::present
