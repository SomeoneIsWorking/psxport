// Dear ImGui mod-toggle overlay. Brings ImGui up on the port's existing Vulkan device + present render
// pass and draws a small window that edits the LIVE mod state (g_mods, mods.h) and 60fps (g_fps60_on).
// Toggle visibility with `~` (grave) or F1. C-callable bridge in imgui_overlay.h; built as C++ and
// linked into the otherwise-C port (build_port.sh / run.sh compile .cpp with $CXX and link with $CXX).
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui_overlay.h"
extern "C" {
#include "mods.h"
}
extern "C" int g_fps60_on;   // engine/fps60.c — the 60fps interpolation gate
extern "C" int cfg_on(const char* name);   // cfg.c — env flag (PSXPORT_UI gates the SSAO/light infra)
// Effective (computed, incl. auto) video status from the renderer; out ptrs may be NULL.
extern "C" void gpu_vk_video_status(int* native_w, int* ires, int* fbw, int* fbh,
                                    int* ww, int* wh, int* ires_cap);

static bool            s_inited  = false;
static bool            s_visible = true;
static bool            s_options_mode = false;   // true while we stand in for the game's in-game Options menu
static VkDevice        s_dev     = VK_NULL_HANDLE;
static VkDescriptorPool s_pool   = VK_NULL_HANDLE;

static void check_vk(VkResult r) { (void)r; }

void imgui_overlay_init(SDL_Window* win, VkInstance inst, VkPhysicalDevice phys, uint32_t qfam,
                        VkDevice dev, VkQueue queue, VkRenderPass present_rpass,
                        uint32_t min_image_count, uint32_t image_count) {
  if (s_inited) return;
  s_dev = dev;
  // A small descriptor pool for ImGui (font atlas + any user textures).
  VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 };
  VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
  dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  dpi.maxSets = 16; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
  if (vkCreateDescriptorPool(dev, &dpi, nullptr, &s_pool) != VK_SUCCESS) {
    fprintf(stderr, "[imgui] descriptor pool create failed; overlay disabled\n");
    return;
  }
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;   // don't write imgui.ini next to the binary
  ImGui::StyleColorsDark();
  ImGui_ImplSDL2_InitForVulkan(win);
  ImGui_ImplVulkan_InitInfo ii = {};
  ii.Instance = inst; ii.PhysicalDevice = phys; ii.Device = dev;
  ii.QueueFamily = qfam; ii.Queue = queue; ii.DescriptorPool = s_pool;
  ii.RenderPass = present_rpass; ii.Subpass = 0;
  ii.MinImageCount = min_image_count < 2 ? 2 : min_image_count;
  ii.ImageCount = image_count < ii.MinImageCount ? ii.MinImageCount : image_count;
  ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  ii.CheckVkResultFn = check_vk;
  ImGui_ImplVulkan_Init(&ii);
  s_inited = true;
  fprintf(stderr, "[imgui] overlay up (toggle with ` or F1)\n");
}

void imgui_overlay_shutdown(void) {
  if (!s_inited) return;
  vkDeviceWaitIdle(s_dev);
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  if (s_pool) vkDestroyDescriptorPool(s_dev, s_pool, nullptr);
  s_inited = false;
}

void imgui_overlay_event(const SDL_Event* e) {
  if (!s_inited || !e) return;
  ImGui_ImplSDL2_ProcessEvent(e);
  // In options-mode the game owns visibility (Circle/Triangle exit) — don't let `~`/F1 fight it.
  if (!s_options_mode && e->type == SDL_KEYDOWN &&
      (e->key.keysym.scancode == SDL_SCANCODE_GRAVE || e->key.keysym.scancode == SDL_SCANCODE_F1))
    s_visible = !s_visible;
}

void imgui_overlay_set_visible(int v) { s_visible = (v != 0); }
void imgui_overlay_set_options_mode(int v) { s_options_mode = (v != 0); }

static void build_ui(void) {
  ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::Begin("Tomba2Engine - PC-native mods");   // ASCII only (default font has no em-dash glyph)
  ImGui::TextDisabled(s_options_mode ? "Circle: back    Triangle: close" : "` or F1 to hide");
  ImGui::PushItemWidth(120.0f);   // keep sliders compact so their labels aren't clipped

  const char* aspects[] = { "4:3", "16:9", "21:9", "Auto (window)" };
  int am = g_mods.aspect;
  if (ImGui::Combo("Aspect", &am, aspects, 4)) { g_mods.aspect = am; mods_save(); }

  int nw = 320, ir = 1, fbw = 320, fbh = 240, ww = 0, wh = 0, cap = 3;
  gpu_vk_video_status(&nw, &ir, &fbw, &fbh, &ww, &wh, &cap);
  bool ia = g_mods.ires_auto != 0;
  if (ImGui::Checkbox("Auto internal res", &ia)) { g_mods.ires_auto = ia; mods_save(); }
  if (g_mods.ires_auto) {
    ImGui::SameLine(); ImGui::TextDisabled("(%dx)", ir);    // computed from window size
  } else {
    int iv = g_mods.ires;
    if (ImGui::SliderInt("Internal res", &iv, 1, cap < 1 ? 1 : cap)) {
      g_mods.ires = iv < 1 ? 1 : (iv > 3 ? 3 : iv); mods_save(); }
  }
  ImGui::TextDisabled("Render %dx%d  |  window %dx%d", fbw, fbh, ww, wh);

  bool fps60 = g_fps60_on != 0;
  if (ImGui::Checkbox("60fps interpolation", &fps60)) { g_fps60_on = fps60; mods_save(); }

  ImGui::Separator();
  // Native per-pixel depth is always on, so SSAO/light toggle LIVE — no launch flag (the deferred infra
  // is always created). Changing any setting persists it (mods_save).
  bool ssao = g_mods.ssao != 0;
  if (ImGui::Checkbox("Ambient occlusion (SSAO)", &ssao)) { g_mods.ssao = ssao; mods_save(); }
  if (g_mods.ssao) {
    if (ImGui::SliderFloat("AO strength", &g_mods.ssao_strength, 0.0f, 2.0f)) mods_save();
    if (ImGui::SliderFloat("AO radius (px)", &g_mods.ssao_radius, 1.0f, 20.0f)) mods_save();
    if (ImGui::SliderFloat("AO bias", &g_mods.ssao_bias, 0.0f, 0.1f, "%.3f")) mods_save();
    if (ImGui::SliderFloat("AO range", &g_mods.ssao_range, 0.02f, 0.6f, "%.3f")) mods_save();
  }

  ImGui::Separator();
  bool light = g_mods.light != 0;
  if (ImGui::Checkbox("Directional light", &light)) { g_mods.light = light; mods_save(); }
  if (g_mods.light) {
    if (ImGui::SliderFloat3("Light dir (view)", g_mods.light_dir, -1.0f, 1.0f)) mods_save();
    if (ImGui::SliderFloat("Ambient", &g_mods.light_ambient, 0.0f, 1.5f)) mods_save();
    if (ImGui::SliderFloat("Diffuse", &g_mods.light_diffuse, 0.0f, 1.5f)) mods_save();
  }
  ImGui::PopItemWidth();
  ImGui::End();
}

void imgui_overlay_new_frame(void) {
  if (!s_inited) return;
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
  if (s_visible) build_ui();
  ImGui::Render();
}

void imgui_overlay_render(VkCommandBuffer cmd) {
  if (!s_inited) return;
  ImDrawData* dd = ImGui::GetDrawData();
  if (dd) ImGui_ImplVulkan_RenderDrawData(dd, cmd);
}

int imgui_overlay_inited(void) { return s_inited ? 1 : 0; }
