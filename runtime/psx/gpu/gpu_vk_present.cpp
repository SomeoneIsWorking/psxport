// gpu_vk_present.cpp — one Game's present in two steps: build the display picture (the record image on
// RenderPath::Record, the device's VRAM on Device and for a 24bpp display, or a host picture) letterboxed
// into the present image, then show that image in the window.
//
// The picture is built identically windowed and headless; the leg decides only whether the built image
// reaches a window (present_plan.h).
#include "core.h"
#include "fs_util.h"
#include "game.h"
#include "gpu_device.h"
#include "gpu_vk.h"
#include "gpu_vk_check.h"
#include "gpu_vk_device.h"
#include "gpu_vk_internal.h"
#include "overlay_glue.h"
#include "present_plan.h"

#include <lucent/log.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kVramW = psx::gpu::kRecordVramWidth;
constexpr int kVramH = psx::gpu::kRecordVramHeight;

// Matches present.frag's `PC { ivec4 disp; ivec4 fade; ivec4 fmt; }`; fmt.x = 1 for a 24bpp display.
struct PresentUniform {
  int32_t disp[4];
  int32_t fade[4];
  int32_t fmt[4];
};

SDL_GPUViewport viewportOf(PaneRect r) {
  return SDL_GPUViewport{(float)r.x, (float)r.y, (float)r.w, (float)r.h, 0.0f, 1.0f};
}

// The plan's inputs out of renderer state; psx::present::planPresent() stays the pure decision.
// `display` is the VRAM rect presented, `nativeW` its width before widening.
PresentInputs presentInputs(const GpuVkState &g, psx::gpu::VramRect display, int nativeW) {
  PresentInputs in{};
  const psx::present::PixelSize sink = g.device->sinkSize();
  in.sink_w = sink.width;
  in.sink_h = sink.height;
  in.sx = display.x;
  in.sy = display.y;
  in.disp_w = display.width;
  in.disp_h = display.height;
  // The game's own 4:3 width before widening: what keeps a native frame of any width at 4:3.
  in.native_w = nativeW;
  // A picture the title says covers only the native width is sampled over those columns alone.
  if (g.game->runtime.guestPictureIsNativeWidth(*g.game)) {
    in.content_w = in.native_w;
  }
  in.disp_rgb24 = g.game->core.gpuDevice().displayArea().depth24 ? 1 : 0;
  // A pane builds its image at the size its host asked for.
  if (g.s_present_img_requested.width > 0 && g.s_present_img_requested.height > 0) {
    in.sink_w = g.s_present_img_requested.width;
    in.sink_h = g.s_present_img_requested.height;
    // A pane is a slice of a host's composite, sized to the panel it will occupy and drawn over the
    // whole of it, so the picture fills the pane and is cropped to the panel's aspect here — at the
    // resolution it was rendered — rather than letterboxed into a band the player is not watching.
    in.fill = 1;
  }
  return in;
}

} // namespace

void GpuVkState::bring_up() {
  device->bringUp();
  // The overlay exists in both legs; it takes the sink's size and the format of the pass it records into.
  if (s_overlay_bound != 0) {
    return;
  }
  s_overlay_bound = 1;
  const psx::present::PixelSize sink = device->sinkSize();
  psx::ui::initOverlay(game,
                       device->s_window.window(),
                       device->s_dev,
                       device->s_headless != 0 ? kPresentImageFormat : device->s_window.swapchain_format(),
                       sink.width,
                       sink.height);
}

void GpuVkState::ensure_targets() {
  if (s_vram_tex != nullptr) {
    return;
  }
  // R8G8: SDL_GPU forbids sampling integer formats, so the 1555 word rides as two bytes.
  SDL_GPUTextureCreateInfo texture = {};
  texture.type = SDL_GPU_TEXTURETYPE_2D;
  texture.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
  texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  texture.width = kVramW;
  texture.height = kVramH;
  texture.layer_count_or_depth = 1;
  texture.num_levels = 1;
  s_vram_tex = SDL_CreateGPUTexture(device->s_dev, &texture);
  GPUCHK(s_vram_tex, "CreateGPUTexture(VRAM)");
  SDL_GPUTransferBufferCreateInfo upload = {};
  upload.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  upload.size = kVramW * kVramH * 2;
  s_vram_xfer = SDL_CreateGPUTransferBuffer(device->s_dev, &upload);
  GPUCHK(s_vram_xfer, "CreateGPUTransferBuffer(VRAM)");
}

void GpuVkState::upload_device_vram(SDL_GPUCommandBuffer *cmd, std::span<const std::uint16_t> vram) {
  ensure_targets();
  void *staging = SDL_MapGPUTransferBuffer(device->s_dev, s_vram_xfer, true);
  std::memcpy(staging, vram.data(), static_cast<std::size_t>(kVramW) * kVramH * 2);
  SDL_UnmapGPUTransferBuffer(device->s_dev, s_vram_xfer);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo source = {};
  source.transfer_buffer = s_vram_xfer;
  source.pixels_per_row = kVramW;
  source.rows_per_layer = kVramH;
  SDL_GPUTextureRegion destination = {};
  destination.texture = s_vram_tex;
  destination.w = kVramW;
  destination.h = kVramH;
  destination.d = 1;
  SDL_UploadToGPUTexture(copy, &source, &destination, false);
  SDL_EndGPUCopyPass(copy);
}

void GpuVkState::ensure_present_img(int w, int h) {
  if (w <= 0 || h <= 0) {
    return;
  }
  if (s_present_img != nullptr && s_present_img_w == w && s_present_img_h == h) {
    return;
  }
  if (s_present_img != nullptr) {
    SDL_ReleaseGPUTexture(device->s_dev, s_present_img);
    SDL_ReleaseGPUTransferBuffer(device->s_dev, s_present_rb);
  }
  SDL_GPUTextureCreateInfo texture = {};
  texture.type = SDL_GPU_TEXTURETYPE_2D;
  texture.format = kPresentImageFormat;
  texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  texture.width = static_cast<Uint32>(w);
  texture.height = static_cast<Uint32>(h);
  texture.layer_count_or_depth = 1;
  texture.num_levels = 1;
  s_present_img = SDL_CreateGPUTexture(device->s_dev, &texture);
  GPUCHK(s_present_img, "CreateGPUTexture(present img)");
  SDL_GPUTransferBufferCreateInfo download = {};
  download.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  download.size = static_cast<Uint32>(w) * static_cast<Uint32>(h) * 4U;
  s_present_rb = SDL_CreateGPUTransferBuffer(device->s_dev, &download);
  GPUCHK(s_present_rb, "present img readback xfer");
  s_present_img_w = w;
  s_present_img_h = h;
  // A new image is a new picture: the measured content belonged to the old size.
  s_present_content = PaneRect{0, 0, 0, 0};
  lucent::info("gpu_vk", "present image {}x{} ({} sink)", w, h, device->s_headless != 0 ? "headless" : "windowed");
}

void GpuVkState::build_display(int sx, int sy, int w, int h) {
  if (!device->enabled()) {
    return;
  }
  bring_up();
  const int dispW = psx::present::presentDisplayWidth(
      psx::present::widePresentation(game->core) != 0, psx::present::widePresentationWidth(game->core), w);
  s_last_sx = sx;
  s_last_sy = sy;
  s_last_w = dispW;
  s_last_h = h;
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device->s_dev);
  GPUCHK(cmd, "AcquireGPUCommandBuffer");
  PresentInputs inputs = presentInputs(*this, {sx, sy, dispW, h}, w);
  present_record(inputs);
  if (!s_present_record) {
    upload_device_vram(cmd, game->core.gpuDevice().vram());
  }
  const PresentPlan plan = psx::present::planPresent(inputs, device->s_headless != 0);
  s_present_viewport = plan.viewport;
  ++s_present_serial;
  lucent::debug("present",
                "sink {}x{} pane={} swapchain={} src_ires={} rgb24={} disp={},{},{},{} viewport={},{},{},{}",
                plan.sink_w,
                plan.sink_h,
                s_present_target == PresentTarget::Pane ? "yes" : "no",
                plan.to_swapchain ? "yes" : "no",
                plan.src_ires,
                plan.fmt[0],
                plan.disp[0],
                plan.disp[1],
                plan.disp[2],
                plan.disp[3],
                plan.viewport.x,
                plan.viewport.y,
                plan.viewport.w,
                plan.viewport.h);
  if (plan.build) {
    build_present_image(cmd, plan);
  }
  device->submit(cmd, "present build");
  // A pane's window belongs to the host compositing it, so the host composites here and shows at show().
  const bool toPaneHost = s_present_target == PresentTarget::Pane && s_pane_host.attached();
  s_show_pending = toPaneHost || (plan.to_swapchain && s_present_target == PresentTarget::Swapchain);
  if (toPaneHost) {
    s_pane_host.built();
  }
}

void GpuVkState::show() {
  if (!s_show_pending) {
    return;
  }
  s_show_pending = false;
  if (s_present_target == PresentTarget::Pane) {
    if (s_pane_host.attached()) {
      s_pane_host.show();
    }
    return;
  }
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device->s_dev);
  GPUCHK(cmd, "AcquireGPUCommandBuffer");
  show_present_image(cmd);
}

// Samples the display region psx::present::planPresent() chose into the present image, letterboxed, with the fade.
void GpuVkState::build_present_image(SDL_GPUCommandBuffer *cmd, const PresentPlan &plan) {
  ensure_targets();
  ensure_present_img(plan.sink_w, plan.sink_h);
  if (s_present_img == nullptr) {
    return;
  }
  SDL_GPUColorTargetInfo target = {};
  target.texture = s_present_img;
  target.clear_color = (SDL_FColor){0, 0, 0, 1};
  target.load_op = SDL_GPU_LOADOP_CLEAR; // paints the letterbox bars
  target.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
  SDL_GPUTexture *source = s_present_record ? s_record->presented().texture : s_vram_tex;
  PresentUniform uniform{};
  for (int i = 0; i < 4; i++) {
    uniform.disp[i] = plan.disp[i];
    uniform.fade[i] = plan.fade[i];
    uniform.fmt[i] = plan.fmt[i];
  }
  SDL_PushGPUFragmentUniformData(cmd, 0, &uniform, sizeof uniform);
  const SDL_GPUViewport viewport = viewportOf(plan.viewport);
  const SDL_Rect scissor = {0, 0, s_present_img_w, s_present_img_h};
  SDL_BindGPUGraphicsPipeline(pass, device->s_present_pipe);
  SDL_SetGPUViewport(pass, &viewport);
  SDL_SetGPUScissor(pass, &scissor);
  const SDL_GPUTextureSamplerBinding binding = {source, device->s_samp_nearest};
  SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
}

// The sink: blit the finished picture into the window, letterboxed, with the overlay on top. Acquires
// without waiting, because a window nobody shows never becomes ready (gpu_present_sink.h).
void GpuVkState::show_present_image(SDL_GPUCommandBuffer *cmd) {
  Uint32 sw = 0;
  Uint32 sh = 0;
  SDL_GPUTexture *swapchain = psx::gpu::acquireSink(s_sink, cmd, device->s_window.window(), &sw, &sh);
  if (swapchain == nullptr) {
    device->submit(cmd, "show_present_image");
    game->hostInput.drainEvents();
    return;
  }
  SDL_GPUColorTargetInfo target = {};
  target.texture = swapchain;
  target.clear_color = (SDL_FColor){0, 0, 0, 1};
  target.load_op = SDL_GPU_LOADOP_CLEAR;
  target.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
  if (s_present_img != nullptr) {
    // Brightness 1.0: the picture's own fade was applied when it was built.
    const float brightness[4] = {1.0f, 0, 0, 0};
    SDL_PushGPUFragmentUniformData(cmd, 0, brightness, sizeof brightness);
    // Letterbox, never stretch: a resize can leave the built image a different shape from the window.
    const SDL_GPUViewport viewport = viewportOf(
        psx::present::letterbox(s_present_img_w, s_present_img_h, static_cast<int>(sw), static_cast<int>(sh)));
    const SDL_Rect scissor = {0, 0, static_cast<int>(sw), static_cast<int>(sh)};
    SDL_BindGPUGraphicsPipeline(pass, device->s_image_pipe);
    SDL_SetGPUViewport(pass, &viewport);
    SDL_SetGPUScissor(pass, &scissor);
    const SDL_GPUTextureSamplerBinding binding = {s_present_img, device->s_samp_nearest};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  }
  // Host UI belongs to the sink, not to the picture a present shot reads.
  psx::ui::recordOverlay(game, cmd, pass, static_cast<int>(sw), static_cast<int>(sh));
  SDL_EndGPURenderPass(pass);
  device->submit(cmd, "show_present_image");
  game->perf.presented(std::chrono::steady_clock::now(), device->s_window.refresh_period_ms());
  game->hostInput.drainEvents();
}

// Re-show the last built picture without building anything: the debug-server pause loop.
void GpuVkState::repaint() {
  if (!device->enabled() || device->s_inited == 0 || device->s_headless != 0) {
    return;
  }
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device->s_dev);
  GPUCHK(cmd, "AcquireGPUCommandBuffer");
  show_present_image(cmd);
}

// The host picture into the present image, letterboxed 4:3 and faded.
void GpuVkState::build_image_present_image(SDL_GPUCommandBuffer *cmd, float fade) {
  const psx::present::PixelSize sink = device->sinkSize();
  ensure_present_img(sink.width, sink.height);
  if (s_present_img == nullptr) {
    return;
  }
  SDL_GPUColorTargetInfo target = {};
  target.texture = s_present_img;
  target.clear_color = (SDL_FColor){0, 0, 0, 1};
  target.load_op = SDL_GPU_LOADOP_CLEAR;
  target.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
  const float brightness[4] = {fade, 0, 0, 0};
  SDL_PushGPUFragmentUniformData(cmd, 0, brightness, sizeof brightness);
  const PaneRect laidOut = psx::present::letterbox(4, 3, s_present_img_w, s_present_img_h);
  const SDL_GPUViewport viewport = viewportOf(laidOut);
  const SDL_Rect scissor = {0, 0, s_present_img_w, s_present_img_h};
  SDL_BindGPUGraphicsPipeline(pass, device->s_image_pipe);
  SDL_SetGPUViewport(pass, &viewport);
  SDL_SetGPUScissor(pass, &scissor);
  const SDL_GPUTextureSamplerBinding binding = {device->s_img_tex, device->s_samp_linear};
  SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
  s_present_viewport = laidOut;
}

// A plain RGBA8 image fullscreen, letterboxed 4:3, every channel scaled by `fade`: movies and splashes.
void GpuVkState::build_image(const std::uint8_t *rgba, psx::present::PixelSize size, float fade) {
  const int width = size.width;
  const int height = size.height;
  if (!device->enabled() || width <= 0 || height <= 0) {
    return;
  }
  bring_up();
  device->ensureImageTexture(width, height);
  fade = fade < 0.0f ? 0.0f : (fade > 1.0f ? 1.0f : fade);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device->s_dev);
  GPUCHK(cmd, "AcquireGPUCommandBuffer");
  void *staging = SDL_MapGPUTransferBuffer(device->s_dev, device->s_img_xfer, true);
  std::memcpy(staging, rgba, static_cast<std::size_t>(width) * height * 4);
  SDL_UnmapGPUTransferBuffer(device->s_dev, device->s_img_xfer);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo source = {};
  source.transfer_buffer = device->s_img_xfer;
  source.pixels_per_row = static_cast<Uint32>(width);
  source.rows_per_layer = static_cast<Uint32>(height);
  SDL_GPUTextureRegion destination = {};
  destination.texture = device->s_img_tex;
  destination.w = static_cast<Uint32>(width);
  destination.h = static_cast<Uint32>(height);
  destination.d = 1;
  SDL_UploadToGPUTexture(copy, &source, &destination, false);
  SDL_EndGPUCopyPass(copy);
  build_image_present_image(cmd, fade);
  device->submit(cmd, "present_image build");
  s_show_pending = device->s_headless == 0 && s_present_target == PresentTarget::Swapchain;
}

void GpuVkState::preseq_arm(int count, const char *dir) {
  std::snprintf(s_preseq_dir, sizeof s_preseq_dir, "%s", dir);
  char path[sizeof s_preseq_dir + 16];
  std::snprintf(path, sizeof path, "%s/p0000.ppm", s_preseq_dir);
  if (!Fs::ensureParentDirs(path)) {
    lucent::error("preseq", "cannot create the parent directory of {}, capture not armed", path);
    s_preseq_left = 0;
    return;
  }
  s_preseq_idx = 0;
  s_preseq_left = count;
}

// Runs after every present, real or in-between.
void GpuVkState::preseq_capture() {
  if (s_preseq_left <= 0) {
    return;
  }
  char path[192];
  std::snprintf(path, sizeof path, "%s/p%04d.ppm", s_preseq_dir, s_preseq_idx++);
  shot(path);
  if (--s_preseq_left == 0) {
    lucent::info("preseq", "done: {} frames -> {}", s_preseq_idx, s_preseq_dir);
  }
}

void GpuVkState::release_device_resources() {
  if (device == nullptr || device->s_dev == nullptr) {
    return;
  }
  releaseFilledPresentImage();
  s_record.reset();
  SDL_GPUDevice *dev = device->s_dev;
  if (s_vram_xfer != nullptr) {
    SDL_ReleaseGPUTransferBuffer(dev, s_vram_xfer);
    s_vram_xfer = nullptr;
  }
  if (s_present_rb != nullptr) {
    SDL_ReleaseGPUTransferBuffer(dev, s_present_rb);
    s_present_rb = nullptr;
  }
  if (s_vram_tex != nullptr) {
    SDL_ReleaseGPUTexture(dev, s_vram_tex);
    s_vram_tex = nullptr;
  }
  if (s_present_img != nullptr) {
    SDL_ReleaseGPUTexture(dev, s_present_img);
    s_present_img = nullptr;
  }
}
