#include "gpu_vk_texture_coverage_selftest.h"

#include "game.h"
#include <lucent/log.h>
#include <string.h>

namespace {

constexpr int kVramWidth = 1024;
constexpr int kVramHeight = 512;
constexpr int kTextureX = 512;
constexpr int kTextureY = 256;
constexpr int kTexelU = 4;
constexpr int kTexelV = 4;

// The same narrow shape the untextured edge probe uses, moved clear of the other selftests. PSX
// evaluates coverage at a pixel's INTEGER coordinate, and vertex B sits exactly on one; a Vulkan
// raster that samples the unshifted fragment centre falls outside this edge and misses it.
constexpr int kApexX = 90;
constexpr int kApexY = 420;
constexpr int kEdgeProbeX = 85;
constexpr int kEdgeProbeY = 430;
constexpr int kFootX = 90;
constexpr int kFootY = 431;
// Covered under either convention, so it separates "the raster missed this edge" from "the draw
// never rasterized at all".
constexpr int kInteriorProbeX = 89;
constexpr int kInteriorProbeY = 429;

constexpr uint16_t kBackground = gpu_vk_selftest_pack555(2, 4, 6);
constexpr uint16_t kTexel = gpu_vk_selftest_pack555(20, 6, 13);

} // namespace

bool gpu_vk_run_texture_coverage_selftest(GpuVkState &gpu,
                                          uint16_t *vram,
                                          size_t vramWords,
                                          GpuVkSelftestRender render) {
  constexpr size_t requiredWords = static_cast<size_t>(kVramWidth) * kVramHeight;
  if (!vram || vramWords < requiredWords || !render) {
    lucent::error("gpu_selftest",
                  "textured PSX coverage: REFUSED — need {} VRAM words and a shipping render callback",
                  requiredWords);
    return false;
  }

  gpu.frame_end(nullptr, 0);
  gpu.ensure_targets();
  gpu.game->mods.ires = 1;
  memset(vram, 0, requiredWords * sizeof(*vram));

  vram[(kTextureY + kTexelV) * kVramWidth + kTextureX + kTexelU] = kTexel;
  vram[kEdgeProbeY * kVramWidth + kEdgeProbeX] = kBackground;
  vram[kInteriorProbeY * kVramWidth + kInteriorProbeX] = kBackground;

  int xs[3] = {kApexX, kEdgeProbeX, kFootX};
  int ys[3] = {kApexY, kEdgeProbeY, kFootY};
  int us[3] = {kTexelU, kTexelU, kTexelU};
  int vs[3] = {kTexelV, kTexelV, kTexelV};
  unsigned char neutral[3] = {128, 128, 128};
  gpu.set_order_2d(1);
  // raw=1: the texel is written verbatim, so the probe reads coverage and nothing else.
  gpu.draw_tritri(xs,
                  ys,
                  us,
                  vs,
                  neutral,
                  neutral,
                  neutral,
                  kTextureX,
                  kTextureY,
                  2,
                  1,
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  0,
                  kVramWidth - 1,
                  kVramHeight - 1);

  uint16_t edge = 0;
  uint16_t interior = 0;
  if (render(gpu, vram)) {
    edge = vram[kEdgeProbeY * kVramWidth + kEdgeProbeX] & 0x7FFFu;
    interior = vram[kInteriorProbeY * kVramWidth + kInteriorProbeX] & 0x7FFFu;
  }

  const bool ok = edge == kTexel && interior == kTexel;
  lucent::info("gpu_selftest",
               "textured PSX integer-pixel coverage: edge={:04X} interior={:04X} expect {:04X} "
               "(background {:04X}) => {}",
               edge,
               interior,
               kTexel,
               kBackground,
               ok ? "PASS" : "FAIL");
  return ok;
}
