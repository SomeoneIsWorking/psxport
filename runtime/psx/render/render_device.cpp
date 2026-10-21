// render_device.cpp — RenderDevice bring-up, the sink, the shared pipelines and the texture transfers.
#include "render_device.h"

#include "config_vars.h"
#include "diagnostic_config.h"
#include "number_text.h"
#include "psxport_generated/render_shaders.h"
#include "render_check.h"
#include "rmlui_library.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace psx::render {
namespace {

constexpr ShaderCode kFullscreenVertex{spv_g_fullscreen_vert, spv_g_fullscreen_vert_len, {0, 0, 0}};
constexpr ShaderCode kPresentFragment{spv_g_present_frag, spv_g_present_frag_len, {1, 1, 0}};
constexpr ShaderCode kImageFragment{spv_g_image_frag, spv_g_image_frag_len, {1, 1, 0}};
constexpr ShaderCode kPaneVertex{spv_g_pane_vert, spv_g_pane_vert_len, {0, 1, 0}};
constexpr ShaderCode kPaneFragment{spv_g_pane_frag, spv_g_pane_frag_len, {1, 1, 0}};
constexpr int kMaxSinkSide = 16384;

} // namespace

MappedTransfer::MappedTransfer(SDL_GPUDevice *device, SDL_GPUTransferBuffer *buffer)
    : device_(device), buffer_(buffer),
      bytes_(static_cast<const std::uint8_t *>(SDL_MapGPUTransferBuffer(device, buffer, false))) {}

MappedTransfer::MappedTransfer(MappedTransfer &&other) noexcept
    : device_(other.device_), buffer_(other.buffer_), bytes_(other.bytes_) {
  other.bytes_ = nullptr;
}

MappedTransfer::~MappedTransfer() {
  if (bytes_ != nullptr) {
    SDL_UnmapGPUTransferBuffer(device_, buffer_);
  }
}

RenderDevice::RenderDevice() = default;

RenderDevice::~RenderDevice() {
  if (device_ != nullptr) {
    SDL_WaitForGPUIdle(device_);
    rmlUi_.reset();
    release(presentPipeline_);
    release(imagePipeline_);
    release(sinkPipeline_);
    release(panePipeline_);
    release(whiteTexture_);
    SDL_ReleaseGPUSampler(device_, nearest_);
    SDL_ReleaseGPUSampler(device_, linear_);
    window_.releaseDevice(device_);
    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;
    window_.destroy();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    lucent::info("render", "device and window released");
  }
}

bool RenderDevice::shown() const {
  return enabled_ && psx::config::show_window();
}

PixelSize RenderDevice::sinkSize() {
  if (window_.window() != nullptr) {
    return window_.pixelSize();
  }
  if (creationSize_.width == 0) {
    creationSize_ = kWindowSize;
    const std::string sink = psx::config::present_sink();
    const std::optional<psx::config::NumberPair> size = psx::config::parsePair(sink, psx::config::kDimensions);
    if (size && size->first > 0 && size->second > 0 && size->first <= kMaxSinkSide && size->second <= kMaxSinkSide) {
      creationSize_ = {static_cast<int>(size->first), static_cast<int>(size->second)};
      namedSink_ = true;
    } else if (!sink.empty()) {
      lucent::warn("render",
                   "PSXPORT_PRESENT_SINK=\"{}\" is not WxH, using {}x{}",
                   sink,
                   creationSize_.width,
                   creationSize_.height);
    }
  }
  return creationSize_;
}

SDL_GPUTextureFormat RenderDevice::sinkFormat() const {
  return window_.swapchainFormat();
}

SDL_GPUSampler *RenderDevice::makeSampler(SDL_GPUFilter filter) {
  SDL_GPUSamplerCreateInfo info = {};
  info.min_filter = filter;
  info.mag_filter = filter;
  info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  SDL_GPUSampler *sampler = SDL_CreateGPUSampler(device_, &info);
  GPUCHK(sampler, "SDL_CreateGPUSampler");
  return sampler;
}

void RenderDevice::bringUp() {
  if (device_ != nullptr) {
    return;
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    lucent::error("render", "SDL_Init(VIDEO) failed: {}", SDL_GetError());
    std::exit(2);
  }
  window_.create(shown(), windowTitle_ != nullptr ? windowTitle_ : "psxport (untitled game)", sinkSize(), namedSink_);
  if (!faultPreflight()) {
    std::exit(3);
  }
  device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, psx::config::gpu_validation(), nullptr);
  GPUCHK(device_, "SDL_CreateGPUDevice");
  const char *driver = SDL_GetGPUDeviceDriver(device_);
  lucent::info("render", "SDL_GPU device up (driver: {})", driver != nullptr ? driver : "(null)");
  window_.claim(device_);
  nearest_ = makeSampler(SDL_GPU_FILTER_NEAREST);
  linear_ = makeSampler(SDL_GPU_FILTER_LINEAR);
  presentPipeline_ = makePipeline(kFullscreenVertex, kPresentFragment, kPresentImageFormat);
  imagePipeline_ = makePipeline(kFullscreenVertex, kImageFragment, kPresentImageFormat);
  sinkPipeline_ = makePipeline(kFullscreenVertex, kImageFragment, sinkFormat());
  SDL_GPUColorTargetBlendState over = {};
  over.enable_blend = true;
  over.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  over.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  over.color_blend_op = SDL_GPU_BLENDOP_ADD;
  over.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  over.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  over.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  panePipeline_ = makePipeline(kPaneVertex, kPaneFragment, kPresentImageFormat, &over);
  const std::uint8_t white[4] = {255, 255, 255, 255};
  whiteTexture_ = makeFilledTexture({1, 1}, white);
  rmlUi_ = std::make_unique<ui::RmlUiLibrary>(*this);
  lucent::info("render", "renderer up");
}

SDL_GPUCommandBuffer *RenderDevice::acquire() {
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device_);
  GPUCHK(cmd, "SDL_AcquireGPUCommandBuffer");
  return cmd;
}

SDL_GPUGraphicsPipeline *RenderDevice::makePipeline(const ShaderCode &vertex,
                                                    const ShaderCode &fragment,
                                                    SDL_GPUTextureFormat format,
                                                    const SDL_GPUColorTargetBlendState *blend,
                                                    const SDL_GPUVertexInputState *input) {
  SDL_GPUShader *vs = makeShader(device_, vertex, SDL_GPU_SHADERSTAGE_VERTEX);
  SDL_GPUShader *fs = makeShader(device_, fragment, SDL_GPU_SHADERSTAGE_FRAGMENT);
  SDL_GPUColorTargetDescription target = {};
  target.format = format;
  if (blend != nullptr) {
    target.blend_state = *blend;
  }
  SDL_GPUGraphicsPipelineCreateInfo info = {};
  info.vertex_shader = vs;
  info.fragment_shader = fs;
  if (input != nullptr) {
    info.vertex_input_state = *input;
  }
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  info.target_info.color_target_descriptions = &target;
  info.target_info.num_color_targets = 1;
  SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
  GPUCHK(pipeline, "SDL_CreateGPUGraphicsPipeline");
  SDL_ReleaseGPUShader(device_, vs);
  SDL_ReleaseGPUShader(device_, fs);
  return pipeline;
}

SDL_GPUTexture *RenderDevice::makeTexture(PixelSize size, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
  SDL_GPUTextureCreateInfo info = {};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = format;
  info.usage = usage;
  info.width = static_cast<Uint32>(size.width);
  info.height = static_cast<Uint32>(size.height);
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  SDL_GPUTexture *texture = SDL_CreateGPUTexture(device_, &info);
  GPUCHK(texture, "SDL_CreateGPUTexture");
  return texture;
}

SDL_GPUTransferBuffer *RenderDevice::makeTransferBuffer(SDL_GPUTransferBufferUsage usage, std::uint32_t bytes) {
  SDL_GPUTransferBufferCreateInfo info = {};
  info.usage = usage;
  info.size = bytes;
  SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(device_, &info);
  GPUCHK(buffer, "SDL_CreateGPUTransferBuffer");
  return buffer;
}

// This SDL has no COPY_DST usage: a texture is filled from a transfer buffer, never by a texture copy.
void RenderDevice::upload(SDL_GPUCommandBuffer *cmd,
                          SDL_GPUTransferBuffer *staging,
                          SDL_GPUTexture *texture,
                          PixelSize size) {
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo source = {};
  source.transfer_buffer = staging;
  source.pixels_per_row = static_cast<Uint32>(size.width);
  source.rows_per_layer = static_cast<Uint32>(size.height);
  SDL_GPUTextureRegion destination = {};
  destination.texture = texture;
  destination.w = static_cast<Uint32>(size.width);
  destination.h = static_cast<Uint32>(size.height);
  destination.d = 1;
  SDL_UploadToGPUTexture(copy, &source, &destination, false);
  SDL_EndGPUCopyPass(copy);
}

void RenderDevice::upload(SDL_GPUCommandBuffer *cmd,
                          SDL_GPUTransferBuffer *staging,
                          std::span<const std::uint8_t> pixels,
                          SDL_GPUTexture *texture,
                          PixelSize size) {
  void *mapped = SDL_MapGPUTransferBuffer(device_, staging, true);
  GPUCHK(mapped, "SDL_MapGPUTransferBuffer");
  std::memcpy(mapped, pixels.data(), pixels.size());
  SDL_UnmapGPUTransferBuffer(device_, staging);
  upload(cmd, staging, texture, size);
}

SDL_GPUTexture *RenderDevice::makeFilledTexture(PixelSize size, std::span<const std::uint8_t> pixels) {
  SDL_GPUTexture *texture = makeTexture(size, kPresentImageFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER);
  SDL_GPUTransferBuffer *staging =
      makeTransferBuffer(SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, static_cast<std::uint32_t>(pixels.size()));
  SDL_GPUCommandBuffer *cmd = acquire();
  upload(cmd, staging, pixels, texture, size);
  submit(cmd, "texture upload");
  release(staging);
  return texture;
}

MappedTransfer RenderDevice::download(SDL_GPUTexture *texture,
                                      const SDL_Rect &region,
                                      SDL_GPUTransferBuffer *readback,
                                      const char *where) {
  SDL_GPUCommandBuffer *cmd = acquire();
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion source = {};
  source.texture = texture;
  source.x = static_cast<Uint32>(region.x);
  source.y = static_cast<Uint32>(region.y);
  source.w = static_cast<Uint32>(region.w);
  source.h = static_cast<Uint32>(region.h);
  source.d = 1;
  SDL_GPUTextureTransferInfo destination = {};
  destination.transfer_buffer = readback;
  destination.pixels_per_row = static_cast<Uint32>(region.w);
  destination.rows_per_layer = static_cast<Uint32>(region.h);
  SDL_DownloadFromGPUTexture(copy, &source, &destination);
  SDL_EndGPUCopyPass(copy);
  // A failed wait leaves an older frame in `readback`, which must not be read as this one.
  if (!submitAndWait(cmd, where)) {
    return {};
  }
  return {device_, readback};
}

void RenderDevice::release(SDL_GPUTexture *&texture) {
  if (texture != nullptr) {
    SDL_ReleaseGPUTexture(device_, texture);
    texture = nullptr;
  }
}

void RenderDevice::release(SDL_GPUTransferBuffer *&buffer) {
  if (buffer != nullptr) {
    SDL_ReleaseGPUTransferBuffer(device_, buffer);
    buffer = nullptr;
  }
}

void RenderDevice::release(SDL_GPUBuffer *&buffer) {
  if (buffer != nullptr) {
    SDL_ReleaseGPUBuffer(device_, buffer);
    buffer = nullptr;
  }
}

void RenderDevice::release(SDL_GPUGraphicsPipeline *&pipeline) {
  if (pipeline != nullptr) {
    SDL_ReleaseGPUGraphicsPipeline(device_, pipeline);
    pipeline = nullptr;
  }
}

} // namespace psx::render
