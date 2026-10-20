// render_device.h — the SDL3 GPU device and its window, plus the samplers, pipelines and transfers every Game
// presenting through it shares. Per-Game targets live on RenderState.
#pragma once
#include "pixel_size.h"
#include "render_shader.h"
#include "render_window.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <memory>
#include <span>

namespace psx::ui {
class RmlUiLibrary;
} // namespace psx::ui

namespace psx::render {

using present::PixelSize;

// The presented-picture format: fixed so it exists identically windowed and headless.
inline constexpr SDL_GPUTextureFormat kPresentImageFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

// A download's bytes, mapped until this is destroyed.
class MappedTransfer {
public:
  MappedTransfer() = default;
  MappedTransfer(SDL_GPUDevice *device, SDL_GPUTransferBuffer *buffer);
  ~MappedTransfer();
  MappedTransfer(MappedTransfer &&other) noexcept;
  MappedTransfer &operator=(MappedTransfer &&) = delete;
  MappedTransfer(const MappedTransfer &) = delete;
  MappedTransfer &operator=(const MappedTransfer &) = delete;

  const std::uint8_t *bytes() const {
    return bytes_;
  }
  explicit operator bool() const {
    return bytes_ != nullptr;
  }

private:
  SDL_GPUDevice *device_ = nullptr;
  SDL_GPUTransferBuffer *buffer_ = nullptr;
  const std::uint8_t *bytes_ = nullptr;
};

class RenderDevice {
public:
  RenderDevice();
  ~RenderDevice();
  RenderDevice(const RenderDevice &) = delete;
  RenderDevice &operator=(const RenderDevice &) = delete;

  // A disabled device presents nothing.
  bool enabled() const {
    return enabled_;
  }
  void disable() {
    enabled_ = false;
  }
  // Is a live on-screen window up (or about to be)? Resolves the leg from config on the first ask.
  bool windowed();
  bool headless() {
    return !windowed();
  }
  bool up() const {
    return device_ != nullptr;
  }
  // Window, device, samplers, present pipelines and the RmlUi library. Idempotent; exits when no device can be made.
  void bringUp();
  // The live drawable windowed, PSXPORT_PRESENT_SINK (default the window size) headless.
  PixelSize sinkSize();

  // The host names the window before the first present; null names it generically.
  void setWindowTitle(const char *title) {
    windowTitle_ = title;
  }
  const char *windowTitle() const {
    return windowTitle_;
  }

  SDL_GPUDevice *handle() const {
    return device_;
  }
  const RenderWindow &window() const {
    return window_;
  }
  // The format of the pass the sink records into: the swapchain's windowed, the present image's headless.
  SDL_GPUTextureFormat sinkFormat();
  SDL_GPUSampler *nearestSampler() const {
    return nearest_;
  }
  SDL_GPUSampler *linearSampler() const {
    return linear_;
  }
  // VRAM or the record image into the present image, with the display rect, fade and 24bpp decode.
  SDL_GPUGraphicsPipeline *presentPipeline() const {
    return presentPipeline_;
  }
  // An RGBA image scaled by a brightness uniform: into the present image, or into the window.
  SDL_GPUGraphicsPipeline *imagePipeline() const {
    return imagePipeline_;
  }
  SDL_GPUGraphicsPipeline *sinkPipeline() const {
    return sinkPipeline_;
  }
  // A seam-cut, tinted rectangle into the present image (render_pane.h), blended over what is there.
  SDL_GPUGraphicsPipeline *panePipeline() const {
    return panePipeline_;
  }
  // The 1x1 source a solid pane is drawn from.
  SDL_GPUTexture *whiteTexture() const {
    return whiteTexture_;
  }
  ui::RmlUiLibrary &rmlUi() {
    return *rmlUi_;
  }

  SDL_GPUCommandBuffer *acquire();
  // Submit, latching the device off for the rest of the process on the first failure.
  bool submit(SDL_GPUCommandBuffer *cmd, const char *where);
  // Submit and wait with a wall-clock bound; false (and latched) on failure or timeout.
  bool submitAndWait(SDL_GPUCommandBuffer *cmd, const char *where);

  // A pipeline drawing triangle lists into one colour target. Without `input` the vertex stage makes its own vertices.
  SDL_GPUGraphicsPipeline *makePipeline(const ShaderCode &vertex,
                                        const ShaderCode &fragment,
                                        SDL_GPUTextureFormat format,
                                        const SDL_GPUColorTargetBlendState *blend = nullptr,
                                        const SDL_GPUVertexInputState *input = nullptr);
  SDL_GPUTexture *makeTexture(PixelSize size, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage);
  SDL_GPUTransferBuffer *makeTransferBuffer(SDL_GPUTransferBufferUsage usage, std::uint32_t bytes);
  // Copies `pixels` into `staging` and records their upload into the whole of `texture`.
  void upload(SDL_GPUCommandBuffer *cmd,
              SDL_GPUTransferBuffer *staging,
              std::span<const std::uint8_t> pixels,
              SDL_GPUTexture *texture,
              PixelSize size);
  // Records an upload of `staging`, already filled, into the whole of `texture`.
  void upload(SDL_GPUCommandBuffer *cmd, SDL_GPUTransferBuffer *staging, SDL_GPUTexture *texture, PixelSize size);
  // A sampled RGBA8 texture holding `pixels`, uploaded before any later submit reads it.
  SDL_GPUTexture *makeFilledTexture(PixelSize size, std::span<const std::uint8_t> pixels);
  // Downloads `region` of `texture` into `readback`, waits, and maps it; empty when the GPU refused.
  MappedTransfer
  download(SDL_GPUTexture *texture, const SDL_Rect &region, SDL_GPUTransferBuffer *readback, const char *where);

  void release(SDL_GPUTexture *&texture);
  void release(SDL_GPUTransferBuffer *&buffer);
  void release(SDL_GPUBuffer *&buffer);
  void release(SDL_GPUGraphicsPipeline *&pipeline);

private:
  // Record the fault for the next process; refuse to start while one stands.
  void persistFault(const char *where, const char *detail);
  bool faultPreflight();
  void latchFault(const char *where, const char *detail);
  SDL_GPUSampler *makeSampler(SDL_GPUFilter filter);

  bool enabled_ = true;
  bool legResolved_ = false;
  bool headless_ = true;
  bool faulted_ = false;
  const char *windowTitle_ = nullptr;
  PixelSize headlessSink_;
  RenderWindow window_;
  SDL_GPUDevice *device_ = nullptr;
  SDL_GPUSampler *nearest_ = nullptr;
  SDL_GPUSampler *linear_ = nullptr;
  SDL_GPUGraphicsPipeline *presentPipeline_ = nullptr;
  SDL_GPUGraphicsPipeline *imagePipeline_ = nullptr;
  SDL_GPUGraphicsPipeline *sinkPipeline_ = nullptr;
  SDL_GPUGraphicsPipeline *panePipeline_ = nullptr;
  SDL_GPUTexture *whiteTexture_ = nullptr;
  std::unique_ptr<ui::RmlUiLibrary> rmlUi_;
};

} // namespace psx::render
