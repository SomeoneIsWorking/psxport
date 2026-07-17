// gpu_native_internal.h — shared internals of the native GPU, split across translation units.
//
// gpu_native.cpp owns the rasterizer, GP0 parser, present/window and the canonical definitions of the
// render state below. De-globalization (2026-06-19): all MUTABLE render machine state now lives on a
// `GpuState` instance owned by `Game` (game.h), not in file-scope globals — so two cores can render
// independently and be diffed. The rasterizer functions are methods of GpuState; any code holding a
// `Core* c` reaches the state via `c->game->gpu`. The auxiliary module carved out of gpu_native —
// gpu_debug.cpp (scene/provenance dumps) — takes a `Core*` and reads the per-instance state through
// it. NOT a public API; internal to the GPU TUs.
#ifndef GPU_NATIVE_INTERNAL_H
#define GPU_NATIVE_INTERNAL_H
#include <stdio.h>
#include <stdint.h>

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

  // ORACLE soft-GPU (docs/oracle.md Phase 2): when set, this GpuState rasterizes its GP0 stream into
  // s_vram in SOFTWARE (the existing tri()/raster_sprite()/raster_line() path) and NEVER touches the VK
  // backend — even though the native port Core keeps the VK backend up (gpu_vk_enabled()==1 is global).
  // The diff harness sets it on the interpreter oracle Core so we get the REAL PSX cutscene framebuffer
  // (s_vram) to dump/diff, fully decoupled from the native render path. Default 0 = the shipping VK path.
  int soft_gpu = 0;
  inline bool vk_path() const { extern int gpu_vk_enabled(void); return gpu_vk_enabled() && !soft_gpu; }
  inline bool sw_path() const { extern int gpu_vk_enabled(void); return soft_gpu || !gpu_vk_enabled(); }

  // Backdrop-vs-HUD / gameplay-frame discrimination (read by the gpu_vk present path via Core).
  int s_seen3d = 0;       // has any GTE-projected (3D) prim been teed yet this frame? (else 2D backdrop band)
  int s_ot_2d_drawn = 0;  // # of genuine 2D-overlay prims drawn during a twoDOnly walk (reset at walk start).
                          // drawOTag reads it: >0 in pc_render => the field needs unimplemented native 2D UI
                          // -> abortUnimplemented (rendering the world alone would MASK the missing overlay).
  int bg_2d(int bx0, int by0, int bx1, int by1);   // FALLBACK 2D backdrop-vs-HUD by screen coverage (un-owned scenes)
  int s_prev_had3d = 0;   // did LAST frame draw any 3D? = "this is a gameplay (3D) frame" (wide pillarbox gate)
  // #54: a pure-2D screen (title/menu) never sets s_seen3d, so s_prev_had3d alone left the widen mechanism
  // permanently OFF there — nothing ever painted the wide-margin VRAM columns, so present() sampled raw
  // adjacent VRAM (texture-atlas garbage) instead of a clean widened/blanked margin. A full-screen 2D
  // BACKDROP (fill==1 in the widen sites below: node_is_bg / sprite_is_bg_texpage / bg_2d coverage, or a
  // full-screen fade) is exactly as legitimate a "this frame owns and redraws the whole width" signal as
  // 3D world geometry — track it the same lagged way so the title backdrop widens like the field backdrop.
  int s_seen_bg2d = 0;        // did a full-screen 2D backdrop prim get classified fill=1 this frame?
  int s_prev_had_bg2d = 0;    // did LAST frame draw one? (wide-widen gate, mirrors s_prev_had3d)

  // M3 provenance — own the 2D layer by WHO submitted it, not per-prim size. The engine's screen-space
  // BACKGROUND drawer(s) (e.g. the field's scrolling-tilemap backdrop FUN_80115598) are bracketed by a
  // native override that records the OT-node (packet-pool) span they produce each frame here. A leftover
  // 2D prim whose OT node falls in a span is the BACKDROP (RQ_BACKGROUND); everything else is HUD. This
  // replaces the bg_2d screen-coverage guess, which is blind to a TILED background (352 sub-threshold
  // tiles all mis-classified as HUD → painted over the world). Stamped per frame; honored only for the
  // frame that filled it. (bg_2d stays as the fallback for scenes whose background drawer isn't owned yet.)
  static const int BG_RANGE_MAX = 8;
  uint32_t s_bg_lo[BG_RANGE_MAX] = {}, s_bg_hi[BG_RANGE_MAX] = {};   // KSEG0 packet-pool spans [lo,hi)
  int s_bg_nrange = 0;
  int s_bg_frame = -1;
  void bg_range_add(uint32_t lo, uint32_t hi);   // record a background drawer's pool span for the current frame
  int  node_is_bg(uint32_t node);                // is this OT node inside a background span this frame?

  // PC-native PER-OBJECT depth by packet-pool SPAN. The engine's native render walk renders each world
  // object into a contiguous packet-pool span and records [lo,hi)->view-depth here (its WORLD-POSITION
  // depth). Geometry rasterizes later during the deferred OT walk, where the per-object render context is
  // gone — so a 2D billboard prim (no projected verts) recovers its object's real depth by which span its
  // OT-node address falls in, exactly like node_is_bg. Stamped per frame; honored only for that frame.
  static const int OBJ_DEPTH_MAX = 512;
  uint32_t s_od_lo[OBJ_DEPTH_MAX] = {}, s_od_hi[OBJ_DEPTH_MAX] = {};
  float    s_od_ord[OBJ_DEPTH_MAX] = {};
  int s_od_n = 0;
  int s_od_frame = -1;
  // Per-face epsilon-depth run state. Was file-scope in gpu_native.cpp (s_od_eps_*) — moved onto
  // GpuState because obj_depth_lookup is a per-Core method; two SBS cores need SEPARATE counters
  // so one core's face ordinal doesn't leak into the other's depth epsilon (2026-07-03).
  int s_od_eps_frame = -1;
  int s_od_eps_span  = -1;
  int s_od_eps_k     = 0;
  void obj_depth_add(uint32_t lo, uint32_t hi, float ord);  // record an object's pool span + world depth
  int  obj_depth_lookup(uint32_t node, float* ord);         // depth ord for the OT node, if in an object span

  // NATIVE-COVER registry (docs/fps60-rework.md REDIRECT — windmill-family GT3/GT4 objects). When
  // Render::cmdListDispatch (perobj_dispatch.cpp) ALSO draws a cmd's geometry through the real
  // per-object float path (Render::gt3gt4, real identity + real per-vertex depth) because
  // perModeDispatch would otherwise route it to the byte-exact substrate mirror
  // (OverlayGt3Gt4::gt3/gt4), the substrate's OWN guest-OT copy of that same geometry becomes
  // redundant. Presence-only span registry: the field's 2D-only OT
  // walk checks this FIRST and unconditionally drops a covered span's guest polys — NOT via
  // obj_depth's billboard promotion (that would draw the coarse GTE-positioned copy AGAIN with a flat
  // per-object depth, i.e. a double draw with a worse picture). The substrate GTE math that produced
  // this span still runs untouched underneath (SBS byte-exactness unaffected); only the PICTURE
  // decision changes.
  static const int NATIVE_COVER_MAX = 64;
  uint32_t s_nc_lo[NATIVE_COVER_MAX] = {}, s_nc_hi[NATIVE_COVER_MAX] = {};
  int s_nc_n = 0;
  int s_nc_frame = -1;
  void nativeCoverAdd(uint32_t lo, uint32_t hi);   // record a natively-redrawn cmd's packet-pool span
  int  nativeCoverLookup(uint32_t addr);           // is this OT node inside a native-covered span?

  // VRAM (textures + framebuffers)
  uint16_t  s_vram[VRAM_W * VRAM_H] = {};
  uint16_t* vram(int x, int y) { return &s_vram[(y & 511) * VRAM_W + (x & 1023)]; }
  uint16_t* fb(int x, int y)   { return &s_vram[(y & 511) * VRAM_W + (x & 1023)]; }

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

  // ---- PC-native VRAM TRANSFER GUARD (atlas-clobber catcher) ------------------------------------
  // The texture/VRAM manager owns every CPU->VRAM and VRAM->VRAM transfer through ONE guarded entry
  // (vram_xfer.cpp). It (1) bounds-validates the rect so a garbage descriptor can never silently fold
  // onto a live texpage via the VRAM wrap, and (2) keeps a registry of the populated TEXTURE-GROUP
  // regions (the big atlas/font/CLUT uploads). Under `debug vramguard` it logs any transfer that lands
  // on a registered, still-resident atlas region from an UNEXPECTED path (a stray render-OT copy / a
  // bad upload) — catching the non-deterministic stripe-corruption clobber deterministically the moment
  // it fires live, with full provenance (source path, rect, OT node), even though it is rare. The
  // registry is pure diagnostic bookkeeping (no guest-RAM write) so it never perturbs content state.
  static const int VG_MAX = 64;
  struct VgRegion { int x, y, w, h; int frame; uint8_t live; char tag[12]; };
  VgRegion s_vg[VG_MAX] = {};
  int s_vg_n = 0;
  long s_vg_oob_log = 0, s_vg_clobber_log = 0;   // vramguard log-rate counters (vram_xfer.cpp)
  // Register a texture-group/atlas upload rect as a protected, populated region (called from the native
  // upload). label distinguishes atlas vs CLUT vs framebuffer. Overlapping re-uploads refresh in place.
  void vram_register_atlas(int x, int y, int w, int h, const char* tag);
  // Guard a transfer: validate bounds; under vramguard, log if it clobbers a protected atlas region.
  // `path` names the writer (e.g. "A0", "80copy", "native"); src is the guest source addr (0 if N/A).
  void vram_guard_check(Core* core, const char* path, int x, int y, int w, int h, uint32_t src);

  // ---- diagnostics (per-Core so SBS cores never share dedupe/log state) -----------------------
  int s_log = 0;                    // PSXPORT_GPU_LOG / `debug gpu` per-frame prim log
  int s_reddbg = 0;                 // PSXPORT_REDDBG: dark-red output anomaly probe
  int s_oracle_prim_log = 0;        // ORACLE diag: when >0, log each soft_gpu primitive
  long s_nd2d_hist[256] = {};       // op histogram of prims that fall to the 2D band (was g_nd2d_hist)
  // PSXPORT_PRIMDUMP=frame: one-frame CSV dump of every OT prim (see primdump_open).
  FILE* s_primdump_f = nullptr;
  int   s_primdump_frame = -2;      // -2 = env not read, -1 = off
  FILE* primdump_open(int frame);
  // Fade-flash diagnostic (PSXPORT_FADEDBG) accumulators, reset per present.
  int s_fade_maxc = 0, s_fade_npoly = 0, s_fade_nsemi = 0, s_fade_lasty = 0;
  int s_fade_semimax = -1, s_fade_semimin = 999, s_fade_bigsemi = 0;
  void fade_note(int r, int g, int b, int offy, int semi);
  void fade_note_size(int w, int h, int semi);
  // PSXPORT_CLUTWATCH[=x,y] watched CLUT point + pending-A0 latch.
  int s_cw_x = -1, s_cw_y = 0, s_cw_pending = 0;
  int clutwatch_covers(int rx, int ry, int rw, int rh);
  // PSXPORT_TEXWATCH="x0,y0,x1,y1" watched VRAM rect.
  int s_tw_init = 0, s_tw_x0 = -1, s_tw_y0 = 0, s_tw_x1 = 0, s_tw_y1 = 0;
  int texwatch_overlap(int rx, int ry, int rw, int rh);
  void gpu_native_init();           // seed the diag gates from cfg (boot + FMV player)

  // Frame + OT bookkeeping
  int s_frame = 0;                                            // present-frame counter
  // Per-frame draw stats — moved off file-scope in gpu_native.cpp so SBS's two cores keep separate
  // per-frame counters (a core reading its own stats or a debug-server `frame` query wouldn't see the
  // other core's contribution) (deglobalize 2026-07-03).
  long s_prims = 0;                                           // primitives drawn since last present
  long s_gp0_words = 0;                                       // GP0 words this frame
  long s_dma2 = 0;                                            // DMA2 (OT linked-list) triggers this frame
  // Backdrop texpage provenance (per-core — SBS runs two cores): published by ov_bg_tilemap_native each
  // field frame; the OT walk drops guest backdrop sprites sampling it (render.md OPEN #1). -1 = unset.
  int s_bgtp_x = -1, s_bgtp_y = -1, s_bgtp_frame = -1;
  uint32_t s_cur_node = 0;                                    // RAM addr of the OT node being fed to GP0
  uint32_t s_ot_madr = 0;                                     // last OT DMA root (was global g_ot_madr)
  uint32_t s_dma_src = 0;                                     // last block-DMA source (was global g_dma_src)
  bool     s_ot_2d_only = false;                              // in-flight OT walk mode (was global g_ot_2d_only):
                                                              // set at gpu_dma2_linked_list entry, read by gp0_exec
                                                              // to drop the OT's 3D-world prims (owned by native
                                                              // scene walk), cleared at walk exit.

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
  // PC-native texture EXPORT: decode w×h texels at the current texpage/CLUT (mode 0=4bpp/1=8bpp/2=15bpp)
  // through MY OWN decoder (not the rasterizer) and write an RGB PPM. Proves the texture decode is owned.
  void tex_export(const char* name, int u0, int v0, int w, int h);
  void raster_line(int x0, int y0, int x1, int y1, uint8_t cr, uint8_t cg, uint8_t cb, int semi);
  void set_texpage(uint16_t tp);
  void set_clut(uint16_t cl);
  void gp0_exec(Core* core);
  void gpu_gp0(Core* core, uint32_t w);
  void gpu_gp1(uint32_t w);
  // twoDOnly=true: enumerate the OT and DROP its 3D-world / backdrop prims (owned by the native
  // scene walk); queue only 2D-overlay prims as RQ_HUD. Was the process-global g_ot_2d_only,
  // set right before the call and cleared right after — now a real parameter.
  void gpu_dma2_linked_list(Core* core, uint32_t madr, bool twoDOnly);
  void gpu_dma2_block(Core* core, uint32_t madr, int count, int to_gpu);
  void gpu_native_load_image(Core* core, int x, int y, int w, int h, uint32_t src);
  int  gpu_native_load_vram(const char* path);
  void ensure_window();
  void blit_src(const uint16_t* src, int sx, int sy);
  void present_window();
  void gpu_repaint();
  void gpu_native_shot(Core* core, const char* path);
  void gpu_present_ex(Core* core, int do_blit);
  void gpu_present(Core* core);
  void frame_finalize(Core* core);          // per-frame reset/bookkeeping (no window blit) — shared by
                                            // gpu_present_ex AND the SBS per-core grab (which skips present)
  uint16_t gpu_vram_peek(int x, int y);
  void gpu_blank_display();                 // zero the display FB rect (no present)
  void gpu_clear_display(Core* core);      // gpu_blank_display + present (FMV teardown)
  void gpu_vram_load(const uint16_t* src);
  void gpu_vram_save(uint16_t* dst);
  void gpu_provat_enable();
  int  gpu_frame_no();
  void gpu_fps60_present_pass(Core* core);   // VK 60fps: present the accumulated batch over s_vram, reset batch (no s_frame++)
};

// ---- Diagnostic dumps (gpu_debug.cpp) — read the per-instance state via Core* -----------------
void gpu_scene_dump(Core* core, FILE* out, uint32_t madr);   // classify an OT's display list (PSXPORT_SCENEDUMP)

// ---- Public GPU API (free functions; thin wrappers over GpuState methods, reached via Core*) ---
void gpu_gp0(Core* core, uint32_t w);
void gpu_gp1(Core* core, uint32_t w);
void gpu_dma2_linked_list(Core* core, uint32_t madr, bool twoDOnly);
void gpu_dma2_block(Core* core, uint32_t madr, int count, int to_gpu);
void gpu_present(Core* core);
void gpu_present_ex(Core* core, int do_blit);
void gpu_clear_display(Core* core);   // FMV/splash teardown: black the display FB + present (no stale pixels)
void gpu_native_load_image(Core* core, int x, int y, int w, int h, uint32_t src);
int  gpu_native_load_vram(Core* core, const char* path);
void gpu_native_shot(Core* core, const char* path);
int  gpu_frame_no(Core* core);
uint16_t gpu_vram_peek(Core* core, int x, int y);
// PC-native SCEA decode: baked 4bpp+CLUT asset -> flat RGBA8 at the 640x468 screen positions (text =
// CLUT color, else transparent black). PSX-free source for gpu_vk_present_image. `out` = 640*468*4 bytes.
void gpu_scea_decode_rgba(uint8_t* out);
void gpu_vram_load(Core* core, const uint16_t* src);
void gpu_vram_save(Core* core, uint16_t* dst);
void gpu_fps60_present_pass(Core* core);   // VK 60fps in-between present pass
// gpu_provat_display / gpu_prov_dump (gpu_debug.cpp) take Core* too:
void gpu_provat_display(Core* core, FILE* out, int qx, int qy);
void gpu_prov_dump(Core* core, int vx, int vy);

uint32_t mem_r32(uint32_t);

#endif // GPU_NATIVE_INTERNAL_H
