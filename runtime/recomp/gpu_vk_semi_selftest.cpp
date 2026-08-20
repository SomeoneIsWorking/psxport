#include "gpu_vk_semi_selftest.h"

#include "game.h"
#include <lucent/log.h>
#include <string.h>

namespace {

constexpr int kVramWidth = 1024;
constexpr int kVramHeight = 512;
constexpr int kTextureX = 512;
constexpr int kTextureY = 256;
constexpr int kCaseCount = 16;

struct SemiCase {
  int x;
  int y;
  int mode;
  bool stp;
  uint16_t background;
};

int saturate5(int value) {
  return value < 0 ? 0 : (value > 31 ? 31 : value);
}

uint16_t referenceBlend(const SemiCase &testCase, uint16_t foreground) {
  if (!testCase.stp) {
    return foreground & 0x7FFFu;
  }

  int output[3];
  for (int shift = 0, channel = 0; shift <= 10; shift += 5, ++channel) {
    const int backgroundChannel = (testCase.background >> shift) & 31;
    const int foregroundChannel = (foreground >> shift) & 31;
    switch (testCase.mode) {
    case 0:
      output[channel] = (backgroundChannel + foregroundChannel) >> 1;
      break;
    case 1:
      output[channel] = saturate5(backgroundChannel + foregroundChannel);
      break;
    case 2:
      output[channel] = saturate5(backgroundChannel - foregroundChannel);
      break;
    default:
      output[channel] = saturate5(backgroundChannel + (foregroundChannel >> 2));
      break;
    }
  }
  return gpu_vk_selftest_pack555(output[0], output[1], output[2]);
}

} // namespace

bool gpu_vk_run_semi_selftest(GpuVkState &gpu, uint16_t *vram, size_t vramWords, GpuVkSelftestRender render) {
  constexpr size_t requiredWords = static_cast<size_t>(kVramWidth) * kVramHeight;
  if (!vram || vramWords < requiredWords || !render) {
    lucent::error("gpu_selftest",
                  "semi textured PSX equations: REFUSED — need {} VRAM words and a shipping render callback",
                  requiredWords);
    return false;
  }

  // Exercise trisemi_hw.frag + the real fixed-function blend state, not a CPU copy of the shader.
  // Each ABR mode gets dark/bright destinations with STP=1 and STP=0. Odd source channels make
  // round-to-nearest implementations of the PSX's integer AVG/ADD_FOURTH equations fail visibly.
  gpu.frame_end(nullptr, 0);
  gpu.ensure_targets();
  gpu.game->mods.ires = 1;
  memset(vram, 0, requiredWords * sizeof(*vram));

  const uint16_t foreground = gpu_vk_selftest_pack555(23, 14, 7);
  const uint16_t backgrounds[2] = {gpu_vk_selftest_pack555(2, 4, 6), gpu_vk_selftest_pack555(24, 20, 16)};
  vram[kTextureY * kVramWidth + kTextureX] = foreground | 0x8000u;
  vram[kTextureY * kVramWidth + kTextureX + 1] = foreground;

  SemiCase cases[kCaseCount];
  int caseCount = 0;
  for (int mode = 0; mode < 4; ++mode) {
    for (int stp = 1; stp >= 0; --stp) {
      for (uint16_t background : backgrounds) {
        const int x = 8 + (caseCount % 8) * 38;
        const int y = 80 + (caseCount / 8) * 30;
        cases[caseCount++] = {x, y, mode, stp != 0, background};
        vram[(y + 2) * kVramWidth + x + 2] = background;
        int xs[3] = {x, x + 8, x};
        int ys[3] = {y, y, y + 8};
        const int u = stp ? 0 : 1;
        int us[3] = {u, u, u};
        int vs[3] = {0, 0, 0};
        unsigned char color[3] = {128, 128, 128};
        gpu.set_order_2d(static_cast<unsigned>(caseCount));
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
                      kVramHeight - 1,
                      mode);
      }
    }
  }

  int passed = 0;
  if (render(gpu, vram)) {
    for (const SemiCase &testCase : cases) {
      const uint16_t got = vram[(testCase.y + 2) * kVramWidth + testCase.x + 2] & 0x7FFFu;
      const uint16_t expected = referenceBlend(testCase, foreground);
      if (got == expected) {
        ++passed;
      } else {
        lucent::error("gpu_selftest",
                      "semi ABR{} STP={} bg={:04X}: got {:04X}, expected {:04X}",
                      testCase.mode,
                      testCase.stp ? 1 : 0,
                      testCase.background,
                      got,
                      expected);
      }
    }
  }

  const bool ok = passed == caseCount;
  lucent::info("gpu_selftest",
               "semi textured PSX equations: {}/{} cases (ABR0..3 x dark/bright x STP1/STP0) => {}",
               passed,
               caseCount,
               ok ? "PASS" : "FAIL");
  return ok;
}
