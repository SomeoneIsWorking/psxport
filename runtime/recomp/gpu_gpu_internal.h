// gpu_gpu_internal.h — the Vulkan present backend's per-instance render machine state.
//
// De-globalization R2 (2026-06-19): gpu_gpu.cpp's PER-FRAME mutable render state (batch counters, the
// current prim's depth/order, the semi-transparency overlap grouping, the dirty-VRAM region list, this
// frame's present origin and the last-frame diag snapshots) now lives on a `GpuGpuState` instance owned
// by `Game` (game.h), not in file-scope globals — so two cores can render independently and be diffed.
// The touching gpu_gpu functions are methods of GpuGpuState (field names keep their historical `s_`
// spelling so the bodies are unchanged by the move); the public C-style API stays stable via thin
// free-function wrappers reached through `core->game->gpu_gpu`.
//
// What STAYS file-scope SHARED in gpu_gpu.cpp (NOT here): the Vulkan device/swapchain/pipeline/buffer
// HANDLES (one VK device per process — host-output singletons, not machine state) and config-caches.
// NOT a public API; internal to the GPU TUs.
#ifndef GPU_GPU_INTERNAL_H
#define GPU_GPU_INTERNAL_H
#include <stdint.h>

struct Game;     // back-pointer target (game.h); only frame_via_fb() uses it (to reach s_seen3d via Core)
struct Panel;    // gpu_gpu.cpp: a self-contained per-target render view (Vulkan-typed; pointer-only here)
struct TexVtx;   // gpu_gpu.cpp: a textured vertex (defined there; pointer-only in the tex_emit signature)
struct Core;     // CPU/RAM handle (core.h)
// SDL_GPU opaque handle types (per-Game render TARGETS live here now — see below). Forward decls only,
// so this header still pulls no SDL headers.
struct SDL_GPUTexture;
struct SDL_GPUBuffer;
struct SDL_GPUTransferBuffer;

// Capacities for the moved array members (also used by the gpu_gpu.cpp bodies via this header).
#define SEMI_GRP_CAP 2048
#define DIRTY_CAP    4096

// A SW-written VRAM region to mirror into the persistent VK image (gpu_gpu_dirty). Plain int rect — NOT
// Vulkan's VkRect2D; named VkRect to keep the gpu_gpu.cpp bodies byte-unchanged.
struct VkRect { int x, y, w, h; };

// Per-instance vertex-batch descriptor. The concrete TriVtx / TexVtx structs live in gpu_gpu.cpp;
// this header exposes only the count fields + opaque `void*` pointers to the CPU-side batches so the
// batches themselves can be per-Core (SBS's two cores keep separate per-frame geometry).
#define GGS_NUM_BLEND_MODES 4                   // PSX semi blend modes (0=avg 1=add 2=sub 3=add4)

// ---- GpuGpuState — the VK backend's per-instance, per-frame render machine state + its methods --------
struct GpuGpuState {
  Game* game = nullptr;   // set by Game(); reached only by frame_via_fb() for s_seen3d (via game->core)

  // ---- per-Game GPU render TARGETS (deglobalized off GpuDevice 2026-07-10) --------------------------
  // Each Game owns its own GPU-side guest VRAM image + everything a frame renders through: the texture/
  // CLUT atlas snapshot, the depth buffer, the float semi-blend intermediate, and the vertex buffers.
  // Under SBS the two cores previously rendered through ONE shared set on the device singleton — a
  // structural cross-core leak. Only true host-device objects (window, SDL device, samplers, PIPELINES)
  // stay on GpuDevice. Created lazily by ensure_targets() (needs the device up); process-lifetime.
  SDL_GPUTexture*         s_vram_tex = nullptr;   // guest VRAM (RG8 = PSX 1555 LE bytes), render target
  SDL_GPUTransferBuffer*  s_vram_xfer = nullptr;  // host→VRAM upload (1024*512*2)
  SDL_GPUTransferBuffer*  s_rb_xfer = nullptr;    // VRAM→host download (readback / shot)
  SDL_GPUTexture*         s_vram_snap = nullptr;  // texture-source snapshot (RG8 SAMPLER)
  SDL_GPUTransferBuffer*  s_snap_xfer = nullptr;
  SDL_GPUTexture*         s_depth = nullptr;      // D32 depth (ordering)
  SDL_GPUTexture*         s_color_rgba = nullptr; // float RGBA semi-blend intermediate
  SDL_GPUBuffer*          s_tri_vbuf = nullptr;
  SDL_GPUBuffer*          s_tex_vbuf = nullptr;
  SDL_GPUBuffer*          s_semi_vbuf[GGS_NUM_BLEND_MODES] = {};
  SDL_GPUTransferBuffer*  s_tri_xfer = nullptr;
  SDL_GPUTransferBuffer*  s_tex_xfer = nullptr;
  SDL_GPUTransferBuffer*  s_semi_xfer[GGS_NUM_BLEND_MODES] = {};
  int s_have_3d = 0;                              // THIS Game's targets created
  void ensure_targets();                          // lazy target creation (device must be inited)

  // M2/M3 batch state (counters + the three host vertex buffers + the semi-overlap grouping + the dirty-
  // VRAM list) moved OUT of GpuGpuState into the file-scope GeomBatch s_gb[2] in gpu_gpu.cpp, so the renderer
  // holds TWO independent batches and draws each into its own panel image (dual-view native-vs-PSX
  // side-by-side, 2026-06-24). The touching methods bind them via BIND_BATCH() — see gpu_gpu.cpp.

  // Current prim's depth/order (set by the gp0 tee before each draw). s_vd/s_vdn = per-vertex native depth
  // (NULL = fall back to the per-prim OT-order s_cur_ord/s_cur_ordn). ordn = the PSXPORT_SBS native channel.
  const float* s_vd = nullptr;
  const float* s_vdn = nullptr;
  float s_cur_ord = 0, s_cur_ordn = 0;
  // Paint-order depth TIEBREAK bias (z-fighting fix): a tiny per-prim increment = paint_order * ZBIAS_UNIT,
  // added to the 3D-band per-vertex depth so that when two world prims are at (near-)EQUAL real depth — the
  // barrel/decoration case where the game's integer geometry is genuinely coplanar to GTE fixed-point
  // resolution (1/4096) and PSX disambiguates purely by OT/paint order — the LATER-drawn prim deterministically
  // wins (GREATER_OR_EQUAL). The bias is far below any genuine world depth separation, so real occlusion is
  // unchanged; it only breaks otherwise-unstable ties. Set in set_order from the emit index; 0 for 2D bands.
  float s_depth_bias = 0.f;

  // Sub-pixel float SCREEN XY for the engine-owned 3D world path (vertex smoothing / issue #15). When set,
  // tex_emit uses these floats for the vertex POSITION instead of the integer xs/ys (which the world submit
  // would otherwise have rounded). NULL = use the integer xs/ys (2D/HUD/un-owned prims keep snapping).
  // Same lifetime contract as s_vd: set by the emit just before a draw call, cleared after.
  const float* s_xf = nullptr;
  const float* s_yf = nullptr;

  // (semi-overlap grouping s_semi_grp[]/s_sg_* and the dirty-VRAM list s_dirty[] moved into GeomBatch —
  // see the note above; each render target owns its own.)

  // (Engine-owned screen fade moved to class ScreenFade at game/render/screen_fade.h; state lives in guest
  // memory so per-Core / SBS isolation comes for free. Native present path reads ScreenFade::get(core).)

  // This frame's faithful display origin (for the LIGHT screen map) and the last-presented region
  // (on-demand gpu_gpu_shot) + last frame's batched vertex counts (vkstats probe).
  int s_present_sx = 0, s_present_sy = 0;
  int s_last_sx = 0, s_last_sy = 0, s_last_w = 320, s_last_h = 240;
  int s_dbg_tri = 0, s_dbg_tex = 0, s_dbg_semi = 0;
  // PRESENT-SEQUENCE capture (REPL `preseq <N> [dir]`): dump the next N PRESENTED frames — every
  // present-pass readback, so REAL and fps60-INTERPOLATED frames both land — as dir/p%04d.ppm.
  // This is the headless way to verify temporal artifacts (interp-frame flicker/judder) that the
  // per-game-frame `shot` can never see (it only samples real frames).
  int  s_preseq_left = 0, s_preseq_idx = 0;
  char s_preseq_dir[128] = {0};

  // Per-Core CPU vertex batches — filled during gp0_exec (draw_tri/draw_tritri/draw_semi below), read
  // by render_geom at grab_pane/present time, and RESET by frame_end. Two SBS cores each keep their
  // own batches so one core's per-frame geometry can't leak into the other's render (2026-07-03).
  // Opaque void*: the concrete TriVtx/TexVtx layouts live in gpu_gpu.cpp — the touching methods cast
  // internally. Buffers are heap-allocated on first use (create_3d) and freed at process exit.
  void* s_tri_buf = nullptr;
  int   s_tri_n   = 0;
  void* s_tex_buf = nullptr;
  int   s_tex_n   = 0;
  void* s_semi_buf[GGS_NUM_BLEND_MODES] = {nullptr, nullptr, nullptr, nullptr};
  int   s_semi_n[GGS_NUM_BLEND_MODES]   = {0, 0, 0, 0};
  // Last-frame draw counts (for the `vkstats` debug-server probe). Written by render_geom, read by
  // `stats` — per-Core so `@a vkstats` / `@b vkstats` return each core's independent counts.
  int   s_dbg_tri_c  = 0;
  int   s_dbg_tex_c  = 0;
  int   s_dbg_semi_c = 0;

  // ---- methods (bodies in gpu_gpu.cpp; reached via core->game->gpu_gpu from the wrappers) ----
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
  // PSXPORT_SBS: composite TWO different cores' frames (already emitted into batches 0 and 1) into the
  // two side-by-side panes in ONE window frame. vramA/vramB are each core's CPU VRAM; each is uploaded to
  // its own staging buffer + panel so the two panes show the two cores. One acquire/cmd/submit/present.
  // repaint=1: re-present the existing persistent panel images (no VRAM upload / geometry re-record), used
  // while the harness is PAUSED so the window stays live. repaint=0: normal two-core upload+render+composite.
  void present_sbs(const uint16_t* vramA, const uint16_t* vramB, int sx, int sy, int w, int h, int repaint);
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
  void shot_b(const char* path);   // SBS: capture target 1 (core B / right pane)
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

#endif // GPU_GPU_INTERNAL_H
