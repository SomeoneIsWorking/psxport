// gpu_native_internal.h — shared internals of the native GPU, split across translation units.
//
// gpu_native.cpp owns the rasterizer, GP0 parser, present/window and the canonical definitions of the
// render state below. De-globalization (2026-06-19): all MUTABLE render machine state now lives on a
// `GpuState` instance owned by `Game` (game.h), not in file-scope globals — so two cores can render
// independently and be diffed. The rasterizer functions are methods of GpuState; any code holding a
// `Core* c` reaches the state via `c->game->gpu`. The auxiliary modules carved out of gpu_native —
// gpu_trace.cpp (GP0-stream capture for gpu_differ) and gpu_debug.cpp (scene/provenance dumps) — take
// a `Core*` and read the per-instance state through it. NOT a public API; internal to the GPU TUs.
#ifndef GPU_NATIVE_INTERNAL_H
#define GPU_NATIVE_INTERNAL_H
#include <stdio.h>
#include <stdint.h>
#include "native_dl.h"   // NativePrim (member type of GpuState::s_ndl_cur)

struct Core;             // CPU/RAM handle (core.h); methods below take Core* but only by pointer
struct Game;             // back-pointer target (game.h); blit_src reaches gpu_vk via game->core

#define VRAM_W 1024
#define VRAM_H 512

// ---- Per-pixel primitive provenance (shared between the rasterizer and gpu_debug.cpp) -------
typedef struct { uint32_t gid, frame, node; int clut_x, clut_y, tp_x, tp_y, x0, y0, u0, v0;
                 uint8_t op, r, g, b, semi, tex, mode, blend; } ProvMeta;
#define PROVRING 16384

typedef struct { int x, y; uint8_t r, g, b; int u, v; } Vtx;   // rasterizer vertex (was local to gpu_native)

// ---- GpuState — the native GPU's per-instance render machine state + rasterizer ----------------
// Owned by Game (game.h has `GpuState gpu;`). Field names keep their historical `s_`/`g_` spelling so
// the rasterizer bodies are unchanged by the move (they now read members via implicit `this`).
struct GpuState {
  Game* game = nullptr;   // set by Game(); blit_src uses &game->core to reach the gpu_vk present wrapper

  // Backdrop-vs-HUD / gameplay-frame discrimination (read by the gpu_vk present path via Core).
  int s_seen3d = 0;       // has any GTE-projected (3D) prim been teed yet this frame? (else 2D backdrop band)
  int s_prev_had3d = 0;   // did LAST frame draw any 3D? = "this is a gameplay (3D) frame" (wide pillarbox gate)

  // VRAM (textures + framebuffers) and the fps60 in-between buffer
  uint16_t  s_vram[VRAM_W * VRAM_H] = {};
  uint16_t  s_interp[VRAM_W * VRAM_H] = {};
  uint16_t* s_fb_base = s_vram;                              // put_px target: s_vram, or s_interp while synthesizing
  uint16_t* vram(int x, int y) { return &s_vram[(y & 511) * VRAM_W + (x & 1023)]; }
  uint16_t* fb(int x, int y)   { return &s_fb_base[(y & 511) * VRAM_W + (x & 1023)]; }

  // Draw env (GP0 E1..E6)
  int s_da_x0 = 0, s_da_y0 = 0, s_da_x1 = 1023, s_da_y1 = 511; // draw clip area
  int s_off_x = 0, s_off_y = 0;                               // draw offset
  int s_tp_x = 0, s_tp_y = 0;                                 // texpage base
  int s_tp_mode = 0, s_tp_blend = 0, s_tp_dither = 0;         // texture color mode / blend / dither
  int s_tw_mx = 0, s_tw_my = 0, s_tw_ox = 0, s_tw_oy = 0;     // texture window mask/offset
  int s_clut_x = 0, s_clut_y = 0;                             // CLUT base

  // Display control (GP1)
  int s_disp_x = 0, s_disp_y = 0;                             // VRAM top-left of the displayed region
  int s_disp_w = 320, s_disp_h = 240;
  int s_disp_vy0 = 0, s_disp_vy1 = 240;                       // GP1(0x07) vertical display range
  int s_disp_480i = 0;                                        // GP1(0x08) interlace + 480-line

  // Per-frame prim ordering + provenance
  uint32_t s_prim_order = 0;                                  // OT submission index of the current prim (VK depth)
  uint32_t s_prim_gid = 0;                                    // monotonic primitive counter (provenance)
  uint32_t s_prov[VRAM_W * VRAM_H] = {};                      // gid of last writer per pixel
  int      s_prov_on = -1;                                    // lazily 1 if PSXPORT_PROVAT set
  ProvMeta s_provmeta[PROVRING] = {};                         // gid -> prim details (ring)

  // GP0 command FIFO + VRAM transfer
  uint32_t s_fifo[256] = {};                                  // big enough for variable-length poly-lines
  uint32_t s_fifo_addr[256] = {};                             // guest source addr of each FIFO word
  uint32_t s_gp0_src = 0;                                     // OT walk sets this per word (Phase-1 attach)
  int s_fcount = 0, s_fneed = 0;
  int s_pl = 0, s_pl_g = 0;                                   // poly-line in progress / gouraud
  int s_xfer = 0, s_xfer_x = 0, s_xfer_y = 0, s_xfer_w = 0, s_xfer_h = 0, s_xfer_px = 0;

  const NativePrim* s_ndl_cur = 0;                            // native-DL prim currently being rendered

  // Frame + OT bookkeeping
  int s_frame = 0;                                            // present-frame counter
  uint32_t s_cur_node = 0;                                    // RAM addr of the OT node being fed to GP0
  uint32_t g_ot_madr = 0;                                     // last OT DMA root
  uint32_t g_dma_src = 0;                                     // last block-DMA source

  // ---- rasterizer / GP0 / present methods (bodies in gpu_native.cpp) ----
  uint16_t sample_tex(int u, int v);
  void put_px_b(int x, int y, uint8_t r, uint8_t g, uint8_t b, int semi);
  void put_px(int x, int y, uint8_t r, uint8_t g, uint8_t b);
  void tri_px(Vtx a, Vtx b, Vtx c, int x, int y, int tex, int shade, int semi, int raw, long aa);
  void tri(Vtx a, Vtx b, Vtx c, int tex, int shade, int semi, int raw);
  void semi_dump(const char* kind, int blend, int r, int g, int b, int x0, int y0, int x1, int y1, int offy);
  void clutwatch_dump(const char* tag, int rx, int ry, int rw, int rh);
  void clutwatch_xfer(const char* tag, int rx, int ry, int rw, int rh);
  void prov_begin(uint8_t op, int tex, int semi, uint8_t r, uint8_t g, uint8_t b, int x0, int y0, int u0, int v0);
  void raster_sprite(int op, int x, int y, int u0, int v0, int w, int h,
                     uint8_t cr, uint8_t cg, uint8_t cb, int textured, int semi);
  void raster_line(int x0, int y0, int x1, int y1, uint8_t cr, uint8_t cg, uint8_t cb, int semi);
  void set_texpage(uint16_t tp);
  void set_clut(uint16_t cl);
  void gp0_exec(Core* core);
  void gpu_gp0(Core* core, uint32_t w);
  void gpu_gp1(uint32_t w);
  void ndl_render_node(Core* core, uint32_t addr);
  void gpu_dma2_linked_list(Core* core, uint32_t madr);
  void gpu_dma2_block(Core* core, uint32_t madr, int count, int to_gpu);
  void gpu_native_load_image(Core* core, int x, int y, int w, int h, uint32_t src);
  int  gpu_native_load_vram(const char* path);
  void ensure_window();
  void blit_src(const uint16_t* src, int sx, int sy);
  void present_window();
  void shot_buf(const uint16_t* src, int dx, int dy, const char* path);
  void gpu_repaint();
  void gpu_native_shot(const char* path);
  void gpu_present_ex(Core* core, int do_blit);
  void gpu_present(Core* core);
  uint16_t gpu_vram_peek(int x, int y);
  void gpu_vram_load(const uint16_t* src);
  void gpu_vram_save(uint16_t* dst);
  void gpu_provat_enable();
  int  gpu_frame_no();
  // fps60 in-between synthesizer entry points (re-rasterize a lerped list into s_interp)
  void gpu_fps60_draw_poly(int op, int nv, const int* xs, const int* ys, const int* us, const int* vs,
                           const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                           int tp_x, int tp_y, int mode, int blend, int dither, int clut_x, int clut_y);
  void gpu_fps60_draw_sprite(int op, int x, int y, int u0, int v0, int w, int h,
                             int r, int g, int b, int tp_x, int tp_y, int mode, int blend,
                             int clut_x, int clut_y);
  void gpu_fps60_draw_line(int x0, int y0, int x1, int y1, int r, int g, int b, int semi);
  void gpu_fps60_begin_interp(int off_x, int off_y, int cx0, int cy0, int cx1, int cy1);
  void gpu_fps60_end_interp();
  void gpu_fps60_blit_vram(int dx, int dy);
  void gpu_fps60_blit_interp(int dx, int dy);
  void gpu_fps60_shot_vram(int dx, int dy, const char* path);
  void gpu_fps60_shot_interp(int dx, int dy, const char* path);
};

// ---- Diagnostic dumps (gpu_debug.cpp) — read the per-instance state via Core* -----------------
void gpu_scene_dump(Core* core, FILE* out, uint32_t madr);   // classify an OT's display list (PSXPORT_SCENEDUMP)

// ---- GP0-stream trace capture (gpu_trace.cpp) -------------------------------------------------
void trace_record(Core* core, uint32_t w);   // record one GP0 word (no-op unless armed); from gpu_gp0
void trace_flush(Core* core);                 // write the captured frame's trace; from gpu_present_ex

// ---- Public GPU API (free functions; thin wrappers over GpuState methods, reached via Core*) ---
void gpu_gp0(Core* core, uint32_t w);
void gpu_gp1(Core* core, uint32_t w);
void gpu_dma2_linked_list(Core* core, uint32_t madr);
void gpu_dma2_block(Core* core, uint32_t madr, int count, int to_gpu);
void gpu_present(Core* core);
void gpu_present_ex(Core* core, int do_blit);
void gpu_native_load_image(Core* core, int x, int y, int w, int h, uint32_t src);
int  gpu_native_load_vram(Core* core, const char* path);
void gpu_native_shot(Core* core, const char* path);
void gpu_repaint(Core* core);
int  gpu_frame_no(Core* core);
void gpu_provat_enable(Core* core);
uint16_t gpu_vram_peek(Core* core, int x, int y);
void gpu_vram_load(Core* core, const uint16_t* src);
void gpu_vram_save(Core* core, uint16_t* dst);
void gpu_fps60_draw_poly(Core* core, int op, int nv, const int* xs, const int* ys, const int* us, const int* vs,
                         const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                         int tp_x, int tp_y, int mode, int blend, int dither, int clut_x, int clut_y);
void gpu_fps60_draw_sprite(Core* core, int op, int x, int y, int u0, int v0, int w, int h,
                           int r, int g, int b, int tp_x, int tp_y, int mode, int blend, int clut_x, int clut_y);
void gpu_fps60_draw_line(Core* core, int x0, int y0, int x1, int y1, int r, int g, int b, int semi);
void gpu_fps60_begin_interp(Core* core, int off_x, int off_y, int cx0, int cy0, int cx1, int cy1);
void gpu_fps60_end_interp(Core* core);
void gpu_fps60_blit_vram(Core* core, int dx, int dy);
void gpu_fps60_blit_interp(Core* core, int dx, int dy);
void gpu_fps60_shot_vram(Core* core, int dx, int dy, const char* path);
void gpu_fps60_shot_interp(Core* core, int dx, int dy, const char* path);
// gpu_provat_display / gpu_prov_dump (gpu_debug.cpp) take Core* too:
void gpu_provat_display(Core* core, FILE* out, int qx, int qy);
void gpu_prov_dump(Core* core, int vx, int vy);

uint32_t mem_r32(uint32_t);

#endif // GPU_NATIVE_INTERNAL_H
