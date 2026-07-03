// Integration glue between the SDL_GPU present path (gpu_gpu.cpp) and the RmlUi overlay owned by
// Game (rmlui_overlay.cpp). Keeps all overlay-specific orchestration out of the renderer.
#include "overlay_glue.h"
#include "rmlui_overlay.h"
#include "sbs.h"
#include "core.h"
#include "game.h"

void overlay_glue_init(Game* game, SDL_Window* win, SDL_GPUDevice* dev, SDL_GPUTextureFormat swap_fmt) {
    if (game) game->rml_overlay.init(win, dev, swap_fmt);
}

void overlay_glue_event(Game* game, const SDL_Event* e) {
    if (game) game->rml_overlay.event(e);
}

void overlay_glue_frame_begin(Core* core) {
    // Live world-position HUD: camera/Tomba position (int16 world units in scratchpad) + the
    // current stage entry pointer. These guest addresses are the same ones the engine RE
    // established for the coordinate readout; reading them here keeps gpu_gpu.cpp free of overlay
    // concerns.
    //
    // In SBS mode both cores hit this per frame; the overlay is one host UI, so we push the world
    // coords from ONE core — the `sbs show`-selected one — rather than let A and B overwrite each
    // other's readout each frame. Standalone: core is the sole core, push unconditionally.
    if (core && core->game) {
        Core* shown = core->game->sbs ? core->game->sbs->shownCore() : nullptr;
        if (!shown || shown == core) {
            core->game->rml_overlay.setWorld(
                (int16_t)core->mem_r16(0x1F8000D2u),
                (int16_t)core->mem_r16(0x1F8000D6u),
                (int16_t)core->mem_r16(0x1F8000DAu),
                core->mem_r32(0x801FE00Cu));
        }
        core->game->rml_overlay.newFrame();
    }
}

void overlay_glue_record(Game* game, SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* rp, int win_w, int win_h) {
    if (game) game->rml_overlay.recordGpu(cmd, rp, win_w, win_h);
}
