// gpu_vk_device_handles.h — the ONE definition of the device-handle spellings the SDL_GPU renderer
// bodies use, so splitting an owner out of gpu_vk.cpp is a MOVE and not a rewrite.
//
// GpuDevice (gpu_vk_device.h) owns the process device, and the renderer reaches it through these
// `s_`-prefixed macros exactly as it did when they were file-scope in gpu_vk.cpp: a body that reads
// `s_dev` keeps reading `s_dev` after the responsibility it belongs to moves to its own translation
// unit. Spelling the fields out field-by-field at every extraction would put the same accessor in N
// places and make each extraction a rewrite of code nobody was changing.
//
// WHY NOT gpu_vk_internal.h (which gpu_vk_device.h includes): these macros expand to GpuDevice's OWN
// member names (`s_dev`, `s_inited`, ...), so the device owner itself — gpu_vk_device.cpp, whose
// methods spell them as members — must never see them. This header therefore includes
// gpu_vk_device.h and is included only by the units that RENDER, never by the unit that owns the
// device.
#pragma once
#include "gpu_vk_device.h"

inline GpuDevice &gdev() {
  return *GpuDevice::sInstance;
}
#define s_gpu_on (gdev().s_gpu_on)
#define s_inited (gdev().s_inited)
#define s_headless (gdev().s_headless)
#define s_win (gdev().s_window.window())
#define s_dev (gdev().s_dev)
#define s_swap_fmt (gdev().s_window.swapchain_format())
#define s_samp_nearest (gdev().s_samp_nearest)
#define s_samp_linear (gdev().s_samp_linear)
#define s_present_pipe (gdev().s_present_pipe)
#define s_image_pipe (gdev().s_image_pipe)
#define s_img_tex (gdev().s_img_tex)
#define s_img_xfer (gdev().s_img_xfer)
#define s_img_w (gdev().s_img_w)
#define s_img_h (gdev().s_img_h)
#define s_tri_pipe (gdev().s_tri_pipe)
#define s_line_pipe (gdev().s_line_pipe)
#define s_tritex_pipe (gdev().s_tritex_pipe)
#define s_semi_pipe (gdev().s_semi_pipe)
#define s_decode_pipe (gdev().s_decode_pipe)
#define s_encode_pipe (gdev().s_encode_pipe)
#define s_ires_downsample_pipe (gdev().s_ires_downsample_pipe)
#define s_semi_cover_pipe (gdev().s_semi_cover_pipe)
#define s_painter_tex_pipe (gdev().s_painter_tex_pipe)
#define s_painter_tri_pipe (gdev().s_painter_tri_pipe)
#define s_painter_semi_pipe (gdev().s_painter_semi_pipe)
#define s_painter_composite_pipe (gdev().s_painter_composite_pipe)
