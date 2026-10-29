#include "gte_control.h"

#include <lucent/rotation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>

namespace psx::present {
namespace {

constexpr std::uint32_t kControlBase = 32u;
constexpr double kUnit = 4096.0; // 1.0 in the GTE's 4.12 matrix elements

using lucent::rotation::Matrix3;

// RT words: R11R12, R13R21, R22R23, R31R32, R33.
Matrix3 decode(const gte::RotationWords &words) {
  std::array<double, 9> e{};
  for (std::size_t i = 0; i < e.size(); ++i) {
    const std::uint32_t word = words[i / 2];
    e[i] = static_cast<double>(static_cast<std::int16_t>(i % 2 == 0 ? word : word >> 16)) / kUnit;
  }
  return {{{e[0], e[1], e[2]}, {e[3], e[4], e[5]}, {e[6], e[7], e[8]}}};
}

std::uint32_t element(double value) {
  const double scaled = std::clamp(std::round(value * kUnit), -32768.0, 32767.0);
  return static_cast<std::uint16_t>(static_cast<std::int16_t>(scaled));
}

gte::RotationWords encode(const Matrix3 &m) {
  gte::RotationWords words{};
  words[0] = element(m[0][0]) | element(m[0][1]) << 16;
  words[1] = element(m[0][2]) | element(m[1][0]) << 16;
  words[2] = element(m[1][1]) | element(m[1][2]) << 16;
  words[3] = element(m[2][0]) | element(m[2][1]) << 16;
  // The GTE holds R33 sign-extended.
  words[4] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(element(m[2][2]))));
  return words;
}

gte::RotationWords elementsBetween(const gte::RotationWords &from, const gte::RotationWords &to, float t) {
  gte::RotationWords shown{};
  for (std::uint32_t word = 0; word < gte::kRotationWords; ++word) {
    shown[word] = lerpHalves(from[word], to[word], t);
  }
  return shown;
}

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
  if (t <= 0.0f) {
    return from;
  }
  if (t >= 1.0f || from == to) {
    return to;
  }
  const std::optional<Matrix3> shown = lucent::rotation::between(decode(from), decode(to), t);
  return shown ? encode(*shown) : elementsBetween(from, to, t);
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
