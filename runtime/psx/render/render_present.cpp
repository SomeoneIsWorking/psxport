// render_present.cpp — one Game's present in two steps: build the display picture (the record image on
// RenderPath::Record, the device's VRAM on Device and for a 24bpp display, or a host picture) letterboxed
// into the present image, then show that image in the window. The leg decides only whether the built
// image reaches a window (present_plan.h).
#include "config_vars.h"
#include "core.h"
#include "fs_util.h"
#include "game.h"
#include "gpu_device.h"
#include "guest_widescreen_projection.h"
#include "present_plan.h"
#include "render_device.h"
#include "render_state.h"

#include <lucent/log.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <chrono>
#include <cstdio>

namespace psx::render {
namespace {

// present.frag's `PC { ivec4 disp; ivec4 fade; ivec4 fmt; }`.
struct PresentUniform {
  std::int32_t disp[4];
  std::int32_t fade[4];
  std::int32_t fmt[4];
};

// image.frag's brightness; 1 when the picture's own fade was applied when it was built.
struct ImageUniform {
  float brightness[4];
};

template <typename T> std::span<const std::byte> bytesOf(const T &value) {
  return std::as_bytes(std::span<const T, 1>(&value, 1));
}

} // namespace

RenderState::RenderState(Game &game, RenderDevice &device)
    : game_(game), device_(device), clip_(device, psx::config::bug_report_clip()) {}

RenderState::~RenderState() {
  if (!device_.up()) {
    return;
  }
  record_.reset();
  for (SDL_GPUTexture **texture : {&vramTexture_, &hostImage_, &presentImage_, &screenImage_}) {
    device_.release(*texture);
  }
  for (SDL_GPUTransferBuffer **buffer : {&vramUpload_, &hostUpload_, &screenReadback_}) {
    device_.release(*buffer);
  }
}

void RenderState::bringUp() {
  device_.bringUp();
  if (overlayBound_) {
    return;
  }
  overlayBound_ = true;
  game_.rml_overlay.init(device_.rmlUi(), device_.sinkSize());
}

void RenderState::ensureVramTexture() {
  if (vramTexture_ != nullptr) {
    return;
  }
  // R8G8: SDL_GPU forbids sampling integer formats, so the 1555 word rides as two bytes.
  vramTexture_ = device_.makeTexture(
      {gpu::kVramWidth, gpu::kVramHeight}, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
  vramUpload_ = device_.makeTransferBuffer(SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, gpu::kVramWidth * gpu::kVramHeight * 2);
}

void RenderState::uploadDeviceVram(SDL_GPUCommandBuffer *cmd, std::span<const std::uint16_t> vram) {
  ensureVramTexture();
  const auto bytes = std::as_bytes(vram);
  device_.upload(cmd,
                 vramUpload_,
                 {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()},
                 vramTexture_,
                 {gpu::kVramWidth, gpu::kVramHeight});
}

void RenderState::ensurePresentImage(PixelSize size) {
  if (size.width <= 0 || size.height <= 0 || (presentImage_ != nullptr && presentImageSize_ == size)) {
    return;
  }
  device_.release(presentImage_);
  presentImage_ =
      device_.makeTexture(size, kPresentImageFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
  presentImageSize_ = size;
  lucent::info("render", "present image {}x{}", size.width, size.height);
}

void RenderState::ensureHostImage(PixelSize size) {
  if (hostImage_ != nullptr && hostImageSize_ == size) {
    return;
  }
  device_.release(hostImage_);
  device_.release(hostUpload_);
  hostImage_ = device_.makeTexture(size, kPresentImageFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER);
  hostUpload_ = device_.makeTransferBuffer(SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                           static_cast<std::uint32_t>(size.width) * size.height * 4);
  hostImageSize_ = size;
}

// The plan's inputs out of renderer state; planPresent() stays the pure decision.
present::PresentInputs RenderState::presentInputs(gpu::VramRect display, int nativeWidth) {
  present::PresentInputs in{};
  const PixelSize image = presentImageSize();
  in.sink_w = image.width;
  in.sink_h = image.height;
  in.sx = display.x;
  in.sy = display.y;
  in.disp_w = display.width;
  in.disp_h = display.height;
  // The game's own 4:3 width before widening: what keeps a native frame of any width at 4:3.
  in.native_w = nativeWidth;
  // A picture the title says covers only the native width is sampled over those columns alone.
  if (game_.runtime.guestPictureIsNativeWidth(game_)) {
    in.content_w = in.native_w;
  }
  in.disp_rgb24 = game_.core.gpuDevice().displayArea().depth24 ? 1 : 0;
  // A pane fills its panel, cropped at the rendered resolution, rather than letterboxing into a band.
  in.fill = target_ == PresentTarget::Pane ? 1 : 0;
  return in;
}

void RenderState::buildDisplay(gpu::VramRect display) {
  if (!device_.enabled()) {
    return;
  }
  bringUp();
  const Core &core = game_.core;
  const int width = present::presentDisplayWidth(
      present::widePresentation(core), present::widePresentationWidth(core), display.width);
  lastDisplay_ = {display.x, display.y, width, display.height};
  SDL_GPUCommandBuffer *cmd = device_.acquire();
  present::PresentInputs inputs = presentInputs(lastDisplay_, display.width);
  presentRecord(inputs);
  if (!presentsRecord_) {
    uploadDeviceVram(cmd, game_.core.gpuDevice().vram());
  }
  const present::PresentPlan plan = present::planPresent(inputs);
  viewport_ = plan.viewport;
  ++presentSerial_;
  lucent::debug("present",
                "sink {}x{} pane={} src_ires={} rgb24={} disp={},{},{},{} viewport={},{},{},{}",
                plan.sink_w,
                plan.sink_h,
                target_ == PresentTarget::Pane ? "yes" : "no",
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
    buildPresentImage(cmd, plan);
  }
  device_.submit(cmd, "present build");
  // A pane's window belongs to the host compositing it, so the host composites here and shows at show().
  const bool toPaneHost = target_ == PresentTarget::Pane && paneHost_.attached();
  showPending_ = toPaneHost || (plan.build && target_ == PresentTarget::Swapchain);
  if (toPaneHost) {
    paneHost_.built();
  }
}

void RenderState::show() {
  if (!showPending_) {
    return;
  }
  showPending_ = false;
  if (target_ == PresentTarget::Pane) {
    if (paneHost_.attached()) {
      paneHost_.show();
    }
    return;
  }
  showPresentImage(device_.acquire());
}

void RenderState::buildPresentImage(SDL_GPUCommandBuffer *cmd, const present::PresentPlan &plan) {
  ensureVramTexture();
  ensurePresentImage({plan.sink_w, plan.sink_h});
  PresentUniform uniform{};
  for (int i = 0; i < 4; i++) {
    uniform.disp[i] = plan.disp[i];
    uniform.fade[i] = plan.fade[i];
    uniform.fmt[i] = plan.fmt[i];
  }
  SDL_GPUTexture *source = presentsRecord_ ? record_->presented().texture : vramTexture_;
  SDL_GPURenderPass *pass = beginClearedPass(cmd, presentImage_);
  drawFullscreen(cmd,
                 pass,
                 device_.presentPipeline(),
                 {source, device_.nearestSampler()},
                 bytesOf(uniform),
                 plan.viewport,
                 presentImageSize_);
  SDL_EndGPURenderPass(pass);
  clipPresent(cmd);
}

void RenderState::clipPresent(SDL_GPUCommandBuffer *cmd) {
  if (target_ == PresentTarget::Swapchain) {
    clip_.record(cmd, presentImage_, presentImageSize_);
  }
}

// The sink: the finished picture into the window, letterboxed, with the overlay on top.
void RenderState::showPresentImage(SDL_GPUCommandBuffer *cmd) {
  Uint32 width = 0;
  Uint32 height = 0;
  SDL_GPUTexture *swapchain = acquireSink(sink_, cmd, device_.window().window(), &width, &height);
  if (swapchain == nullptr) {
    device_.submit(cmd, "show");
    game_.hostInput.drainEvents();
    return;
  }
  recordScreen(cmd, swapchain, {static_cast<int>(width), static_cast<int>(height)});
  device_.submit(cmd, "show");
  game_.perf.presented(std::chrono::steady_clock::now(), device_.window().refreshPeriodMs());
  game_.hostInput.drainEvents();
}

void RenderState::recordScreen(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, PixelSize size) {
  SDL_GPURenderPass *pass = beginClearedPass(cmd, target);
  if (presentImage_ != nullptr) {
    // Letterbox, never stretch: a resize can leave the built image a different shape from the window.
    drawFullscreen(cmd,
                   pass,
                   device_.sinkPipeline(),
                   {presentImage_, device_.nearestSampler()},
                   bytesOf(ImageUniform{{1.0f, 0.0f, 0.0f, 0.0f}}),
                   present::letterbox(presentImageSize_.width, presentImageSize_.height, size.width, size.height),
                   size);
  }
  game_.rml_overlay.recordGpu(cmd, pass, size.width, size.height);
  SDL_EndGPURenderPass(pass);
}

void RenderState::repaint() {
  if (!device_.enabled() || !device_.up()) {
    return;
  }
  showPresentImage(device_.acquire());
}

void RenderState::buildHostImage(SDL_GPUCommandBuffer *cmd, float fade) {
  ensurePresentImage(presentImageSize());
  const present::PaneRect laidOut = present::letterbox(4, 3, presentImageSize_.width, presentImageSize_.height);
  SDL_GPURenderPass *pass = beginClearedPass(cmd, presentImage_);
  drawFullscreen(cmd,
                 pass,
                 device_.imagePipeline(),
                 {hostImage_, device_.linearSampler()},
                 bytesOf(ImageUniform{{fade, 0.0f, 0.0f, 0.0f}}),
                 laidOut,
                 presentImageSize_);
  SDL_EndGPURenderPass(pass);
  clipPresent(cmd);
  viewport_ = laidOut;
}

void RenderState::buildImage(const std::uint8_t *rgba, PixelSize size, float fade) {
  if (!device_.enabled() || size.width <= 0 || size.height <= 0) {
    return;
  }
  bringUp();
  ensureHostImage(size);
  SDL_GPUCommandBuffer *cmd = device_.acquire();
  device_.upload(cmd, hostUpload_, {rgba, static_cast<std::size_t>(size.width) * size.height * 4}, hostImage_, size);
  buildHostImage(cmd, std::clamp(fade, 0.0f, 1.0f));
  device_.submit(cmd, "host image build");
  showPending_ = target_ == PresentTarget::Swapchain;
}

void RenderState::preseqArm(int count, const char *dir) {
  std::snprintf(preseqDir_, sizeof preseqDir_, "%s", dir);
  char path[sizeof preseqDir_ + 16];
  std::snprintf(path, sizeof path, "%s/p0000.ppm", preseqDir_);
  if (!Fs::ensureParentDirs(path)) {
    lucent::error("preseq", "cannot create the parent directory of {}, capture not armed", path);
    preseqLeft_ = 0;
    return;
  }
  preseqIndex_ = 0;
  preseqLeft_ = count;
}

// Runs after every present, real or in-between.
void RenderState::preseqCapture() {
  if (preseqLeft_ <= 0) {
    return;
  }
  char path[192];
  std::snprintf(path, sizeof path, "%s/p%04d.ppm", preseqDir_, preseqIndex_++);
  shot(path);
  if (--preseqLeft_ == 0) {
    lucent::info("preseq", "done: {} frames -> {}", preseqIndex_, preseqDir_);
  }
}

} // namespace psx::render
