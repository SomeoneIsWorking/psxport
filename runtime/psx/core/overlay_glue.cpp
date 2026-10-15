// Integration glue between the SDL_GPU present path (gpu_vk.cpp) and the RmlUi overlay owned by
// Game (rmlui_overlay.cpp). Keeps all overlay-specific orchestration out of the renderer.
#include "overlay_glue.h"
#include "core.h"
#include "game.h"
#include "rmlui_overlay.h"

void psx::ui::initOverlay(
    Game *game, SDL_Window *win, SDL_GPUDevice *dev, SDL_GPUTextureFormat target_fmt, int sink_w, int sink_h) {
  if (game) {
    game->rml_overlay.init(win, dev, target_fmt, sink_w, sink_h);
  }
}

void psx::ui::beginOverlayFrame(Core *core) {
  if (core && core->game) {
    core->game->rml_overlay.newFrame();
  }
}

void psx::ui::recordOverlay(Game *game, SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int win_w, int win_h) {
  if (game) {
    game->rml_overlay.recordGpu(cmd, rp, win_w, win_h);
  }
}

void psx::ui::recordOverlayScreen(Game *game, SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int w, int h) {
  if (game) {
    game->rml_overlay.recordScreenGpu(cmd, rp, w, h);
  }
}
