// gpu_vk_semi_selftest.h — production-path verification of PSX semi-textured blend equations.
#ifndef PSXPORT_GPU_VK_SEMI_SELFTEST_H
#define PSXPORT_GPU_VK_SEMI_SELFTEST_H

#include "gpu_vk_selftest_support.h"
#include <stddef.h>

// The owner supplies the shipping Vulkan render/readback operation. The selftest owns the input
// matrix, emits through GpuVkState's real draw path, and checks the returned packed-1555 image.
bool gpu_vk_run_semi_selftest(GpuVkState &gpu, uint16_t *vram, size_t vramWords, GpuVkSelftestRender render);

#endif // PSXPORT_GPU_VK_SEMI_SELFTEST_H
