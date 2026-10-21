// render_panes.cpp — a present image built from panes: several sessions' pictures and host furniture.
#include "render_device.h"
#include "render_pane.h"
#include "render_state.h"

#include <lucent/log.h>

#include <SDL3/SDL_gpu.h>

namespace psx::render {
namespace {

// pane.vert (std140): the rectangle's corner, its two edge vectors and the target size.
struct PaneUbo {
  float origin[2];
  float axisU[2];
  float axisV[2];
  float viewport[2];
};

// pane.frag: the tint and the seam lines.
struct PanePc {
  float tint[4]; // rgb = channel scale, w = desaturation
  float seam[4]; // left top, left bottom, right top, right bottom
};

} // namespace

void RenderState::buildPanes(std::span<const Pane> panes) {
  if (!device_.enabled()) {
    return;
  }
  bringUp();
  const PixelSize size = presentImageSize();
  ensurePresentImage(size);
  if (presentImage_ == nullptr) {
    return;
  }
  SDL_GPUCommandBuffer *cmd = device_.acquire();
  SDL_GPURenderPass *pass = beginClearedPass(cmd, presentImage_);
  const SDL_GPUViewport viewport = {
      0.0f, 0.0f, static_cast<float>(size.width), static_cast<float>(size.height), 0.0f, 1.0f};
  SDL_SetGPUViewport(pass, &viewport);
  const SDL_Rect scissor = {0, 0, size.width, size.height};
  SDL_SetGPUScissor(pass, &scissor);
  SDL_BindGPUGraphicsPipeline(pass, device_.panePipeline());
  int drawn = 0;
  for (const Pane &pane : panes) {
    SDL_GPUTexture *source = pane.solid ? device_.whiteTexture() : pane.texture;
    if (source == nullptr) {
      continue;
    }
    const PaneUbo ubo{{pane.x, pane.y},
                      {pane.width, 0.0f},
                      {0.0f, pane.height},
                      {static_cast<float>(size.width), static_cast<float>(size.height)}};
    SDL_PushGPUVertexUniformData(cmd, 0, &ubo, sizeof ubo);
    const PanePc pc{{pane.tintR, pane.tintG, pane.tintB, pane.desaturation},
                    {pane.seamLeftTop, pane.seamLeftBottom, pane.seamRightTop, pane.seamRightBottom}};
    SDL_PushGPUFragmentUniformData(cmd, 0, &pc, sizeof pc);
    const SDL_GPUTextureSamplerBinding binding = {source, device_.nearestSampler()};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    ++drawn;
  }
  SDL_EndGPURenderPass(pass);
  device_.submit(cmd, "panes build");
  viewport_ = present::PaneRect{0, 0, size.width, size.height};
  ++presentSerial_;
  showPending_ = target_ == PresentTarget::Swapchain;
  lucent::debug("pane", "{} of {} pane(s) drawn into {}x{}", drawn, panes.size(), size.width, size.height);
}

} // namespace psx::render
