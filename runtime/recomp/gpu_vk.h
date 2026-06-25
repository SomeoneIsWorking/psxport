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

// Dynamic shadow mapping: capture one OPAQUE world-geometry triangle's VIEW-SPACE positions (v0/v1/v2,
// each {x=ir1, y=ir2, z=pz} — the metric view space the deferred pass reconstructs) into the host shadow
// geometry stream. Rasterized from the directional light's view into a depth map, then sampled in the
// deferred pass to darken occluded pixels. Called from the opaque world submitters (engine_submit.cpp,
// native_terrain.cpp). Cheap no-op when shadows are off. v0/v1/v2 point to 3 floats each.
void gpu_vk_shadow_push_tri(Core* core, const float* v0, const float* v1, const float* v2);
int  gpu_vk_shadows_active(void);   // shadows toggle (g_mods.shadows && g_mods.light) — submitters gate capture

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
// PC-native fullscreen IMAGE present: draw a plain RGBA8 image (iw x ih) FULLSCREEN, letterboxed to 4:3
// (pillarbox, black bars), every rgb scaled by `fade` (0..1). Reusable, PSX-free (no VRAM/GP0/CLUT) —
// uploads the host RGBA into its own texture and draws it. Windowed presents to the swapchain; headless
// only uploads (no present) — verify headless via the caller's own CPU-side dump.
void gpu_vk_present_image(Core* core, const uint8_t* rgba, int iw, int ih, float fade);
void gpu_vk_frame_end(Core* core, const uint16_t* svram, int frame);
void gpu_vk_shot(Core* core, const char* path);
void gpu_vk_stats(Core* core, int* tri, int* tex, int* semi);
void gpu_vk_tritest(Core* core);

// Engine-owned screen FADE (replaces the PSX full-screen semi OT rect + the gpu_native fade_full_2d
// heuristic). The engine sets the fade each logic frame; present.frag + the headless readback apply
// clamp(rgb +/- color/255). mode: 0 none, 1 additive (to/from white), 2 subtractive (to/from black).
void gpu_set_fade(int mode, uint8_t r, uint8_t g, uint8_t b);
void gpu_clear_fade(void);
// Caller-facing wrapper mirroring the old FUN_8007e9c8(color, a1, otslot) SCREEN-FADE entry: a1!=0 =>
// additive (white), a1==0 => subtractive (black); color channels r=color&0xff, g=(color>>8)&0xff,
// b=(color>>16)&0xff. (Core* kept for symmetry with the other engine entries; currently unused.)
void engine_fade_set(Core* core, uint32_t color, uint32_t a1);

// this-/last-frame 3D status (defined in gpu_native.cpp; read by the gpu_vk present path) — now per-instance
int gpu_seen3d_this_frame(Core* core);
int gpu_had3d_last_frame(Core* core);

#endif // GPU_VK_H
