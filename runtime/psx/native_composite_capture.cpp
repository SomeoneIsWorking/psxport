#include "core.h"
#include "game.h"
#include "gpu_vk.h"
#include "gpu_vk_device.h"
#include <SDL3/SDL_gpu.h>
#include <lucent/log.h>
#include <stdlib.h>

namespace {

SDL_GPUDevice *capture_device() {
  return GpuDevice::sInstance ? GpuDevice::sInstance->s_dev : nullptr;
}

[[noreturn]] void capture_failure(const char *what) {
  lucent::error("native_composite_capture", "{}: {}", what, SDL_GetError());
  exit(2);
}

NativeCompositeFrame composite_frame(bool ires, int scale, int sx, int sy, int width, int height) {
  return {
      ires ? NativeCompositeSource::Ires : NativeCompositeSource::Native,
      {VRAM_W * scale, VRAM_H * scale},
      {sx * scale, sy * scale, width * scale, height * scale},
      scale,
  };
}

} // namespace

bool GpuVkState::request_native_composite_capture() {
  if (!s_native_composite_capture.request()) {
    lucent::warn("native_composite_capture", "rejected request before a completed native composite fence");
    return false;
  }
  return true;
}

void GpuVkState::note_native_composite_completed(bool ires, int scale, int sx, int sy, int width, int height) {
  s_native_composite_capture.noteCompletedComposite(composite_frame(ires, scale, sx, sy, width, height));
}

void GpuVkState::capture_native_composite(SDL_GPUCommandBuffer *cmd) {
  const NativeCompositeCapturePlan plan = s_native_composite_capture.plan();
  if (!plan.copy) {
    return;
  }

  SDL_GPUTexture *source = plan.frame.source == NativeCompositeSource::Ires ? s_ires_color : s_vram_tex;
  SDL_GPUDevice *const device = capture_device();
  if (!source || !device) {
    capture_failure("completed source fence refers to a released render target");
  }
  if (plan.allocate) {
    if (s_native_composite_capture_tex) {
      SDL_ReleaseGPUTexture(device, s_native_composite_capture_tex);
      s_native_composite_capture_tex = nullptr;
    }
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = (Uint32)plan.captureExtent.width;
    info.height = (Uint32)plan.captureExtent.height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    s_native_composite_capture_tex = SDL_CreateGPUTexture(device, &info);
    if (!s_native_composite_capture_tex) {
      capture_failure("CreateGPUTexture(native composite capture)");
    }
  }

  SDL_GPUBlitInfo copy = {};
  copy.source.texture = source;
  copy.source.x = (Uint32)plan.frame.sourceRect.x;
  copy.source.y = (Uint32)plan.frame.sourceRect.y;
  copy.source.w = (Uint32)plan.frame.sourceRect.width;
  copy.source.h = (Uint32)plan.frame.sourceRect.height;
  copy.destination.texture = s_native_composite_capture_tex;
  copy.destination.w = (Uint32)plan.captureExtent.width;
  copy.destination.h = (Uint32)plan.captureExtent.height;
  copy.load_op = SDL_GPU_LOADOP_DONT_CARE;
  copy.filter = SDL_GPU_FILTER_NEAREST;
  SDL_BlitGPUTexture(cmd, &copy);
  if (!s_native_composite_capture.didCapture(plan)) {
    capture_failure("capture plan no longer matches its completed source fence");
  }
}

bool GpuVkState::apply_native_composite_base(
    SDL_GPUCommandBuffer *cmd, bool ires, int scale, int sx, int sy, int width, int height) {
  const NativeCompositeFrame target = composite_frame(ires, scale, sx, sy, width, height);
  const NativeCompositeBasePlan plan = s_native_composite_capture.takeBase(target);
  if (plan.refused) {
    lucent::warn("native_composite_capture", "discarded retained backdrop because the next target changed");
    return false;
  }
  if (!plan.blit) {
    return false;
  }
  if (!s_native_composite_capture_tex) {
    capture_failure("native composite capture state has no retained texture");
  }
  SDL_GPUTexture *destination = target.source == NativeCompositeSource::Ires ? s_ires_color : s_vram_tex;
  if (!destination) {
    capture_failure("native composite base target is unavailable");
  }
  SDL_GPUBlitInfo base = {};
  base.source.texture = s_native_composite_capture_tex;
  base.source.w = (Uint32)plan.destination.width;
  base.source.h = (Uint32)plan.destination.height;
  base.destination.texture = destination;
  base.destination.x = (Uint32)plan.destination.x;
  base.destination.y = (Uint32)plan.destination.y;
  base.destination.w = (Uint32)plan.destination.width;
  base.destination.h = (Uint32)plan.destination.height;
  base.load_op = SDL_GPU_LOADOP_LOAD;
  base.filter = SDL_GPU_FILTER_NEAREST;
  SDL_BlitGPUTexture(cmd, &base);
  return true;
}

void GpuVkState::release_native_composite_capture() {
  if (s_native_composite_capture_tex) {
    if (SDL_GPUDevice *const device = capture_device()) {
      SDL_ReleaseGPUTexture(device, s_native_composite_capture_tex);
    }
    s_native_composite_capture_tex = nullptr;
  }
  s_native_composite_capture.resetResource();
}

GpuVkState::~GpuVkState() {
  release_native_composite_capture();
}

bool gpu_vk_request_native_composite_capture(Core *core) {
  return core && core->game && core->game->gpu_vk.request_native_composite_capture();
}

bool gpu_vk_native_composite_capture_ready(Core *core) {
  return core && core->game && core->game->gpu_vk.s_native_composite_capture.valid();
}
