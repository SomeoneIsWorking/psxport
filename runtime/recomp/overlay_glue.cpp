// Integration glue between the SDL_GPU present path (gpu_gpu.cpp) and the RmlUi overlay
// (rmlui_overlay.cpp). Keeps all overlay-specific orchestration out of the renderer — see
// overlay_glue.h. Built as C++ (reads guest RAM via Core); the public hooks have C linkage.
#include "overlay_glue.h"
#include "rmlui_overlay.h"
#include "core.h"

void overlay_glue_init(SDL_Window* win, SDL_GPUDevice* dev, SDL_GPUTextureFormat swap_fmt) {
    rmlui_overlay_init(win, dev, swap_fmt);
}

void overlay_glue_event(const SDL_Event* e) {
    rmlui_overlay_event(e);
}

void overlay_glue_frame_begin(Core* core) {
    // Live world-position HUD: camera/Tomba position (int16 world units in scratchpad) + the current
    // stage entry pointer. These guest addresses are the same ones the engine RE established for the
    // coordinate readout; reading them here keeps gpu_gpu.cpp free of overlay concerns.
    if (core) {
        rmlui_overlay_set_world((int16_t)core->mem_r16(0x1F8000D2u),
                                (int16_t)core->mem_r16(0x1F8000D6u),
                                (int16_t)core->mem_r16(0x1F8000DAu),
                                core->mem_r32(0x801FE00Cu));
    }
    rmlui_overlay_new_frame();
}

void overlay_glue_record(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* rp, int win_w, int win_h) {
    rmlui_overlay_record_gpu(cmd, rp, win_w, win_h);
}
