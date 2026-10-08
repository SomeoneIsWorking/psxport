// GpuDevice teardown: the SDL window, GPU device, samplers and pipelines a Game's first present created.
//
// Until 2026-10-01 nothing released any of it. A process only ever ran one Game, so process exit was
// the owner. A product that runs one Game after another (a title picker, then a title, then the picker
// again) would otherwise leave every earlier Game's window open and its device alive, and the next
// Game's init would see `s_inited` of a fresh GpuDevice and create a second window beside the first.
#include "gpu_vk_device.h"

#include "rmlui_overlay.h"
#include <lucent/log.h>

namespace {
template <typename T, typename Release> void releaseHandle(T *&handle, SDL_GPUDevice *device, Release release) {
  if (handle != nullptr) {
    release(device, handle);
    handle = nullptr;
  }
}
} // namespace

// The first Game constructed claims the process device slot (see gpu_vk_device.h). The DEFINITION
// belongs here, beside the class's own teardown, and not in whichever renderer unit happened to spell
// it first: gpu_vk_device_handles.h — the `s_*` handle macros every renderer TU reaches the device
// through — expands to these very member names, so it cannot be included by this file.
GpuDevice *GpuDevice::sInstance = nullptr;

GpuDevice::~GpuDevice() {
  if (s_inited == 0) {
    return;
  }
  if (s_dev != nullptr) {
    SDL_WaitForGPUIdle(s_dev);
    const auto pipeline = [](SDL_GPUDevice *d, SDL_GPUGraphicsPipeline *p) {
      SDL_ReleaseGPUGraphicsPipeline(d, p);
    };
    releaseHandle(s_present_pipe, s_dev, pipeline);
    releaseHandle(s_image_pipe, s_dev, pipeline);
    releaseHandle(s_tri_pipe, s_dev, pipeline);
    releaseHandle(s_line_pipe, s_dev, pipeline);
    releaseHandle(s_tritex_pipe, s_dev, pipeline);
    releaseHandle(s_decode_pipe, s_dev, pipeline);
    releaseHandle(s_encode_pipe, s_dev, pipeline);
    releaseHandle(s_ires_downsample_pipe, s_dev, pipeline);
    releaseHandle(s_semi_cover_pipe, s_dev, pipeline);
    releaseHandle(s_painter_tex_pipe, s_dev, pipeline);
    releaseHandle(s_painter_tri_pipe, s_dev, pipeline);
    releaseHandle(s_painter_composite_pipe, s_dev, pipeline);
    for (SDL_GPUGraphicsPipeline *&semi : s_semi_pipe) {
      releaseHandle(semi, s_dev, pipeline);
    }
    for (SDL_GPUGraphicsPipeline *&semi : s_painter_semi_pipe) {
      releaseHandle(semi, s_dev, pipeline);
    }
    releaseHandle(s_samp_nearest, s_dev, [](SDL_GPUDevice *d, SDL_GPUSampler *s) {
      SDL_ReleaseGPUSampler(d, s);
    });
    releaseHandle(s_samp_linear, s_dev, [](SDL_GPUDevice *d, SDL_GPUSampler *s) {
      SDL_ReleaseGPUSampler(d, s);
    });
    releaseHandle(s_img_tex, s_dev, [](SDL_GPUDevice *d, SDL_GPUTexture *t) {
      SDL_ReleaseGPUTexture(d, t);
    });
    releaseHandle(s_img_xfer, s_dev, [](SDL_GPUDevice *d, SDL_GPUTransferBuffer *b) {
      SDL_ReleaseGPUTransferBuffer(d, b);
    });
    // The window leaves the device before the device is destroyed; the window itself outlives it.
    // RmlUi's own teardown is an atexit hook, which runs after this: it would release onto a dead
    // device and leave its texture, view, pipeline, sampler and shader modules alive at
    // vkDestroyDevice. Give them back here, while s_dev is still up.
    psx::ui::releaseDeviceResources();
    s_window.release_device(s_dev);
    SDL_DestroyGPUDevice(s_dev);
    s_dev = nullptr;
  }
  s_window.destroy();
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  s_inited = 0;
  lucent::info("gpu_vk", "device and window released");
}
