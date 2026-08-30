// gpu_vk_texture_coverage_selftest.h — production-path verification of textured PSX pixel coverage.
#ifndef PSXPORT_GPU_VK_TEXTURE_COVERAGE_SELFTEST_H
#define PSXPORT_GPU_VK_TEXTURE_COVERAGE_SELFTEST_H

#include "gpu_vk_selftest_support.h"
#include <stddef.h>

// The owner supplies the shipping Vulkan render/readback operation. The selftest owns the narrow
// triangle whose vertex sits exactly on a PSX integer pixel coordinate, emits it through the real
// textured draw path, and checks that the shipping raster covers that pixel as the PSX does.
bool gpu_vk_run_texture_coverage_selftest(GpuVkState &gpu,
                                          uint16_t *vram,
                                          size_t vramWords,
                                          GpuVkSelftestRender render);

#endif // PSXPORT_GPU_VK_TEXTURE_COVERAGE_SELFTEST_H
