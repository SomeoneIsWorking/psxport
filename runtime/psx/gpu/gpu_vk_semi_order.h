// gpu_vk_semi_order.h — ordered submission runs for world semi-transparent geometry.
#ifndef GPU_VK_SEMI_ORDER_H
#define GPU_VK_SEMI_ORDER_H

#include <stdint.h>

struct GpuVkSemiRun {
  uint32_t first;
  uint32_t count;
  uint8_t blend;
};

constexpr int kGpuVkTextureVertexCapacity = 196608;
// Every run contains at least one triangle from one per-ABR vertex buffer.
constexpr int kGpuVkSemiRunCap = kGpuVkTextureVertexCapacity / 3;

// Appends one triangle to a contiguous same-ABR run, preserving source submission order.
bool gpu_vk_append_semi_run(GpuVkSemiRun *runs, int &runCount, int blend, int firstVertex);

#endif
