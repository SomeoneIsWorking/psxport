#pragma once

#include <array>
#include <cstdint>

namespace psxport::native_projection {

struct FixedAffine {
  std::array<std::array<int16_t, 3>, 3> m{};
  std::array<int32_t, 3> t{};
};

struct ProjectionParams {
  int32_t ofx = 0;
  int32_t ofy = 0;
  uint16_t h = 0;
};

struct ModelVertex {
  int16_t x = 0;
  int16_t y = 0;
  int16_t z = 0;
};

struct NativeProjectedVertex {
  std::array<int64_t, 3> raw_view_fixed{}; // signed wrapped 44-bit, 12 fractional bits
  std::array<float, 3> raw_view{};
  std::array<int32_t, 3> ir{};
  uint16_t sz = 0;
  int16_t sx = 0;
  int16_t sy = 0;
  float px = 0.0f;
  float py = 0.0f;
  float pz = 0.0f;
};

// Pure PSX fixed-point affine transform + RTPS projection for sf=1,lm=0. No
// Core, GTE binding, diagnostics, or ambient projection state. Integer outputs
// match the hardware endpoint; raw/floats retain information discarded by
// SXY/SZ for native rendering. This is endpoint projection only: projected
// output is not a temporal recipe and must not be stored/lerped in place of
// authored model, transform, and camera inputs.
NativeProjectedVertex project(const FixedAffine &affine, const ProjectionParams &projection, ModelVertex vertex);

} // namespace psxport::native_projection
