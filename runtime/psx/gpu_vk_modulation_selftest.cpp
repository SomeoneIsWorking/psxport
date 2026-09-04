#include "gpu_vk_modulation_selftest.h"

#include "game.h"
#include <lucent/log.h>
#include <string.h>

namespace {

constexpr int kVramWidth = 1024;
constexpr int kVramHeight = 512;
constexpr int kTextureX = 512;
constexpr int kTextureY = 256;
// One dark texel in both PSX transparency forms: bit15 set selects the blend equation, bit15 clear
// writes opaquely. Both carry 5-bit (1,1,1) — the exact retail witness texel 0x8421.
constexpr int kSemiTexelU = 0;
constexpr int kOpaqueTexelU = 1;
constexpr unsigned char kModulationColor = 74;

// A PSX textured polygon computes (texel5 * color8) / 128 and TRUNCATES. For texel 1 and color 74
// that is zero in every channel, so a dark texel contributes nothing; round-to-nearest instead
// contributes one 5-bit step. Each case seeds a destination the shader must visibly change or
// visibly leave alone, so a case that never rasterized cannot be read as a pass.
struct ModulationCase {
  const char *name;
  int x;
  int y;
  int u;
  int blend;
  uint16_t background;
  uint16_t expected;
};

constexpr uint16_t kBackground = gpu_vk_selftest_pack555(1, 0, 3);

constexpr ModulationCase kCases[] = {
    // ABR1 additive, the retail witness: background survives untouched.
    {"semi additive", 40, 180, kSemiTexelU, 1, kBackground, kBackground},
    // ABR0 average: a zero contribution still halves the destination, which proves the draw fired.
    {"semi average", 100, 180, kSemiTexelU, 0, kBackground, gpu_vk_selftest_pack555(0, 0, 1)},
    // Opaque: the modulated texel is written verbatim, so the seeded background must go to black.
    {"opaque", 160, 180, kOpaqueTexelU, 1, kBackground, 0},
};

constexpr int kCaseCount = static_cast<int>(sizeof(kCases) / sizeof(kCases[0]));

void stageCase(GpuVkState &gpu, const ModulationCase &testCase, unsigned order) {
  int xs[3] = {testCase.x, testCase.x + 8, testCase.x};
  int ys[3] = {testCase.y, testCase.y, testCase.y + 8};
  int us[3] = {testCase.u, testCase.u, testCase.u};
  int vs[3] = {0, 0, 0};
  unsigned char color[3] = {kModulationColor, kModulationColor, kModulationColor};
  gpu.set_order_2d(order);
  gpu.draw_semi(xs,
                ys,
                us,
                vs,
                color,
                color,
                color,
                kTextureX,
                kTextureY,
                2,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                kVramWidth - 1,
                kVramHeight - 1,
                testCase.blend);
}

} // namespace

bool gpu_vk_run_modulation_selftest(GpuVkState &gpu, uint16_t *vram, size_t vramWords, GpuVkSelftestRender render) {
  constexpr size_t requiredWords = static_cast<size_t>(kVramWidth) * kVramHeight;
  if (!vram || vramWords < requiredWords || !render) {
    lucent::error("gpu_selftest",
                  "PSX texture modulation: REFUSED — need {} VRAM words and a shipping render callback",
                  requiredWords);
    return false;
  }

  // Exercise tritex.frag / trisemi_hw.frag through the shipping textured draw path, not a CPU copy.
  gpu.frame_end(nullptr, 0);
  gpu.ensure_targets();
  gpu.game->mods.ires = 1;
  memset(vram, 0, requiredWords * sizeof(*vram));

  const uint16_t darkTexel = gpu_vk_selftest_pack555(1, 1, 1);
  vram[kTextureY * kVramWidth + kTextureX + kSemiTexelU] = darkTexel | 0x8000u;
  vram[kTextureY * kVramWidth + kTextureX + kOpaqueTexelU] = darkTexel;

  unsigned order = 1;
  for (const ModulationCase &testCase : kCases) {
    vram[(testCase.y + 2) * kVramWidth + testCase.x + 2] = testCase.background;
    stageCase(gpu, testCase, order++);
  }

  int passed = 0;
  if (render(gpu, vram)) {
    for (const ModulationCase &testCase : kCases) {
      const uint16_t got = vram[(testCase.y + 2) * kVramWidth + testCase.x + 2] & 0x7FFFu;
      if (got == testCase.expected) {
        ++passed;
      } else {
        lucent::error("gpu_selftest",
                      "modulation {}: got {:04X}, expected {:04X} (background {:04X})",
                      testCase.name,
                      got,
                      testCase.expected,
                      testCase.background);
      }
    }
  }

  const bool ok = passed == kCaseCount;
  lucent::info("gpu_selftest",
               "PSX texture modulation truncation: {}/{} cases (additive/average/opaque dark texel) => {}",
               passed,
               kCaseCount,
               ok ? "PASS" : "FAIL");
  return ok;
}
