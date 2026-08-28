// gpu_vk_depth.cpp — normalized depth and per-primitive ordering policy for the SDL GPU renderer.
//
// This is separate from gpu_vk.cpp's command recording and raster batching. The policy is shared by
// geometry emission, z-fight diagnostics, and the 2D/3D order-band classifier.
#include "cfg.h"
#include "game.h"
#include "gpu_vk.h"
#include <cmath>
#include <cstdlib>

namespace {

float ord3d(float depth) {
  return kGpuNative3dMin + depth * (kGpuNative3dMax - kGpuNative3dMin);
}

float zbias_max() {
  static float maxBias = -1.f;
  if (maxBias < 0.f) {
    const char *e = cfg_str("PSXPORT_ZBIAS_MAX");
    maxBias = e ? (float)atof(e) : 1.5e-3f;
    if (maxBias < 0.f) {
      maxBias = 0.f;
    }
  }
  return maxBias;
}

} // namespace

float gpu_vk_map_3d_depth(float depth) {
  return ord3d(depth);
}

float gpu_vk_map_biased_3d_depth(float depth, float bias) {
  // Clamp to the 3D band: painter order must not move a world primitive into either 2D band.
  const float mapped = ord3d(depth) + bias;
  return mapped < kGpuNative3dMin ? kGpuNative3dMin : (mapped > kGpuNative3dMax ? kGpuNative3dMax : mapped);
}

float gpu_vk_next_distinct_3d_depth(float depth, float nearer_limit) {
  const float mapped = ord3d(depth);
  while (depth < nearer_limit) {
    const float next = std::nextafter(depth, nearer_limit);
    if (!(next < nearer_limit)) {
      return nearer_limit;
    }
    depth = next;
    if (ord3d(depth) > mapped) {
      return depth;
    }
  }
  return nearer_limit;
}

// Exposed for the z-fight scanner so it can model the shipped paint-order tiebreak without rerunning
// the renderer. The values are sweep knobs, not title-specific geometry constants.
float gpu_zbias_unit() {
  static float unit = -1.f;
  if (unit < 0.f) {
    const char *e = cfg_str("PSXPORT_ZBIAS");
    unit = e ? (float)atof(e) : 4e-7f;
    if (unit < 0.f) {
      unit = 0.f;
    }
  }
  return unit;
}

float gpu_vk_map_ordered_3d_depth(float depth, uint32_t order) {
  const float cap = zbias_max();
  const float rawBias = (float)order * gpu_zbias_unit();
  return gpu_vk_map_biased_3d_depth(depth, rawBias > cap ? cap : rawBias);
}

bool gpu_vk_order_bias_distinguishes(uint32_t seq) {
  const float unit = gpu_zbias_unit();
  return unit > 0.f && (double)seq * unit < zbias_max();
}

void GpuVkState::set_vd(const float *depth) {
  s_vd = depth;
}

void GpuVkState::set_vd_n(const float *depth) {
  s_vdn = depth;
}

void GpuVkState::set_xyf(const float *x, const float *y) {
  s_xf = x;
  s_yf = y;
}

void GpuVkState::set_order(unsigned idx) {
  if (s_order_override >= 0) {
    idx = (unsigned)s_order_override;
    s_order_override = -1;
  }
  s_cur_ord = (float)(idx + 1) / 65536.0f;
  if (s_cur_ord > 1.0f) {
    s_cur_ord = 1.0f;
  }
  s_cur_ordn = s_cur_ord;
  s_vd = nullptr;
  s_vdn = nullptr;
  s_xf = nullptr;
  s_yf = nullptr;
  const float bias = (float)idx * gpu_zbias_unit();
  s_depth_bias = bias > zbias_max() ? zbias_max() : bias;
}

void GpuVkState::set_order_2d(unsigned idx) {
  float t = (float)(idx + 1) / 65536.0f;
  if (t > 1.0f) {
    t = 1.0f;
  }
  s_cur_ord = kGpuNative3dMax + (1.0f - kGpuNative3dMax) * t;
  s_vd = nullptr;
}

void GpuVkState::set_order_2d_n(unsigned idx) {
  float t = (float)(idx + 1) / 65536.0f;
  if (t > 1.0f) {
    t = 1.0f;
  }
  s_cur_ordn = kGpuNative3dMax + (1.0f - kGpuNative3dMax) * t;
  s_vdn = nullptr;
}

void GpuVkState::set_order_2d_bg(unsigned idx) {
  float t = (float)(idx + 1) / 65536.0f;
  if (t > 1.0f) {
    t = 1.0f;
  }
  s_cur_ord = kGpuNative3dMin * t;
  s_vd = nullptr;
}

void GpuVkState::set_order_2d_bg_n(unsigned idx) {
  float t = (float)(idx + 1) / 65536.0f;
  if (t > 1.0f) {
    t = 1.0f;
  }
  s_cur_ordn = kGpuNative3dMin * t;
  s_vdn = nullptr;
}
