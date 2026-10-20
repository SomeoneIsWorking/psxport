// present_sink.h — the window sink, which never waits for the window, and the fullscreen passes into it.
#pragma once
#include "pixel_size.h"
#include "viewport_layout.h"

#include <SDL3/SDL_gpu.h>

#include <lucent/log.h>

#include <span>

namespace psx::render {

// Idle/resume latch over the per-field sink attempts: only the transitions are reported.
struct SinkIdleState {
  long skipped = 0;
  long presented = 0;
  bool idle = false;

  // True when the idle state changed.
  bool observe(bool imageAvailable) {
    if (imageAvailable) {
      presented++;
      if (!idle) {
        return false;
      }
      idle = false;
      return true;
    }
    skipped++;
    if (idle) {
      return false;
    }
    idle = true;
    return true;
  }
};

// The swapchain image without waiting: a window nobody shows never becomes ready. Null skips this field's sink.
inline SDL_GPUTexture *
acquireSink(SinkIdleState &state, SDL_GPUCommandBuffer *cmd, SDL_Window *window, Uint32 *w, Uint32 *h) {
  SDL_GPUTexture *texture = nullptr;
  const bool available = SDL_AcquireGPUSwapchainTexture(cmd, window, &texture, w, h) && texture != nullptr;
  if (state.observe(available)) {
    if (available) {
      lucent::info("sinkidle", "sink resumed (skipped {} field(s), presented {})", state.skipped, state.presented);
    } else {
      lucent::info("sinkidle",
                   "sink idle, no swapchain image (skipped {}, presented {}); the guest keeps running; SDL: {}",
                   state.skipped,
                   state.presented,
                   SDL_GetError());
    }
  }
  return available ? texture : nullptr;
}

// A render pass into `target`, cleared to opaque black.
inline SDL_GPURenderPass *beginClearedPass(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target) {
  SDL_GPUColorTargetInfo colour = {};
  colour.texture = target;
  colour.clear_color = SDL_FColor{0, 0, 0, 1};
  colour.load_op = SDL_GPU_LOADOP_CLEAR;
  colour.store_op = SDL_GPU_STOREOP_STORE;
  return SDL_BeginGPURenderPass(cmd, &colour, 1, nullptr);
}

// One fullscreen-triangle draw of `source` into `viewport` of an open pass, scissored to `target`.
inline void drawFullscreen(SDL_GPUCommandBuffer *cmd,
                           SDL_GPURenderPass *pass,
                           SDL_GPUGraphicsPipeline *pipeline,
                           SDL_GPUTextureSamplerBinding source,
                           std::span<const std::byte> uniform,
                           present::PaneRect viewport,
                           present::PixelSize target) {
  SDL_PushGPUFragmentUniformData(cmd, 0, uniform.data(), static_cast<Uint32>(uniform.size()));
  const SDL_GPUViewport area = {static_cast<float>(viewport.x),
                                static_cast<float>(viewport.y),
                                static_cast<float>(viewport.w),
                                static_cast<float>(viewport.h),
                                0.0f,
                                1.0f};
  const SDL_Rect scissor = {0, 0, target.width, target.height};
  SDL_BindGPUGraphicsPipeline(pass, pipeline);
  SDL_SetGPUViewport(pass, &area);
  SDL_SetGPUScissor(pass, &scissor);
  SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

} // namespace psx::render
