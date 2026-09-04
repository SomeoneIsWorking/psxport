// gpu_vk_modulation_selftest.h — production-path verification of PSX texture*color modulation.
#ifndef PSXPORT_GPU_VK_MODULATION_SELFTEST_H
#define PSXPORT_GPU_VK_MODULATION_SELFTEST_H

#include "gpu_vk_selftest_support.h"
#include <stddef.h>

// The owner supplies the shipping Vulkan render/readback operation. The selftest owns the dark-texel
// input pair, emits through GpuVkState's real textured draw path, and checks the returned packed-1555
// image against the PSX's truncating modulation.
bool gpu_vk_run_modulation_selftest(GpuVkState &gpu, uint16_t *vram, size_t vramWords, GpuVkSelftestRender render);

#endif // PSXPORT_GPU_VK_MODULATION_SELFTEST_H
