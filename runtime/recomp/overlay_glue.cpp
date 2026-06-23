// Integration glue between the Vulkan present path (gpu_vk.cpp) and the RmlUi overlay
// (rmlui_overlay.cpp). Keeps all overlay-specific orchestration out of the renderer — see
// overlay_glue.h. Built as C++ (reads guest RAM via Core); the public hooks have C linkage.
#include "overlay_glue.h"
#include "rmlui_overlay.h"
#include "core.h"

void overlay_glue_init(SDL_Window* win, VkInstance inst, VkPhysicalDevice phys, uint32_t qfam,
                       VkDevice dev, VkQueue queue, VkRenderPass present_rpass,
                       uint32_t min_image_count, uint32_t image_count) {
    rmlui_overlay_init(win, inst, phys, qfam, dev, queue, present_rpass, min_image_count, image_count);
}

void overlay_glue_event(const SDL_Event* e) {
    rmlui_overlay_event(e);
}

void overlay_glue_frame_begin(Core* core) {
    // Live world-position HUD: camera/Tomba position (int16 world units in scratchpad) + the current
    // stage entry pointer. These guest addresses are the same ones the engine RE established for the
    // coordinate readout; reading them here keeps gpu_vk.cpp free of overlay concerns.
    if (core) {
        rmlui_overlay_set_world((int16_t)core->mem_r16(0x1F8000D2u),
                                (int16_t)core->mem_r16(0x1F8000D6u),
                                (int16_t)core->mem_r16(0x1F8000DAu),
                                core->mem_r32(0x801FE00Cu));
    }
    rmlui_overlay_new_frame();
}

void overlay_glue_record(VkCommandBuffer cmd, uint32_t frame_index, VkExtent2D extent) {
    // The menu must cover the FULL window (not the letterboxed game-pane viewport the present pass set
    // for the framebuffer blit), so build a full-window viewport + scissor from the swapchain extent.
    VkViewport full_vp = { 0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f };
    VkRect2D full_sc = { {0, 0}, extent };
    rmlui_overlay_render_vk(cmd, frame_index, full_vp, full_sc);
}
