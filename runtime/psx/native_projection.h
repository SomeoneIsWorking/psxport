#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace psxport::native_projection {

struct FixedAffine {
  std::array<std::array<int16_t, 3>, 3> m{};
  std::array<int32_t, 3> t{};
};

struct ProjectionParams {
  int32_t ofx = 0;
  int32_t ofy = 0;
  uint16_t h = 0;
  int16_t dqa = 0;
  int32_t dqb = 0;
};

struct ModelVertex {
  int16_t x = 0;
  int16_t y = 0;
  int16_t z = 0;
};

struct RawViewVertex {
  std::array<int64_t, 3> raw_view_fixed{}; // signed wrapped 44-bit, 12 fractional bits
  uint32_t mac_flags = 0;                  // MAC1-3 overflow history (FLAG bits 30..25)
};

// Preserve the raw affine result and overflow history before IR/SZ saturation.
RawViewVertex transform(const FixedAffine &affine, ModelVertex vertex);

struct ContinuousProjectedVertex {
  float px = 0.0f;
  float py = 0.0f;
  float pz = 0.0f;
};

// Project finite, unsaturated view coordinates for native rendering. XY retain
// fractions within the signed IR range; Z clamps only to H/2, without hardware
// SZ/IR narrowing. Screen coordinates retain the PSX [-1024,1023] clamp.
// This pure projection owns no temporal history or ambient GTE state.
ContinuousProjectedVertex project_view(const std::array<float, 3> &raw_view, const ProjectionParams &projection);

struct NativeProjectedVertex {
  std::array<int64_t, 3> raw_view_fixed{}; // signed wrapped 44-bit, 12 fractional bits
  std::array<float, 3> raw_view{};
  std::array<int32_t, 3> ir{};
  uint16_t sz = 0;
  int16_t sx = 0;
  int16_t sy = 0;
  uint32_t flags = 0;
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

// The same sf=1,lm=0 projection/classification stage used by project(). Input
// must retain the signed-44 values and MAC flags produced by transform().
NativeProjectedVertex project_transformed(const RawViewVertex &view, const ProjectionParams &projection);

// Sample matching source transforms, never projected endpoints. Reject invalid
// t/ranges and either endpoint's MAC overflow: wrapped accumulator history has
// no defined interpolation. Exact t=0/1 projects that endpoint unchanged;
// interior raw fixed values interpolate in double and round toward -infinity
// to 1/4096 view units before the shared integer/FLAG and float projection.
// Callers own source identity and must sample each authored precision stream
// independently. Projection parameters are constant throughout this interval.
std::optional<NativeProjectedVertex>
sample_view(const RawViewVertex &previous, const RawViewVertex &current, const ProjectionParams &projection, double t);

} // namespace psxport::native_projection
