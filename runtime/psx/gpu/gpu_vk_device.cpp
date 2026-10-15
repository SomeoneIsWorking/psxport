// gpu_vk_device.cpp — GpuDevice bring-up, the present sink and teardown.
#include "gpu_vk_device.h"

#include "config_vars.h" // windowRequested — the window decision
#include "diagnostic_config.h"
#include "gpu_vk_check.h"
#include "gpu_vk_shader.h"
#include "number_text.h" // present_sink, gpu_validation
#include "psxport_generated/gpu_vk_shaders.h"
#include "rmlui_overlay.h"

#include <lucent/log.h>
#include <optional>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

template <typename T, typename Release> void releaseHandle(T *&handle, SDL_GPUDevice *device, Release release) {
  if (handle != nullptr) {
    release(device, handle);
    handle = nullptr;
  }
}

// A fullscreen-triangle pipeline sampling one fragment texture with one fragment uniform buffer.
SDL_GPUGraphicsPipeline *makeFullscreenPipeline(SDL_GPUDevice *device,
                                                const uint32_t *vsCode,
                                                unsigned vsLen,
                                                const uint32_t *fsCode,
                                                unsigned fsLen,
                                                SDL_GPUTextureFormat format) {
  SDL_GPUShader *vs = psx::gpu::makeShader(device, vsCode, vsLen, SDL_GPU_SHADERSTAGE_VERTEX, {0, 0});
  SDL_GPUShader *fs = psx::gpu::makeShader(device, fsCode, fsLen, SDL_GPU_SHADERSTAGE_FRAGMENT, {1, 1});
  SDL_GPUColorTargetDescription target = {};
  target.format = format;
  SDL_GPUGraphicsPipelineCreateInfo info = {};
  info.vertex_shader = vs;
  info.fragment_shader = fs;
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  info.target_info.color_target_descriptions = &target;
  info.target_info.num_color_targets = 1;
  SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
  GPUCHK(pipeline, "SDL_CreateGPUGraphicsPipeline");
  SDL_ReleaseGPUShader(device, vs);
  SDL_ReleaseGPUShader(device, fs);
  return pipeline;
}

} // namespace

GpuDevice::~GpuDevice() {
  if (s_inited == 0) {
    return;
  }
  if (s_dev != nullptr) {
    SDL_WaitForGPUIdle(s_dev);
    const auto pipeline = [](SDL_GPUDevice *d, SDL_GPUGraphicsPipeline *p) {
      SDL_ReleaseGPUGraphicsPipeline(d, p);
    };
    releaseHandle(s_present_pipe, s_dev, pipeline);
    releaseHandle(s_image_pipe, s_dev, pipeline);
    const auto sampler = [](SDL_GPUDevice *d, SDL_GPUSampler *s) {
      SDL_ReleaseGPUSampler(d, s);
    };
    releaseHandle(s_samp_nearest, s_dev, sampler);
    releaseHandle(s_samp_linear, s_dev, sampler);
    releaseHandle(s_img_tex, s_dev, [](SDL_GPUDevice *d, SDL_GPUTexture *t) {
      SDL_ReleaseGPUTexture(d, t);
    });
    releaseHandle(s_img_xfer, s_dev, [](SDL_GPUDevice *d, SDL_GPUTransferBuffer *b) {
      SDL_ReleaseGPUTransferBuffer(d, b);
    });
    // RmlUi's own teardown runs at exit, after the device; its resources go back while it is alive.
    psx::ui::releaseDeviceResources();
    s_window.release_device(s_dev);
    SDL_DestroyGPUDevice(s_dev);
    s_dev = nullptr;
  }
  s_window.destroy();
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  s_inited = 0;
  lucent::info("gpu_vk", "device and window released");
}

bool GpuDevice::windowed() {
  if (legResolved_ == 0) {
    // Headless unless a window is asked for, so a forgotten flag never pops a window.
    s_headless = psx::config::windowRequested() ? 0 : 1;
    legResolved_ = 1;
  }
  return enabled_ && s_headless == 0;
}

psx::present::PixelSize GpuDevice::sinkSize() {
  if (windowed()) {
    return {s_window.pixel_width(), s_window.pixel_height()};
  }
  if (sinkW_ == 0) {
    sinkW_ = PRESENT_WINDOW_W;
    sinkH_ = PRESENT_WINDOW_H;
    const std::string sink = psx::config::present_sink();
    const std::optional<psx::config::NumberPair> size = psx::config::parsePair(sink, psx::config::kDimensions);
    if (size.has_value() && size->first > 0 && size->second > 0 && size->first <= 16384 && size->second <= 16384) {
      sinkW_ = static_cast<int>(size->first);
      sinkH_ = static_cast<int>(size->second);
    } else if (!sink.empty()) {
      lucent::warn("gpu_vk", "PSXPORT_PRESENT_SINK=\"{}\" is not WxH — using the default {}x{}", sink, sinkW_, sinkH_);
    }
  }
  return {sinkW_, sinkH_};
}

void GpuDevice::bringUp() {
  if (s_inited != 0) {
    return;
  }
  (void)windowed();
  s_inited = 1;
  // SDL_GPU needs the video subsystem even headless.
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    lucent::error("gpu_vk", "SDL_Init(VIDEO) failed: {}", SDL_GetError());
    std::exit(2);
  }
  s_window.create(s_headless != 0, s_window_title != nullptr ? s_window_title : "psxport (untitled game)");
  if (!faultPreflight()) {
    faulted_ = true;
    std::exit(3);
  }
  s_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, psx::config::gpu_validation(), nullptr);
  GPUCHK(s_dev, "SDL_CreateGPUDevice");
  const char *driver = SDL_GetGPUDeviceDriver(s_dev);
  lucent::info("gpu_vk", "SDL_GPU device up (driver: {})", driver != nullptr ? driver : "(null)");
  if (s_headless == 0) {
    s_window.claim(s_dev);
  }
  SDL_GPUSamplerCreateInfo sampler = {};
  sampler.min_filter = SDL_GPU_FILTER_NEAREST;
  sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
  sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  s_samp_nearest = SDL_CreateGPUSampler(s_dev, &sampler);
  GPUCHK(s_samp_nearest, "CreateGPUSampler(nearest)");
  sampler.min_filter = SDL_GPU_FILTER_LINEAR;
  sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
  s_samp_linear = SDL_CreateGPUSampler(s_dev, &sampler);
  GPUCHK(s_samp_linear, "CreateGPUSampler(linear)");
  // The present pass targets the per-Game present image in both legs.
  s_present_pipe = makeFullscreenPipeline(s_dev,
                                          spv_g_present_vert,
                                          spv_g_present_vert_len,
                                          spv_g_present_frag,
                                          spv_g_present_frag_len,
                                          kPresentImageFormat);
  // The image pass draws into the swapchain windowed and into the present image headless.
  s_image_pipe = makeFullscreenPipeline(s_dev,
                                        spv_g_image_vert,
                                        spv_g_image_vert_len,
                                        spv_g_image_frag,
                                        spv_g_image_frag_len,
                                        s_headless != 0 ? kPresentImageFormat : s_window.swapchain_format());
  lucent::info("gpu_vk", "{} renderer up", s_headless != 0 ? "headless" : "windowed");
}

void GpuDevice::ensureImageTexture(int width, int height) {
  if (s_img_tex != nullptr && s_img_w == width && s_img_h == height) {
    return;
  }
  if (s_img_tex != nullptr) {
    SDL_ReleaseGPUTexture(s_dev, s_img_tex);
    SDL_ReleaseGPUTransferBuffer(s_dev, s_img_xfer);
  }
  SDL_GPUTextureCreateInfo texture = {};
  texture.type = SDL_GPU_TEXTURETYPE_2D;
  texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  texture.width = static_cast<Uint32>(width);
  texture.height = static_cast<Uint32>(height);
  texture.layer_count_or_depth = 1;
  texture.num_levels = 1;
  s_img_tex = SDL_CreateGPUTexture(s_dev, &texture);
  GPUCHK(s_img_tex, "CreateGPUTexture(image)");
  SDL_GPUTransferBufferCreateInfo upload = {};
  upload.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  upload.size = static_cast<Uint32>(width) * static_cast<Uint32>(height) * 4U;
  s_img_xfer = SDL_CreateGPUTransferBuffer(s_dev, &upload);
  GPUCHK(s_img_xfer, "CreateGPUTransferBuffer(image)");
  s_img_w = width;
  s_img_h = height;
}
