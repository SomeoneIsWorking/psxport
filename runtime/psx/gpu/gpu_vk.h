// gpu_vk.h — the Core-facing present entry points: each composes the calling Game's GpuVkState with the
// guest display state it presents.
#ifndef GPU_GPU_H
#define GPU_GPU_H

#include "gpu_device.h"

#include <cstdint>
#include <span>

class Core;

namespace psx::gpu {

// Resolution plan: native width, internal-resolution scale, framebuffer, sink and scale cap.
struct VideoStatus {
  int nativeWidth = 320;
  int ires = 1;
  int framebufferWidth = 320;
  int framebufferHeight = 240;
  int sinkWidth = 0;
  int sinkHeight = 0;
  int iresCap = 4;
};
VideoStatus videoStatus(Core &core);

// Builds the display rect in VRAM (sx, sy, w, h) into the present image: the record image on RenderPath::Record,
// the device's VRAM otherwise. GpuVkState::show puts it in the window.
void buildDisplay(Core &core, int sx, int sy, int w, int h);
// Re-show the last presented frame without building anything (debug-server pause loop).
void repaint(Core &core);
// The device's VRAM: a rect as an image, `n` raw words along a row, or the whole of it raw.
void vramRegion(Core &core, const char *path, VramRect rect);
void vramWords(Core &core, VramPoint at, std::span<uint16_t> out);
void vramRaw(Core &core, const char *path);
// Bring the device (and, windowed, the window and the overlay) up now rather than on the first present.
void ensureDevice(Core &core);

} // namespace psx::gpu

#endif // GPU_GPU_H
