// gpu_vk.h — public API of the Vulkan present backend (gpu_vk.cpp), de-globalized (R2, 2026-06-19).
//
// Every entry point that touches the per-frame render machine state now takes a `Core*` first: it is a
// thin wrapper that forwards to `core->game->gpu_vk` (the GpuVkState instance). This is the single
// declaration site — the scattered local forward-decls inside the gp0 tee (gpu_native.cpp) are gone.
// The device-singleton / config functions (gpu_vk_enabled, gpu_vk_wide_engine*, gpu_vk_video_status,
// gpu_vk_rawdump_arm, gpu_vk_vram_region) keep their Core*-less signatures and are declared at use.
#ifndef GPU_VK_H
#define GPU_VK_H
#include <stdint.h>
struct Core;

// per-prim depth / OT-submission order (set by the gp0 tee before each VK draw)
void gpu_vk_set_order(Core* core, unsigned idx);
void gpu_vk_set_order_2d(Core* core, unsigned idx);
void gpu_vk_set_order_2d_n(Core* core, unsigned idx);
void gpu_vk_set_order_2d_bg(Core* core, unsigned idx);
void gpu_vk_set_order_2d_bg_n(Core* core, unsigned idx);
void gpu_vk_set_vd(Core* core, const float* d3);
void gpu_vk_set_vd_n(Core* core, const float* d3);
void gpu_vk_set_xyf(Core* core, const float* xf, const float* yf);  // sub-pixel screen XY (#15 smoothing)

// geometry tee + dirty-region mirror
void gpu_vk_dirty(Core* core, int x, int y, int w, int h);
void gpu_vk_semi_group(Core* core, int x0, int y0, int x1, int y1);
void gpu_vk_draw_tri(Core* core, int x0,int y0,int r0,int g0,int b0, int x1,int y1,int r1,int g1,int b1,
                     int x2,int y2,int r2,int g2,int b2);
void gpu_vk_draw_tritri(Core* core, const int* xs, const int* ys, const int* us, const int* vs,
                        const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                        int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                        int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1);
void gpu_vk_draw_semi(Core* core, const int* xs, const int* ys, const int* us, const int* vs,
                      const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                      int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                      int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1, int blend);

// present / per-frame / readback
void gpu_vk_present(Core* core, const uint16_t* src, int sx, int sy, int w, int h);
void gpu_vk_frame_end(Core* core, const uint16_t* svram, int frame);
void gpu_vk_shot(Core* core, const char* path);
void gpu_vk_stats(Core* core, int* tri, int* tex, int* semi);
void gpu_vk_tritest(Core* core);

// this-/last-frame 3D status (defined in gpu_native.cpp; read by the gpu_vk present path) — now per-instance
int gpu_seen3d_this_frame(Core* core);
int gpu_had3d_last_frame(Core* core);

#endif // GPU_VK_H
