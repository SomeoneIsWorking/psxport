// gpu_vk.c — Vulkan present backend (M0) for the Tomba2Engine port.
//
// M0 scope: take over PRESENTATION via Vulkan (SDL_Vulkan swapchain) — the software rasterizer in
// gpu_native.c still produces s_vram; here we upload the display region to a texture and draw it as a
// fullscreen quad (letterboxed 4:3), replacing the SDL_Renderer blit. This proves the VK device /
// swapchain / pipeline path end-to-end with the game running. Later milestones move rasterization
// itself onto the GPU (VRAM as a device image, triangle/sprite pipelines).
//
// Enabled by PSXPORT_VK=1 (and only when a window is on). Headless (PSXPORT_NOWINDOW) never touches VK.
// Cross-platform: MoltenVK-ready (portability enumeration/subset added only when the loader reports it).
#ifdef PSXPORT_SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "gpu_vk_shaders.h"   // generated: spv_present_vert / spv_present_frag (tools/gen_vk_shaders.sh)

#define VRAM_W 1024
#define VRAM_H 512
#define MAX_SWAP 8

#define VKC(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "[gpu_vk] %s failed: VkResult %d (%s:%d)\n", #x, _r, __FILE__, __LINE__); exit(2); } } while (0)

static int s_vk_on = -1;
static int s_inited = 0;
static int s_failed = 0;

static SDL_Window*     s_win;
static VkInstance      s_inst;
static VkSurfaceKHR    s_surf;
static VkPhysicalDevice s_phys;
static VkDevice        s_dev;
static uint32_t        s_qfam;
static VkQueue         s_queue;

static VkSwapchainKHR  s_swap;
static VkFormat        s_swap_fmt;
static VkExtent2D      s_extent;
static uint32_t        s_swap_n;
static VkImage         s_swap_img[MAX_SWAP];
static VkImageView     s_swap_view[MAX_SWAP];
static VkFramebuffer   s_swap_fb[MAX_SWAP];

static VkRenderPass    s_rpass;
static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout s_pll;
static VkPipeline      s_pipe;
static VkDescriptorPool s_dpool;
static VkDescriptorSet s_dset;
static VkSampler       s_sampler;
static VkCommandPool   s_cpool;
static VkCommandBuffer s_cmd;
static VkSemaphore     s_sem_acq, s_sem_rel[MAX_SWAP];   // release: one per swapchain image (spec-correct reuse)
static VkFence         s_fence;

// GPU VRAM image (R16_UINT 1024x512) + host-visible staging (upload) + readback
static int             s_tex_undef;
static VkImage         s_tex;
static VkDeviceMemory  s_tex_mem;
static VkImageView     s_tex_view;
static VkBuffer        s_stage;
static VkDeviceMemory  s_stage_mem;
static void*           s_stage_ptr;
static VkDeviceSize    s_stage_sz;
static VkBuffer        s_rb;          // readback buffer (VRAM image -> host, for VK-vs-SW diff)
static VkDeviceMemory  s_rb_mem;
static void*           s_rb_ptr;

// M2 triangle rasterizer: render pass over the VRAM image + pipeline + batched vertex buffer
static VkRenderPass    s_vram_rpass;
static VkFramebuffer   s_vram_fb;
static VkPipeline      s_tri_pipe;
static VkBuffer        s_vbuf;        // host-visible vertex batch
static VkDeviceMemory  s_vbuf_mem;
static void*           s_vbuf_ptr;
typedef struct { float x, y, r, g, b; } TriVtx;
#define TRI_CAP 196608                // max batched vertices (= 65536 tris)
static int             s_tri_n;

// M3 textured rasterizer: a VRAM snapshot image the texture sampler reads (avoids render/sample feedback
// loop), its descriptor set, the textured pipeline, and a textured-vertex batch.
typedef struct { float x, y, u, v, r, g, b; int32_t tp[4], clut[4], tw[4], da[4]; } TexVtx;  // 92 bytes
#define TEX_CAP 196608
static VkImage         s_vram_tex;    // texture-source snapshot (copy of VRAM before a draw batch)
static VkDeviceMemory  s_vram_tex_mem;
static VkImageView     s_vram_tex_view;
static int             s_vram_tex_undef;
static VkDescriptorSet s_dset_tex;    // binding0 = s_vram_tex
static VkPipeline      s_tritex_pipe;
static VkBuffer        s_tvbuf;
static VkDeviceMemory  s_tvbuf_mem;
static void*           s_tvbuf_ptr;
static int             s_tex_n;
static VkBuffer        s_semibuf;     // SEMI-transparent prims (drawn after opaque, sampling the framebuffer)
static VkDeviceMemory  s_semibuf_mem;
static void*           s_semibuf_ptr;
static int             s_semi_n;
// Dirty VRAM regions written by SW this frame (CPU->VRAM uploads, VRAM copies, fills) — mirrored from
// s_vram into the PERSISTENT VK VRAM image at present (the framebuffer region stays VK-owned/persistent).
typedef struct { int x, y, w, h; } VkRect;
#define DIRTY_CAP 4096
static VkRect s_dirty[DIRTY_CAP];
static int    s_dirty_n;
void gpu_vk_dirty(int x, int y, int w, int h) {
  if (s_dirty_n >= DIRTY_CAP || w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
  if (x + w > VRAM_W) w = VRAM_W - x; if (y + h > VRAM_H) h = VRAM_H - y;
  if (w <= 0 || h <= 0) return;
  s_dirty[s_dirty_n++] = (VkRect){ x, y, w, h };
}

int gpu_vk_enabled(void) {
  if (s_vk_on < 0) {
    // VK is the DEFAULT renderer for windowed runs. PSXPORT_SW_GPU=1 (or PSXPORT_VK=0) forces the SW
    // rasterizer (the oracle / fallback). Headless (no window) always stays SW.
    const char* w = getenv("PSXPORT_GPU_WINDOW");
    const char* sw = getenv("PSXPORT_SW_GPU");
    const char* v = getenv("PSXPORT_VK");
    int win = w && atoi(w) != 0;
    int want = (sw && atoi(sw) != 0) ? 0 : (v ? (atoi(v) != 0) : 1);   // default on; SW_GPU/VK=0 disables
    s_vk_on = (win && want) ? 1 : 0;
  }
  return s_vk_on && !s_failed;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(s_phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  fprintf(stderr, "[gpu_vk] no memory type for 0x%x want 0x%x\n", bits, want); exit(2);
}

static int has_ext(const VkExtensionProperties* a, uint32_t n, const char* name) {
  for (uint32_t i = 0; i < n; i++) if (!strcmp(a[i].extensionName, name)) return 1;
  return 0;
}

static VkShaderModule make_shader(const uint32_t* code, unsigned len) {
  VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
  ci.codeSize = len; ci.pCode = code;
  VkShaderModule m; VKC(vkCreateShaderModule(s_dev, &ci, 0, &m)); return m;
}

static void create_vram(void);   // forward decl: defined after init_vk, called from it
static void img_barrier_on(VkImage, VkImageLayout, VkImageLayout, VkPipelineStageFlags,
                           VkPipelineStageFlags, VkAccessFlags, VkAccessFlags);

static void create_swapchain(void) {
  VkSurfaceCapabilitiesKHR caps;
  VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_phys, s_surf, &caps));
  s_extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent
           : (VkExtent2D){ 1280, 720 };
  uint32_t fn = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surf, &fn, 0);
  VkSurfaceFormatKHR fmts[32]; if (fn > 32) fn = 32;
  vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surf, &fn, fmts);
  VkSurfaceFormatKHR pick = fmts[0];
  for (uint32_t i = 0; i < fn; i++)
    if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = fmts[i]; break; }
  s_swap_fmt = pick.format;
  uint32_t want = caps.minImageCount + 1;
  if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;

  VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
  ci.surface = s_surf; ci.minImageCount = want; ci.imageFormat = pick.format;
  ci.imageColorSpace = pick.colorSpace; ci.imageExtent = s_extent; ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // always supported
  ci.clipped = VK_TRUE;
  VKC(vkCreateSwapchainKHR(s_dev, &ci, 0, &s_swap));

  vkGetSwapchainImagesKHR(s_dev, s_swap, &s_swap_n, 0);
  if (s_swap_n > MAX_SWAP) s_swap_n = MAX_SWAP;
  vkGetSwapchainImagesKHR(s_dev, s_swap, &s_swap_n, s_swap_img);
  for (uint32_t i = 0; i < s_swap_n; i++) {
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = s_swap_img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = s_swap_fmt;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VKC(vkCreateImageView(s_dev, &vi, 0, &s_swap_view[i]));
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = s_rpass; fi.attachmentCount = 1; fi.pAttachments = &s_swap_view[i];
    fi.width = s_extent.width; fi.height = s_extent.height; fi.layers = 1;
    VKC(vkCreateFramebuffer(s_dev, &fi, 0, &s_swap_fb[i]));
  }
}

static void destroy_swapchain(void) {
  for (uint32_t i = 0; i < s_swap_n; i++) {
    vkDestroyFramebuffer(s_dev, s_swap_fb[i], 0);
    vkDestroyImageView(s_dev, s_swap_view[i], 0);
  }
  vkDestroySwapchainKHR(s_dev, s_swap, 0);
}

static void recreate_swapchain(void) {
  vkDeviceWaitIdle(s_dev);
  destroy_swapchain();
  create_swapchain();
}

static void init_vk(void) {
  s_inited = 1;
  SDL_Init(SDL_INIT_VIDEO);
  int windowed = getenv("PSXPORT_WINDOWED") && atoi(getenv("PSXPORT_WINDOWED")) != 0;
  Uint32 flags = SDL_WINDOW_VULKAN | (windowed ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN_DESKTOP);
  s_win = SDL_CreateWindow("Tomba! 2 (Vulkan)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           960, 720, flags);
  if (!s_win) { fprintf(stderr, "[gpu_vk] SDL_CreateWindow(VULKAN) failed: %s\n", SDL_GetError()); exit(2); }

  // instance extensions: SDL surface exts (+ portability enumeration if the loader has it)
  unsigned ext_n = 0; SDL_Vulkan_GetInstanceExtensions(s_win, &ext_n, 0);
  const char* exts[16]; if (ext_n > 12) ext_n = 12;
  SDL_Vulkan_GetInstanceExtensions(s_win, &ext_n, exts);
  uint32_t avail_n = 0; vkEnumerateInstanceExtensionProperties(0, &avail_n, 0);
  VkExtensionProperties* avail = malloc(sizeof *avail * avail_n);
  vkEnumerateInstanceExtensionProperties(0, &avail_n, avail);
  VkInstanceCreateFlags inst_flags = 0;
  if (has_ext(avail, avail_n, "VK_KHR_portability_enumeration")) {
    exts[ext_n++] = "VK_KHR_portability_enumeration";
    inst_flags |= 0x00000001; /* VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR */
  }
  free(avail);

  VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
  app.pApplicationName = "Tomba2Engine"; app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
  ici.flags = inst_flags; ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = ext_n; ici.ppEnabledExtensionNames = exts;
  VKC(vkCreateInstance(&ici, 0, &s_inst));

  if (!SDL_Vulkan_CreateSurface(s_win, s_inst, &s_surf)) {
    fprintf(stderr, "[gpu_vk] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError()); exit(2);
  }

  // pick a physical device with a graphics+present queue family + swapchain extension
  uint32_t pn = 0; vkEnumeratePhysicalDevices(s_inst, &pn, 0);
  VkPhysicalDevice* phys = malloc(sizeof *phys * pn);
  vkEnumeratePhysicalDevices(s_inst, &pn, phys);
  int dev_portability = 0;
  s_phys = VK_NULL_HANDLE;
  for (uint32_t i = 0; i < pn && s_phys == VK_NULL_HANDLE; i++) {
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, 0);
    VkQueueFamilyProperties* qf = malloc(sizeof *qf * qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, qf);
    for (uint32_t q = 0; q < qn; q++) {
      VkBool32 present = 0; vkGetPhysicalDeviceSurfaceSupportKHR(phys[i], q, s_surf, &present);
      if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
        s_phys = phys[i]; s_qfam = q; break;
      }
    }
    free(qf);
  }
  free(phys);
  if (s_phys == VK_NULL_HANDLE) { fprintf(stderr, "[gpu_vk] no suitable GPU\n"); exit(2); }

  uint32_t den = 0; vkEnumerateDeviceExtensionProperties(s_phys, 0, &den, 0);
  VkExtensionProperties* de = malloc(sizeof *de * den);
  vkEnumerateDeviceExtensionProperties(s_phys, 0, &den, de);
  dev_portability = has_ext(de, den, "VK_KHR_portability_subset");
  free(de);

  const char* dexts[2]; uint32_t dext_n = 0;
  dexts[dext_n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  if (dev_portability) dexts[dext_n++] = "VK_KHR_portability_subset";

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
  qci.queueFamilyIndex = s_qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = dext_n; dci.ppEnabledExtensionNames = dexts;
  VKC(vkCreateDevice(s_phys, &dci, 0, &s_dev));
  vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);

  // render pass (single color attachment: clear -> present)
  VkAttachmentDescription at = {0};
  // format set after we know the swapchain format; create swapchain format first:
  // (we need the format before the render pass; query it here)
  { uint32_t fn = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surf, &fn, 0);
    VkSurfaceFormatKHR fmts[32]; if (fn > 32) fn = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surf, &fn, fmts);
    s_swap_fmt = fmts[0].format;
    for (uint32_t i = 0; i < fn; i++) if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { s_swap_fmt = fmts[i].format; break; } }
  at.format = s_swap_fmt; at.samples = VK_SAMPLE_COUNT_1_BIT;
  at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
  VkSubpassDescription sp = {0}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
  VkSubpassDependency dep = {0};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
  rpi.attachmentCount = 1; rpi.pAttachments = &at; rpi.subpassCount = 1; rpi.pSubpasses = &sp;
  rpi.dependencyCount = 1; rpi.pDependencies = &dep;
  VKC(vkCreateRenderPass(s_dev, &rpi, 0, &s_rpass));

  // descriptor set layout: binding 0 = combined image sampler (fragment)
  VkDescriptorSetLayoutBinding b = {0};
  b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
  dli.bindingCount = 1; dli.pBindings = &b;
  VKC(vkCreateDescriptorSetLayout(s_dev, &dli, 0, &s_dsl));
  VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };   // ivec4 display rect (x,y,w,h)
  VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
  pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
  pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
  VKC(vkCreatePipelineLayout(s_dev, &pli, 0, &s_pll));

  // graphics pipeline: fullscreen triangle, no vertex input, dynamic viewport/scissor
  VkShaderModule vs = make_shader(spv_present_vert, spv_present_vert_len);
  VkShaderModule fs = make_shader(spv_present_frag, spv_present_frag_len);
  VkPipelineShaderStageCreateInfo st[2] = {
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", 0 },
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", 0 },
  };
  VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
  VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba = {0}; cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
  cb.attachmentCount = 1; cb.pAttachments = &cba;
  VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
  ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
  gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb; gp.pDynamicState = &ds; gp.layout = s_pll; gp.renderPass = s_rpass;
  VKC(vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, 0, &s_pipe));
  vkDestroyShaderModule(s_dev, vs, 0); vkDestroyShaderModule(s_dev, fs, 0);

  // sampler + descriptor pool + set
  VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;   // R16_UINT VRAM: integer texture (no linear filter)
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VKC(vkCreateSampler(s_dev, &si, 0, &s_sampler));
  VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
  VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
  dpi.maxSets = 2; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
  VKC(vkCreateDescriptorPool(s_dev, &dpi, 0, &s_dpool));
  VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
  dai.descriptorPool = s_dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_dsl;
  VKC(vkAllocateDescriptorSets(s_dev, &dai, &s_dset));        // binding0 = VRAM image (present)
  VKC(vkAllocateDescriptorSets(s_dev, &dai, &s_dset_tex));    // binding0 = VRAM snapshot (textured sampling)

  // command pool + buffer, sync
  VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = s_qfam;
  VKC(vkCreateCommandPool(s_dev, &cpi, 0, &s_cpool));
  VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
  cai.commandPool = s_cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
  VKC(vkAllocateCommandBuffers(s_dev, &cai, &s_cmd));
  VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
  VKC(vkCreateSemaphore(s_dev, &sci, 0, &s_sem_acq));
  for (int i = 0; i < MAX_SWAP; i++) VKC(vkCreateSemaphore(s_dev, &sci, 0, &s_sem_rel[i]));
  VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, VK_FENCE_CREATE_SIGNALED_BIT };
  VKC(vkCreateFence(s_dev, &fci, 0, &s_fence));

  create_vram();
  void create_tri_pipeline(void); create_tri_pipeline();
  create_swapchain();
  fprintf(stderr, "[gpu_vk] Vulkan present backend up (%ux%u, %u swap images; VRAM 1024x512 R16_UINT)\n",
          s_extent.width, s_extent.height, s_swap_n);
}

// The GPU VRAM image: R16_UINT 1024x512 = PSX VRAM (1555), created once. Render target for the GPU
// rasterizer (M2+), sampled by the present pass, transfer src/dst for upload/readback.
static void create_vram(void) {
  s_tex_undef = 1;
  VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R16_UINT;
  ii.extent = (VkExtent3D){ VRAM_W, VRAM_H, 1 }; ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VKC(vkCreateImage(s_dev, &ii, 0, &s_tex));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(s_dev, s_tex, &mr);
  VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKC(vkAllocateMemory(s_dev, &ma, 0, &s_tex_mem));
  VKC(vkBindImageMemory(s_dev, s_tex, s_tex_mem, 0));
  VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
  vi.image = s_tex; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R16_UINT;
  vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  VKC(vkCreateImageView(s_dev, &vi, 0, &s_tex_view));

  s_stage_sz = (VkDeviceSize)VRAM_W * VRAM_H * 2;   // uint16 per texel
  VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
  bi.size = s_stage_sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VKC(vkCreateBuffer(s_dev, &bi, 0, &s_stage));
  VkMemoryRequirements br; vkGetBufferMemoryRequirements(s_dev, s_stage, &br);
  VkMemoryAllocateInfo bm = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  bm.allocationSize = br.size; bm.memoryTypeIndex = mem_type(br.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VKC(vkAllocateMemory(s_dev, &bm, 0, &s_stage_mem));
  VKC(vkBindBufferMemory(s_dev, s_stage, s_stage_mem, 0));
  VKC(vkMapMemory(s_dev, s_stage_mem, 0, s_stage_sz, 0, &s_stage_ptr));

  VkDescriptorImageInfo di = { s_sampler, s_tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
  wr.dstSet = s_dset; wr.dstBinding = 0; wr.descriptorCount = 1;
  wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &di;
  vkUpdateDescriptorSets(s_dev, 1, &wr, 0, 0);

  // VRAM snapshot image (texture source for the textured pipeline; copied from VRAM before a batch).
  VkImageCreateInfo ti = ii; ti.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  VKC(vkCreateImage(s_dev, &ti, 0, &s_vram_tex));
  VkMemoryRequirements tr; vkGetImageMemoryRequirements(s_dev, s_vram_tex, &tr);
  VkMemoryAllocateInfo tm = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  tm.allocationSize = tr.size; tm.memoryTypeIndex = mem_type(tr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKC(vkAllocateMemory(s_dev, &tm, 0, &s_vram_tex_mem));
  VKC(vkBindImageMemory(s_dev, s_vram_tex, s_vram_tex_mem, 0));
  VkImageViewCreateInfo tv = vi; tv.image = s_vram_tex;
  VKC(vkCreateImageView(s_dev, &tv, 0, &s_vram_tex_view));
  s_vram_tex_undef = 1;
  VkDescriptorImageInfo tdi = { s_sampler, s_vram_tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  VkWriteDescriptorSet twr = wr; twr.dstSet = s_dset_tex; twr.pImageInfo = &tdi;
  vkUpdateDescriptorSets(s_dev, 1, &twr, 0, 0);
}

static void img_barrier(VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                        VkAccessFlags sa, VkAccessFlags da) {
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.oldLayout = from; b.newLayout = to; b.image = s_tex;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  b.srcAccessMask = sa; b.dstAccessMask = da;
  vkCmdPipelineBarrier(s_cmd, ss, ds, 0, 0, 0, 0, 0, 1, &b);
}

static void poll_quit(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) exit(0);
    if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) exit(0);
  }
}

// Present the display region [sx,sy .. +w,h] of `src` (s_vram or s_interp) via Vulkan, fit to 4:3.
void gpu_vk_present(const uint16_t* src, int sx, int sy, int w, int h) {
  if (!gpu_vk_enabled()) return;
  if (!s_inited) init_vk();

  // Mirror the whole CPU VRAM (s_vram/s_interp) into the GPU R16_UINT image (M1: SW still rasterizes;
  // M2+ will draw into this image directly and skip the upload for drawn regions). The display region
  // [sx,sy,w,h] is selected at sample time via the present push constant.
  memcpy(s_stage_ptr, src, (size_t)VRAM_W * VRAM_H * 2);

  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
  uint32_t idx;
  VkResult acq = vkAcquireNextImageKHR(s_dev, s_swap, UINT64_MAX, s_sem_acq, VK_NULL_HANDLE, &idx);
  if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return; }
  if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "[gpu_vk] acquire %d\n", acq); exit(2); }
  VKC(vkResetFences(s_dev, 1, &s_fence));

  VKC(vkResetCommandBuffer(s_cmd, 0));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC(vkBeginCommandBuffer(s_cmd, &bi));

  img_barrier(s_tex_undef ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkBufferImageCopy bc = {0};
  bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  bc.imageExtent = (VkExtent3D){ VRAM_W, VRAM_H, 1 };
  if (s_tex_undef) {                          // first frame: initialize the whole VK VRAM from s_vram
    vkCmdCopyBufferToImage(s_cmd, s_stage, s_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
  } else {                                    // mirror only SW-written regions (uploads/copies/fills);
    for (int d = 0; d < s_dirty_n; d++) {     // the framebuffer stays VK-owned/persistent
      VkBufferImageCopy r = {0};
      r.bufferOffset = ((VkDeviceSize)s_dirty[d].y * VRAM_W + s_dirty[d].x) * 2;
      r.bufferRowLength = VRAM_W;
      r.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      r.imageOffset = (VkOffset3D){ s_dirty[d].x, s_dirty[d].y, 0 };
      r.imageExtent = (VkExtent3D){ s_dirty[d].w, s_dirty[d].h, 1 };
      vkCmdCopyBufferToImage(s_cmd, s_stage, s_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
  }
  s_tex_undef = 0;
  // M5: render this frame's tee'd geometry into VK VRAM (over the uploaded SW VRAM = textures/bg),
  // then present from VK VRAM. Textured prims sample a snapshot (s_vram_tex) of the uploaded VRAM.
  VkRenderPassBeginInfo grp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  grp.renderPass = s_vram_rpass; grp.framebuffer = s_vram_fb; grp.renderArea.extent = (VkExtent2D){ VRAM_W, VRAM_H };
  VkViewport gv = { 0, 0, VRAM_W, VRAM_H, 0.0f, 1.0f }; VkRect2D gs = { {0,0}, { VRAM_W, VRAM_H } };
  VkDeviceSize go = 0;
  VkImageCopy ic = { { VK_IMAGE_ASPECT_COLOR_BIT,0,0,1 }, {0,0,0}, { VK_IMAGE_ASPECT_COLOR_BIT,0,0,1 }, {0,0,0}, { VRAM_W, VRAM_H, 1 } };
  if (s_tri_n || s_tex_n || s_semi_n) {
    // snapshot the uploaded VRAM -> vram_tex (the textures) for the opaque pass
    img_barrier_on(s_vram_tex, s_vram_tex_undef ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    s_vram_tex_undef = 0;
    vkCmdCopyBufferToImage(s_cmd, s_stage, s_vram_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    img_barrier_on(s_vram_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    // OPAQUE pass
    vkCmdBeginRenderPass(s_cmd, &grp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(s_cmd, 0, 1, &gv); vkCmdSetScissor(s_cmd, 0, 1, &gs);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pll, 0, 1, &s_dset_tex, 0, 0);
    if (s_tri_n) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tri_pipe);
                   vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_vbuf, &go); vkCmdDraw(s_cmd, s_tri_n, 1, 0, 0); }
    if (s_tex_n) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tritex_pipe);
                   vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_tvbuf, &go); vkCmdDraw(s_cmd, s_tex_n, 1, 0, 0); }
    vkCmdEndRenderPass(s_cmd);
    if (s_semi_n) {
      // snapshot the post-opaque framebuffer (+ atlas) -> vram_tex: semi prims sample it for BOTH the
      // texture and the blend destination.
      img_barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      img_barrier_on(s_vram_tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      vkCmdCopyImage(s_cmd, s_tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_vram_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
      img_barrier_on(s_vram_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
      img_barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
      vkCmdBeginRenderPass(s_cmd, &grp, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdSetViewport(s_cmd, 0, 1, &gv); vkCmdSetScissor(s_cmd, 0, 1, &gs);
      vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pll, 0, 1, &s_dset_tex, 0, 0);
      vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tritex_pipe);
      vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_semibuf, &go); vkCmdDraw(s_cmd, s_semi_n, 1, 0, 0);
      vkCmdEndRenderPass(s_cmd);
    }
    img_barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  } else {
    img_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  }

  VkClearValue clear = {{{0, 0, 0, 1}}};
  VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  rp.renderPass = s_rpass; rp.framebuffer = s_swap_fb[idx];
  rp.renderArea.extent = s_extent; rp.clearValueCount = 1; rp.pClearValues = &clear;
  vkCmdBeginRenderPass(s_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

  // letterbox/pillarbox to a 4:3 rect within the swapchain extent (never stretch)
  int ow = s_extent.width, oh = s_extent.height, dw, dh;
  if (ow * 3 >= oh * 4) { dh = oh; dw = oh * 4 / 3; } else { dw = ow; dh = ow * 3 / 4; }
  VkViewport vpt = { (float)((ow - dw) / 2), (float)((oh - dh) / 2), (float)dw, (float)dh, 0.0f, 1.0f };
  VkRect2D sc = { {0, 0}, s_extent };
  vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
  vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
  vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pll, 0, 1, &s_dset, 0, 0);
  int32_t disp[4] = { sx, sy, w, h };   // VRAM display region the present pass samples
  vkCmdPushConstants(s_cmd, s_pll, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, disp);
  vkCmdDraw(s_cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(s_cmd);
  VKC(vkEndCommandBuffer(s_cmd));

  VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo su = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  su.waitSemaphoreCount = 1; su.pWaitSemaphores = &s_sem_acq; su.pWaitDstStageMask = &wait;
  su.commandBufferCount = 1; su.pCommandBuffers = &s_cmd;
  su.signalSemaphoreCount = 1; su.pSignalSemaphores = &s_sem_rel[idx];
  VKC(vkQueueSubmit(s_queue, 1, &su, s_fence));

  VkPresentInfoKHR pr = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
  pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &s_sem_rel[idx];
  pr.swapchainCount = 1; pr.pSwapchains = &s_swap; pr.pImageIndices = &idx;
  VkResult pres = vkQueuePresentKHR(s_queue, &pr);
  if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) recreate_swapchain();
  else if (pres != VK_SUCCESS) { fprintf(stderr, "[gpu_vk] present %d\n", pres); exit(2); }

  poll_quit();
}

// ---- M2: GPU triangle rasterizer (flat/gouraud) into the VRAM image --------------------
static void make_hostbuf(VkDeviceSize sz, VkBufferUsageFlags use, VkBuffer* buf, VkDeviceMemory* mem, void** ptr) {
  VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
  bi.size = sz; bi.usage = use; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VKC(vkCreateBuffer(s_dev, &bi, 0, buf));
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_dev, *buf, &mr);
  VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(mr.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VKC(vkAllocateMemory(s_dev, &ma, 0, mem));
  VKC(vkBindBufferMemory(s_dev, *buf, *mem, 0));
  VKC(vkMapMemory(s_dev, *mem, 0, sz, 0, ptr));
}

void create_tri_pipeline(void) {
  // render pass over the VRAM image: LOAD existing contents, draw, store; stays COLOR_ATTACHMENT layout.
  VkAttachmentDescription at = {0};
  at.format = VK_FORMAT_R16_UINT; at.samples = VK_SAMPLE_COUNT_1_BIT;
  at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  at.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  at.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
  VkSubpassDescription sp = {0}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
  VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
  rpi.attachmentCount = 1; rpi.pAttachments = &at; rpi.subpassCount = 1; rpi.pSubpasses = &sp;
  VKC(vkCreateRenderPass(s_dev, &rpi, 0, &s_vram_rpass));

  VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
  fi.renderPass = s_vram_rpass; fi.attachmentCount = 1; fi.pAttachments = &s_tex_view;
  fi.width = VRAM_W; fi.height = VRAM_H; fi.layers = 1;
  VKC(vkCreateFramebuffer(s_dev, &fi, 0, &s_vram_fb));

  VkShaderModule vs = make_shader(spv_tri_vert, spv_tri_vert_len);
  VkShaderModule fs = make_shader(spv_tri_frag, spv_tri_frag_len);
  VkPipelineShaderStageCreateInfo st[2] = {
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", 0 },
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", 0 },
  };
  VkVertexInputBindingDescription vbd = { 0, sizeof(TriVtx), VK_VERTEX_INPUT_RATE_VERTEX };
  VkVertexInputAttributeDescription vad[2] = {
    { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },            // pos (VRAM coords)
    { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8 },         // rgb 0..1
  };
  VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
  vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vbd;
  vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = vad;
  VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba = {0}; cba.colorWriteMask = 0xF;   // R16_UINT: opaque (no blend)
  VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
  cb.attachmentCount = 1; cb.pAttachments = &cba;
  VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
  ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
  gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb; gp.pDynamicState = &ds; gp.layout = s_pll; gp.renderPass = s_vram_rpass;
  VKC(vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, 0, &s_tri_pipe));
  vkDestroyShaderModule(s_dev, vs, 0); vkDestroyShaderModule(s_dev, fs, 0);

  make_hostbuf(sizeof(TriVtx) * TRI_CAP, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s_vbuf, &s_vbuf_mem, &s_vbuf_ptr);
  make_hostbuf((VkDeviceSize)VRAM_W * VRAM_H * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s_rb, &s_rb_mem, &s_rb_ptr);

  // textured pipeline (TexVtx input, samples the VRAM snapshot, renders to the VRAM render pass)
  VkShaderModule tvs = make_shader(spv_tritex_vert, spv_tritex_vert_len);
  VkShaderModule tfs = make_shader(spv_tritex_frag, spv_tritex_frag_len);
  VkPipelineShaderStageCreateInfo tst[2] = {
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, tvs, "main", 0 },
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, tfs, "main", 0 },
  };
  VkVertexInputBindingDescription tvbd = { 0, sizeof(TexVtx), VK_VERTEX_INPUT_RATE_VERTEX };
  VkVertexInputAttributeDescription tvad[7] = {
    { 0, 0, VK_FORMAT_R32G32_SFLOAT,       0 },   // pos
    { 1, 0, VK_FORMAT_R32G32_SFLOAT,       8 },   // uv
    { 2, 0, VK_FORMAT_R32G32B32_SFLOAT,    16 },  // col
    { 3, 0, VK_FORMAT_R32G32B32A32_SINT,   28 },  // tp (tpx,tpy,mode,raw)
    { 4, 0, VK_FORMAT_R32G32B32A32_SINT,   44 },  // clut (clutx,cluty,-,-)
    { 5, 0, VK_FORMAT_R32G32B32A32_SINT,   60 },  // tw (mask_x,mask_y,off_x,off_y)
    { 6, 0, VK_FORMAT_R32G32B32A32_SINT,   76 },  // da (x0,y0,x1,y1)
  };
  VkPipelineVertexInputStateCreateInfo tvi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
  tvi.vertexBindingDescriptionCount = 1; tvi.pVertexBindingDescriptions = &tvbd;
  tvi.vertexAttributeDescriptionCount = 7; tvi.pVertexAttributeDescriptions = tvad;
  gp.pStages = tst; gp.pVertexInputState = &tvi;   // reuse ia/vp/rs/ms/cb/ds/layout/renderPass from above
  VKC(vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, 0, &s_tritex_pipe));
  vkDestroyShaderModule(s_dev, tvs, 0); vkDestroyShaderModule(s_dev, tfs, 0);
  make_hostbuf(sizeof(TexVtx) * TEX_CAP, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s_tvbuf, &s_tvbuf_mem, &s_tvbuf_ptr);
  make_hostbuf(sizeof(TexVtx) * TEX_CAP, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s_semibuf, &s_semibuf_mem, &s_semibuf_ptr);
}

// Append one triangle (VRAM coords + per-vertex RGB 0..255) to the batch.
void gpu_vk_draw_tri(int x0,int y0,int r0,int g0,int b0, int x1,int y1,int r1,int g1,int b1,
                     int x2,int y2,int r2,int g2,int b2) {
  if (!s_inited || s_tri_n + 3 > TRI_CAP) return;
  TriVtx* v = (TriVtx*)s_vbuf_ptr + s_tri_n;
  v[0] = (TriVtx){ x0, y0, r0/255.f, g0/255.f, b0/255.f };
  v[1] = (TriVtx){ x1, y1, r1/255.f, g1/255.f, b1/255.f };
  v[2] = (TriVtx){ x2, y2, r2/255.f, g2/255.f, b2/255.f };
  s_tri_n += 3;
}

// Append one TEXTURED triangle: per-vertex pos/uv/color (rgb 0..255) + shared page/CLUT/mode/raw state.
static void tex_emit(TexVtx* t, const int* xs, const int* ys, const int* us, const int* vs,
                     const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                     int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                     int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1,
                     int semi, int blend) {
  for (int i = 0; i < 3; i++) {
    t[i].x = xs[i]; t[i].y = ys[i]; t[i].u = us[i]; t[i].v = vs[i];
    t[i].r = rs[i]/255.f; t[i].g = gs[i]/255.f; t[i].b = bs[i]/255.f;
    t[i].tp[0] = tpx; t[i].tp[1] = tpy; t[i].tp[2] = mode; t[i].tp[3] = raw;
    t[i].clut[0] = clutx; t[i].clut[1] = cluty; t[i].clut[2] = semi; t[i].clut[3] = blend;
    t[i].tw[0] = twmx; t[i].tw[1] = twmy; t[i].tw[2] = twox; t[i].tw[3] = twoy;
    t[i].da[0] = dax0; t[i].da[1] = day0; t[i].da[2] = dax1; t[i].da[3] = day1;
  }
}
void gpu_vk_draw_tritri(const int* xs, const int* ys, const int* us, const int* vs,
                        const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                        int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                        int twmx, int twmy, int twox, int twoy,
                        int dax0, int day0, int dax1, int day1) {
  if (!s_inited || s_tex_n + 3 > TEX_CAP) return;
  tex_emit((TexVtx*)s_tvbuf_ptr + s_tex_n, xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
           twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 0, 0);
  s_tex_n += 3;
}
// Semi-transparent triangle (mode 3 = untextured flat). Drawn AFTER opaque, blending against the
// framebuffer snapshot, per `blend` (0=avg,1=add,2=sub,3=add/4).
void gpu_vk_draw_semi(const int* xs, const int* ys, const int* us, const int* vs,
                      const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                      int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                      int twmx, int twmy, int twox, int twoy,
                      int dax0, int day0, int dax1, int day1, int blend) {
  if (!s_inited || s_semi_n + 3 > TEX_CAP) return;
  tex_emit((TexVtx*)s_semibuf_ptr + s_semi_n, xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
           twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 1, blend);
  s_semi_n += 3;
}

// Self-test: clear VRAM, draw batched tris into it, read back. Returns the readback (uint16 VRAM).
static void tri_render_and_readback(uint16_t* out) {
  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
  VKC(vkResetFences(s_dev, 1, &s_fence));
  VKC(vkResetCommandBuffer(s_cmd, 0));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC(vkBeginCommandBuffer(s_cmd, &bi));

  img_barrier(s_tex_undef ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  s_tex_undef = 0;
  VkClearColorValue ccv = { .uint32 = {0,0,0,0} };
  VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  vkCmdClearColorImage(s_cmd, s_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &rng);
  img_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

  VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  rp.renderPass = s_vram_rpass; rp.framebuffer = s_vram_fb;
  rp.renderArea.extent = (VkExtent2D){ VRAM_W, VRAM_H };
  vkCmdBeginRenderPass(s_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vpt = { 0, 0, VRAM_W, VRAM_H, 0.0f, 1.0f };
  VkRect2D sc = { {0,0}, { VRAM_W, VRAM_H } };
  vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
  vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tri_pipe);
  VkDeviceSize off = 0; vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_vbuf, &off);
  if (s_tri_n) vkCmdDraw(s_cmd, s_tri_n, 1, 0, 0);
  vkCmdEndRenderPass(s_cmd);

  img_barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy bc = {0};
  bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  bc.imageExtent = (VkExtent3D){ VRAM_W, VRAM_H, 1 };
  vkCmdCopyImageToBuffer(s_cmd, s_tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_rb, 1, &bc);
  // leave it TRANSFER_SRC; next present's barrier expects SHADER_READ — mark undef so it re-barriers.
  s_tex_undef = 1;

  VKC(vkEndCommandBuffer(s_cmd));
  VkSubmitInfo su = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  su.commandBufferCount = 1; su.pCommandBuffers = &s_cmd;
  VKC(vkQueueSubmit(s_queue, 1, &su, s_fence));
  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
  memcpy(out, s_rb_ptr, (size_t)VRAM_W * VRAM_H * 2);
}

static void img_barrier_on(VkImage im, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss,
                           VkPipelineStageFlags ds, VkAccessFlags sa, VkAccessFlags da) {
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.oldLayout = from; b.newLayout = to; b.image = im;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  b.srcAccessMask = sa; b.dstAccessMask = da;
  vkCmdPipelineBarrier(s_cmd, ss, ds, 0, 0, 0, 0, 0, 1, &b);
}

// Render the batched tris (untextured + textured) on top of `bg` (the SW VRAM) and read the result back.
// Textured prims sample a SNAPSHOT of `bg` (s_vram_tex) — same textures SW used — so where VK's
// rasterization/sampling matches SW, out==bg; mismatches reveal rule deltas. Avoids render/sample loop.
static void tri_over_bg_readback(const uint16_t* bg, uint16_t* out) {
  memcpy(s_stage_ptr, bg, (size_t)VRAM_W * VRAM_H * 2);
  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
  VKC(vkResetFences(s_dev, 1, &s_fence));
  VKC(vkResetCommandBuffer(s_cmd, 0));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC(vkBeginCommandBuffer(s_cmd, &bi));
  VkBufferImageCopy bc = {0};
  bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  bc.imageExtent = (VkExtent3D){ VRAM_W, VRAM_H, 1 };

  // upload bg into the render target
  img_barrier(s_tex_undef ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  s_tex_undef = 0;
  vkCmdCopyBufferToImage(s_cmd, s_stage, s_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
  img_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  // snapshot bg into the texture-source image (sampled by the textured pipeline)
  img_barrier_on(s_vram_tex, s_vram_tex_undef ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  s_vram_tex_undef = 0;
  vkCmdCopyBufferToImage(s_cmd, s_stage, s_vram_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
  img_barrier_on(s_vram_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

  VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  rp.renderPass = s_vram_rpass; rp.framebuffer = s_vram_fb; rp.renderArea.extent = (VkExtent2D){ VRAM_W, VRAM_H };
  vkCmdBeginRenderPass(s_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vpt = { 0, 0, VRAM_W, VRAM_H, 0.0f, 1.0f }; VkRect2D sc = { {0,0}, { VRAM_W, VRAM_H } };
  vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
  vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pll, 0, 1, &s_dset_tex, 0, 0);
  VkDeviceSize off = 0;
  if (s_tri_n) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tri_pipe);
                 vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_vbuf, &off); vkCmdDraw(s_cmd, s_tri_n, 1, 0, 0); }
  if (s_tex_n) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tritex_pipe);
                 vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_tvbuf, &off); vkCmdDraw(s_cmd, s_tex_n, 1, 0, 0); }
  vkCmdEndRenderPass(s_cmd);

  img_barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  vkCmdCopyImageToBuffer(s_cmd, s_tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_rb, 1, &bc);
  s_tex_undef = 1;
  VKC(vkEndCommandBuffer(s_cmd));
  VkSubmitInfo su = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  su.commandBufferCount = 1; su.pCommandBuffers = &s_cmd;
  VKC(vkQueueSubmit(s_queue, 1, &su, s_fence));
  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
  memcpy(out, s_rb_ptr, (size_t)VRAM_W * VRAM_H * 2);
}

// PSXPORT_VK_SHOT=frame -> dump the live VK-rendered VRAM (display region) to a PPM, to eyeball.
void gpu_vk_dump(int sx, int sy, int w, int h, int frame) {
  if (!gpu_vk_enabled() || !s_inited) return;
  static int sf = -2; if (sf == -2) { const char* e = getenv("PSXPORT_VK_SHOT"); sf = e ? atoi(e) : -1; }
  if (sf < 0 || frame != sf) return;
  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX)); VKC(vkResetFences(s_dev, 1, &s_fence));
  VKC(vkResetCommandBuffer(s_cmd, 0));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; VKC(vkBeginCommandBuffer(s_cmd, &bi));
  img_barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy bc = {0}; bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT,0,0,1 };
  bc.imageExtent = (VkExtent3D){ VRAM_W, VRAM_H, 1 };
  vkCmdCopyImageToBuffer(s_cmd, s_tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_rb, 1, &bc);
  img_barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
  VKC(vkEndCommandBuffer(s_cmd));
  VkSubmitInfo su = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; su.commandBufferCount = 1; su.pCommandBuffers = &s_cmd;
  VKC(vkQueueSubmit(s_queue, 1, &su, s_fence)); VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
  const uint16_t* vram = (const uint16_t*)s_rb_ptr;
  FILE* f = fopen("scratch/screenshots/vk_live.ppm", "wb"); if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint16_t p = vram[((sy+y)&511)*VRAM_W + ((sx+x)&1023)];
    unsigned char c[3] = { (unsigned char)((p&31)<<3), (unsigned char)(((p>>5)&31)<<3), (unsigned char)(((p>>10)&31)<<3) };
    fwrite(c, 1, 3, f); }
  fclose(f); fprintf(stderr, "[vk_shot] f%d wrote scratch/screenshots/vk_live.ppm\n", frame);
}

// Per-frame: PSXPORT_VK_DIFF=frame -> diff this frame's tee'd untextured tris (VK) vs SW VRAM. Always
// resets the batch. `frame` is the SW frame counter; `svram` is the SW VRAM (source of truth).
void gpu_vk_frame_end(const uint16_t* svram, int frame) {
  if (!gpu_vk_enabled() || !s_inited) { s_tri_n = 0; return; }
  static int dframe = -2;
  if (dframe == -2) { const char* e = getenv("PSXPORT_VK_DIFF"); dframe = e ? atoi(e) : -1; }
  if (dframe >= 0 && frame == dframe) {
    if (!s_tri_n && !s_tex_n) { fprintf(stderr, "[vk_diff] f%d: no tris this frame\n", frame); s_tri_n = s_tex_n = 0; return; }
    static uint16_t got[VRAM_W * VRAM_H];
    tri_over_bg_readback(svram, got);
    fprintf(stderr, "[vk_diff] f%d  untex_tris=%d tex_tris=%d  -> wrote vk_out.ppm + sw_out.ppm\n",
            frame, s_tri_n / 3, s_tex_n / 3);
    // Dump the actual VK render and the SW render of the front framebuffer region, side by eye.
    const char* names[2] = { "scratch/screenshots/vk_out.ppm", "scratch/screenshots/sw_out.ppm" };
    const uint16_t* srcs[2] = { got, svram };
    for (int s = 0; s < 2; s++) {
      FILE* f = fopen(names[s], "wb"); if (!f) continue;
      int W = 320, H = 240, ox = 0, oy = 256;     // Tomba2 front framebuffer (x 0..319, y 256..495)
      fprintf(f, "P6\n%d %d\n255\n", W, H);
      for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint16_t p = srcs[s][(y + oy) * VRAM_W + (x + ox)];
        unsigned char c[3] = { (unsigned char)(((p)&31)<<3), (unsigned char)(((p>>5)&31)<<3), (unsigned char)(((p>>10)&31)<<3) };
        fwrite(c, 1, 3, f); }
      fclose(f);
    }
  }
  s_tri_n = 0; s_tex_n = 0; s_semi_n = 0; s_dirty_n = 0;
}

// PSXPORT_VK_TRITEST=1: headless-ish self-test of the triangle rasterizer. Draws a known flat tri and a
// gouraud tri, reads back, checks expected pixels, prints PASS/FAIL, exits. Validates the GPU raster path.
void gpu_vk_tritest(void) {
  if (!getenv("PSXPORT_VK_TRITEST")) return;
  if (!s_inited) init_vk();
  s_tri_n = 0;
  gpu_vk_draw_tri( 10,10, 255,0,0,  200,10, 255,0,0,  10,200, 255,0,0);    // flat red, big right-triangle
  gpu_vk_draw_tri(300,300, 255,0,0, 460,300, 0,255,0, 300,460, 0,0,255);   // gouraud r/g/b corners
  static uint16_t vram[VRAM_W * VRAM_H];
  tri_render_and_readback(vram);
  uint16_t inside_flat = vram[40 * VRAM_W + 40];        // inside flat tri -> red 0x001F
  uint16_t outside     = vram[400 * VRAM_W + 50];       // background -> 0
  uint16_t g_corner    = vram[305 * VRAM_W + 445];      // inside, near green vertex (460,300) -> green-dominant
  int gr = g_corner & 31, gg = (g_corner >> 5) & 31, gb = (g_corner >> 10) & 31;
  int ok = (inside_flat == 0x001F) && (outside == 0x0000) && (gg > gr && gg > gb && gg > 16);
  fprintf(stderr, "[vk_tritest] flat(40,40)=0x%04x (want 0x001f)  bg(50,400)=0x%04x (want 0)  "
          "gouraud(445,305)=0x%04x r=%d g=%d b=%d  => %s\n",
          inside_flat, outside, g_corner, gr, gg, gb, ok ? "PASS" : "FAIL");
  // dump the GPU-rendered region (0,0)-(480,480) as a PPM for visual confirmation
  FILE* f = fopen("scratch/screenshots/vk_tritest.ppm", "wb");
  if (f) { int W = 480, H = 480; fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
      uint16_t p = vram[y * VRAM_W + x];
      unsigned char rgb[3] = { (unsigned char)(((p)&31)<<3), (unsigned char)(((p>>5)&31)<<3), (unsigned char)(((p>>10)&31)<<3) };
      fwrite(rgb, 1, 3, f); }
    fclose(f); fprintf(stderr, "[vk_tritest] wrote scratch/screenshots/vk_tritest.ppm\n"); }
  exit(ok ? 0 : 3);
}
#else
#include <stdint.h>
int  gpu_vk_enabled(void) { return 0; }
void gpu_vk_present(const uint16_t* src, int sx, int sy, int w, int h) { (void)src;(void)sx;(void)sy;(void)w;(void)h; }
void gpu_vk_tritest(void) {}
void gpu_vk_frame_end(const uint16_t* svram, int frame) { (void)svram; (void)frame; }
void gpu_vk_dump(int sx, int sy, int w, int h, int frame) { (void)sx;(void)sy;(void)w;(void)h;(void)frame; }
void gpu_vk_dirty(int x, int y, int w, int h) { (void)x;(void)y;(void)w;(void)h; }
void gpu_vk_draw_tri(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j,int k,int l,int m,int n,int o) {
  (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;(void)m;(void)n;(void)o;
}
void gpu_vk_draw_tritri(const int* xs, const int* ys, const int* us, const int* vs,
                        const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                        int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                        int twmx, int twmy, int twox, int twoy,
                        int dax0, int day0, int dax1, int day1) {
  (void)xs;(void)ys;(void)us;(void)vs;(void)rs;(void)gs;(void)bs;(void)tpx;(void)tpy;(void)mode;(void)raw;
  (void)clutx;(void)cluty;(void)twmx;(void)twmy;(void)twox;(void)twoy;(void)dax0;(void)day0;(void)dax1;(void)day1;
}
void gpu_vk_draw_semi(const int* xs, const int* ys, const int* us, const int* vs, const unsigned char* rs,
                      const unsigned char* gs, const unsigned char* bs, int tpx, int tpy, int mode, int raw,
                      int clutx, int cluty, int twmx, int twmy, int twox, int twoy,
                      int dax0, int day0, int dax1, int day1, int blend) {
  (void)xs;(void)ys;(void)us;(void)vs;(void)rs;(void)gs;(void)bs;(void)tpx;(void)tpy;(void)mode;(void)raw;
  (void)clutx;(void)cluty;(void)twmx;(void)twmy;(void)twox;(void)twoy;(void)dax0;(void)day0;(void)dax1;(void)day1;(void)blend;
}
#endif
