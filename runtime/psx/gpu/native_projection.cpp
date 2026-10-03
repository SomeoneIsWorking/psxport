#include "native_projection.h"
#include "native_projection_internal.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <compat/intrinsics.h>

namespace psxport::native_projection {
namespace {

struct DivTable {
  std::array<uint8_t, 0x101> values{};
};

constexpr DivTable make_div_table() {
  DivTable table{};
  for (uint32_t divisor = 0x8000; divisor < 0x10000; divisor += 0x80) {
    uint32_t x = 512;
    for (unsigned i = 1; i < 5; ++i) {
      x = (x * (1024 * 512 - ((divisor >> 7) * x))) >> 18;
    }
    table.values[(divisor >> 7) & 0xff] = (uint8_t)(((x + 1) >> 1) - 0x101);
  }
  table.values[0x100] = table.values[0xff];
  return table;
}

constexpr DivTable kDivTable = make_div_table();

int32_t reciprocal(uint16_t divisor) {
  const int32_t x = 0x101 + kDivTable.values[((divisor & 0x7fff) + 0x40) >> 7];
  const int32_t t = (((int32_t)divisor * -x) + 0x80) >> 8;
  return ((x * (131072 + t)) + 0x80) >> 8;
}

uint32_t divide_unr(uint16_t dividend, uint16_t divisor, uint32_t &flags) {
  if ((uint32_t)divisor * 2u <= dividend) {
    flags |= 1u << 17;
    return 0x1ffffu;
  }
  const unsigned shift = compat_clz_u16(divisor);
  const uint32_t numerator = (uint32_t)dividend << shift;
  const uint32_t denominator = (uint32_t)divisor << shift;
  const uint32_t result =
      (uint32_t)(((uint64_t)numerator * reciprocal((uint16_t)(denominator | 0x8000)) + 32768) >> 16);
  return std::min(result, 0x1ffffu);
}

int64_t wrap44(int64_t value, unsigned row, uint32_t &flags) {
  if (value >= (INT64_C(1) << 43)) {
    flags |= 1u << (30 - row);
  }
  if (value < -(INT64_C(1) << 43)) {
    flags |= 1u << (27 - row);
  }
  constexpr uint64_t mask = (UINT64_C(1) << 44) - 1;
  constexpr uint64_t sign = UINT64_C(1) << 43;
  const uint64_t bits = (uint64_t)value & mask;
  return bits < sign ? (int64_t)bits : (int64_t)bits - (INT64_C(1) << 44);
}

int32_t clampi(int32_t value, int32_t low, int32_t high) {
  return std::clamp(value, low, high);
}

int32_t clamp_flagged(int32_t value, int32_t low, int32_t high, unsigned bit, uint32_t &flags) {
  if (value < low || value > high) {
    flags |= 1u << bit;
  }
  return clampi(value, low, high);
}

void check_mac0_overflow(int64_t value, uint32_t &flags) {
  if (value < INT32_MIN) {
    flags |= 1u << 15;
  }
  if (value > INT32_MAX) {
    flags |= 1u << 16;
  }
}

} // namespace

RawViewVertex transform(const FixedAffine &affine, ModelVertex vertex) {
  RawViewVertex out{};
  const int32_t v[3] = {vertex.x, vertex.y, vertex.z};
  for (unsigned row = 0; row < 3; ++row) {
    int64_t accumulator = (int64_t)affine.t[row] * 4096;
    for (unsigned column = 0; column < 3; ++column) {
      accumulator = wrap44(accumulator + (int64_t)affine.m[row][column] * v[column], row, out.mac_flags);
    }
    out.raw_view_fixed[row] = accumulator;
  }
  return out;
}

ContinuousProjectedVertex project_view(const std::array<float, 3> &raw_view, const ProjectionParams &projection) {
  ContinuousProjectedVertex out{};
  out.pz = std::max((float)projection.h * 0.5f, raw_view[2]);
  const float scale = out.pz > 0.0f ? (float)projection.h / out.pz : 0.0f;
  const float centerX = (float)projection.ofx / 65536.0f;
  const float centerY = (float)projection.ofy / 65536.0f;
  const float x = std::clamp(raw_view[0], -32768.0f, 32767.0f);
  const float y = std::clamp(raw_view[1], -32768.0f, 32767.0f);
  out.px = std::clamp(centerX + x * scale, -1024.0f, 1023.0f);
  out.py = std::clamp(centerY + y * scale, -1024.0f, 1023.0f);
  return out;
}

namespace {

NativeProjectedVertex project_transformed_mode(const RawViewVertex &transformed,
                                               const ProjectionParams &projection,
                                               unsigned shift,
                                               bool limit_mode) {
  NativeProjectedVertex out{};
  if (shift != 0 && shift != 12) {
    return out;
  }
  out.flags = transformed.mac_flags;
  for (unsigned row = 0; row < 3; ++row) {
    const int64_t accumulator = transformed.raw_view_fixed[row];
    out.raw_view_fixed[row] = accumulator;
    out.raw_view[row] = (float)accumulator / 4096.0f;
    const int32_t mac = (int32_t)(accumulator >> shift);
    const int32_t flag_value = row == 2 ? (int32_t)(accumulator >> 12) : mac;
    out.ir[row] = clamp_flagged(flag_value, -32768, 32767, 24 - row, out.flags);
    out.ir[row] = clampi(mac, limit_mode ? 0 : -32768, 32767);
  }
  out.sz = (uint16_t)clamp_flagged((int32_t)(out.raw_view_fixed[2] >> 12), 0, 65535, 18, out.flags);
  const uint32_t ratio = divide_unr(projection.h, out.sz, out.flags);
  const auto project_axis = [&](int32_t offset, int32_t ir, unsigned bit) {
    const int64_t expression = (int64_t)offset + (int64_t)ir * ratio;
    check_mac0_overflow(expression, out.flags);
    return (int16_t)clamp_flagged((int32_t)(expression >> 16), -1024, 1023, bit, out.flags);
  };
  out.sx = project_axis(projection.ofx, out.ir[0], 14);
  out.sy = project_axis(projection.ofy, out.ir[1], 13);

  const int64_t depth_cue = (int64_t)projection.dqb + (int64_t)projection.dqa * ratio;
  check_mac0_overflow(depth_cue, out.flags);
  out.mac0 = (int32_t)depth_cue;
  const int32_t ir0 = (int32_t)(depth_cue >> 12);
  if (ir0 < 0 || ir0 > 4096) {
    out.flags |= 1u << 12;
  }
  if ((out.flags & 0x7f87e000u) != 0) {
    out.flags |= 1u << 31;
  }

  // The producer endpoint preserves fractional affine coordinates. Other sf/lm
  // modes belong to the diagnostic adapter and retain their existing IR inputs.
  const std::array<float, 3> view = shift == 12 && !limit_mode
                                        ? out.raw_view
                                        : std::array<float, 3>{(float)out.ir[0], (float)out.ir[1], out.raw_view[2]};
  const auto projected = project_view(view, projection);
  out.px = projected.px;
  out.py = projected.py;
  out.pz = projected.pz;
  return out;
}

bool sampleable(const RawViewVertex &view) {
  constexpr int64_t bound = INT64_C(1) << 43;
  return view.mac_flags == 0 && std::all_of(view.raw_view_fixed.begin(), view.raw_view_fixed.end(), [](int64_t value) {
           return value >= -bound && value < bound;
         });
}

} // namespace

NativeProjectedVertex detail::project_gte_mode(const FixedAffine &affine,
                                               const ProjectionParams &projection,
                                               ModelVertex vertex,
                                               unsigned shift,
                                               bool limit_mode) {
  return project_transformed_mode(transform(affine, vertex), projection, shift, limit_mode);
}

NativeProjectedVertex project(const FixedAffine &affine, const ProjectionParams &projection, ModelVertex vertex) {
  return project_transformed(transform(affine, vertex), projection);
}

NativeProjectedVertex project_transformed(const RawViewVertex &view, const ProjectionParams &projection) {
  return project_transformed_mode(view, projection, 12, false);
}

std::optional<NativeProjectedVertex>
sample_view(const RawViewVertex &previous, const RawViewVertex &current, const ProjectionParams &projection, double t) {
  if (!std::isfinite(t) || t < 0.0 || t > 1.0 || !sampleable(previous) || !sampleable(current)) {
    return std::nullopt;
  }
  if (t == 0.0) {
    return project_transformed(previous, projection);
  }
  if (t == 1.0) {
    return project_transformed(current, projection);
  }
  RawViewVertex sampled{};
  for (unsigned row = 0; row < 3; ++row) {
    sampled.raw_view_fixed[row] =
        (int64_t)std::floor(std::lerp((double)previous.raw_view_fixed[row], (double)current.raw_view_fixed[row], t));
  }
  return project_transformed(sampled, projection);
}

std::array<std::array<int16_t, 3>, 3> rotationFromControlWords(const std::array<uint32_t, 5> &words) {
  const uint32_t c0 = words[0], c1 = words[1], c2 = words[2], c3 = words[3], c4 = words[4];
  return {{{(int16_t)c0, (int16_t)(c0 >> 16), (int16_t)c1},
           {(int16_t)(c1 >> 16), (int16_t)c2, (int16_t)(c2 >> 16)},
           {(int16_t)c3, (int16_t)(c3 >> 16), (int16_t)c4}}};
}

} // namespace psxport::native_projection
