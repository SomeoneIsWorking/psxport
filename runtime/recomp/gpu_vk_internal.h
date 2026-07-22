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
#ifndef GPU_GPU_INTERNAL_H
#define GPU_GPU_INTERNAL_H
#include <stdint.h>

struct Game;     // back-pointer target (game.h); only frame_via_fb() uses it (to reach s_seen3d via Core)
struct Panel;    // gpu_vk.cpp: a self-contained per-target render view (Vulkan-typed; pointer-only here)
struct TexVtx;   // gpu_vk.cpp: a textured vertex (defined there; pointer-only in the tex_emit signature)
struct Core;     // CPU/RAM handle (core.h)
// SDL_GPU opaque handle types (per-Game render TARGETS live here now — see below). Forward decls only,
// so this header still pulls no SDL headers.
struct SDL_GPUTexture;
struct SDL_GPUBuffer;
struct SDL_GPUTransferBuffer;
struct SDL_GPUCommandBuffer;

// Capacities for the moved array members (also used by the gpu_vk.cpp bodies via this header).
#define SEMI_GRP_CAP 2048
#define DIRTY_CAP    4096

// A SW-written VRAM region to mirror into the persistent VK image (gpu_vk_dirty). Plain int rect — NOT
// Vulkan's VkRect2D; named VkRect to keep the gpu_vk.cpp bodies byte-unchanged.
struct VkRect { int x, y, w, h; };

// Per-instance vertex-batch descriptor. The concrete TriVtx / TexVtx structs live in gpu_vk.cpp;
// this header exposes only the count fields + opaque `void*` pointers to the CPU-side batches so the
// batches themselves can be per-Core (SBS's two cores keep separate per-frame geometry).
#define GGS_NUM_BLEND_MODES 4                   // PSX semi blend modes (0=avg 1=add 2=sub 3=add4)

// 2D order bands (bug #55 fix): content submitted via RQ_OM_2D_BG / RQ_OM_2D_FG (render_queue.cpp) is NOT
// part of the 3D world — it must render at NATIVE VRAM resolution regardless of the live ires scale (see
// gpu_vk.cpp render_geom's band split). Kept as two bands, not one, because the engine's existing order
// scheme (NATIVE_3D_MIN/MAX in gpu_vk.cpp) already draws 2D_BG strictly BEHIND the 3D world and 2D_FG
// strictly IN FRONT of it — the render_geom band order (2D_BG -> 3D -> 2D_FG) reproduces that without a
// depth test shared across targets.
#define GGS_2D_BG 0
#define GGS_2D_FG 1
#define GGS_NUM_2D_BANDS 2

// ---- GpuVkState — the VK backend's per-instance, per-frame render machine state + its methods --------
struct GpuVkState {
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

  // ---- 2D (non-world) GPU vertex buffers — bug #55 fix ------------------------------------------------
  // A SEPARATE vertex-buffer set per 2D band (GGS_2D_BG / GGS_2D_FG), so 2D content never shares the
  // 3D-world buffers above and never gets bound to the ires-scaled target: render_geom draws these bands
  // directly onto s_vram_tex/s_depth/s_color_rgba at native VRAM resolution, regardless of the live ires
  // scale. Smaller capacity than the 3D buffers (TRI2D_CAP/TEX2D_CAP in gpu_vk.cpp) — HUD/menu/2D-layer
  // geometry per frame is a small fraction of the 3D world's.
  SDL_GPUBuffer*          s_tri2d_vbuf[GGS_NUM_2D_BANDS] = {};
  SDL_GPUBuffer*          s_tex2d_vbuf[GGS_NUM_2D_BANDS] = {};
  SDL_GPUBuffer*          s_semi2d_vbuf[GGS_NUM_2D_BANDS][GGS_NUM_BLEND_MODES] = {};
  SDL_GPUTransferBuffer*  s_tri2d_xfer[GGS_NUM_2D_BANDS] = {};
  SDL_GPUTransferBuffer*  s_tex2d_xfer[GGS_NUM_2D_BANDS] = {};
  SDL_GPUTransferBuffer*  s_semi2d_xfer[GGS_NUM_2D_BANDS][GGS_NUM_BLEND_MODES] = {};

  // ---- ires (internal resolution) scaled 3D target — Pass 2, gpu_vk.cpp render_geom -----------------
  // A SEPARATE, larger color+depth(+semi-blend-intermediate) target that the opaque/semi geometry passes
  // render into at `i`x the fixed VRAM canvas (1024*i x 512*i) when the live ires scale is >1, so 3D edges
  // rasterize at higher internal resolution; render_geom then downsamples ONLY the display sub-rect back
  // into s_vram_tex (linear-filtered SDL_BlitGPUTexture) so every 2D-space consumer (texture pages, CLUTs,
  // sprite blits, readback, SBS) stays on the fixed, pixel-exact VRAM texture untouched. i==1 never touches
  // these fields (render_geom stays on the direct s_vram_tex path — no extra blit). Lazily created; torn
  // down + rebuilt by ensure_ires_targets() whenever the live scale changes (RmlUi overlay can flip
  // mods.ires mid-run).
  SDL_GPUTexture* s_ires_color = nullptr;   // RG8 (packed 1555), VRAM_W*i x VRAM_H*i
  SDL_GPUTexture* s_ires_depth = nullptr;   // D32
  SDL_GPUTexture* s_ires_rgba  = nullptr;   // float RGBA semi-blend intermediate, ires-scaled
  int s_ires_scale = 1;                     // scale these targets are built for (1 = none built/needed)
  void ensure_ires_targets(int i);          // i<=1: tear down (no targets held); i>1: (re)build if changed

  // M2/M3 batch state (counters + the three host vertex buffers + the semi-overlap grouping + the dirty-
  // VRAM list) moved OUT of GpuVkState into the file-scope GeomBatch s_gb[2] in gpu_vk.cpp, so the renderer
  // holds TWO independent batches and draws each into its own panel image (dual-view native-vs-PSX
  // side-by-side, 2026-06-24). The touching methods bind them via BIND_BATCH() — see gpu_vk.cpp.

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
  // (on-demand gpu_vk_shot) + last frame's batched vertex counts (vkstats probe).
  int s_present_sx = 0, s_present_sy = 0;
  // HIGH-RES PRESENT (ires>1): scale of the ires composite built THIS frame (render_geom sets it when
  // ires && has3d — the s_ires_color high-res composite is valid). 0 = present from native s_vram_tex
  // (pure-2D frames / ires=1). Read by present() to pick the present source. See gpu_vk.cpp.
  int s_present_ires = 0;
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
  // Opaque void*: the concrete TriVtx/TexVtx layouts live in gpu_vk.cpp — the touching methods cast
  // internally. Buffers are heap-allocated on first use (create_3d) and freed at process exit.
  void* s_tri_buf = nullptr;
  int   s_tri_n   = 0;
  void* s_tex_buf = nullptr;
  int   s_tex_n   = 0;
  void* s_semi_buf[GGS_NUM_BLEND_MODES] = {nullptr, nullptr, nullptr, nullptr};
  int   s_semi_n[GGS_NUM_BLEND_MODES]   = {0, 0, 0, 0};
  // 2D (non-world) CPU-side batches — bug #55 fix. Same lifetime/reset contract as the 3D ones above
  // (lazily allocated, reset every frame_end); routed here instead of s_tri_buf/s_tex_buf/s_semi_buf
  // whenever the emitting draw call is NOT tagged world-3D (s_vd unset) — see draw_tri/draw_tritri/
  // draw_semi + the ggs_2d_band() classifier in gpu_vk.cpp.
  void* s_tri2d_buf[GGS_NUM_2D_BANDS] = {};
  int   s_tri2d_n[GGS_NUM_2D_BANDS]   = {};
  void* s_tex2d_buf[GGS_NUM_2D_BANDS] = {};
  int   s_tex2d_n[GGS_NUM_2D_BANDS]   = {};
  void* s_semi2d_buf[GGS_NUM_2D_BANDS][GGS_NUM_BLEND_MODES] = {};
  int   s_semi2d_n[GGS_NUM_2D_BANDS][GGS_NUM_BLEND_MODES]   = {};
  // Last-frame draw counts (for the `vkstats` debug-server probe). Written by render_geom, read by
  // `stats` — per-Core so `@a vkstats` / `@b vkstats` return each core's independent counts.
  int   s_dbg_tri_c  = 0;
  int   s_dbg_tex_c  = 0;
  int   s_dbg_semi_c = 0;

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
  // Re-show the last BUILT frame (no VRAM upload, no geometry re-render, no batch reset) — the debug-server
  // pause loop's window keep-alive. See the body in gpu_vk.cpp for why a pause must never re-render.
  void repaint();
  void show_composite(SDL_GPUCommandBuffer* cmd);   // the shared swapchain half of present()/repaint()
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
