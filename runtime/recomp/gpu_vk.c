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
static VkSemaphore     s_sem_acq, s_sem_rel;
static VkFence         s_fence;

// display-region texture (RGBA8) + host-visible staging
static int             s_tex_w, s_tex_h, s_tex_undef;
static VkImage         s_tex;
static VkDeviceMemory  s_tex_mem;
static VkImageView     s_tex_view;
static VkBuffer        s_stage;
static VkDeviceMemory  s_stage_mem;
static void*           s_stage_ptr;
static VkDeviceSize    s_stage_sz;

int gpu_vk_enabled(void) {
  if (s_vk_on < 0) {
    const char* v = getenv("PSXPORT_VK");
    const char* w = getenv("PSXPORT_GPU_WINDOW");
    s_vk_on = (v && atoi(v) != 0 && w && atoi(w) != 0) ? 1 : 0;
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
  VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
  pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
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
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VKC(vkCreateSampler(s_dev, &si, 0, &s_sampler));
  VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
  VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
  dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
  VKC(vkCreateDescriptorPool(s_dev, &dpi, 0, &s_dpool));
  VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
  dai.descriptorPool = s_dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_dsl;
  VKC(vkAllocateDescriptorSets(s_dev, &dai, &s_dset));

  // command pool + buffer, sync
  VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = s_qfam;
  VKC(vkCreateCommandPool(s_dev, &cpi, 0, &s_cpool));
  VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
  cai.commandPool = s_cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
  VKC(vkAllocateCommandBuffers(s_dev, &cai, &s_cmd));
  VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
  VKC(vkCreateSemaphore(s_dev, &sci, 0, &s_sem_acq));
  VKC(vkCreateSemaphore(s_dev, &sci, 0, &s_sem_rel));
  VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, VK_FENCE_CREATE_SIGNALED_BIT };
  VKC(vkCreateFence(s_dev, &fci, 0, &s_fence));

  create_swapchain();
  fprintf(stderr, "[gpu_vk] Vulkan present backend up (%ux%u, %u swap images)\n",
          s_extent.width, s_extent.height, s_swap_n);
}

// (re)create the display-region texture + staging buffer when the display size changes.
static void ensure_tex(int w, int h) {
  if (s_tex && s_tex_w == w && s_tex_h == h) return;
  if (s_tex) { vkDeviceWaitIdle(s_dev);
    vkDestroyImageView(s_dev, s_tex_view, 0); vkDestroyImage(s_dev, s_tex, 0); vkFreeMemory(s_dev, s_tex_mem, 0);
    vkDestroyBuffer(s_dev, s_stage, 0); vkUnmapMemory(s_dev, s_stage_mem); vkFreeMemory(s_dev, s_stage_mem, 0); }
  s_tex_w = w; s_tex_h = h; s_tex_undef = 1;

  VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
  ii.extent = (VkExtent3D){ w, h, 1 }; ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VKC(vkCreateImage(s_dev, &ii, 0, &s_tex));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(s_dev, s_tex, &mr);
  VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKC(vkAllocateMemory(s_dev, &ma, 0, &s_tex_mem));
  VKC(vkBindImageMemory(s_dev, s_tex, s_tex_mem, 0));
  VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
  vi.image = s_tex; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  VKC(vkCreateImageView(s_dev, &vi, 0, &s_tex_view));

  s_stage_sz = (VkDeviceSize)w * h * 4;
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
  ensure_tex(w, h);

  // VRAM (1555) region -> RGBA8 staging (same expansion as the SW blit)
  uint32_t* dst = (uint32_t*)s_stage_ptr;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint16_t p = src[((sy + y) & (VRAM_H - 1)) * VRAM_W + ((sx + x) & (VRAM_W - 1))];
      dst[y * w + x] = 0xFF000000u | ((p & 31) << 3) | (((p >> 5) & 31) << 11) | (((p >> 10) & 31) << 19);
    }

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
  s_tex_undef = 0;
  VkBufferImageCopy bc = {0};
  bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  bc.imageExtent = (VkExtent3D){ w, h, 1 };
  vkCmdCopyBufferToImage(s_cmd, s_stage, s_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
  img_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

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
  vkCmdDraw(s_cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(s_cmd);
  VKC(vkEndCommandBuffer(s_cmd));

  VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo su = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  su.waitSemaphoreCount = 1; su.pWaitSemaphores = &s_sem_acq; su.pWaitDstStageMask = &wait;
  su.commandBufferCount = 1; su.pCommandBuffers = &s_cmd;
  su.signalSemaphoreCount = 1; su.pSignalSemaphores = &s_sem_rel;
  VKC(vkQueueSubmit(s_queue, 1, &su, s_fence));

  VkPresentInfoKHR pr = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
  pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &s_sem_rel;
  pr.swapchainCount = 1; pr.pSwapchains = &s_swap; pr.pImageIndices = &idx;
  VkResult pres = vkQueuePresentKHR(s_queue, &pr);
  if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) recreate_swapchain();
  else if (pres != VK_SUCCESS) { fprintf(stderr, "[gpu_vk] present %d\n", pres); exit(2); }

  poll_quit();
}
#else
#include <stdint.h>
int  gpu_vk_enabled(void) { return 0; }
void gpu_vk_present(const uint16_t* src, int sx, int sy, int w, int h) { (void)src;(void)sx;(void)sy;(void)w;(void)h; }
#endif
