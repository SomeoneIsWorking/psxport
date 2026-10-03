// pane_composite.cpp — psxport::PaneCompositor: the host's own window frame, assembled from the
// presented pictures of several sessions. See pane_composite.h for what this is and is not.
#include "pane_composite.h"

#include "core.h"
#include "game.h"
#include "gpu_present_sink.h"
#include "gpu_vk.h"
#include "gpu_vk_device.h"
#include "gpu_vk_internal.h"
#include "image_writer.h"

#include "psxport_generated/gpu_vk_shaders.h"

#include <lucent/log.h>

#include <cstdlib>
#include <vector>

namespace psxport {
namespace {

constexpr SDL_GPUTextureFormat kCompositeFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

// How long a readback's fence may take to signal before the capture is refused rather than faked.
constexpr Uint64 kFenceBudgetMs = 5000;

// The vertex uniform: the pane rectangle's corner and its two vectors, plus the sink size they are
// expressed in (std140: three vec2s, 24 bytes, every field 16-byte aligned by construction).
struct PaneUbo {
  float origin[2];
  float axisU[2];
  float axisV[2];
  float viewport[2];
};

// The fragment uniform: which sub-rectangle of the pane's picture this draw samples, the tint, and
// the seam lines the pane's shape is cut to.
struct PanePc {
  float source[4]; // xy = UV origin, zw = UV size
  float tint[4];   // rgb = channel scale, w = desaturation
  float seam[4];   // left top, left bottom, right top, right bottom — sink pixels
  float alpha;     // 1 owns its pixels; below 1 blends over the pane beneath
};

SDL_GPUShader *makeShader(SDL_GPUDevice *dev,
                          const uint32_t *code,
                          unsigned len,
                          SDL_GPUShaderStage stage,
                          Uint32 num_samplers,
                          Uint32 num_uniform_buffers) {
  SDL_GPUShaderCreateInfo ci = {};
  ci.code_size = len;
  ci.code = reinterpret_cast<const Uint8 *>(code);
  ci.entrypoint = "main";
  ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
  ci.stage = stage;
  ci.num_samplers = num_samplers;
  ci.num_uniform_buffers = num_uniform_buffers;
  SDL_GPUShader *s = SDL_CreateGPUShader(dev, &ci);
  if (!s) {
    lucent::error("pane", "SDL_CreateGPUShader failed: {}", SDL_GetError());
  }
  return s;
}

SDL_GPUGraphicsPipeline *makePipeline(SDL_GPUDevice *dev, SDL_GPUTextureFormat format) {
  SDL_GPUShader *vs = makeShader(dev, spv_g_pane_vert, spv_g_pane_vert_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
  SDL_GPUShader *fs = makeShader(dev, spv_g_pane_frag, spv_g_pane_frag_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
  SDL_GPUColorTargetDescription ct = {};
  ct.format = format;
  // A picture pane and a divider both write alpha 1, so blending changes nothing for them; it is what
  // lets a caption backing strip sit ON a panel's picture instead of replacing it.
  ct.blend_state.enable_blend = true;
  ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  SDL_GPUGraphicsPipelineCreateInfo gp = {};
  gp.vertex_shader = vs;
  gp.fragment_shader = fs;
  gp.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  gp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  gp.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  gp.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  gp.target_info.color_target_descriptions = &ct;
  gp.target_info.num_color_targets = 1;
  SDL_GPUGraphicsPipeline *pipe = SDL_CreateGPUGraphicsPipeline(dev, &gp);
  if (!pipe) {
    lucent::error("pane", "SDL_CreateGPUGraphicsPipeline failed: {}", SDL_GetError());
  }
  SDL_ReleaseGPUShader(dev, vs);
  SDL_ReleaseGPUShader(dev, fs);
  return pipe;
}

// The composite → swapchain blit is the plain fullscreen image pass (spv_g_image_*): the composite
// image IS the sink's shape, so this is the 1:1 copy it claims to be. Windowed only, like every other
// pass that names the swapchain's format.
SDL_GPUGraphicsPipeline *makeBlitPipeline(SDL_GPUDevice *dev, SDL_GPUTextureFormat format) {
  SDL_GPUShader *vs = makeShader(dev, spv_g_image_vert, spv_g_image_vert_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader *fs = makeShader(dev, spv_g_image_frag, spv_g_image_frag_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
  SDL_GPUColorTargetDescription ct = {};
  ct.format = format;
  SDL_GPUGraphicsPipelineCreateInfo gp = {};
  gp.vertex_shader = vs;
  gp.fragment_shader = fs;
  gp.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  gp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  gp.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  gp.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  gp.target_info.color_target_descriptions = &ct;
  gp.target_info.num_color_targets = 1;
  SDL_GPUGraphicsPipeline *pipe = SDL_CreateGPUGraphicsPipeline(dev, &gp);
  if (!pipe) {
    lucent::error("pane", "swapchain blit pipeline failed: {}", SDL_GetError());
  }
  SDL_ReleaseGPUShader(dev, vs);
  SDL_ReleaseGPUShader(dev, fs);
  return pipe;
}

// The pipeline this object owns is created against the process device, so it dies with it: the
// destructor's job is to give the handles back, and to say so by name if the device is already gone.
void releaseTexture(SDL_GPUDevice *dev, SDL_GPUTexture *&texture) {
  if (texture && dev) {
    SDL_ReleaseGPUTexture(dev, texture);
  }
  texture = nullptr;
}

void releasePipeline(SDL_GPUDevice *dev, SDL_GPUGraphicsPipeline *&pipeline) {
  if (pipeline && dev) {
    SDL_ReleaseGPUGraphicsPipeline(dev, pipeline);
  }
  pipeline = nullptr;
}

} // namespace

PaneCompositor::PaneCompositor(GpuDevice &device, Core *hostCore) : m_device(device), m_hostCore(hostCore) {}

PaneCompositor::~PaneCompositor() {
  SDL_GPUDevice *dev = m_device.s_dev;
  releaseTexture(dev, m_image);
  releaseTexture(dev, m_white);
  releasePipeline(dev, m_panePipe);
  releasePipeline(dev, m_blitPipe);
  if (m_imageRb && dev) {
    SDL_ReleaseGPUTransferBuffer(dev, m_imageRb);
  }
  if (m_linear && dev) {
    SDL_ReleaseGPUSampler(dev, m_linear);
  }
}

void PaneCompositor::ensureState() {
  if (m_device.s_dev == nullptr) {
    lucent::error("pane", "no GPU device — bring one up with gpu_vk_ensure_device before compositing panes");
    return;
  }
  if (!gpu_vk_enabled()) {
    return;
  }
  if (!m_panePipe) {
    m_panePipe = makePipeline(m_device.s_dev, kCompositeFormat);
    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter = SDL_GPU_FILTER_LINEAR; // a pane scales a session's picture up to its own size
    si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    m_linear = SDL_CreateGPUSampler(m_device.s_dev, &si);
    if (!m_linear) {
      lucent::error("pane", "CreateGPUSampler(linear) failed: {}", SDL_GetError());
    }
    // A 1x1 white source, so an authored divider line is drawn through the SAME pane pass with its
    // colour in the tint instead of a second pipeline that would have to keep the same contract.
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = kCompositeFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = 1;
    ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    m_white = SDL_CreateGPUTexture(m_device.s_dev, &ti);
    if (m_white) {
      SDL_GPUTransferBufferCreateInfo up = {};
      up.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
      up.size = 4;
      SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(m_device.s_dev, &up);
      if (xfer) {
        void *mapped = SDL_MapGPUTransferBuffer(m_device.s_dev, xfer, false);
        if (mapped) {
          static_cast<uint8_t *>(mapped)[0] = 255;
          static_cast<uint8_t *>(mapped)[1] = 255;
          static_cast<uint8_t *>(mapped)[2] = 255;
          static_cast<uint8_t *>(mapped)[3] = 255;
          SDL_UnmapGPUTransferBuffer(m_device.s_dev, xfer);
        }
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device.s_dev);
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion white = {};
        white.texture = m_white;
        white.w = 1;
        white.h = 1;
        white.d = 1;
        SDL_GPUTextureTransferInfo from = {};
        from.transfer_buffer = xfer;
        from.pixels_per_row = 4;
        from.rows_per_layer = 1;
        SDL_UploadToGPUTexture(cp, &from, &white, false);
        SDL_EndGPUCopyPass(cp);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(m_device.s_dev, xfer);
      }
    }
  }
  // The swapchain blit is the ONE leg-dependent pipeline, so it is created in the leg that has a
  // swapchain — the same rule gpu_vk.cpp follows for its own image pipeline.
  if (!m_blitPipe && !m_device.s_headless) {
    m_blitPipe = makeBlitPipeline(m_device.s_dev, m_device.s_window.swapchain_format());
  }
}

void PaneCompositor::ensureImage(int w, int h) {
  if (w <= 0 || h <= 0) {
    return;
  }
  if (m_image && m_imageW == w && m_imageH == h) {
    return;
  }
  SDL_GPUDevice *dev = m_device.s_dev;
  releaseTexture(dev, m_image);
  if (m_imageRb) {
    SDL_ReleaseGPUTransferBuffer(dev, m_imageRb);
    m_imageRb = nullptr;
  }
  SDL_GPUTextureCreateInfo ti = {};
  ti.type = SDL_GPU_TEXTURETYPE_2D;
  ti.format = kCompositeFormat;
  ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  ti.width = (Uint32)w;
  ti.height = (Uint32)h;
  ti.layer_count_or_depth = 1;
  ti.num_levels = 1;
  m_image = SDL_CreateGPUTexture(dev, &ti);
  if (!m_image) {
    lucent::error("pane", "CreateGPUTexture(composite) failed: {}", SDL_GetError());
    return;
  }
  SDL_GPUTransferBufferCreateInfo dn = {};
  dn.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  dn.size = (Uint32)w * (Uint32)h * 4;
  m_imageRb = SDL_CreateGPUTransferBuffer(dev, &dn);
  if (!m_imageRb) {
    lucent::error("pane", "composite readback transfer buffer failed: {}", SDL_GetError());
  }
  m_imageW = w;
  m_imageH = h;
  lucent::info("pane", "composite image {}x{}", w, h);
}

void PaneCompositor::composite(std::span<const Pane> panes, const PaneOverlayPass &overlay) {
  if (!gpu_vk_enabled()) {
    return;
  }
  ensureState();
  if (!m_panePipe) {
    return; // no pipeline: the device refused it, and there is nothing to draw with
  }
  int sinkW = 0, sinkH = 0;
  gpu_vk_present_sink_size(&sinkW, &sinkH);
  ensureImage(sinkW, sinkH);
  if (!m_image || !m_imageRb) {
    return;
  }

  SDL_GPUDevice *dev = m_device.s_dev;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
  if (!cmd) {
    lucent::error("pane", "AcquireGPUCommandBuffer failed: {}", SDL_GetError());
    return;
  }
  SDL_GPUColorTargetInfo cti = {};
  cti.texture = m_image;
  cti.clear_color = (SDL_FColor){0, 0, 0, 1};
  cti.load_op = SDL_GPU_LOADOP_CLEAR;
  cti.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &cti, 1, nullptr);
  SDL_GPUViewport vp = {0.0f, 0.0f, (float)sinkW, (float)sinkH, 0.0f, 1.0f};
  SDL_SetGPUViewport(rp, &vp);
  SDL_Rect scissor = {0, 0, sinkW, sinkH};
  SDL_SetGPUScissor(rp, &scissor);
  SDL_BindGPUGraphicsPipeline(rp, m_panePipe);

  unsigned drawn = 0;
  unsigned empty = 0;
  for (const Pane &pane : panes) {
    // A SOLID pane is host furniture (a divider, a backdrop), not a session's picture: it draws the
    // compositor's own white source in its tint and names no Core. A pane with a HOST texture draws
    // that texture from the whole of it, at its own size.
    SDL_GPUTexture *source = m_white;
    float sourceRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    if (pane.texture != nullptr) {
      source = pane.texture;
    } else if (!pane.solid) {
      // A pane with no session, or one whose session has not presented yet, draws NOTHING. Filling it
      // would be a panel that looks like a title that booted and showed nothing.
      GpuVkState::PresentedImage image{};
      if (pane.core != nullptr) {
        // THE HELD PICTURE, not the newest frame. A session between its own scenes — a fade, a load
        // screen — presents black, and a panel that showed that would read as a title that stopped.
        // The held frame is the newest one this session actually had a picture in.
        image = pane.core->game->gpu_vk.lastFilledPresented();
        if (!image.valid()) {
          image = pane.core->game->gpu_vk.lastPresented();
        }
      }
      if (!image.valid()) {
        ++empty;
        continue;
      }
      source = image.texture;
      // WHERE THE PICTURE IS: the content rect when a probe has measured one — the viewport minus the
      // black border rows the title draws inside its own frame — and the viewport itself before that,
      // which already excludes the letterbox bar the present plan put around it. Showing the border
      // rows is how a panel ends up with black bars across its own middle.
      const PaneRect picture = image.content.w > 0 && image.content.h > 0 ? image.content : image.viewport;
      const float pictureLeft = (float)picture.x / (float)image.width;
      const float pictureTop = (float)picture.y / (float)image.height;
      const float pictureWidth = (float)picture.w / (float)image.width;
      const float pictureHeight = (float)picture.h / (float)image.height;
      // The HOST decides which part of the picture a pane shows (pane.sourceU/V/W/H); this only maps
      // that fraction onto the picture's viewport rectangle, so the crop rule lives in one place — the
      // host's layout, which can be tested — and this stays a compositor.
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
    ubo.viewport[0] = (float)sinkW;
    ubo.viewport[1] = (float)sinkH;
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

    SDL_GPUTextureSamplerBinding binding = {source, m_linear};
    SDL_BindGPUFragmentSamplers(rp, 0, &binding, 1);
    SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
    ++drawn;
  }
  // The host's own UI, in the same pass, so it lands ON the panes rather than in a second blit.
  if (overlay) {
    overlay(cmd, rp, sinkW, sinkH);
  }
  SDL_EndGPURenderPass(rp);

  if (m_device.s_headless) {
    SDL_SubmitGPUCommandBuffer(cmd);
  } else {
    showToWindow(cmd);
  }
  m_frames++;
  lucent::debug("pane", "frame {}: {} pane(s) drawn, {} empty, {}x{}", m_frames, drawn, empty, sinkW, sinkH);
}

void PaneCompositor::showToWindow(SDL_GPUCommandBuffer *cmd) {
  SDL_GPUDevice *dev = m_device.s_dev;
  Uint32 w = 0, h = 0;
  SDL_GPUTexture *swap = sink_acquire(m_sink, cmd, m_device.s_window.window(), &w, &h);
  if (!swap) {
    // An occluded window or frames in flight: an idle sink, not a stuck host. The guest sessions keep
    // stepping; the next composite will find an image again.
    SDL_SubmitGPUCommandBuffer(cmd);
    return;
  }
  if (!m_blitPipe) {
    m_blitPipe = makeBlitPipeline(dev, m_device.s_window.swapchain_format());
  }
  SDL_GPUColorTargetInfo cti = {};
  cti.texture = swap;
  cti.clear_color = (SDL_FColor){0, 0, 0, 1};
  cti.load_op = SDL_GPU_LOADOP_CLEAR;
  cti.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &cti, 1, nullptr);
  if (m_blitPipe && m_image && m_linear) {
    const float brightness[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    SDL_PushGPUFragmentUniformData(cmd, 0, brightness, sizeof brightness);
    SDL_GPUViewport vp = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    SDL_SetGPUViewport(rp, &vp);
    SDL_Rect scissor = {0, 0, (int)w, (int)h};
    SDL_SetGPUScissor(rp, &scissor);
    SDL_BindGPUGraphicsPipeline(rp, m_blitPipe);
    SDL_GPUTextureSamplerBinding binding = {m_image, m_linear};
    SDL_BindGPUFragmentSamplers(rp, 0, &binding, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
  }
  SDL_EndGPURenderPass(rp);
  SDL_SubmitGPUCommandBuffer(cmd);
  // The window's own events (close, ESC, and the key states the sessions' pads must learn) belong to
  // whoever shows the window, which from here is this object rather than any one session.
  if (m_hostCore != nullptr) {
    gpu_vk_pump_host_events(m_hostCore);
  }
}

void PaneCompositor::presentShot(const char *path) {
  if (!gpu_vk_enabled() || !m_image || !m_imageRb) {
    lucent::warn("pane", "no composited frame yet — NOTHING captured");
    return;
  }
  const int w = m_imageW, h = m_imageH;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device.s_dev);
  if (!cmd) {
    lucent::error("pane", "AcquireGPUCommandBuffer failed: {}", SDL_GetError());
    return;
  }
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion source = {};
  source.texture = m_image;
  source.w = (Uint32)w;
  source.h = (Uint32)h;
  source.d = 1;
  SDL_GPUTextureTransferInfo destination = {};
  destination.transfer_buffer = m_imageRb;
  destination.pixels_per_row = (Uint32)w;
  destination.rows_per_layer = (Uint32)h;
  SDL_DownloadFromGPUTexture(cp, &source, &destination);
  SDL_EndGPUCopyPass(cp);
  // A readback that is not fenced would read whatever the GPU had finished, not this frame. The wait is
  // BOUNDED for the same reason the renderer's is: a fence that never signals is a hung device, and
  // reading the buffer anyway would capture an older frame presented as this one.
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (!fence) {
    lucent::error(
        "pane", "NOTHING captured for {} — the GPU refused the submit: {}", path ? path : "(null)", SDL_GetError());
    return;
  }
  const Uint64 start = SDL_GetTicks();
  bool signalled = false;
  while (!signalled) {
    signalled = SDL_QueryGPUFence(m_device.s_dev, fence);
    if (!signalled && SDL_GetTicks() - start >= kFenceBudgetMs) {
      break;
    }
    if (!signalled) {
      SDL_Delay(1);
    }
  }
  SDL_ReleaseGPUFence(m_device.s_dev, fence);
  if (!signalled) {
    lucent::error("pane",
                  "NOTHING captured for {} — the GPU fence did not signal within {} ms",
                  path ? path : "(null)",
                  (unsigned long long)kFenceBudgetMs);
    return;
  }
  const uint8_t *rgba = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(m_device.s_dev, m_imageRb, false));
  if (!rgba) {
    lucent::error("pane", "NOTHING captured for {} — the transfer buffer would not map", path ? path : "(null)");
    return;
  }
  std::vector<unsigned char> rgb((size_t)w * h * 3);
  long nonblack = 0;
  for (size_t i = 0; i < (size_t)w * h; ++i) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
    if (rgba[i * 4] || rgba[i * 4 + 1] || rgba[i * 4 + 2]) {
      ++nonblack;
    }
  }
  SDL_UnmapGPUTransferBuffer(m_device.s_dev, m_imageRb);
  const bool wrote = image_write_rgb24(path, rgb.data(), w, h);
  lucent::info("pane",
               "{} {} ({}x{}) non-black {}/{} ({:.2f}%)",
               wrote ? "wrote" : "NOTHING captured for",
               path ? path : "(null)",
               w,
               h,
               nonblack,
               (long)w * h,
               100.0 * (double)nonblack / ((double)w * (double)h));
}

} // namespace psxport
