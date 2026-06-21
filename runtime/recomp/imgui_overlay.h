#ifndef PSXPORT_IMGUI_OVERLAY_H
#define PSXPORT_IMGUI_OVERLAY_H
// C bridge to the Dear ImGui mod-toggle overlay (implemented in imgui_overlay.cpp). gpu_vk.c drives it
// from the windowed present path; it edits g_mods (mods.h) + g_fps60_on live. No-op until init'd.
#include <SDL2/SDL.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
// Bring ImGui up on the existing VK device + the present render pass. Call once after the swapchain and
// present render pass exist (windowed only). api_version = the VkApplicationInfo apiVersion used.
void imgui_overlay_init(SDL_Window* win, VkInstance inst, VkPhysicalDevice phys, uint32_t qfam,
                        VkDevice dev, VkQueue queue, VkRenderPass present_rpass,
                        uint32_t min_image_count, uint32_t image_count);
void imgui_overlay_shutdown(void);
void imgui_overlay_event(const SDL_Event* e);   // feed every SDL event (also toggles visibility on `~`/F1)
void imgui_overlay_new_frame(void);             // CPU: SDL/VK NewFrame + build the toggle window
void imgui_overlay_render(VkCommandBuffer cmd); // record the draw data into cmd (inside the present rpass)
int  imgui_overlay_inited(void);
void imgui_overlay_set_visible(int v);          // force show/hide (used when it replaces the game's Options menu)
// Options-mode: the overlay is currently standing in for the game's in-game Options menu. Suppresses the
// `~`/F1 toggle (the game owns exit via Circle/Triangle) and shows the in-game back/close hint instead.
void imgui_overlay_set_options_mode(int v);
// Push the live world readout (camera/Tomba position + current stage) for the always-on corner HUD.
void imgui_overlay_set_world(int x, int y, int z, unsigned stage);
#ifdef __cplusplus
}
#endif
#endif
