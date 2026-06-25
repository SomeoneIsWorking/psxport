#ifndef PSXPORT_RMLUI_OVERLAY_H
#define PSXPORT_RMLUI_OVERLAY_H
// C bridge to the RmlUi mod-toggle overlay (implemented in rmlui_overlay.cpp). gpu_gpu.c drives it
// from the windowed present path; it edits g_mods (mods.h) + g_fps60_on live. No-op until init'd.
#include <SDL2/SDL.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
// Bring RmlUi up on the existing VK device + the present render pass. Call once after the swapchain and
// present render pass exist (windowed only). api_version = the VkApplicationInfo apiVersion used.
void rmlui_overlay_init(SDL_Window* win, VkInstance inst, VkPhysicalDevice phys, uint32_t qfam,
                        VkDevice dev, VkQueue queue, VkRenderPass present_rpass,
                        uint32_t min_image_count, uint32_t image_count);
void rmlui_overlay_shutdown(void);
void rmlui_overlay_event(const SDL_Event* e);   // feed every SDL event (also toggles visibility on `~`/F1)
void rmlui_overlay_new_frame(void);             // CPU: update the RmlUi context + sync the live mod state
// Record the menu geometry into `cmd` inside the present render pass. `frame_index` selects this frame's
// UBO ring; `viewport`/`full_scissor` should be the FULL window region (not the letterboxed game pane) so
// the menu covers the whole window. No-op when the menu is hidden / overlay not inited.
void rmlui_overlay_render_vk(VkCommandBuffer cmd, uint32_t frame_index,
                             VkViewport viewport, VkRect2D full_scissor);
int  rmlui_overlay_inited(void);
void rmlui_overlay_set_visible(int v);          // force show/hide (used when it replaces the game's Options menu)
// Options-mode: the overlay is currently standing in for the game's in-game Options menu. Suppresses the
// `~`/F1 toggle (the game owns exit via Circle/Triangle) and shows the in-game back/close hint instead.
void rmlui_overlay_set_options_mode(int v);
// Push the live world readout (camera/Tomba position + current stage) for the always-on corner HUD.
void rmlui_overlay_set_world(int x, int y, int z, unsigned stage);
#ifdef __cplusplus
}
#endif
#endif
