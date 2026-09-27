#include "fps60_gpu_present.h"

#include "core.h"
#include "game.h"
#include "gpu_native_internal.h"
#include "gpu_vk.h"

void gpu_fps60_present_pass(Core *core) {
  GpuState &gpu = core->game->gpu;
  gpu.present_window();
  gpu_vk_frame_end(core, gpu.s_vram, gpu.s_frame);
  gpu.s_prim_order = 0;
  // The in-between is a PRESENTATION that reached the screen, so it is counted here and only here —
  // this is the single place an interpolated frame is handed to the display, and `gpu_frame_no` does
  // not move for it. Without this, a product that presents interpolated frames and one that does not are
  // indistinguishable by any counter the control channel can read.
  gpu.s_interpolated_frames++;
}
