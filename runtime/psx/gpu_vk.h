// gpu_vk.h — public API of the Vulkan present backend (gpu_vk.cpp), de-globalized (R2, 2026-06-19).
//
// Every entry point that touches the per-frame render machine state now takes a `Core*` first: it is a
// thin wrapper that forwards to `core->game->gpu_vk` (the GpuVkState instance). This is the single
// declaration site — the scattered local forward-decls inside the gp0 tee (gpu_native.cpp) are gone.
#ifndef GPU_GPU_H
#define GPU_GPU_H
#include "guest_widescreen_projection.h"
#include <stdint.h>
struct Core;

inline constexpr float kGpuNative3dMin = 0.0625f;
inline constexpr float kGpuNative3dMax = 0.9375f;

// Widescreen geometry. Per-core, because widescreen never touches the PSX oracle: one process can
// hold a wide user core and a pure 4:3 oracle core at the same time.
//   wide_engine     — is this core rendering wider than 4:3 (and not the oracle)?
//   native_w        — this title's current 4:3 framebuffer width
//   wide_engine_w   — the native render width in that aspect
//   wide_engine_ofx — the projection centre for that width (w/2), i.e. the GTE OFX to use
// These were forward-declared inline at each call site, which is how three separate writers of the
// projection centre grew without anything tying them together. One declaration site now.
int gpu_vk_wide_engine(Core *core);
int gpu_vk_native_w(Core *core);
int gpu_vk_wide_engine_w(Core *core);
int gpu_vk_wide_engine_ofx(Core *core);
int gpu_vk_wide_presentation(Core *core);
int gpu_vk_wide_presentation_w(Core *core);
GuestProjectionPlan gpu_vk_latch_guest_projection(Core *core, GuestProjectionGeometry geometry);
// Leg-independent sink extent used by the guest projection latch and renderer planning.
void gpu_vk_present_sink_size(int *width, int *height);

// per-prim depth / OT-submission order (set by the gp0 tee before each VK draw)
void gpu_vk_set_order(Core *core, unsigned idx);
void gpu_vk_set_order_2d(Core *core, unsigned idx);
void gpu_vk_set_order_2d_n(Core *core, unsigned idx);
void gpu_vk_set_order_2d_bg(Core *core, unsigned idx);
void gpu_vk_set_order_2d_bg_n(Core *core, unsigned idx);
void gpu_vk_set_vd(Core *core, const float *d3);
void gpu_vk_set_vd_n(Core *core, const float *d3);
void gpu_vk_set_xyf(Core *core, const float *xf, const float *yf); // sub-pixel screen XY (#15 smoothing)
void gpu_vk_set_order_override(Core *core, uint32_t seq);
void gpu_vk_set_untextured_material(Core *core, int gouraud, int dither);
bool gpu_vk_order_bias_distinguishes(uint32_t seq);
float gpu_zbias_unit();
// The exact normalized-depth mapping used by world vertices, and the next input whose mapped D32
// value is representably distinct. Key-order ties use these instead of guessing an input epsilon.
float gpu_vk_map_3d_depth(float depth);
float gpu_vk_map_biased_3d_depth(float depth, float bias);
float gpu_vk_map_ordered_3d_depth(float depth, uint32_t order);
enum class GpuWorldDepthCompare { GreaterOrEqual };
inline constexpr GpuWorldDepthCompare kGpuWorldDepthCompare = GpuWorldDepthCompare::GreaterOrEqual;
inline bool gpu_vk_world_depth_test_passes(float candidate, float current) {
  return candidate >= current;
}
inline const char *gpu_vk_world_depth_compare_name() {
  return "GREATER_OR_EQUAL";
}
float gpu_vk_next_distinct_3d_depth(float depth, float nearer_limit);

// Dynamic shadow mapping: capture one OPAQUE world-geometry triangle's VIEW-SPACE positions (v0/v1/v2,
// each {x=ir1, y=ir2, z=pz} — the metric view space the deferred pass reconstructs) into the host shadow
// geometry stream. Rasterized from the directional light's view into a depth map, then sampled in the
// deferred pass to darken occluded pixels. Called from the opaque world submitters (submit.cpp,
// native_terrain.cpp). Cheap no-op when shadows are off. v0/v1/v2 point to 3 floats each.
void gpu_vk_shadow_push_tri(Core *core, const float *v0, const float *v1, const float *v2);
int gpu_vk_shadows_active(void); // shadows toggle (g_mods.shadows && g_mods.light) — submitters gate capture

// geometry tee + dirty-region mirror
void gpu_vk_dirty(Core *core, int x, int y, int w, int h);
void gpu_vk_semi_group(Core *core, int x0, int y0, int x1, int y1);
void gpu_vk_draw_tri(Core *core,
                     int x0,
                     int y0,
                     int r0,
                     int g0,
                     int b0,
                     int x1,
                     int y1,
                     int r1,
                     int g1,
                     int b1,
                     int x2,
                     int y2,
                     int r2,
                     int g2,
                     int b2,
                     int dax0,
                     int day0,
                     int dax1,
                     int day1);
void gpu_vk_draw_line(Core *core,
                      int x0,
                      int y0,
                      int r0,
                      int g0,
                      int b0,
                      int x1,
                      int y1,
                      int r1,
                      int g1,
                      int b1,
                      int dax0,
                      int day0,
                      int dax1,
                      int day1);
void gpu_vk_draw_tritri(Core *core,
                        const int *xs,
                        const int *ys,
                        const int *us,
                        const int *vs,
                        const unsigned char *rs,
                        const unsigned char *gs,
                        const unsigned char *bs,
                        int tpx,
                        int tpy,
                        int mode,
                        int raw,
                        int clutx,
                        int cluty,
                        int twmx,
                        int twmy,
                        int twox,
                        int twoy,
                        int dax0,
                        int day0,
                        int dax1,
                        int day1);
void gpu_vk_draw_semi(Core *core,
                      const int *xs,
                      const int *ys,
                      const int *us,
                      const int *vs,
                      const unsigned char *rs,
                      const unsigned char *gs,
                      const unsigned char *bs,
                      int tpx,
                      int tpy,
                      int mode,
                      int raw,
                      int clutx,
                      int cluty,
                      int twmx,
                      int twmy,
                      int twox,
                      int twoy,
                      int dax0,
                      int day0,
                      int dax1,
                      int day1,
                      int blend);
bool gpu_vk_painter_begin(Core *core, uint32_t range_id);
void gpu_vk_painter_set_item_object(Core *core, uint32_t object);
bool gpu_vk_painter_end(Core *core);

// present / per-frame / readback
void gpu_vk_present(Core *core, const uint16_t *src, int sx, int sy, int w, int h);
// Re-show the last presented frame without advancing or rebuilding anything (debug-server pause loop).
void gpu_vk_repaint(Core *core);
// PC-native fullscreen IMAGE present: draw a plain RGBA8 image (iw x ih) FULLSCREEN, letterboxed to 4:3
// (pillarbox, black bars), every rgb scaled by `fade` (0..1). Reusable, PSX-free (no VRAM/GP0/CLUT) —
// uploads the host RGBA into its own texture and draws it. Windowed presents to the swapchain; headless
// only uploads (no present) — verify headless via the caller's own CPU-side dump.
void gpu_vk_present_image(Core *core, const uint8_t *rgba, int iw, int ih, float fade);
void gpu_vk_frame_end(Core *core, const uint16_t *svram, int frame);
// Retain the last completed native composite before the next present can rebuild the live target.
// This is a request-only title boundary; the retained SDL texture remains private to GpuVkState.
// Returns false when no completed composite fence exists, rather than capturing a later scene.
bool gpu_vk_request_native_composite_capture(Core *core);
bool gpu_vk_native_composite_capture_ready(Core *core);
// preseqobj (per-object motion tracker): the present index this emit pass will dump, or -1 if no preseq
// capture is armed. Lets RenderQueue::emitItem key each [preseqobj] line to its present frame.
int gpu_vk_preseq_present_index(Core *core);
void gpu_vk_shot(Core *core, const char *path);
// Capture the PRESENTED PICTURE (s_present_img) rather than guest VRAM — the composite as the player
// sees it: letterboxed, faded, source-selected, 24bpp-decoded. Works in BOTH legs, and is the only
// capture in this framework that samples the present stage. See PSXPORT_PRESENT_SHOT_AT.
void gpu_vk_present_shot(Core *core, const char *path);
void gpu_vk_stats(Core *core, int *tri, int *tex, int *semi);

// (Engine-owned screen fade is now the PC-native subsystem class ScreenFade at
// game/render/screen_fade.h. The old gpu_set_fade / gpu_clear_fade / engine_fade_set entries
// lived here — deleted; native present path reads ScreenFade::get(core).)

// this-/last-frame native-geometry status (defined in gpu_native.cpp; read by the gpu_vk present path
// and by consuming games) — now per-instance. A frame with neither 3D nor a full-screen 2D backdrop is
// a raw framebuffer (FMV). BOTH belong here: gpu_seen3d_this_frame was previously declared only at its
// call sites, so a caller audit could not see it and the de-globalization deleted it as dead.
int gpu_had3d_last_frame(Core *core);
int gpu_seen3d_this_frame(Core *core);

#endif // GPU_GPU_H
