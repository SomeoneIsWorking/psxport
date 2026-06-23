#ifndef PSXPORT_OVERLAY_GLUE_H
#define PSXPORT_OVERLAY_GLUE_H
// Thin integration layer between the Vulkan present path (gpu_vk.cpp) and the RmlUi mod/debug overlay
// (rmlui_overlay.cpp). gpu_vk.cpp calls these four hooks and nothing more — all overlay-specific logic
// (full-window viewport build for the menu, the live world-position latch read from guest RAM, the
// per-frame CPU update, and the present-pass record) lives HERE, not crammed into the renderer.
//
// Implemented in C++ (reads Core via core.h); exposed with C linkage so gpu_vk.cpp's call sites are
// trivial. All hooks are no-ops until the overlay is initialised (windowed only).
#include <SDL2/SDL.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
class Core;
extern "C" {
#endif

// Bring the overlay up on the port's existing VK device + present render pass (call once, after the
// swapchain + present render pass exist; windowed only). Mirrors rmlui_overlay_init's parameters.
void overlay_glue_init(SDL_Window* win, VkInstance inst, VkPhysicalDevice phys, uint32_t qfam,
                       VkDevice dev, VkQueue queue, VkRenderPass present_rpass,
                       uint32_t min_image_count, uint32_t image_count);

// Feed every SDL event (overlay mouse/keys; ESC toggles the menu). No-op if not inited.
void overlay_glue_event(const SDL_Event* e);

#ifdef __cplusplus
} // extern "C"

// Per-frame CPU step: latch the live world readout from guest RAM (camera/Tomba pos + stage) for the
// menu's HUD line, then run the overlay's CPU update. Called from GpuVkState::present before recording.
// (C++-only: takes a Core*.)
void overlay_glue_frame_begin(Core* core);

extern "C" {
#endif

// Record the menu geometry into the present-pass command buffer `cmd`. `frame_index` selects the
// per-frame UBO ring; `extent` is the FULL swapchain extent (the glue builds a full-window viewport +
// scissor from it so the menu covers the whole window, not the letterboxed game pane). No-op if hidden.
void overlay_glue_record(VkCommandBuffer cmd, uint32_t frame_index, VkExtent2D extent);

#ifdef __cplusplus
}
#endif
#endif
