#include "native_projection.h"
#include "native_projection_internal.h"

#include <algorithm>
#include <array>

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

uint32_t divide_unr(uint16_t dividend, uint16_t divisor) {
  if ((uint32_t)divisor * 2u <= dividend) {
    return 0x1ffffu;
  }
  const unsigned shift = compat_clz_u16(divisor);
  const uint32_t numerator = (uint32_t)dividend << shift;
  const uint32_t denominator = (uint32_t)divisor << shift;
  const uint32_t result =
      (uint32_t)(((uint64_t)numerator * reciprocal((uint16_t)(denominator | 0x8000)) + 32768) >> 16);
  return std::min(result, 0x1ffffu);
}

int64_t wrap44(int64_t value) {
  constexpr uint64_t mask = (UINT64_C(1) << 44) - 1;
  constexpr uint64_t sign = UINT64_C(1) << 43;
  const uint64_t bits = (uint64_t)value & mask;
  return bits < sign ? (int64_t)bits : (int64_t)bits - (INT64_C(1) << 44);
}

int32_t clampi(int32_t value, int32_t low, int32_t high) {
  return std::clamp(value, low, high);
}

} // namespace

NativeProjectedVertex detail::project_gte_mode(const FixedAffine &affine,
                                               const ProjectionParams &projection,
                                               ModelVertex vertex,
                                               unsigned shift,
                                               bool limit_mode) {
  NativeProjectedVertex out{};
  if (shift != 0 && shift != 12) {
    return out;
  }
  const int32_t v[3] = {vertex.x, vertex.y, vertex.z};
  for (unsigned row = 0; row < 3; ++row) {
    int64_t accumulator = (int64_t)affine.t[row] * 4096;
    for (unsigned column = 0; column < 3; ++column) {
      accumulator = wrap44(accumulator + (int64_t)affine.m[row][column] * v[column]);
    }
    out.raw_view_fixed[row] = accumulator;
    out.raw_view[row] = (float)accumulator / 4096.0f;
    out.ir[row] = clampi((int32_t)(accumulator >> shift), limit_mode ? 0 : -32768, 32767);
  }
  out.sz = (uint16_t)clampi((int32_t)(out.raw_view_fixed[2] >> 12), 0, 65535);
  const uint32_t ratio = divide_unr(projection.h, out.sz);
  out.sx = (int16_t)clampi((int32_t)(((int64_t)projection.ofx + (int64_t)out.ir[0] * ratio) >> 16), -1024, 1023);
  out.sy = (int16_t)clampi((int32_t)(((int64_t)projection.ofy + (int64_t)out.ir[1] * ratio) >> 16), -1024, 1023);

  out.pz = std::max((float)projection.h * 0.5f, out.raw_view[2]);
  const float scale = out.pz > 0.0f ? (float)projection.h / out.pz : 0.0f;
  const float centerX = (float)projection.ofx / 65536.0f;
  const float centerY = (float)projection.ofy / 65536.0f;
  out.px = std::clamp(centerX + (float)out.ir[0] * scale, -1024.0f, 1023.0f);
  out.py = std::clamp(centerY + (float)out.ir[1] * scale, -1024.0f, 1023.0f);
  return out;
}

NativeProjectedVertex project(const FixedAffine &affine, const ProjectionParams &projection, ModelVertex vertex) {
  return detail::project_gte_mode(affine, projection, vertex, 12, false);
}

} // namespace psxport::native_projection
