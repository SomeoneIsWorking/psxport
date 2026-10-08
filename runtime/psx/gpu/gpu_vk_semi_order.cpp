// gpu_vk_semi_order.cpp — ordered submission runs for world semi-transparent geometry.
#include "gpu_vk_semi_order.h"

bool gpu_vk_append_semi_run(GpuVkSemiRun *runs, int &runCount, int blend, int firstVertex) {
  if (runCount > 0) {
    GpuVkSemiRun &last = runs[runCount - 1];
    if (last.blend == blend && last.first + last.count == static_cast<uint32_t>(firstVertex)) {
      last.count += 3;
      return true;
    }
  }
  if (runCount == kGpuVkSemiRunCap) {
    return false;
  }
  runs[runCount++] = {.first = static_cast<uint32_t>(firstVertex), .count = 3, .blend = static_cast<uint8_t>(blend)};
  return true;
}
