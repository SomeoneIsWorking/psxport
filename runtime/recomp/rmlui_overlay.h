#ifndef PSXPORT_RMLUI_OVERLAY_H
#define PSXPORT_RMLUI_OVERLAY_H
// C bridge to the RmlUi mod-toggle overlay (implemented in rmlui_overlay.cpp). gpu_gpu.cpp drives it
// from the windowed present path; it edits g_mods (mods.h) + g_fps60_on live. No-op until init'd.
#include <SDL3/SDL.h>
#ifdef __cplusplus
extern "C" {
#endif
// Bring RmlUi up on the port's existing SDL_GPU device, for the given swapchain colour format. Call once
// after the device + swapchain exist (windowed only).
void rmlui_overlay_init(SDL_Window* win, SDL_GPUDevice* dev, SDL_GPUTextureFormat swap_fmt);
void rmlui_overlay_shutdown(void);
void rmlui_overlay_event(const SDL_Event* e);   // feed every SDL event (ESC toggles the menu; F1 debugger)
void rmlui_overlay_new_frame(void);             // CPU: update the RmlUi context + sync the live mod state
// Record the menu geometry into `rp` (the present render pass). `cmd` is its command buffer; `win_w`/
// `win_h` are the FULL window pixel size so the menu covers the whole window (not the letterboxed game
// pane). No-op when the menu is hidden / overlay not inited.
void rmlui_overlay_record_gpu(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* rp, int win_w, int win_h);
int  rmlui_overlay_inited(void);
void rmlui_overlay_set_visible(int v);          // force show/hide (used when it replaces the game's Options menu)
// Options-mode: the overlay is currently standing in for the game's in-game Options menu. Suppresses the
// ESC toggle (the game owns exit via Circle/Triangle) and shows the in-game back/close hint instead.
void rmlui_overlay_set_options_mode(int v);
// Push the live world readout (camera/Tomba position + current stage) for the always-on corner HUD.
void rmlui_overlay_set_world(int x, int y, int z, unsigned stage);
#ifdef __cplusplus
}
#endif
#endif
