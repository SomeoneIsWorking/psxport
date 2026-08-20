// Production-path verification of PSX integer-pixel texture interpolation.
#ifndef PSXPORT_GPU_VK_TEXTURE_PHASE_SELFTEST_H
#define PSXPORT_GPU_VK_TEXTURE_PHASE_SELFTEST_H

#include "gpu_vk_selftest_support.h"
#include <stddef.h>

bool gpu_vk_run_texture_phase_selftest(GpuVkState &gpu, uint16_t *vram, size_t vramWords, GpuVkSelftestRender render);

#endif // PSXPORT_GPU_VK_TEXTURE_PHASE_SELFTEST_H
