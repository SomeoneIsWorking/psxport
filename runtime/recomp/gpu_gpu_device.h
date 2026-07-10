// gpu_gpu_device.h — class GpuDevice: the SDL3 GPU present backend's HOST-DEVICE state
// (window, GPU device, pipelines, textures, transfer buffers) that used to be ~25 file-scope
// statics in gpu_gpu.cpp. Owned by Game (`game->gpu_dev`).
//
// ONE window/device per process: the FIRST Game constructed claims the process device via
// `sInstance` (same first-wins pattern as DbgServer) — in SBS both cores render through it by
// design (the harness composes both panes into the one window). Per-frame BATCH state is separate
// and per-Core (GpuGpuState, gpu_gpu_internal.h). Field names keep their historical `s_` spelling
// so the gpu_gpu.cpp bodies are unchanged by the move (accessed via shadow macros there).
#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <stdint.h>
#include "gpu_gpu_internal.h"   // GGS_NUM_BLEND_MODES

class GpuDevice {
public:
  // First Game to construct claims the process device slot (signal-free singleton claim; see above).
  static GpuDevice* sInstance;

  // ---- enable / init gates ----
  int s_gpu_on = -1;      // -1 = env not read yet
  int s_inited = 0;
  int s_headless = 0;

  // ---- window / device / swapchain ----
  SDL_Window*          s_win = nullptr;
  SDL_GPUDevice*       s_dev = nullptr;
  SDL_GPUTextureFormat s_swap_fmt = SDL_GPU_TEXTUREFORMAT_INVALID;

  // (per-Game render TARGETS — the guest-VRAM image, snapshot, depth, color intermediate and vertex
  //  buffers — moved to GpuGpuState 2026-07-10: two Games must not share mutable GPU surfaces.)
  SDL_GPUSampler*         s_samp_nearest = nullptr; // integer VRAM sampler (nearest)
  SDL_GPUSampler*         s_samp_linear = nullptr;  // RGBA image sampler (linear)
  SDL_GPUGraphicsPipeline* s_present_pipe = nullptr;
  SDL_GPUGraphicsPipeline* s_image_pipe = nullptr;

  // ---- fullscreen IMAGE present (RGBA8, recreated on size change) ----
  SDL_GPUTexture*        s_img_tex = nullptr;
  SDL_GPUTransferBuffer* s_img_xfer = nullptr;
  int                    s_img_w = 0, s_img_h = 0;

  // ---- Pass 2: native 3D / textured raster PIPELINES (shared; per-Game targets on GpuGpuState) ----
  SDL_GPUGraphicsPipeline* s_tri_pipe = nullptr;     // flat opaque (depth test + write)
  SDL_GPUGraphicsPipeline* s_tritex_pipe = nullptr;  // textured opaque (depth test + write)
  SDL_GPUGraphicsPipeline* s_semi_pipe[GGS_NUM_BLEND_MODES] = {};  // textured semi, real HW blend
  SDL_GPUGraphicsPipeline* s_decode_pipe = nullptr;  // fullscreen: packed 1555 -> float RGBA
  SDL_GPUGraphicsPipeline* s_encode_pipe = nullptr;  // fullscreen: float RGBA -> packed 1555
  int s_pipes_3d = 0;                                // 3D pipelines created (once per process)

  // ---- SBS two-pane composite textures ----
  SDL_GPUTexture*        s_sbs_tex[2] = {};
  SDL_GPUTransferBuffer* s_sbs_xfer[2] = {};
  int                    s_sbs_w[2] = {}, s_sbs_h[2] = {};

  // ---- diagnostics dedupe / scratch ----
  int s_trace_n = 0;                                  // PSXPORT_GPU_TRACE print counter
  int s_fw_lastmode = -999;                           // `debug fadewatch` change detector (present)
  uint8_t s_fw_lr = 0, s_fw_lg = 0, s_fw_lb = 0;
  int s_fw_lsx = -999, s_fw_lsy = -999, s_fw_lw = -999, s_fw_lh = -999;
  int s_fws_lastmode = -999;                          // fadewatch-state tap (wrapper) change detector
  uint8_t s_fws_lr = 0, s_fws_lg = 0, s_fws_lb = 0;
  int s_fws_lsx = -999, s_fws_lsy = -999, s_fws_lw = -999, s_fws_lh = -999;
  uint16_t s_selftest_pat[1024 * 512] = {};           // gpu_gpu_selftest VRAM test pattern scratch
};
