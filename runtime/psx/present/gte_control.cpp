#include "gte_control.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>

namespace psx::present {
namespace {

constexpr std::uint32_t kControlBase = 32u;
constexpr double kUnit = 4096.0; // 1.0 in the GTE's 4.12 matrix elements
constexpr int kPolarSteps = 24;

using Matrix = std::array<std::array<double, 3>, 3>;
using Quaternion = std::array<double, 4>; // w, x, y, z

// RT words: R11R12, R13R21, R22R23, R31R32, R33.
Matrix decode(const gte::RotationWords &words) {
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

gte::RotationWords encode(const Matrix &m) {
  gte::RotationWords words{};
  words[0] = element(m[0][0]) | element(m[0][1]) << 16;
  words[1] = element(m[0][2]) | element(m[1][0]) << 16;
  words[2] = element(m[1][1]) | element(m[1][2]) << 16;
  words[3] = element(m[2][0]) | element(m[2][1]) << 16;
  // The GTE holds R33 sign-extended.
  words[4] = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(element(m[2][2]))));
  return words;
}

double determinant(const Matrix &m) {
  return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
         m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

Matrix multiply(const Matrix &a, const Matrix &b) {
  Matrix out{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
    }
  }
  return out;
}

Matrix transpose(const Matrix &m) {
  Matrix out{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      out[r][c] = m[c][r];
    }
  }
  return out;
}

// M = Q * S: Q orthogonal, S the symmetric stretch; nothing for a singular M.
struct Polar {
  Matrix turn;
  Matrix stretch;
};

std::optional<Polar> polar(const Matrix &m) {
  const double det = determinant(m);
  if (std::abs(det) < 1e-9) {
    return std::nullopt;
  }
  Matrix q = m;
  for (int step = 0; step < kPolarSteps; ++step) {
    const double d = determinant(q);
    // Q^-T is the cofactor matrix over the determinant.
    Matrix inverseTranspose{};
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) {
        const std::size_t r1 = (r + 1) % 3;
        const std::size_t r2 = (r + 2) % 3;
        const std::size_t c1 = (c + 1) % 3;
        const std::size_t c2 = (c + 2) % 3;
        inverseTranspose[r][c] = (q[r1][c1] * q[r2][c2] - q[r1][c2] * q[r2][c1]) / d;
      }
    }
    double change = 0.0;
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) {
        const double next = 0.5 * (q[r][c] + inverseTranspose[r][c]);
        change = std::max(change, std::abs(next - q[r][c]));
        q[r][c] = next;
      }
    }
    if (change < 1e-12) {
      break;
    }
  }
  return Polar{q, multiply(transpose(q), m)};
}

Quaternion quaternionOf(const Matrix &m) {
  const double trace = m[0][0] + m[1][1] + m[2][2];
  Quaternion q{};
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    q = {0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
  } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
    const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
    q = {(m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
  } else if (m[1][1] > m[2][2]) {
    const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
    q = {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s};
  } else {
    const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
    q = {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s};
  }
  return q;
}

Matrix matrixOf(const Quaternion &q) {
  const auto [w, x, y, z] = q;
  return {{{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
           {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
           {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}}};
}

// The shorter arc from `a` to `b`.
Quaternion slerp(const Quaternion &a, Quaternion b, double t) {
  double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (dot < 0.0) {
    dot = -dot;
    for (double &component : b) {
      component = -component;
    }
  }
  double wa = 1.0 - t;
  double wb = t;
  if (dot < 0.9995) {
    const double angle = std::acos(std::min(dot, 1.0));
    const double sine = std::sin(angle);
    wa = std::sin((1.0 - t) * angle) / sine;
    wb = std::sin(t * angle) / sine;
  }
  Quaternion out{};
  double norm = 0.0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = wa * a[i] + wb * b[i];
    norm += out[i] * out[i];
  }
  norm = std::sqrt(norm);
  for (double &component : out) {
    component /= norm;
  }
  return out;
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
  std::optional<Polar> a = polar(decode(from));
  std::optional<Polar> b = polar(decode(to));
  // A singular matrix has no turn, and no turn leads from a mirror to a rotation.
  if (!a || !b || (determinant(a->turn) < 0.0) != (determinant(b->turn) < 0.0)) {
    return elementsBetween(from, to, t);
  }
  for (Polar *side : {&*a, &*b}) {
    if (determinant(side->turn) < 0.0) {
      // -Q is a rotation and (-Q)(-S) is the same matrix.
      for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
          side->turn[r][c] = -side->turn[r][c];
          side->stretch[r][c] = -side->stretch[r][c];
        }
      }
    }
  }
  const double at = t;
  Matrix stretch{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      stretch[r][c] = a->stretch[r][c] + (b->stretch[r][c] - a->stretch[r][c]) * at;
    }
  }
  const Matrix turn = matrixOf(slerp(quaternionOf(a->turn), quaternionOf(b->turn), at));
  return encode(multiply(turn, stretch));
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
