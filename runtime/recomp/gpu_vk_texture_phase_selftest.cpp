#include "gpu_vk_texture_phase_selftest.h"

#include "game.h"
#include <lucent/log.h>
#include <string.h>

namespace {

constexpr int kVramWidth = 1024;
constexpr int kVramHeight = 512;
constexpr int kTextureX = 512;
constexpr int kTextureY = 256;
constexpr int kCaseCount = 5;
constexpr int kPathCount = 2;

struct TexturePhaseCase {
  const char *name;
  int x;
  int y;
  int uDx;
  int uDy;
  int vDx;
  int vDy;
  int sampleDx;
  int sampleDy;
};

uint16_t texturePhasePattern(int u, int v) {
  return gpu_vk_selftest_pack555(1 + u % 30, 1 + v % 30, 1 + (u + 3 * v) % 30);
}

} // namespace

bool gpu_vk_run_texture_phase_selftest(GpuVkState &gpu, uint16_t *vram, size_t vramWords, GpuVkSelftestRender render) {
  constexpr size_t requiredWords = static_cast<size_t>(kVramWidth) * kVramHeight;
  if (!vram || vramWords < requiredWords || !render) {
    lucent::error("gpu_selftest",
                  "textured PSX UV phase: REFUSED — need {} VRAM words and a shipping render callback",
                  requiredWords);
    return false;
  }

  // The PSX interpolates UV at integer pixel coordinates. SDL_GPU supplies the affine varying at the
  // fragment centre instead. Positive unit slopes happened to survive int() truncation; negative slopes
  // sampled the preceding texel, which shifted Tomba! 2's vertically mirrored health segment by one row.
  // Each expected texel below is evaluated at the integer display pixel, not at +0.5.
  constexpr int kExtent = 16;
  constexpr int kBaseU = 96;
  constexpr int kBaseV = 112;
  const TexturePhaseCase cases[kCaseCount] = {
      {"positive X", 16, 40, 16, 0, 0, 0, 3, 3},
      {"negative X", 64, 40, -16, 0, 0, 0, 3, 3},
      {"positive Y", 112, 40, 0, 0, 0, 16, 3, 3},
      {"negative Y", 160, 40, 0, 0, 0, -16, 3, 3},
      {"mixed non-unit", 208, 40, -8, 4, 4, -8, 4, 4},
  };

  int passed = 0;
  for (int scale : {1, 3}) {
    gpu.frame_end(nullptr, 0);
    gpu.ensure_targets();
    gpu.game->mods.ires = scale;
    memset(vram, 0, requiredWords * sizeof(*vram));
    for (int v = 64; v <= 128; ++v) {
      for (int u = 64; u <= 128; ++u) {
        vram[(kTextureY + v) * kVramWidth + kTextureX + u] = texturePhasePattern(u, v);
      }
    }

    unsigned order = 1;
    unsigned char neutral[3] = {128, 128, 128};
    for (const TexturePhaseCase &testCase : cases) {
      const int expectedU = kBaseU + (testCase.uDx * testCase.sampleDx + testCase.uDy * testCase.sampleDy) / kExtent;
      const int expectedV = kBaseV + (testCase.vDx * testCase.sampleDx + testCase.vDy * testCase.sampleDy) / kExtent;
      auto drawCase = [&](int yOffset, bool semi, bool constantUv) {
        int xs[3] = {testCase.x, testCase.x + kExtent, testCase.x};
        int ys[3] = {testCase.y + yOffset, testCase.y + yOffset, testCase.y + yOffset + kExtent};
        int us[3] = {kBaseU, kBaseU + testCase.uDx, kBaseU + testCase.uDy};
        int vs[3] = {kBaseV, kBaseV + testCase.vDx, kBaseV + testCase.vDy};
        if (constantUv) {
          for (int vertex = 0; vertex < 3; ++vertex) {
            us[vertex] = expectedU;
            vs[vertex] = expectedV;
          }
        }
        gpu.set_order_2d(order++);
        if (semi) {
          gpu.draw_semi(xs,
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
                        kVramHeight - 1,
                        0);
        } else {
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
        }
      };
      drawCase(0, false, false);
      drawCase(32, false, true);
      drawCase(72, true, false);
      drawCase(104, true, true);
    }

    if (render(gpu, vram)) {
      for (const TexturePhaseCase &testCase : cases) {
        const int expectedU = kBaseU + (testCase.uDx * testCase.sampleDx + testCase.uDy * testCase.sampleDy) / kExtent;
        const int expectedV = kBaseV + (testCase.vDx * testCase.sampleDx + testCase.vDy * testCase.sampleDy) / kExtent;
        for (int path = 0; path < kPathCount; ++path) {
          const int phaseYOffset = path == 0 ? 0 : 72;
          const int controlYOffset = path == 0 ? 32 : 104;
          const int sampleX = testCase.x + testCase.sampleDx;
          const int sampleY = testCase.y + testCase.sampleDy;
          const uint16_t got = vram[(sampleY + phaseYOffset) * kVramWidth + sampleX] & 0x7FFFu;
          const uint16_t control = vram[(sampleY + controlYOffset) * kVramWidth + sampleX] & 0x7FFFu;
          if (control != 0 && got == control) {
            ++passed;
          } else {
            lucent::error("gpu_selftest",
                          "texture UV phase {} {} ires={}: got {:04X}, constant-UV control {:04X} at uv=({}, {})",
                          path == 0 ? "opaque" : "semi/STP0",
                          testCase.name,
                          scale,
                          got,
                          control,
                          expectedU,
                          expectedV);
          }
        }
      }
    }
  }

  const int totalCases = kCaseCount * kPathCount * 2;
  const bool ok = passed == totalCases;
  lucent::info("gpu_selftest",
               "textured PSX UV phase: {}/{} cases (ires 1/3 x opaque/semi x +/-X, +/-Y, mixed non-unit) => {}",
               passed,
               totalCases,
               ok ? "PASS" : "FAIL");
  return ok;
}
