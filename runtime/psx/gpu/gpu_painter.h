#ifndef PSXPORT_GPU_PAINTER_H
#define PSXPORT_GPU_PAINTER_H

#include <stdint.h>

struct GpuVkState;

// Production-path discriminator for the authored untextured painter shader's PSX draw-area clip.
// The caller owns command submission/readback; this owner supplies the exact staged input and verdict.
void gpu_vk_painter_stage_draw_area_selftest(GpuVkState &gpu, uint16_t *vram, int vramWidth);
bool gpu_vk_painter_check_draw_area_selftest(const uint16_t *localColor, int scaledWidth, int scale);

#endif // PSXPORT_GPU_PAINTER_H
