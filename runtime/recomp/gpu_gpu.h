// gpu_gpu.h — public API of the Vulkan present backend (gpu_gpu.cpp), de-globalized (R2, 2026-06-19).
//
// Every entry point that touches the per-frame render machine state now takes a `Core*` first: it is a
// thin wrapper that forwards to `core->game->gpu_gpu` (the GpuGpuState instance). This is the single
// declaration site — the scattered local forward-decls inside the gp0 tee (gpu_native.cpp) are gone.
// The device-singleton / config functions (gpu_gpu_enabled, gpu_gpu_wide_engine*, gpu_gpu_video_status,
// gpu_gpu_rawdump_arm, gpu_gpu_vram_region) keep their Core*-less signatures and are declared at use.
#ifndef GPU_GPU_H
#define GPU_GPU_H
#include <stdint.h>
struct Core;

// per-prim depth / OT-submission order (set by the gp0 tee before each VK draw)
void gpu_gpu_set_order(Core* core, unsigned idx);
void gpu_gpu_set_order_2d(Core* core, unsigned idx);
void gpu_gpu_set_order_2d_n(Core* core, unsigned idx);
void gpu_gpu_set_order_2d_bg(Core* core, unsigned idx);
void gpu_gpu_set_order_2d_bg_n(Core* core, unsigned idx);
void gpu_gpu_set_vd(Core* core, const float* d3);
void gpu_gpu_set_vd_n(Core* core, const float* d3);
void gpu_gpu_set_xyf(Core* core, const float* xf, const float* yf);  // sub-pixel screen XY (#15 smoothing)

// Dynamic shadow mapping: capture one OPAQUE world-geometry triangle's VIEW-SPACE positions (v0/v1/v2,
// each {x=ir1, y=ir2, z=pz} — the metric view space the deferred pass reconstructs) into the host shadow
// geometry stream. Rasterized from the directional light's view into a depth map, then sampled in the
// deferred pass to darken occluded pixels. Called from the opaque world submitters (submit.cpp,
// native_terrain.cpp). Cheap no-op when shadows are off. v0/v1/v2 point to 3 floats each.
void gpu_gpu_shadow_push_tri(Core* core, const float* v0, const float* v1, const float* v2);
int  gpu_gpu_shadows_active(void);   // shadows toggle (g_mods.shadows && g_mods.light) — submitters gate capture

// geometry tee + dirty-region mirror
void gpu_gpu_dirty(Core* core, int x, int y, int w, int h);
void gpu_gpu_semi_group(Core* core, int x0, int y0, int x1, int y1);
void gpu_gpu_draw_tri(Core* core, int x0,int y0,int r0,int g0,int b0, int x1,int y1,int r1,int g1,int b1,
                     int x2,int y2,int r2,int g2,int b2);
void gpu_gpu_draw_tritri(Core* core, const int* xs, const int* ys, const int* us, const int* vs,
                        const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                        int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                        int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1);
void gpu_gpu_draw_semi(Core* core, const int* xs, const int* ys, const int* us, const int* vs,
                      const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                      int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                      int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1, int blend);

// present / per-frame / readback
void gpu_gpu_present(Core* core, const uint16_t* src, int sx, int sy, int w, int h);
// PC-native fullscreen IMAGE present: draw a plain RGBA8 image (iw x ih) FULLSCREEN, letterboxed to 4:3
// (pillarbox, black bars), every rgb scaled by `fade` (0..1). Reusable, PSX-free (no VRAM/GP0/CLUT) —
// uploads the host RGBA into its own texture and draws it. Windowed presents to the swapchain; headless
// only uploads (no present) — verify headless via the caller's own CPU-side dump.
void gpu_gpu_present_image(Core* core, const uint8_t* rgba, int iw, int ih, float fade);
void gpu_gpu_frame_end(Core* core, const uint16_t* svram, int frame);
// preseqobj (per-object motion tracker): the present index this emit pass will dump, or -1 if no preseq
// capture is armed. Lets RenderQueue::emitItem key each [preseqobj] line to its present frame.
int gpu_gpu_preseq_present_index(Core* core);
void gpu_gpu_shot(Core* core, const char* path);
void gpu_gpu_stats(Core* core, int* tri, int* tex, int* semi);

// (Engine-owned screen fade is now the PC-native subsystem class ScreenFade at
// game/render/screen_fade.h. The old gpu_set_fade / gpu_clear_fade / engine_fade_set entries
// lived here — deleted; native present path reads ScreenFade::get(core).)

// this-/last-frame native-geometry status (defined in gpu_native.cpp; read by the gpu_gpu present path)
// — now per-instance. A frame with neither 3D nor a full-screen 2D backdrop is a raw framebuffer (FMV).
int gpu_seen3d_this_frame(Core* core);
int gpu_had3d_last_frame(Core* core);

#endif // GPU_GPU_H
