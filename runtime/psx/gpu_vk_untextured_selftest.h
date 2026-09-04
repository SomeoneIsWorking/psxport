#ifndef PSXPORT_GPU_VK_UNTEXTURED_SELFTEST_H
#define PSXPORT_GPU_VK_UNTEXTURED_SELFTEST_H

#include <stdint.h>

struct GpuVkState;

void gpu_vk_stage_untextured_gouraud_selftest(GpuVkState &gpu);
bool gpu_vk_check_untextured_gouraud_selftest(const uint16_t *vram, int vramWidth);

#endif // PSXPORT_GPU_VK_UNTEXTURED_SELFTEST_H
