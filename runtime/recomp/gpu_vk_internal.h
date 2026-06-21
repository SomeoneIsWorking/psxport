// gpu_vk_internal.h — the Vulkan present backend's per-instance render machine state.
//
// De-globalization R2 (2026-06-19): gpu_vk.cpp's PER-FRAME mutable render state (batch counters, the
// current prim's depth/order, the semi-transparency overlap grouping, the dirty-VRAM region list, this
// frame's present origin and the last-frame diag snapshots) now lives on a `GpuVkState` instance owned
// by `Game` (game.h), not in file-scope globals — so two cores can render independently and be diffed.
// The touching gpu_vk functions are methods of GpuVkState (field names keep their historical `s_`
// spelling so the bodies are unchanged by the move); the public C-style API stays stable via thin
// free-function wrappers reached through `core->game->gpu_vk`.
//
// What STAYS file-scope SHARED in gpu_vk.cpp (NOT here): the Vulkan device/swapchain/pipeline/buffer
// HANDLES (one VK device per process — host-output singletons, not machine state) and config-caches.
// NOT a public API; internal to the GPU TUs.
#ifndef GPU_VK_INTERNAL_H
#define GPU_VK_INTERNAL_H
#include <stdint.h>

struct Game;     // back-pointer target (game.h); only frame_via_fb() uses it (to reach s_seen3d via Core)
struct Panel;    // gpu_vk.cpp: a self-contained per-target render view (Vulkan-typed; pointer-only here)
struct TexVtx;   // gpu_vk.cpp: a textured vertex (defined there; pointer-only in the tex_emit signature)
struct Core;     // CPU/RAM handle (core.h)

// Capacities for the moved array members (also used by the gpu_vk.cpp bodies via this header).
#define SEMI_GRP_CAP 2048
#define DIRTY_CAP    4096

// A SW-written VRAM region to mirror into the persistent VK image (gpu_vk_dirty). Plain int rect — NOT
// Vulkan's VkRect2D; named VkRect to keep the gpu_vk.cpp bodies byte-unchanged.
struct VkRect { int x, y, w, h; };

// ---- GpuVkState — the VK backend's per-instance, per-frame render machine state + its methods --------
struct GpuVkState {
  Game* game = nullptr;   // set by Game(); reached only by frame_via_fb() for s_seen3d (via game->core)

  // M2/M3 batch counters: vertices appended to the opaque-flat / opaque-textured / semi batches this frame
  int s_tri_n = 0, s_tex_n = 0, s_semi_n = 0;

  // Current prim's depth/order (set by the gp0 tee before each draw). s_vd/s_vdn = per-vertex native depth
  // (NULL = fall back to the per-prim OT-order s_cur_ord/s_cur_ordn). ordn = the PSXPORT_SBS native channel.
  const float* s_vd = nullptr;
  const float* s_vdn = nullptr;
  float s_cur_ord = 0, s_cur_ordn = 0;

  // Sub-pixel float SCREEN XY for the engine-owned 3D world path (vertex smoothing / issue #15). When set,
  // tex_emit uses these floats for the vertex POSITION instead of the integer xs/ys (which the world submit
  // would otherwise have rounded). NULL = use the integer xs/ys (2D/HUD/un-owned prims keep snapping).
  // Same lifetime contract as s_vd: set by the emit just before a draw call, cleared after.
  const float* s_xf = nullptr;
  const float* s_yf = nullptr;

  // OT-order-correct semi grouping: vertex-index boundaries of each non-overlapping group + the current
  // group's accumulated abs bbox.
  int s_semi_grp[SEMI_GRP_CAP] = {};
  int s_semi_grp_n = 0;
  int s_sg_x0 = 0, s_sg_y0 = 0, s_sg_x1 = 0, s_sg_y1 = 0, s_sg_valid = 0;

  // Dirty VRAM regions written by SW this frame (uploads / copies / fills), mirrored at present.
  VkRect s_dirty[DIRTY_CAP] = {};
  int    s_dirty_n = 0;

  // This frame's faithful display origin (for the LIGHT screen map) and the last-presented region
  // (on-demand gpu_vk_shot) + last frame's batched vertex counts (vkstats probe).
  int s_present_sx = 0, s_present_sy = 0;
  int s_last_sx = 0, s_last_sy = 0, s_last_w = 320, s_last_h = 240;
  int s_dbg_tri = 0, s_dbg_tex = 0, s_dbg_semi = 0;

  // ---- methods (bodies in gpu_vk.cpp; reached via core->game->gpu_vk from the wrappers) ----
  // public-API methods
  void set_vd(const float* d3);
  void set_vd_n(const float* d3);
  void set_xyf(const float* xf, const float* yf);   // sub-pixel screen XY for the world path (#15)
  void set_order(unsigned idx);
  void set_order_2d(unsigned idx);
  void set_order_2d_n(unsigned idx);
  void set_order_2d_bg(unsigned idx);
  void set_order_2d_bg_n(unsigned idx);
  void semi_group(int x0, int y0, int x1, int y1);
  void stats(int* tri, int* tex, int* semi);
  void dirty(int x, int y, int w, int h);
  void present(const uint16_t* src, int sx, int sy, int w, int h);
  void draw_tri(int x0,int y0,int r0,int g0,int b0, int x1,int y1,int r1,int g1,int b1,
                int x2,int y2,int r2,int g2,int b2);
  void draw_tritri(const int* xs, const int* ys, const int* us, const int* vs,
                   const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                   int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                   int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1);
  void draw_semi(const int* xs, const int* ys, const int* us, const int* vs,
                 const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                 int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                 int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1, int blend);
  void shot(const char* path);
  void frame_end(const uint16_t* svram, int frame);
  void tritest();
  // internal helpers (called via this from the methods above)
  void panel_upload(Panel* p);
  void panel_render(Panel* p);
  void ssao_pass();
  void shadow_pass();   // rasterize the captured world geometry from the light's view into the shadow map
  int  frame_via_fb();
  void tex_emit(TexVtx* t, const int* xs, const int* ys, const int* us, const int* vs,
                const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1,
                int semi, int blend);
  void tri_render_and_readback(uint16_t* out);
  void tri_over_bg_readback(const uint16_t* bg, uint16_t* out);
};

#endif // GPU_VK_INTERNAL_H
