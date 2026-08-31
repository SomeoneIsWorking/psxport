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

} // namespace

bool GpuVkState::request_native_composite_capture() {
  if (!s_native_composite_capture.request()) {
    lucent::warn("native_composite_capture", "rejected request before a completed native composite fence");
    return false;
  }
  return true;
}

void GpuVkState::note_native_composite_completed(bool ires, int width, int height) {
  s_native_composite_capture.noteCompletedComposite(ires ? NativeCompositeSource::Ires : NativeCompositeSource::Native,
                                                    {width, height});
}

void GpuVkState::capture_native_composite(SDL_GPUCommandBuffer *cmd) {
  const NativeCompositeCapturePlan plan = s_native_composite_capture.plan();
  if (!plan.copy) {
    return;
  }

  SDL_GPUTexture *source = plan.source == NativeCompositeSource::Ires ? s_ires_color : s_vram_tex;
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
    info.width = (Uint32)plan.extent.width;
    info.height = (Uint32)plan.extent.height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    s_native_composite_capture_tex = SDL_CreateGPUTexture(device, &info);
    if (!s_native_composite_capture_tex) {
      capture_failure("CreateGPUTexture(native composite capture)");
    }
  }

  SDL_GPUBlitInfo copy = {};
  copy.source.texture = source;
  copy.source.w = (Uint32)plan.extent.width;
  copy.source.h = (Uint32)plan.extent.height;
  copy.destination.texture = s_native_composite_capture_tex;
  copy.destination.w = (Uint32)plan.extent.width;
  copy.destination.h = (Uint32)plan.extent.height;
  copy.load_op = SDL_GPU_LOADOP_DONT_CARE;
  copy.filter = SDL_GPU_FILTER_NEAREST;
  SDL_BlitGPUTexture(cmd, &copy);
  if (!s_native_composite_capture.didCapture(plan)) {
    capture_failure("capture plan no longer matches its completed source fence");
  }
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
