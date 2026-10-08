#ifndef PSXPORT_OVERLAY_GLUE_H
#define PSXPORT_OVERLAY_GLUE_H
// Thin integration layer between the SDL_GPU present path (gpu_vk.cpp) and the RmlUi mod/debug
// overlay (rmlui_overlay.cpp). gpu_vk.cpp calls these three hooks and nothing more — all
// overlay-specific logic (full-window record, the live world-position latch read from guest RAM,
// the per-frame CPU update) lives HERE, not crammed into the renderer.
//
// Each hook takes the calling Game/Core so it can reach `game->rml_overlay` — one overlay per
// Game (see rmlui_overlay.h). All hooks are no-ops until the overlay is initialised.
//
// EVENTS ARE NOT HERE: they arrive through the host input owner (psx::input::HostInput), which is
// the one drain of the SDL event queue and already holds the overlay.
#include <SDL3/SDL.h>

class Core;
class Game;

// Bring the overlay up on the port's SDL_GPU device (call once per Game after the device exists).
// `win` may be NULL — the overlay EXISTS in both legs; the window is a sink, not a mode. `sink_w`/
// `sink_h` is the sink's measured size and `target_fmt` the colour format of the pass the overlay
// records into. See rmlui_overlay.h for why none of this is re-derived from the window.
void overlay_glue_init(
    Game *game, SDL_Window *win, SDL_GPUDevice *dev, SDL_GPUTextureFormat target_fmt, int sink_w, int sink_h);

// Per-frame CPU step: latch the live world readout from guest RAM (camera/Tomba pos + stage) for
// the menu's HUD line, then run the overlay's CPU update. Called from present() before recording.
void overlay_glue_frame_begin(Core *core);

// Record the menu geometry into the present render pass `rp` (its command buffer `cmd`). `win_w`/
// `win_h` = the FULL window pixel size (glue passes them through so the menu covers the whole
// window, not the letterboxed game pane). No-op if hidden.
void overlay_glue_record(Game *game, SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int win_w, int win_h);

// Record the choice SCREEN (a title picker) into the present-image render pass `rp`. Unlike the menu this
// is picture content, so it goes into the image a present shot reads. No-op when no screen is shown.
void overlay_glue_record_screen(Game *game, SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int w, int h);

#endif
