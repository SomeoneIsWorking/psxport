#include "gpu_vk_untextured_selftest.h"

#include "gpu_vk_internal.h"
#include "gpu_vk_selftest_support.h"

#include <lucent/log.h>

namespace {

constexpr int kProbeX = 111;
constexpr int kProbeY = 205;
constexpr uint16_t kExpected = gpu_vk_selftest_pack555(3, 0, 6);
constexpr uint16_t kDitherNegative = gpu_vk_selftest_pack555(15, 15, 15);
constexpr uint16_t kDitherPositive = gpu_vk_selftest_pack555(16, 16, 16);

} // namespace

void gpu_vk_stage_untextured_gouraud_selftest(GpuVkState &gpu) {
  float depth[3] = {.40f, .40f, .40f};
  gpu.set_order(7);
  gpu.set_vd(depth);
  gpu.s_untextured_gouraud = 1;
  gpu.s_untextured_dither = 0;
  // Translated from a measured legal PSX G3 packet. At the probe, integer barycentric interpolation
  // rounds blue to 54 before 8->5-bit truncation, so the shipping opaque path must encode blue=6.
  gpu.draw_tri(3, 210, 57, 8, 90, 199, 230, 8, 0, 33, 86, 161, 33, 0, 49, 0, 0, 1023, 511);
  gpu.set_order(8);
  gpu.set_vd(depth);
  gpu.s_untextured_dither = 1;
  gpu.draw_tri(288, 190, 127, 127, 127, 318, 190, 127, 127, 127, 303, 230, 127, 127, 127, 0, 0, 1023, 511);
}

bool gpu_vk_check_untextured_gouraud_selftest(const uint16_t *vram, int vramWidth) {
  const uint16_t actual = vram[kProbeY * vramWidth + kProbeX];
  const uint16_t negative = vram[200 * vramWidth + 300];
  const uint16_t positive = vram[200 * vramWidth + 303];
  const bool ok = actual == kExpected && negative == kDitherNegative && positive == kDitherPositive;
  lucent::info("gpu_selftest",
               "untextured G3 interpolation={:04X} expect {:04X} dither={:04X}/{:04X} expect {:04X}/{:04X} => {}",
               actual,
               kExpected,
               negative,
               positive,
               kDitherNegative,
               kDitherPositive,
               ok ? "PASS" : "FAIL");
  return ok;
}
