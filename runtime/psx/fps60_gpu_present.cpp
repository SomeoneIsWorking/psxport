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
}
