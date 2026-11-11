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

gte::TranslationWords
viewTranslationBetween(const ViewTransform &from, const ViewTransform &to, const gte::RotationWords &shown, float t) {
  if (t <= 0.0f) {
    return from.translation;
  }
  if (t >= 1.0f) {
    return to.translation;
  }
  using Vector = std::array<double, 3>;
  const auto apply = [](const Matrix3 &m, const Vector &v) {
    Vector out{};
    for (std::size_t row = 0; row < 3; ++row) {
      out[row] = m[row][0] * v[0] + m[row][1] * v[1] + m[row][2] * v[2];
    }
    return out;
  };
  const auto translationOf = [](const ViewTransform &view) {
    Vector out{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      out[axis] = static_cast<double>(static_cast<std::int32_t>(view.translation[axis]));
    }
    return out;
  };
  // The camera -R^T*T; the 4.12 rows are not quite unit, so the offset is taken against R*C, not -T.
  const auto camera = [&](const Matrix3 &m, const Vector &translation) {
    Vector out{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      out[axis] = -(m[0][axis] * translation[0] + m[1][axis] * translation[1] + m[2][axis] * translation[2]);
    }
    return out;
  };
  const Matrix3 earlier = decode(from.rotation);
  const Matrix3 later = decode(to.rotation);
  const Vector earlierTranslation = translationOf(from);
  const Vector laterTranslation = translationOf(to);
  const Vector earlierCamera = camera(earlier, earlierTranslation);
  const Vector laterCamera = camera(later, laterTranslation);
  Vector midCamera{};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    midCamera[axis] = lerpExact(earlierCamera[axis], laterCamera[axis], t);
  }
  const Vector shownCamera = apply(decode(shown), midCamera);
  const Vector earlierPlaced = apply(earlier, earlierCamera);
  const Vector laterPlaced = apply(later, laterCamera);
  gte::TranslationWords out{};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const double straight = lerpExact(earlierTranslation[axis], laterTranslation[axis], t);
    const double turned = shownCamera[axis] - lerpExact(earlierPlaced[axis], laterPlaced[axis], t);
    out[axis] = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::nearbyint(straight - turned)));
  }
  return out;
}

gte::RotationWords scaledRowsBetween(const gte::RotationWords &from, const gte::RotationWords &to, float t) {
  if (t <= 0.0f) {
    return from;
  }
  if (t >= 1.0f || from == to) {
    return to;
  }
  Matrix3 earlier = decode(from);
  Matrix3 later = decode(to);
  std::array<double, 3> scales{};
  for (std::size_t row = 0; row < scales.size(); ++row) {
    const auto length = [&](const Matrix3 &m) {
      return std::hypot(m[row][0], m[row][1], m[row][2]);
    };
    const double before = length(earlier);
    scales[row] = length(later);
    if (before == 0.0 || scales[row] == 0.0) {
      return elementsBetween(from, to, t);
    }
    for (std::size_t column = 0; column < 3; ++column) {
      earlier[row][column] /= before;
      later[row][column] /= scales[row];
    }
  }
  std::optional<Matrix3> shown = lucent::rotation::between(earlier, later, t);
  if (!shown) {
    return elementsBetween(from, to, t);
  }
  for (std::size_t row = 0; row < scales.size(); ++row) {
    for (double &value : (*shown)[row]) {
      value *= scales[row];
    }
  }
  return encode(*shown);
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
