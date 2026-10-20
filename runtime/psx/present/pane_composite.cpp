// pane_composite.cpp — see pane_composite.h.
#include "pane_composite.h"

#include "core.h"
#include "game.h"
#include "render.h"
#include "render_device.h"
#include "render_state.h"

#include "psxport_generated/render_shaders.h"

#include <lucent/log.h>

#include <chrono>

namespace psxport {
namespace {

// pane.vert: the pane rectangle's corner, its two edge vectors and the sink size (std140).
struct PaneUbo {
  float origin[2];
  float axisU[2];
  float axisV[2];
  float viewport[2];
};

// pane.frag: the sampled sub-rectangle, the tint, and the seam lines the shape is cut to.
struct PanePc {
  float source[4]; // xy = UV origin, zw = UV size
  float tint[4];   // rgb = channel scale, w = desaturation
  float seam[4];   // left top, left bottom, right top, right bottom, in sink pixels
  float alpha;
};

constexpr psx::render::ShaderCode kPaneVertex{spv_g_pane_vert, spv_g_pane_vert_len, {0, 1, 0}};
constexpr psx::render::ShaderCode kPaneFragment{spv_g_pane_frag, spv_g_pane_frag_len, {1, 1, 0}};

} // namespace

PaneCompositor::PaneCompositor(psx::render::RenderDevice &device, Core *hostCore)
    : m_device(device), m_hostCore(hostCore) {}

PaneCompositor::~PaneCompositor() {
  m_device.release(m_image);
  m_device.release(m_white);
  m_device.release(m_panePipe);
  m_device.release(m_imageRb);
}

void PaneCompositor::ensureState() {
  if (m_panePipe != nullptr) {
    return;
  }
  // Opaque panes write alpha 1, so blending only matters for furniture laid over a picture.
  SDL_GPUColorTargetBlendState blend = {};
  blend.enable_blend = true;
  blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
  blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  m_panePipe = m_device.makePipeline(kPaneVertex, kPaneFragment, psx::render::kPresentImageFormat, &blend);
  const std::uint8_t white[4] = {255, 255, 255, 255};
  m_white = m_device.makeFilledTexture({1, 1}, white);
}

void PaneCompositor::ensureImage(psx::present::PixelSize size) {
  if (size.width <= 0 || size.height <= 0 ||
      (m_image != nullptr && m_imageW == size.width && m_imageH == size.height)) {
    return;
  }
  m_device.release(m_image);
  m_device.release(m_imageRb);
  m_image = m_device.makeTexture(
      size, psx::render::kPresentImageFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
  m_imageRb =
      m_device.makeTransferBuffer(SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                  static_cast<std::uint32_t>(size.width) * static_cast<std::uint32_t>(size.height) * 4);
  m_imageW = size.width;
  m_imageH = size.height;
  lucent::info("pane", "composite image {}x{}", size.width, size.height);
}

void PaneCompositor::composite(std::span<const Pane> panes, const PaneOverlayPass &overlay) {
  if (!m_device.enabled()) {
    return;
  }
  ensureState();
  const psx::present::PixelSize sink = m_device.sinkSize();
  ensureImage(sink);
  if (m_image == nullptr) {
    return;
  }
  SDL_GPUCommandBuffer *cmd = m_device.acquire();
  SDL_GPURenderPass *rp = psx::render::beginClearedPass(cmd, m_image);
  SDL_GPUViewport vp = {0.0f, 0.0f, (float)sink.width, (float)sink.height, 0.0f, 1.0f};
  SDL_SetGPUViewport(rp, &vp);
  SDL_Rect scissor = {0, 0, sink.width, sink.height};
  SDL_SetGPUScissor(rp, &scissor);
  SDL_BindGPUGraphicsPipeline(rp, m_panePipe);

  unsigned drawn = 0;
  unsigned empty = 0;
  for (const Pane &pane : panes) {
    SDL_GPUTexture *source = m_white;
    float sourceRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    if (pane.texture != nullptr) {
      source = pane.texture;
    } else if (!pane.solid) {
      // A session that has not presented draws nothing rather than pass for a title that booted.
      const psx::render::PresentedImage image =
          pane.core != nullptr ? pane.core->game->render().newestPresentedPicture() : psx::render::PresentedImage{};
      if (!image.valid()) {
        ++empty;
        continue;
      }
      source = image.texture;
      const psx::present::PaneRect picture = image.picture();
      const float pictureLeft = (float)picture.x / (float)image.width;
      const float pictureTop = (float)picture.y / (float)image.height;
      const float pictureWidth = (float)picture.w / (float)image.width;
      const float pictureHeight = (float)picture.h / (float)image.height;
      sourceRect[0] = pictureLeft + pane.sourceU * pictureWidth;
      sourceRect[1] = pictureTop + pane.sourceV * pictureHeight;
      sourceRect[2] = pane.sourceW * pictureWidth;
      sourceRect[3] = pane.sourceH * pictureHeight;
    }
    PaneUbo ubo{};
    ubo.origin[0] = pane.originX;
    ubo.origin[1] = pane.originY;
    ubo.axisU[0] = pane.axisUX;
    ubo.axisU[1] = pane.axisUY;
    ubo.axisV[0] = pane.axisVX;
    ubo.axisV[1] = pane.axisVY;
    ubo.viewport[0] = (float)sink.width;
    ubo.viewport[1] = (float)sink.height;
    SDL_PushGPUVertexUniformData(cmd, 0, &ubo, sizeof ubo);

    PanePc pc{};
    pc.source[0] = sourceRect[0];
    pc.source[1] = sourceRect[1];
    pc.source[2] = sourceRect[2];
    pc.source[3] = sourceRect[3];
    pc.tint[0] = pane.tintR;
    pc.tint[1] = pane.tintG;
    pc.tint[2] = pane.tintB;
    pc.tint[3] = pane.desaturation;
    pc.seam[0] = pane.seamLeftTop;
    pc.seam[1] = pane.seamLeftBottom;
    pc.seam[2] = pane.seamRightTop;
    pc.seam[3] = pane.seamRightBottom;
    pc.alpha = pane.alpha;
    SDL_PushGPUFragmentUniformData(cmd, 0, &pc, sizeof pc);

    SDL_GPUTextureSamplerBinding binding = {source, m_device.linearSampler()};
    SDL_BindGPUFragmentSamplers(rp, 0, &binding, 1);
    SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
    ++drawn;
  }
  if (overlay) {
    overlay(cmd, rp, sink.width, sink.height);
  }
  SDL_EndGPURenderPass(rp);
  m_device.submit(cmd, "pane composite");
  m_showPending = m_device.windowed();
  m_frames++;
  lucent::debug("pane", "frame {}: {} pane(s) drawn, {} empty, {}x{}", m_frames, drawn, empty, sink.width, sink.height);
}

void PaneCompositor::show() {
  if (!m_showPending) {
    return;
  }
  m_showPending = false;
  SDL_GPUCommandBuffer *cmd = m_device.acquire();
  Uint32 w = 0;
  Uint32 h = 0;
  SDL_GPUTexture *swap = psx::render::acquireSink(m_sink, cmd, m_device.window().window(), &w, &h);
  if (swap == nullptr) {
    m_device.submit(cmd, "pane show");
    return;
  }
  const psx::present::PixelSize window{static_cast<int>(w), static_cast<int>(h)};
  SDL_GPURenderPass *rp = psx::render::beginClearedPass(cmd, swap);
  const float brightness[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  psx::render::drawFullscreen(cmd,
                              rp,
                              m_device.sinkPipeline(),
                              {m_image, m_device.linearSampler()},
                              std::as_bytes(std::span(brightness)),
                              {0, 0, window.width, window.height},
                              window);
  SDL_EndGPURenderPass(rp);
  m_device.submit(cmd, "pane show");
  // The window's events belong to whoever shows it, which is this object rather than any one session.
  if (m_hostCore != nullptr) {
    m_hostCore->game->perf.presented(std::chrono::steady_clock::now(), m_device.window().refreshPeriodMs());
    m_hostCore->game->hostInput.drainEvents();
  }
}

void PaneCompositor::presentShot(const char *path) {
  if (!m_device.enabled() || m_image == nullptr) {
    lucent::warn("pane", "no composited frame yet, nothing captured for {}", path);
    return;
  }
  const psx::render::MappedTransfer rgba =
      m_device.download(m_image, {0, 0, m_imageW, m_imageH}, m_imageRb, "pane shot");
  if (!rgba) {
    lucent::error("pane", "the GPU refused the readback, nothing captured for {}", path);
    return;
  }
  psx::render::writePresentShot("pane", path, rgba.bytes(), {m_imageW, m_imageH});
}

} // namespace psxport
