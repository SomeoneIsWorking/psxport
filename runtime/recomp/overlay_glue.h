#ifndef PSXPORT_OVERLAY_GLUE_H
#define PSXPORT_OVERLAY_GLUE_H
// Thin integration layer between the SDL_GPU present path (gpu_vk.cpp) and the RmlUi mod/debug
// overlay (rmlui_overlay.cpp). gpu_vk.cpp calls these four hooks and nothing more — all
// overlay-specific logic (full-window record, the live world-position latch read from guest RAM,
// the per-frame CPU update) lives HERE, not crammed into the renderer.
//
// Each hook takes the calling Game/Core so it can reach `game->rml_overlay` — one overlay per
// Game (see rmlui_overlay.h). All hooks are no-ops until the overlay is initialised (windowed only).
#include <SDL3/SDL.h>

class Core;
class Game;

// Bring the overlay up on the port's SDL_GPU device for the given swapchain format (call once
// per Game after the device + swapchain exist; windowed only).
void overlay_glue_init(Game* game, SDL_Window* win, SDL_GPUDevice* dev, SDL_GPUTextureFormat swap_fmt);

// Feed every SDL event (overlay mouse/keys; ESC toggles the menu). No-op if not inited.
void overlay_glue_event(Game* game, const SDL_Event* e);

// Per-frame CPU step: latch the live world readout from guest RAM (camera/Tomba pos + stage) for
// the menu's HUD line, then run the overlay's CPU update. Called from present() before recording.
void overlay_glue_frame_begin(Core* core);

// Record the menu geometry into the present render pass `rp` (its command buffer `cmd`). `win_w`/
// `win_h` = the FULL window pixel size (glue passes them through so the menu covers the whole
// window, not the letterboxed game pane). No-op if hidden.
void overlay_glue_record(Game* game, SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* rp, int win_w, int win_h);

#endif
