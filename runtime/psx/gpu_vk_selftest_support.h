// Shared input/output contract for production-path Vulkan GPU selftests.
#ifndef PSXPORT_GPU_VK_SELFTEST_SUPPORT_H
#define PSXPORT_GPU_VK_SELFTEST_SUPPORT_H

#include <stdint.h>

struct GpuVkState;

using GpuVkSelftestRender = bool (*)(GpuVkState &gpu, uint16_t *vram);

constexpr uint16_t gpu_vk_selftest_pack555(int r, int g, int b) {
  return static_cast<uint16_t>(r | (g << 5) | (b << 10));
}

#endif // PSXPORT_GPU_VK_SELFTEST_SUPPORT_H
