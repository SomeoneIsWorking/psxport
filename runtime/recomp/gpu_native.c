// Native GPU — PC rendering of the game's own draw primitives (NOT PSX-GPU emulation).
//
// The game emits GP0 command packets (polygons/sprites/lines + VRAM transfers + draw-env)
// as its output protocol, usually via GPU DMA (channel 2) walking ordering-table linked
// lists. We parse that stream and rasterize it with our OWN renderer into a VRAM-backed
// framebuffer, then present it. No PSX GPU hardware is emulated; the renderer is ours, so
// resolution/widescreen/60fps are under our control (wide60 tier builds on this).
//
// VRAM is 1024x512 16-bit (5-5-5 BGR + mask), holding both textures (sampled by textured
// primitives via texpage+CLUT) and the framebuffer regions the game composes & displays.
#include "r3000.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PSXPORT_SDL
#include <SDL.h>
#endif

#define VRAM_W 1024
#define VRAM_H 512
extern int g_wide60_on;           // wide60: capture/synth enabled (PSXPORT_WIDE60); 0 = faithful path
static uint16_t s_vram[VRAM_W * VRAM_H];
static inline uint16_t* vram(int x, int y) { return &s_vram[(y & 511) * VRAM_W + (x & 1023)]; }

// wide60 60fps layer: a SEPARATE off-screen framebuffer the interpolated frame is rasterized into,
// so the 60fps tier sits ON TOP of the renderer and NEVER writes the game's VRAM. put_px_b writes
// (and its blend-reads) go to s_fb_base — normally s_vram, switched to s_interp only while synthesizing
// the in-between; sample_tex always reads s_vram, so textures still come from the live atlas.
static uint16_t s_interp[VRAM_W * VRAM_H];
static uint16_t* s_fb_base = s_vram;
static inline uint16_t* fb(int x, int y) { return &s_fb_base[(y & 511) * VRAM_W + (x & 1023)]; }

// ---- Draw state (set by GP0 env commands E1..E6) ------------------------------------
static int s_da_x0, s_da_y0, s_da_x1 = 1023, s_da_y1 = 511;  // draw clip area
static int s_off_x, s_off_y;                                 // draw offset
int gpu_vk_enabled(void);                                    // gpu_vk.c (declared early for the gp0 tee)
void gpu_vk_dirty(int, int, int, int);                       // mark a SW-written VRAM region to mirror to VK
static int s_tp_x, s_tp_y;        // texpage base (64-px / 256-line units -> *64 / *256)
static int s_reddbg;              // PSXPORT_REDDBG: dark-red output anomaly probe
static int s_tp_mode;             // texture color mode: 0=4bpp,1=8bpp,2=15bpp
static int s_tp_blend;            // semi-transparency mode 0..3
static int s_tp_dither;           // ordered-dither enable (E1 bit 9)
static int s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy;  // texture window mask/offset (8px units)
static int s_clut_x, s_clut_y;    // CLUT base (per-primitive, from packet)

// ---- Display control (GP1) ----------------------------------------------------------
static int s_disp_x, s_disp_y;    // VRAM top-left of the displayed region
static int s_disp_w = 320, s_disp_h = 240;
static int s_disp_vy0, s_disp_vy1 = 240;  // GP1(0x07) vertical display range (scanlines)
static int s_disp_480i;           // GP1(0x08): interlace + 480-line vertical resolution

typedef struct { int x, y; uint8_t r, g, b; int u, v; } Vtx;

static int g_log = 0;             // PSXPORT_GPU_LOG
static long s_prims = 0;          // primitives drawn since last present
static long s_gp0_words = 0, s_dma2 = 0;  // diagnostics: GP0 words + DMA2 triggers per frame
static uint32_t s_cur_node = 0;   // REDDBG: RAM addr of the OT node currently being fed to GP0
static uint32_t g_ot_madr = 0;    // last OT DMA root (for the clobber analyzer)
static int s_frame;               // present-frame counter (defined below); forward tentative def

// ---- Per-pixel primitive provenance (PSXPORT_PROVAT="x,y[:frame]") --------------------------
// Records, for every VRAM pixel, the global id of the primitive that last wrote it. A wrong
// DISPLAYED pixel can then be traced to the exact prim that produced it — or shown to be STALE
// (last written many frames ago = revealed through a terrain/coverage gap, never overdrawn this
// frame). Queried in DISPLAY space at present time, which sidesteps the GPU double-buffer offset
// entirely (no more guessing which buffer / which native frame drew the shown pixel).
static uint32_t s_prov[VRAM_W * VRAM_H];   // gid of last writer per pixel (0 = never written)
static uint32_t s_prim_gid = 0;            // monotonic primitive counter
static int      s_prov_on = -1;            // lazily: 1 if PSXPORT_PROVAT set, else 0
typedef struct { uint32_t gid, frame, node; int clut_x, clut_y, tp_x, tp_y, x0, y0, u0, v0;
                 uint8_t op, r, g, b, semi, tex, mode, blend; } ProvMeta;
#define PROVRING 16384
static ProvMeta s_provmeta[PROVRING];      // gid -> prim details (ring; older gids evicted)

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint16_t to555(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

// ---- Semi-transparency (blend) ------------------------------------------------------
// PSX blends a source pixel (foreground, F) over the existing VRAM pixel (background, B)
// in 5-bit-per-channel space, using one of four modes selected by the texpage blend bits
// (s_tp_blend, also reachable per-poly via the prim's texpage). The formulas (per channel):
//   mode0: B/2 + F/2   mode1: B + F   mode2: B - F   mode3: B + F/4
// All results saturate to [0,31]. blend555() takes already-5-bit dest (existing VRAM 555,
// mask bit stripped) and 5-bit source channels, returns the blended 555 word.
static inline int sat5(int v) { return v < 0 ? 0 : v > 31 ? 31 : v; }
static inline uint16_t blend555(uint16_t bg, int fr, int fg, int fb, int mode) {
  int br = bg & 31, bgn = (bg >> 5) & 31, bb = (bg >> 10) & 31, rr, rg, rb;
  switch (mode) {
    case 0: rr = (br + fr) >> 1; rg = (bgn + fg) >> 1; rb = (bb + fb) >> 1; break;
    case 1: rr = sat5(br + fr); rg = sat5(bgn + fg); rb = sat5(bb + fb); break;
    case 2: rr = sat5(br - fr); rg = sat5(bgn - fg); rb = sat5(bb - fb); break;
    default: rr = sat5(br + (fr >> 2)); rg = sat5(bgn + (fg >> 2)); rb = sat5(bb + (fb >> 2)); break;
  }
  return (uint16_t)(rr | (rg << 5) | (rb << 10));
}

// Sample a texel at texture coords (u,v) through the current texpage/CLUT. Returns 0 if the
// texel is fully transparent (PSX: a 16-bit value of 0 = transparent).
static uint16_t sample_tex(int u, int v) {
  u = (u & ~(s_tw_mx * 8)) | ((s_tw_ox & s_tw_mx) * 8);  // texture window wrap
  v = (v & ~(s_tw_my * 8)) | ((s_tw_oy & s_tw_my) * 8);
  int bx = s_tp_x, by = s_tp_y;
  if (s_tp_mode == 2) return *vram(bx + u, by + v);       // 15bpp direct
  if (s_tp_mode == 1) {                                   // 8bpp: index -> CLUT
    uint16_t w = *vram(bx + (u >> 1), by + v);
    int idx = (u & 1) ? (w >> 8) : (w & 0xFF);
    return *vram(s_clut_x + idx, s_clut_y);
  }
  uint16_t w = *vram(bx + (u >> 2), by + v);              // 4bpp: nibble -> CLUT
  int idx = (w >> ((u & 3) * 4)) & 0xF;
  return *vram(s_clut_x + idx, s_clut_y);
}

// Write one pixel. If `semi` is set, blend the source (r,g,b) over the existing VRAM pixel
// using the current texpage blend mode (s_tp_blend); otherwise overwrite. The mask bit is
// always set on the written pixel (we don't model mask-test reads).
static inline void put_px_b(int x, int y, uint8_t r, uint8_t g, uint8_t b, int semi) {
  if (x < s_da_x0 || x > s_da_x1 || y < s_da_y0 || y > s_da_y1) return;
  uint16_t out;
  if (semi) out = blend555(*fb(x, y) & 0x7FFF, r >> 3, g >> 3, b >> 3, s_tp_blend);
  else out = to555(r, g, b);
  *fb(x, y) = out | 0x8000;
  if (s_prov_on > 0) s_prov[(y & 511) * VRAM_W + (x & 1023)] = s_prim_gid;
}
static inline void put_px(int x, int y, uint8_t r, uint8_t g, uint8_t b) { put_px_b(x, y, r, g, b, 0); }

// PSX ordered 4x4 dither matrix (applied to 8-bit channels before 5-bit truncation, when
// the texpage dither bit is set, on gouraud + texture-modulated pixels). We add the per-pixel
// bias then clamp to [0,255] so the subsequent >>3 truncation effectively rounds.
static const int s_dither4[4][4] = {
  { -4,  0, -3,  1 },
  {  2, -2,  3, -1 },
  { -3,  1, -4,  0 },
  {  3, -1,  2, -2 },
};
static inline uint8_t dith(int v, int x, int y) {
  v += s_dither4[y & 3][x & 3];
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// ---- mednafen-exact triangle coverage (integer scanline edge-walk) ------------------
// To match the oracle's rasterizer COVERAGE exactly (which pixels a triangle claims), we
// replicate Beetle/mednafen's gpu_polygon.c edge-walk verbatim, rather than a half-space
// test. mednafen walks scanlines computing a fixed-point left/right edge per row and fills
// the span [x_start, x_bound) (left/top inclusive, right/bottom exclusive). A generic
// top-left half-space rule gets the DIRECTION right but not the exact sub-pixel endpoint
// rounding (MakePolyXFP/Step), so abutting prims still mis-claim a pixel here and there
// (journal: text-banner residual — our coverage over-claimed one edge, under-claimed
// another). Porting the exact integer math removes that variable entirely. These three
// helpers are mednafen's fixed-point edge primitives (COORD_FBS world, 32-frac fixed point).
static inline int64_t MakePolyXFP(int x) {
  return ((int64_t)x << 32) + (((int64_t)1 << 32) - (1 << 11));
}
static inline int64_t MakePolyXFPStep(int dx, int dy) {   // dy is always > 0 at our call sites
  int64_t dx_ex = (int64_t)dx << 32;
  if (dx_ex < 0) dx_ex -= dy - 1;
  if (dx_ex > 0) dx_ex += dy - 1;
  return dx_ex / dy;
}
static inline int GetPolyXFP_Int(int64_t xfp) { return (int)(xfp >> 32); }

// Shade + write ONE covered pixel of triangle (a,b,c) at integer screen (x,y). Coverage is
// decided by the caller (tri()); this only does the per-pixel math, which stays barycentric
// off the ORIGINAL (unsorted) a,b,c and the doubled signed area `aa` — already validated to
// match Beetle's per-pixel output (modulation/UV-round/dither). `tex`/`shade`/`semi` as tri().
static inline void tri_px(Vtx a, Vtx b, Vtx c, int x, int y, int tex, int shade, int semi, int raw, long aa) {
      long l0 = (long)((b.x-x)*(c.y-y)-(b.y-y)*(c.x-x));
      long l1 = (long)((c.x-x)*(a.y-y)-(c.y-y)*(a.x-x));
      long l2 = aa - l0 - l1;
      uint8_t r, g, bl;
      int px_semi = semi;                           // whether THIS pixel blends
      int dithered = 0;                             // PSX dithers gouraud + modulated-texture
      int pt_u = 0, pt_v = 0; uint16_t pt_t = 0;    // PSXPORT_PIXTRACE capture
      int pt_cr = a.r, pt_cg = a.g, pt_cb = a.b;    // interpolated modulation color (set below)
      if (tex) {
        // Affine UV, ROUND-TO-NEAREST (not truncate): PSX/Beetle add a +0.5-texel bias before the
        // integer truncation (gpu_polygon.c affine seed `+(1<<(COORD_FBS-1))`), i.e. sample the
        // nearest texel. Truncating instead biases sampling half a texel toward the origin, picking a
        // neighbouring texel at fractional coords (journal later-44 residual). Round in sign-
        // normalized form since `aa` (doubled area) may be negative.
        long su = l0*a.u + l1*b.u + l2*c.u, sv = l0*a.v + l1*b.v + l2*c.v, den = aa;
        if (den < 0) { su = -su; sv = -sv; den = -den; }
        int u = (int)((su + den/2) / den);
        int v = (int)((sv + den/2) / den);
        uint16_t t = sample_tex(u, v);
        pt_u = u; pt_v = v; pt_t = t;
        if (t == 0) return;                         // transparent texel — skip this pixel
        // PSX: a textured pixel blends only when its bit15 is set AND the prim semi bit is set.
        px_semi = semi && (t & 0x8000);
        r = (t & 31) << 3; g = ((t >> 5) & 31) << 3; bl = ((t >> 10) & 31) << 3;
        // RAW TEXTURE (PSX poly cmd bit0 = texture-blend-disable): output the texel verbatim — NO
        // modulation by vertex color and NO dither. Beetle's TM0 template path does exactly this
        // (journal: the op-2D banner-board residual — ours modulated raw texel 2E12 by the command
        // color (168,72,31) → near-black, while Beetle left it raw (18,16,11)). Same bit0 gating
        // the sprite path already honors (commit fb0c228); the polygon path was missing it.
        if (!raw) {
          // texture*color modulation (texel * vertexcolor / 128). PSX textured polygons modulate
          // the texel by the vertex color, INTERPOLATED per pixel across the face (the command color
          // for flat-shaded prims, where all vertices carry it). The modulation color must be the
          // barycentric-interpolated (cr,cg,cb), NOT vertex A's color held flat — using v0 flat
          // collapses a gouraud gradient (a soft shadow quad: dark center vertex, bright edges) into
          // a uniform block (journal later 44: black-wedge shadow). PSX hardware SATURATES the
          // product to 0xFF; doing it in uint8_t wraps mod 256, turning a bright grass texel red, so
          // compute wide and clamp (the grass red-block bug, journal later 42).
          int cr = (int)((l0*a.r + l1*b.r + l2*c.r) / aa);
          int cg = (int)((l0*a.g + l1*b.g + l2*c.g) / aa);
          int cb = (int)((l0*a.b + l1*b.b + l2*c.b) / aa);
          pt_cr = cr; pt_cg = cg; pt_cb = cb;
          int rr = r * cr / 128, gg = g * cg / 128, bb = bl * cb / 128;
          r = rr > 255 ? 255 : rr; g = gg > 255 ? 255 : gg; bl = bb > 255 ? 255 : bb;
          dithered = 1;
        } else { pt_cr = pt_cg = pt_cb = 128; }   // raw: undithered texel, modulation color = neutral
      } else if (shade) {
        r = (uint8_t)((l0*a.r + l1*b.r + l2*c.r) / aa);
        g = (uint8_t)((l0*a.g + l1*b.g + l2*c.g) / aa);
        bl = (uint8_t)((l0*a.b + l1*b.b + l2*c.b) / aa);
        dithered = 1;
      } else { r = a.r; g = a.g; bl = a.b; }
      if (s_tp_dither && dithered) { r = dith(r, x, y); g = dith(g, x, y); bl = dith(bl, x, y); }
      // PSXPORT_PIXTRACE="vx,vy": dump every prim that writes this absolute VRAM pixel (post-offset),
      // with its sampled texel + interpolated color + modulated output — for per-pixel-math diffing
      // against Beetle's gpu_polygon.c (which carries the matching [pixtrace beetle] log).
      { static int tx = -2, ty; if (tx == -2) { const char* e = getenv("PSXPORT_PIXTRACE");
          if (e) sscanf(e, "%d,%d", &tx, &ty); else tx = -1; }
        if (tx >= 0 && x == tx && y == ty)
          fprintf(stderr, "[pixtrace ours] (%d,%d) tex=%d shade=%d semi=%d px_semi=%d blend=%d dith=%d "
                  "uv=(%d,%d) texel=%04X out8=(%d,%d,%d) out5=(%d,%d,%d) vcol=(%d,%d,%d)\n",
                  x, y, tex, shade, semi, px_semi, s_tp_blend, (s_tp_dither && dithered),
                  pt_u, pt_v, pt_t, r, g, bl, r >> 3, g >> 3, bl >> 3, pt_cr, pt_cg, pt_cb); }
      // REDDBG: dark-red output anomaly probe (grass blocks). Log the prim's params once.
      if (s_reddbg && tex && r >= 64 && g < 24 && bl < 24 && x >= s_da_x0 && x <= s_da_x1) {
        static int n = 0;
        if (n++ < 6) {
          int uu = (int)((l0*a.u + l1*b.u + l2*c.u) / aa);
          int vv = (int)((l0*a.v + l1*b.v + l2*c.v) / aa);
          fprintf(stderr, "[reddbg] @(%d,%d) out=(%d,%d,%d) tpmode=%d clut=(%d,%d) tp=(%d,%d) uv=(%d,%d)\n",
                  x, y, r, g, bl, s_tp_mode, s_clut_x, s_clut_y, s_tp_x, s_tp_y, uu, vv);
          fprintf(stderr, "[reddbg]   palette[16]@(%d,%d):", s_clut_x, s_clut_y);
          for (int k = 0; k < 16; k++) fprintf(stderr, " %04X", *vram(s_clut_x + k, s_clut_y));
          fprintf(stderr, "\n[reddbg]   texrow@(%d,%d) words:", s_tp_x + (uu>>2), s_tp_y + vv);
          for (int k = 0; k < 8; k++) fprintf(stderr, " %04X", *vram(s_tp_x + (uu>>2) + k, s_tp_y + vv));
          fprintf(stderr, "\n");
        }
      }
      put_px_b(x, y, r, g, bl, px_semi);
}

// Rasterize a gouraud/textured triangle. `tex` selects textured sampling, `semi` requests
// semi-transparency. Coverage = mednafen's exact integer edge-walk (so it matches the oracle
// pixel-for-pixel); per-pixel shading = tri_px (barycentric off the original a,b,c).
static void tri(Vtx a, Vtx b, Vtx c, int tex, int shade, int semi, int raw) {
  a.x += s_off_x; a.y += s_off_y; b.x += s_off_x; b.y += s_off_y; c.x += s_off_x; c.y += s_off_y;
  long aa = (long)((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x));
  if (aa == 0) return;                          // degenerate (zero area)

  // --- Exact port of mednafen's DEFINE_DrawTriangle coverage (gpu_polygon.c). Operates on a
  // y-sorted copy of the vertices; shading (tri_px) still uses the original a,b,c order. ---
  int vx[3] = { a.x, b.x, c.x }, vy[3] = { a.y, b.y, c.y };
  unsigned cvtemp;                               // "core vertex" select (rasterisation order)
  if (vx[1] <= vx[0]) cvtemp = (vx[2] <= vx[1]) ? (1u << 2) : (1u << 1);
  else if (vx[2] < vx[0]) cvtemp = (1u << 2);
  else cvtemp = (1u << 0);
  #define VSWAP(i,j) do { int t; t=vx[i];vx[i]=vx[j];vx[j]=t; t=vy[i];vy[i]=vy[j];vy[j]=t; } while (0)
  if (vy[2] < vy[1]) { VSWAP(2,1); cvtemp = ((cvtemp>>1)&0x2)|((cvtemp<<1)&0x4)|(cvtemp&0x1); }
  if (vy[1] < vy[0]) { VSWAP(1,0); cvtemp = ((cvtemp>>1)&0x1)|((cvtemp<<1)&0x2)|(cvtemp&0x4); }
  if (vy[2] < vy[1]) { VSWAP(2,1); cvtemp = ((cvtemp>>1)&0x2)|((cvtemp<<1)&0x4)|(cvtemp&0x1); }
  #undef VSWAP
  unsigned core_vertex = cvtemp >> 1;
  if (vy[0] == vy[2]) return;                    // 0-height after sort

  int64_t base_coord = MakePolyXFP(vx[0]);
  int64_t base_step  = MakePolyXFPStep(vx[2]-vx[0], vy[2]-vy[0]);
  int64_t bound_coord_us, bound_coord_ls;
  int right_facing;
  if (vy[1] == vy[0]) { bound_coord_us = 0; right_facing = (vx[1] > vx[0]); }
  else { bound_coord_us = MakePolyXFPStep(vx[1]-vx[0], vy[1]-vy[0]); right_facing = (bound_coord_us > base_step); }
  bound_coord_ls = (vy[2] == vy[1]) ? 0 : MakePolyXFPStep(vx[2]-vx[1], vy[2]-vy[1]);

  unsigned vo = core_vertex ? 1 : 0;
  unsigned vp = (core_vertex == 2) ? 3 : 0;
  struct { int64_t x_coord[2], x_step[2]; int y_coord, y_bound, dec_mode; } tp[2];
  { int k = vo;
    tp[k].y_coord = vy[0 ^ vo]; tp[k].y_bound = vy[1 ^ vo];
    tp[k].x_coord[right_facing] = MakePolyXFP(vx[0 ^ vo]);
    tp[k].x_step[right_facing] = bound_coord_us;
    tp[k].x_coord[!right_facing] = base_coord + (int64_t)(vy[vo]-vy[0]) * base_step;
    tp[k].x_step[!right_facing] = base_step;
    tp[k].dec_mode = (vo != 0); }
  { int k = vo ^ 1;
    tp[k].y_coord = vy[1 ^ vp]; tp[k].y_bound = vy[2 ^ vp];
    tp[k].x_coord[right_facing] = MakePolyXFP(vx[1 ^ vp]);
    tp[k].x_step[right_facing] = bound_coord_ls;
    tp[k].x_coord[!right_facing] = base_coord + (int64_t)(vy[1 ^ vp]-vy[0]) * base_step;
    tp[k].x_step[!right_facing] = base_step;
    tp[k].dec_mode = (vp != 0); }

  for (int i = 0; i < 2; i++) {
    int yi = tp[i].y_coord, yb = tp[i].y_bound;
    int64_t lc = tp[i].x_coord[0], ls = tp[i].x_step[0];
    int64_t rc = tp[i].x_coord[1], rs = tp[i].x_step[1];
    if (tp[i].dec_mode) {
      while (yi > yb) {
        yi--; lc -= ls; rc -= rs;
        if (yi < s_da_y0) break;
        if (yi > s_da_y1) continue;
        int xs = GetPolyXFP_Int(lc), xb = GetPolyXFP_Int(rc);
        if (xs < s_da_x0) xs = s_da_x0;
        if (xb > s_da_x1 + 1) xb = s_da_x1 + 1;
        for (int x = xs; x < xb; x++) tri_px(a, b, c, x, yi, tex, shade, semi, raw, aa);
      }
    } else {
      while (yi < yb) {
        if (yi > s_da_y1) break;
        if (yi >= s_da_y0) {
          int xs = GetPolyXFP_Int(lc), xb = GetPolyXFP_Int(rc);
          if (xs < s_da_x0) xs = s_da_x0;
          if (xb > s_da_x1 + 1) xb = s_da_x1 + 1;
          for (int x = xs; x < xb; x++) tri_px(a, b, c, x, yi, tex, shade, semi, raw, aa);
        }
        yi++; lc += ls; rc += rs;
      }
    }
  }
}

// ---- GP0 command FIFO ---------------------------------------------------------------
static uint32_t s_fifo[256];   // big enough for variable-length poly-lines (many vertices)
static int s_fcount, s_fneed;
static int s_pl, s_pl_g;       // poly-line in progress (variable length); s_pl_g = gouraud
// VRAM transfer state (GP0 0xA0 CPU->VRAM)
static int s_xfer, s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h, s_xfer_px;

// PC-native CPU->VRAM upload. The game's libgs-style upload library (FUN_80081218 and the
// GsSortObject ring at 0x800A5AC8) is replaced by writing the rect directly here, so the GPU
// library does not need to be a faithful recomp. `src` is a RAM (or physical) address holding
// w*h contiguous 16-bit pixels, row-major; mem_r16 masks the region so KSEG0/physical both work.
// Identical effect to the GP0 0xA0 stream below, minus the FIFO/DMA round-trip.
// Transplant harness: overwrite our full VRAM from a raw 1024x512x16 dump (oracle's, via
// PSXPORT_VRAMDUMP). Lets us drop the oracle's clean green-field VRAM into our running port and
// watch whether our continued execution keeps it clean or re-corrupts (accumulation test).
int gpu_native_load_vram(const char* path) {
  FILE* f = fopen(path, "rb"); if (!f) return 0;
  size_t n = fread(s_vram, 2, (size_t)VRAM_W * VRAM_H, f); fclose(f);
  fprintf(stderr, "[transplant] loaded VRAM %zu px from %s\n", n, path);
  return n == (size_t)VRAM_W * VRAM_H;
}
void gpu_native_load_image(int x, int y, int w, int h, uint32_t src) {
  for (int v = 0; v < h; v++)
    for (int u = 0; u < w; u++)
      *vram(x + u, y + v) = mem_r16(src + (uint32_t)((v * w + u) * 2));
  if (getenv("PSXPORT_UPLOADLOG"))
    fprintf(stderr, "[upload] f%d NATIVE dest=(%d,%d) %dx%d src=0x%08X\n", s_frame, x, y, w, h, src);
}

// GP0 command-word color packs as 0x00BBGGRR — R in the low byte, B in the high byte.
static inline uint8_t cmd_r(uint32_t c) { return c & 0xFF; }
static inline uint8_t cmd_g(uint32_t c) { return (c >> 8) & 0xFF; }
static inline uint8_t cmd_b(uint32_t c) { return (c >> 16) & 0xFF; }
static inline int cx(uint32_t w) { int v = w & 0x7FF; return v >= 0x400 ? v - 0x800 : v; }
static inline int cy(uint32_t w) { int v = (w >> 16) & 0x7FF; return v >= 0x400 ? v - 0x800 : v; }

static void set_texpage(uint16_t tp) {
  s_tp_x = (tp & 0xF) * 64;
  s_tp_y = ((tp >> 4) & 1) * 256;
  s_tp_blend = (tp >> 5) & 3;
  s_tp_mode = (tp >> 7) & 3; if (s_tp_mode > 2) s_tp_mode = 2;
  s_tp_dither = (tp >> 9) & 1;     // ordered 4x4 dither enable
}
static void set_clut(uint16_t cl) { s_clut_x = (cl & 0x3F) * 16; s_clut_y = (cl >> 6) & 0x1FF; }

// Begin a primitive for provenance tracking: bump the global id and record this prim's details
// (frame/node/op/clut/texpage/color/first-vertex) so put_px_b can stamp each pixel it writes.
static void prov_begin(uint8_t op, int tex, int semi, uint8_t r, uint8_t g, uint8_t b,
                       int x0, int y0, int u0, int v0) {
  if (s_prov_on < 0) s_prov_on = getenv("PSXPORT_PROVAT") ? 1 : 0;
  if (!s_prov_on) return;
  s_prim_gid++;
  ProvMeta* m = &s_provmeta[s_prim_gid % PROVRING];
  m->gid = s_prim_gid; m->frame = (uint32_t)s_frame; m->node = s_cur_node; m->op = op;
  m->clut_x = s_clut_x; m->clut_y = s_clut_y; m->tp_x = s_tp_x; m->tp_y = s_tp_y;
  m->x0 = x0; m->y0 = y0; m->u0 = u0; m->v0 = v0; m->r = r; m->g = g; m->b = b;
  m->semi = (uint8_t)semi; m->tex = (uint8_t)tex; m->mode = (uint8_t)s_tp_mode;
  m->blend = (uint8_t)s_tp_blend;
}

// PSXPORT_CLUTWATCH[=x,y] — log every VRAM upload whose rect covers a watched CLUT point
// (default 880,507 = the wrong grass palette found via the oracle compare, journal later 39),
// in order, with the resulting 16-entry palette. Reveals whether the right palette is written
// then overwritten, or never written, and by which transfer.
static int s_cw_x = -1, s_cw_y = 0, s_cw_pending = 0;
static void clutwatch_dump(const char* tag, int rx, int ry, int rw, int rh) {
  fprintf(stderr, "[clutwatch] %s f%d rect=(%d,%d %dx%d) covers (%d,%d) palette:",
          tag, s_frame, rx, ry, rw, rh, s_cw_x, s_cw_y);
  for (int k = 0; k < 16; k++) fprintf(stderr, " %04X", *vram(s_cw_x + k, s_cw_y));
  fprintf(stderr, "\n");
}
static int clutwatch_covers(int rx, int ry, int rw, int rh) {
  if (s_cw_x < 0) return 0;
  return s_cw_y >= ry && s_cw_y < ry + rh && s_cw_x >= rx && s_cw_x < rx + rw;
}
// For 0xA0 the pixels stream in AFTER setup, so mark pending and dump on completion; for 0x80 the
// copy already happened, so dump immediately.
static void clutwatch_xfer(const char* tag, int rx, int ry, int rw, int rh) {
  if (!clutwatch_covers(rx, ry, rw, rh)) return;
  if (tag[0] == 'A') { s_cw_pending = 1; fprintf(stderr,
      "[clutwatch] A0 upload START f%d rect=(%d,%d %dx%d) covers (%d,%d)\n",
      s_frame, rx, ry, rw, rh, s_cw_x, s_cw_y); }
  else clutwatch_dump(tag, rx, ry, rw, rh);
}

// PSXPORT_TEXWATCH="x0,y0,x1,y1" — log every A0 CPU->VRAM upload or 0x80 VRAM->VRAM copy whose
// DEST rect overlaps the watched VRAM rect (e.g. a character's texpage), with frame, dest rect,
// DMA source addr, and the first source/dest bytes. Traces exactly when a model's texture pixels
// change and what data fed them (gameplay sprite/CLUT corruption hunt).
static int s_tw_init = 0, s_tw_x0 = -1, s_tw_y0 = 0, s_tw_x1 = 0, s_tw_y1 = 0;
static int texwatch_overlap(int rx, int ry, int rw, int rh) {
  if (!s_tw_init) {
    s_tw_init = 1;
    const char* e = getenv("PSXPORT_TEXWATCH");
    if (e) sscanf(e, "%d,%d,%d,%d", &s_tw_x0, &s_tw_y0, &s_tw_x1, &s_tw_y1);
  }
  if (s_tw_x0 < 0) return 0;
  return rx < s_tw_x1 && rx + rw > s_tw_x0 && ry < s_tw_y1 && ry + rh > s_tw_y0;
}

// Rasterize one sprite/rect with the CURRENT draw state (s_off, s_da clip, texpage via sample_tex,
// command color). Shared by gp0_exec and the wide60 in-between synthesizer so both go through the
// exact same texel/blend/clip logic. (op bit0 = raw-texel select; semi = semi-transparency.)
static void raster_sprite(int op, int x, int y, int u0, int v0, int w, int h,
                          uint8_t cr, uint8_t cg, uint8_t cb, int textured, int semi) {
  // Clip the iteration to the drawing area up front: off-screen sprites otherwise spin w*h
  // sample_tex calls for pixels put_px_b would discard (could burn millions of iterations).
  int dx0 = 0, dx1 = w, dy0 = 0, dy1 = h;
  if (s_da_x0 - x - s_off_x > dx0) dx0 = s_da_x0 - x - s_off_x;
  if (s_da_x1 - x - s_off_x + 1 < dx1) dx1 = s_da_x1 - x - s_off_x + 1;
  if (s_da_y0 - y - s_off_y > dy0) dy0 = s_da_y0 - y - s_off_y;
  if (s_da_y1 - y - s_off_y + 1 < dy1) dy1 = s_da_y1 - y - s_off_y + 1;
  for (int dy = dy0; dy < dy1; dy++)
    for (int dx = dx0; dx < dx1; dx++) {
      if (textured) {
        uint16_t t = sample_tex(u0 + dx, v0 + dy);
        if (t == 0) continue;                     // transparent texel
        int px_semi = semi && (t & 0x8000);
        int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
        if (!(op & 1)) {                          // bit0=0 -> modulate texel by command color
          tr = tr * cr / 128; tg = tg * cg / 128; tb = tb * cb / 128;
          if (tr > 255) tr = 255; if (tg > 255) tg = 255; if (tb > 255) tb = 255;
        }
        put_px_b(x + dx + s_off_x, y + dy + s_off_y, tr, tg, tb, px_semi);
      } else put_px_b(x + dx + s_off_x, y + dy + s_off_y, cr, cg, cb, semi);
    }
}

// Rasterize one flat line segment with the CURRENT draw state (s_off, clip). Shared by gp0_exec and
// the wide60 synthesizer so poly-lines are reproduced in the interpolated frame (else they flicker).
static void raster_line(int x0, int y0, int x1, int y1, uint8_t cr, uint8_t cg, uint8_t cb, int semi) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) { put_px_b(x0 + s_off_x, y0 + s_off_y, cr, cg, cb, semi); if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
}

// Execute a complete GP0 primitive packet held in s_fifo[0..s_fcount).
static void gp0_exec(void) {
  uint32_t c = s_fifo[0];
  uint8_t op = c >> 24;
  if (op >= 0x20 && op <= 0x3F) {            // polygon
    int gouraud = op & 0x10, quad = op & 0x08, textured = op & 0x04, semi = (op & 0x02) ? 1 : 0;
    int raw = textured && (op & 0x01);      // bit0 = texture-blend-disable (raw texel, no modulation)
    int nv = quad ? 4 : 3;
    Vtx v[4]; int idx = 1;
    for (int i = 0; i < nv; i++) {
      uint8_t cr, cg, cb;
      if (gouraud) { uint32_t col = (i == 0) ? c : s_fifo[idx++]; cr = cmd_r(col); cg = cmd_g(col); cb = cmd_b(col); }
      else { cr = cmd_r(c); cg = cmd_g(c); cb = cmd_b(c); }
      uint32_t xy = s_fifo[idx++];
      v[i].x = cx(xy); v[i].y = cy(xy); v[i].r = cr; v[i].g = cg; v[i].b = cb;
      if (textured) {
        uint32_t uv = s_fifo[idx++];
        v[i].u = uv & 0xFF; v[i].v = (uv >> 8) & 0xFF;
        if (i == 0) set_clut((uv >> 16) & 0xFFFF);
        if (i == 1) set_texpage((uv >> 16) & 0xFFFF);
      }
    }
    int shade = gouraud || !textured;       // flat-untextured uses the command color
    { void wide60_join_poly(int, int); wide60_join_poly(v[0].x, v[0].y); }  // wide60: object join
    if (g_wide60_on) {                       // wide60: tee the full primitive into PrimFrame B
      void wide60_cap_poly(int, int, const int*, const int*, const int*, const int*,
                           const unsigned char*, const unsigned char*, const unsigned char*,
                           int, int, int, int, int, int, int, int, int);
      int xs[4], ys[4], us[4], vs[4]; unsigned char rs[4], gs[4], bs[4];
      for (int i = 0; i < nv; i++) { xs[i]=v[i].x; ys[i]=v[i].y; us[i]=v[i].u; vs[i]=v[i].v;
                                     rs[i]=v[i].r; gs[i]=v[i].g; bs[i]=v[i].b; }
      wide60_cap_poly(op, nv, xs, ys, us, vs, rs, gs, bs, s_off_x, s_off_y,
                      s_tp_x, s_tp_y, s_tp_mode, s_tp_blend, s_tp_dither, s_clut_x, s_clut_y);
    }
    // VK backend (M5): tee polys to the GPU rasterizer in absolute VRAM coords. Opaque textured/
    // untextured -> opaque batch; semi -> semi batch (mode 3 = untextured flat). VK owns these now.
    if (gpu_vk_enabled()) {
      void gpu_vk_draw_tritri_f(const float*,const float*,const int*,const int*,const unsigned char*,
                                const unsigned char*,const unsigned char*,const float*,const float*,const float*,
                                int,int,int,int,int,int,int,int,int,int,int,int,int,int);
      void gpu_vk_draw_semi_f(const float*,const float*,const int*,const int*,const unsigned char*,
                              const unsigned char*,const unsigned char*,const float*,const float*,const float*,
                              int,int,int,int,int,int,int,int,int,int,int,int,int,int,int);
      // Vertex smoothing (PGXP): replace each integer screen vertex with the GTE's subpixel-precise
      // projected position (cached in gte_beetle.c, keyed by the packed int SXY) so 3D geometry stops
      // wobbling. Cache miss -> integer fallback. Gated PSXPORT_PGXP (default on; =0 for A/B vs oracle).
      int pgxp_lookup(int, int, float*, float*, float*);
      int pgxp_lookup_view(int, int, float*, float*, float*);
      static int s_pgxp_on = -1;
      if (s_pgxp_on < 0) { const char* e = getenv("PSXPORT_PGXP"); s_pgxp_on = (e && atoi(e)) ? 1 : 0; }  // default OFF: value-keyed vertex smoothing is a rejected GP0-stream trick
      float xs[4], ys[4]; int us[4], vs[4]; unsigned char rs[4], gs[4], bs[4];
      // view-space positions for native lighting (per-face normal): all verts must hit the PGXP cache
      // or we pass NULL (unlit, integer fallback). 2D HUD bypasses the GTE so it never hits -> stays unlit.
      float vpx[4], vpy[4], vpz[4]; int have_view = s_pgxp_on;
      for (int i = 0; i < nv; i++) {
        float px, py;
        if (s_pgxp_on && pgxp_lookup(v[i].x, v[i].y, &px, &py, 0)) { xs[i] = px + s_off_x; ys[i] = py + s_off_y; }
        else { xs[i] = (float)(v[i].x + s_off_x); ys[i] = (float)(v[i].y + s_off_y); }
        if (have_view && !pgxp_lookup_view(v[i].x, v[i].y, &vpx[i], &vpy[i], &vpz[i])) have_view = 0;
        us[i]=v[i].u; vs[i]=v[i].v; rs[i]=v[i].r; gs[i]=v[i].g; bs[i]=v[i].b;
      }
      const float *lx = have_view ? vpx : 0, *ly = have_view ? vpy : 0, *lz = have_view ? vpz : 0;
      int mode = textured ? s_tp_mode : 3, rw = raw ? 1 : 0;
      if (semi) {
        gpu_vk_draw_semi_f(xs, ys, us, vs, rs, gs, bs, lx, ly, lz, s_tp_x, s_tp_y, mode, rw, s_clut_x, s_clut_y,
                         s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1, s_tp_blend);
        if (nv == 4) gpu_vk_draw_semi_f(&xs[1], &ys[1], &us[1], &vs[1], &rs[1], &gs[1], &bs[1],
                         lx?lx+1:0, ly?ly+1:0, lz?lz+1:0, s_tp_x, s_tp_y, mode, rw,
                         s_clut_x, s_clut_y, s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1, s_tp_blend);
      } else {
        gpu_vk_draw_tritri_f(xs, ys, us, vs, rs, gs, bs, lx, ly, lz, s_tp_x, s_tp_y, mode, rw, s_clut_x, s_clut_y,
                           s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1);
        if (nv == 4) gpu_vk_draw_tritri_f(&xs[1], &ys[1], &us[1], &vs[1], &rs[1], &gs[1], &bs[1],
                           lx?lx+1:0, ly?ly+1:0, lz?lz+1:0, s_tp_x, s_tp_y, mode, rw,
                           s_clut_x, s_clut_y, s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1);
      }
    }
    // PSXPORT_POLYDUMP=frame — log every poly at `frame` (our port side, to compare vs oracle
    // polywatch). Finds the garbage-block prims in the GAME level.
    { static int pd = -2, pax = -1, pay = -1;
      if (pd == -2) { const char* e = getenv("PSXPORT_POLYDUMP"); pd = e ? atoi(e) : -1;
        const char* pa = getenv("PSXPORT_POLYAT"); if (pa) sscanf(pa, "%d,%d", &pax, &pay); }
      if (pd >= 0 && s_frame == pd) {
        int hit = (pax < 0);   // no point filter -> log all
        if (pax >= 0) {        // log only prims whose screen bbox (incl offset) covers (pax,pay)
          int xmin=99999,xmax=-99999,ymin=99999,ymax=-99999;
          for (int i=0;i<nv;i++){ int X=v[i].x+s_off_x,Y=v[i].y+s_off_y;
            if(X<xmin)xmin=X; if(X>xmax)xmax=X; if(Y<ymin)ymin=Y; if(Y>ymax)ymax=Y; }
          hit = (pax>=xmin && pax<=xmax && pay>=ymin && pay<=ymax);
        }
        static int n = 0;
        if (hit && n++ < 2000)
          fprintf(stderr, "[polydump] f%d node=%08X op=%02X tex=%d gou=%d clut=(%d,%d) tp=(%d,%d) "
                  "cols[(%d,%d,%d)(%d,%d,%d)(%d,%d,%d)(%d,%d,%d)] "
                  "V[(%d,%d)(%d,%d)(%d,%d)(%d,%d)] off=(%d,%d)\n",
                  s_frame, s_cur_node, op, textured?1:0, gouraud?1:0, s_clut_x, s_clut_y, s_tp_x, s_tp_y,
                  v[0].r,v[0].g,v[0].b, v[1].r,v[1].g,v[1].b, v[2].r,v[2].g,v[2].b, v[3].r,v[3].g,v[3].b,
                  v[0].x,v[0].y, v[1].x,v[1].y, v[2].x,v[2].y, v[3].x,v[3].y, s_off_x, s_off_y);
      } }
    if (s_reddbg && textured && s_cw_x >= 0 && s_clut_x == s_cw_x && s_clut_y == s_cw_y) {
      static int n = 0;
      if (n++ < 12)
        fprintf(stderr, "[redpkt] f%d stage=%08X node=0x%08X op=%02X nv=%d gou=%d semi=%d clut=(%d,%d) tp=(%d,%d) blend=%d mode=%d "
                "V[(%d,%d)uv(%d,%d) (%d,%d)uv(%d,%d) (%d,%d)uv(%d,%d)%s] off=(%d,%d)\n",
                s_frame, mem_r32(0x801fe00c), s_cur_node, op, nv, gouraud, semi, s_clut_x, s_clut_y, s_tp_x, s_tp_y, s_tp_blend, s_tp_mode,
                v[0].x,v[0].y,v[0].u,v[0].v, v[1].x,v[1].y,v[1].u,v[1].v, v[2].x,v[2].y,v[2].u,v[2].v,
                quad?" +q":"", s_off_x, s_off_y);
    }
    prov_begin(op, textured ? 1 : 0, semi, v[0].r, v[0].g, v[0].b,
               v[0].x + s_off_x, v[0].y + s_off_y, v[0].u, v[0].v);
    if (!gpu_vk_enabled()) {                  // VK owns poly raster now (tee'd above); SW does the rest
      tri(v[0], v[1], v[2], textured, shade, semi, raw);
      if (quad) tri(v[1], v[2], v[3], textured, shade, semi, raw);
    }
    s_prims++;
  } else if (op >= 0x60 && op <= 0x7F) {     // rectangle / sprite
    int textured = op & 0x04, semi = (op & 0x02) ? 1 : 0, size = (op >> 3) & 3;
    uint8_t cr = cmd_r(c), cg = cmd_g(c), cb = cmd_b(c);
    int idx = 1;
    uint32_t xy = s_fifo[idx++]; int x = cx(xy), y = cy(xy);
    int u0 = 0, v0 = 0;
    if (textured) { uint32_t uv = s_fifo[idx++]; u0 = uv & 0xFF; v0 = (uv >> 8) & 0xFF; set_clut((uv >> 16) & 0xFFFF); }
    int w, h;
    if (size == 0) { uint32_t wh = s_fifo[idx++]; w = wh & 0x3FF; h = (wh >> 16) & 0x1FF; }
    else { w = h = (size == 1) ? 1 : (size == 2) ? 8 : 16; }
    // PSXPORT_POLYDUMP (+POLYAT): also log sprites/rects, so the garbage-block source can be a sprite.
    { static int pd = -2, pax = -1, pay = -1;
      if (pd == -2) { const char* e = getenv("PSXPORT_POLYDUMP"); pd = e ? atoi(e) : -1;
        const char* pa = getenv("PSXPORT_POLYAT"); if (pa) sscanf(pa, "%d,%d", &pax, &pay); }
      if (pd >= 0 && s_frame == pd) {
        int X=x+s_off_x, Y=y+s_off_y;
        int hit = (pax < 0) || (pax>=X && pax<X+w && pay>=Y && pay<Y+h);
        static int n = 0;
        if (hit && n++ < 2000)
          fprintf(stderr, "[polydump] f%d node=%08X SPRITE op=%02X tex=%d semi=%d clut=(%d,%d) tp=(%d,%d) "
                  "col=(%d,%d,%d) at=(%d,%d) %dx%d uv0=(%d,%d) off=(%d,%d)\n",
                  s_frame, s_cur_node, op, textured?1:0, semi, s_clut_x, s_clut_y, s_tp_x, s_tp_y,
                  cr, cg, cb, x, y, w, h, u0, v0, s_off_x, s_off_y);
      } }
    prov_begin(op, textured ? 1 : 0, semi, cr, cg, cb, x + s_off_x, y + s_off_y, u0, v0);
    if (g_wide60_on) {                       // wide60: tee the sprite/rect into PrimFrame B (snaps)
      void wide60_cap_sprite(int, int, int, int, int, int, int, int, int, int,
                             int, int, int, int, int, int, int, int);
      wide60_cap_sprite(op, x, y, u0, v0, w, h, cr, cg, cb, s_off_x, s_off_y,
                        s_tp_x, s_tp_y, s_tp_mode, s_tp_blend, s_clut_x, s_clut_y);
    }
    // bit0=1 -> raw texel; bit0=0 -> modulate by command color (beetle sprite decode table:
    // 0x64/0x66 = TM1 modulate, 0x65/0x67 = TM0 raw). Modulating unconditionally once wrongly
    // tinted raw 0x65 sprites (turned a blue item green).
    if (!gpu_vk_enabled()) raster_sprite(op, x, y, u0, v0, w, h, cr, cg, cb, textured, semi);  // VK owns it (tee'd below)
    // VK backend (M5): tee rects/sprites as two triangles (opaque or semi; mode 3 = untextured solid).
    if (gpu_vk_enabled()) {
      void gpu_vk_draw_tritri(const int*,const int*,const int*,const int*,const unsigned char*,
                              const unsigned char*,const unsigned char*,int,int,int,int,int,int,int,int,int,int,int,int,int,int);
      void gpu_vk_draw_semi(const int*,const int*,const int*,const int*,const unsigned char*,
                            const unsigned char*,const unsigned char*,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int);
      int X = x + s_off_x, Y = y + s_off_y;
      // widescreen: anchor the sprite to its proportional screen position (HUD edge-anchoring) instead
      // of letting the renderer center it. No-op when not wide. (s_da_x0 = active framebuffer origin.)
      { int gpu_vk_sprite_anchor_dx(int); X += gpu_vk_sprite_anchor_dx((X - s_da_x0) + w/2); }
      int qx[4] = { X, X+w, X, X+w }, qy[4] = { Y, Y, Y+h, Y+h };
      int qu[4] = { u0, u0+w, u0, u0+w }, qv[4] = { v0, v0, v0+h, v0+h };
      unsigned char qr[4]={cr,cr,cr,cr}, qg[4]={cg,cg,cg,cg}, qb[4]={cb,cb,cb,cb};
      int mode = textured ? s_tp_mode : 3, rw = op & 1;
      if (semi) {
        gpu_vk_draw_semi(qx, qy, qu, qv, qr, qg, qb, s_tp_x, s_tp_y, mode, rw, s_clut_x, s_clut_y,
                         s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1, s_tp_blend);
        gpu_vk_draw_semi(&qx[1], &qy[1], &qu[1], &qv[1], &qr[1], &qg[1], &qb[1], s_tp_x, s_tp_y, mode, rw,
                         s_clut_x, s_clut_y, s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1, s_tp_blend);
      } else {
        gpu_vk_draw_tritri(qx, qy, qu, qv, qr, qg, qb, s_tp_x, s_tp_y, mode, rw, s_clut_x, s_clut_y,
                           s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1);
        gpu_vk_draw_tritri(&qx[1], &qy[1], &qu[1], &qv[1], &qr[1], &qg[1], &qb[1], s_tp_x, s_tp_y, mode, rw,
                           s_clut_x, s_clut_y, s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy, s_da_x0, s_da_y0, s_da_x1, s_da_y1);
      }
    }
    s_prims++;
  } else if (op == 0x02) {                   // fill rectangle (in VRAM, ignores clip/offset)
    uint8_t cr = cmd_r(c), cg = cmd_g(c), cb = cmd_b(c);
    uint32_t xy = s_fifo[1], wh = s_fifo[2];
    int x = xy & 0x3F0, y = (xy >> 16) & 0x1FF, w = ((wh & 0x3FF) + 0xF) & ~0xF, h = (wh >> 16) & 0x1FF;
    uint16_t col = to555(cr, cg, cb);
    for (int dy = 0; dy < h; dy++) for (int dx = 0; dx < w; dx++) *vram(x + dx, y + dy) = col;
    if (gpu_vk_enabled()) gpu_vk_dirty(x, y, w, h);   // mirror fill to VK
  } else if (op >= 0x40 && op <= 0x5F) {     // line / poly-line (flat or gouraud)
    int semi = (op & 0x02) ? 1 : 0, gouraud = (op & 0x10) ? 1 : 0;
    // Collect the vertex list from s_fifo (cmd carries v0's colour). Single lines have 2 verts;
    // poly-lines have N (gouraud: cmd,xy0,(c,xy)*; mono: cmd,xy0,xy*). Then draw each segment.
    uint8_t r0 = cmd_r(c), g0 = cmd_g(c), b0 = cmd_b(c);
    int vx[64], vy[64]; uint8_t vr[64], vg[64], vb[64]; int nv = 0, i = 1;
    vx[0] = cx(s_fifo[i]); vy[0] = cy(s_fifo[i]); vr[0] = r0; vg[0] = g0; vb[0] = b0; nv = 1; i++;
    while (i < s_fcount && nv < 64) {
      uint8_t r = r0, g = g0, b = b0;
      if (gouraud) { if (i >= s_fcount) break; uint32_t col = s_fifo[i++]; r = cmd_r(col); g = cmd_g(col); b = cmd_b(col); }
      if (i >= s_fcount) break;
      vx[nv] = cx(s_fifo[i]); vy[nv] = cy(s_fifo[i]); vr[nv] = r; vg[nv] = g; vb[nv] = b; nv++; i++;
    }
    void gpu_vk_draw_tritri(const int*,const int*,const int*,const int*,const unsigned char*,
                            const unsigned char*,const unsigned char*,int,int,int,int,int,int,int,int,int,int,int,int,int,int);
    void gpu_vk_draw_semi(const int*,const int*,const int*,const int*,const unsigned char*,
                          const unsigned char*,const unsigned char*,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int);
    for (int s = 0; s + 1 < nv; s++) {        // flat colour = start vertex
      if (!gpu_vk_enabled())
        raster_line(vx[s], vy[s], vx[s+1], vy[s+1], vr[s], vg[s], vb[s], semi);
      else {                                   // VK: tee the segment as a 1px-thick quad (mode 3 flat)
        int x0=vx[s]+s_off_x, y0=vy[s]+s_off_y, x1=vx[s+1]+s_off_x, y1=vy[s+1]+s_off_y;
        int ox = (abs(x1-x0) >= abs(y1-y0)) ? 0 : 1, oy = ox ? 0 : 1;
        int xa[4]={x0,x1,x0+ox,x1+ox}, ya[4]={y0,y1,y0+oy,y1+oy}, zu[4]={0,0,0,0};
        unsigned char rr[4]={vr[s],vr[s+1],vr[s],vr[s+1]}, gg[4]={vg[s],vg[s+1],vg[s],vg[s+1]}, bb[4]={vb[s],vb[s+1],vb[s],vb[s+1]};
        int o1[3]={0,1,2}, o2[3]={1,2,3};      // tris (p0,p1,p0') and (p1,p0',p1')
        for (int t = 0; t < 2; t++) { int* o = t ? o2 : o1;
          int X[3]={xa[o[0]],xa[o[1]],xa[o[2]]}, Y[3]={ya[o[0]],ya[o[1]],ya[o[2]]};
          unsigned char R[3]={rr[o[0]],rr[o[1]],rr[o[2]]}, G[3]={gg[o[0]],gg[o[1]],gg[o[2]]}, B[3]={bb[o[0]],bb[o[1]],bb[o[2]]};
          if (semi) gpu_vk_draw_semi(X,Y,zu,zu,R,G,B, 0,0,3,0,0,0, 0,0,0,0, s_da_x0,s_da_y0,s_da_x1,s_da_y1, s_tp_blend);
          else      gpu_vk_draw_tritri(X,Y,zu,zu,R,G,B, 0,0,3,0,0,0, 0,0,0,0, s_da_x0,s_da_y0,s_da_x1,s_da_y1);
        }
      }
      if (g_wide60_on) {                       // wide60: tee each segment (snaps; obj 0) so the interp
        void wide60_cap_line(int, int, int, int, int, int, int, int, int);   // frame keeps the line
        wide60_cap_line(op, vx[s], vy[s], vx[s+1], vy[s+1], vr[s], vg[s], vb[s], semi);
      }
    }
    s_prims++;
  }
  // env commands (E1..E6) handled in gpu_gp0 directly (single-word).
}

// ---- wide60 in-between synthesizer: direct-draw entry points -------------------------
// The 60fps tier (wide60.c) re-rasterizes a lerped copy of the captured display list into the SEPARATE
// s_interp buffer (see gpu_w60_begin_interp). These reuse the SAME tri()/raster_sprite() path as
// gp0_exec (no GP0 re-encode round-trip), driven by explicit decoded state; textures read from VRAM.
void gpu_w60_draw_poly(int op, int nv, const int* xs, const int* ys, const int* us, const int* vs,
                       const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                       int tp_x, int tp_y, int mode, int blend, int dither, int clut_x, int clut_y) {
  int gouraud = op & 0x10, textured = op & 0x04, semi = (op & 0x02) ? 1 : 0;
  int raw = textured && (op & 0x01);
  s_tp_x = tp_x; s_tp_y = tp_y; s_tp_mode = mode; s_tp_blend = blend; s_tp_dither = dither;
  s_clut_x = clut_x; s_clut_y = clut_y;
  Vtx v[4];
  for (int i = 0; i < nv; i++) { v[i].x=xs[i]; v[i].y=ys[i]; v[i].u=us[i]; v[i].v=vs[i];
                                 v[i].r=rs[i]; v[i].g=gs[i]; v[i].b=bs[i]; }
  int shade = gouraud || !textured;
  tri(v[0], v[1], v[2], textured, shade, semi, raw);
  if (nv == 4) tri(v[1], v[2], v[3], textured, shade, semi, raw);
}
void gpu_w60_draw_sprite(int op, int x, int y, int u0, int v0, int w, int h,
                         int r, int g, int b, int tp_x, int tp_y, int mode, int blend,
                         int clut_x, int clut_y) {
  int textured = op & 0x04, semi = (op & 0x02) ? 1 : 0;
  s_tp_x = tp_x; s_tp_y = tp_y; s_tp_mode = mode; s_tp_blend = blend;
  s_clut_x = clut_x; s_clut_y = clut_y;
  raster_sprite(op, x, y, u0, v0, w, h, (uint8_t)r, (uint8_t)g, (uint8_t)b, textured, semi);
}
void gpu_w60_draw_line(int x0, int y0, int x1, int y1, int r, int g, int b, int semi) {
  raster_line(x0, y0, x1, y1, (uint8_t)r, (uint8_t)g, (uint8_t)b, semi);
}

// Words needed to complete the packet beginning with command word `c`.
static int gp0_len(uint32_t c) {
  uint8_t op = c >> 24;
  if (op >= 0x20 && op <= 0x3F) {
    int n = 1, nv = (op & 8) ? 4 : 3;
    n += nv * (1 + ((op & 4) ? 1 : 0));        // xy (+uv) per vertex
    if (op & 0x10) n += nv - 1;                 // extra colors for gouraud (first is cmd)
    return n;
  }
  if (op >= 0x60 && op <= 0x7F) { int n = 2; if (op & 4) n++; if (((op >> 3) & 3) == 0) n++; return n; }
  if (op >= 0x40 && op <= 0x5F) return (op & 0x10) ? 4 : 3;  // (poly-line term not modeled)
  if (op == 0x02) return 3;                      // fill
  if (op == 0x80) return 4;                       // VRAM->VRAM copy: cmd + src + dst + size
  if (op == 0xA0 || op == 0xC0) return 3;        // CPU<->VRAM xfer headers (pixels stream after)
  return 1;                                      // env / nop / single-word
}

// ---- GP0 trace capture (PSXPORT_GPUTRACE="frame[:path]") ------------------------------------
// Records, for ONE target frame, a snapshot of VRAM at frame start plus the EXACT GP0 word stream
// fed to gpu_gp0() during that frame. Replaying that file through BOTH our renderer and Beetle's
// (tools/gpu_differ) from the identical initial VRAM makes any output-VRAM difference a pure
// rasterizer-fidelity difference — no live game-state alignment needed (the whole point: our HLE
// port and the full-emulation oracle run at different timings, so we can't align by frame number;
// feeding the same primitive stream to both rasterizers sidesteps that entirely).
//
// File format ("GP0TRC01"): magic[8], u32 frame, u32 word_count, u32 vram_w(1024), u32 vram_h(512),
// then vram_w*vram_h u16 (initial VRAM), then word_count u32 (the GP0 stream, in feed order).
// PSXPORT_GPUTRACE="frame[,frame...][:path]". A single frame writes to `path` exactly (default
// scratch/bin/gp0trace.bin) — back-compat with the differ pipeline. A comma-separated LIST writes
// one file per frame at "<path>_f<N>.bin" (so a single deterministic run captures many scenes; the
// game is deterministic under PSXPORT_FORCE_BUTTONS, so frame N is reproducible). Targets are kept
// in feed order; we capture them one at a time as s_frame reaches each.
#define TRACE_MAX 64
static int        s_trace_on = -1;       // -1 lazy, 0 off, 1 armed
static int        s_trace_frames[TRACE_MAX]; // target frames
static int        s_trace_count;         // number of targets
static int        s_trace_idx;           // index of the target currently being captured
static int        s_trace_multi;         // 1 if a list (per-frame filenames), 0 if single exact path
static const char* s_trace_path = "scratch/bin/gp0trace.bin";
static uint16_t*  s_trace_init;          // VRAM snapshot at frame start
static uint32_t*  s_trace_words;         // captured GP0 words (grown)
static size_t     s_trace_cap, s_trace_n;
static int        s_trace_inited;

static void trace_init_env(void) {
  const char* e = getenv("PSXPORT_GPUTRACE");
  s_trace_on = e ? 1 : 0;
  if (!e) return;
  const char* c = strchr(e, ':');
  if (c) s_trace_path = c + 1;
  s_trace_multi = (strchr(e, ',') != NULL);
  const char* p = e;
  while (*p && p != c && s_trace_count < TRACE_MAX) {
    s_trace_frames[s_trace_count++] = atoi(p);
    const char* nc = strchr(p, ',');
    if (!nc || (c && nc > c)) break;
    p = nc + 1;
  }
}

static void trace_record(uint32_t w) {
  if (s_trace_on < 0) trace_init_env();
  if (s_trace_on <= 0 || s_trace_idx >= s_trace_count || s_frame != s_trace_frames[s_trace_idx]) return;
  if (!s_trace_inited) {
    if (!s_trace_init) s_trace_init = (uint16_t*)malloc(sizeof(uint16_t) * VRAM_W * VRAM_H);
    memcpy(s_trace_init, s_vram, sizeof(uint16_t) * VRAM_W * VRAM_H);
    s_trace_inited = 1;
  }
  if (s_trace_n >= s_trace_cap) {
    s_trace_cap = s_trace_cap ? s_trace_cap * 2 : 65536;
    s_trace_words = (uint32_t*)realloc(s_trace_words, s_trace_cap * sizeof(uint32_t));
  }
  s_trace_words[s_trace_n++] = w;
}

static void trace_flush(void) {  // called from gpu_present while s_frame is still the target
  if (s_trace_on <= 0 || s_trace_idx >= s_trace_count || !s_trace_inited ||
      s_frame != s_trace_frames[s_trace_idx]) return;
  char namebuf[512];
  const char* path = s_trace_path;
  if (s_trace_multi) { snprintf(namebuf, sizeof namebuf, "%s_f%d.bin", s_trace_path, s_frame); path = namebuf; }
  FILE* f = fopen(path, "wb");
  if (f) {
    uint32_t meta[4] = { (uint32_t)s_frame, (uint32_t)s_trace_n, VRAM_W, VRAM_H };
    fwrite("GP0TRC01", 1, 8, f);
    fwrite(meta, 4, 4, f);
    fwrite(s_trace_init, 2, VRAM_W * VRAM_H, f);
    fwrite(s_trace_words, 4, s_trace_n, f);
    fclose(f);
    fprintf(stderr, "[gputrace] f%d -> %s (%zu words)\n", s_frame, path, s_trace_n);
  }
  s_trace_idx++;            // advance to the next target frame; reset the per-frame capture
  s_trace_inited = 0;
  s_trace_n = 0;
}

// One word into the GP0 port (direct write or DMA).
void gpu_gp0(uint32_t w) {
  s_gp0_words++;
  trace_record(w);
  if (s_xfer) {                                  // CPU->VRAM pixel stream (2 px/word)
    for (int k = 0; k < 2; k++) {
      int px = s_xfer_px % s_xfer_w, py = s_xfer_px / s_xfer_w;
      if (py < s_xfer_h) *vram(s_xfer_x + px, s_xfer_y + py) = (k ? (w >> 16) : w) & 0xFFFF;
      s_xfer_px++;
    }
    if (s_xfer_px >= s_xfer_w * s_xfer_h) {
      s_xfer = 0;
      if (s_cw_pending) { clutwatch_dump("A0 DONE", s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h); s_cw_pending = 0; }
    }
    return;
  }
  if (s_fcount == 0) {
    uint8_t op = w >> 24;
    switch (op) {                                // single-word env / state commands
      case 0x00: return;                         // nop
      case 0x01: return;                         // clear cache
      case 0xE1: set_texpage(w & 0xFFFF); return;
      case 0xE2: s_tw_mx = w & 31; s_tw_my = (w >> 5) & 31; s_tw_ox = (w >> 10) & 31; s_tw_oy = (w >> 15) & 31; return;
      case 0xE3: s_da_x0 = w & 0x3FF; s_da_y0 = (w >> 10) & 0x1FF;
        if (getenv("PSXPORT_ENVDBG")) fprintf(stderr, "[env] E3 clip_tl=(%d,%d)\n", s_da_x0, s_da_y0); return;
      case 0xE4: s_da_x1 = w & 0x3FF; s_da_y1 = (w >> 10) & 0x1FF;
        if (getenv("PSXPORT_ENVDBG")) fprintf(stderr, "[env] E4 clip_br=(%d,%d)\n", s_da_x1, s_da_y1); return;
      case 0xE5: s_off_x = ((int)(w & 0x7FF) << 21) >> 21; s_off_y = ((int)((w >> 11) & 0x7FF) << 21) >> 21;
        if (getenv("PSXPORT_ENVDBG")) fprintf(stderr, "[env] E5 offset=(%d,%d)\n", s_off_x, s_off_y); return;
      case 0xE6: return;                         // mask settings (mask-test not modeled)
      default: break;
    }
    // Poly-lines (op 0x48-0x4F mono / 0x58-0x5F gouraud — line group 0x40-0x5F with bit 0x08) are
    // VARIABLE length: a vertex list terminated by a word with (w & 0xF000F000)==0x50005000
    // (0x55555555). gp0_len can't know the length from the first word, so accumulate until the
    // terminator. Mishandling this (treating it as a fixed 3/4-word single line) drifts the whole
    // GP0 parse and makes a later data word decode as a spurious VRAM copy (atlas corruption).
    s_pl = (op >= 0x40 && op <= 0x5F && (op & 0x08)) ? 1 : 0;
    s_pl_g = (op & 0x10) ? 1 : 0;
    s_fneed = gp0_len(w);
  }
  s_fifo[s_fcount++] = w;
  if (s_pl) {
    int idx = s_fcount - 1;                          // index of the word just stored
    // A terminator may appear at a vertex-START slot, only after the mandatory 2 vertices:
    //   gouraud: color slots = even indices >= 4 (cmd,xy0,c1,xy1, then c2/term,xy2,...)
    //   mono:    xy slots    = indices >= 3        (cmd,xy0,xy1, then xy2/term,...)
    int term_slot = s_pl_g ? (idx >= 4 && !(idx & 1)) : (idx >= 3);
    if (term_slot && (w & 0xF000F000u) == 0x50005000u) {
      s_fcount = idx;                                // drop the terminator; render cmd+vertices
      gp0_exec();
      s_fcount = 0; s_fneed = 0; s_pl = 0;
      return;
    }
    if (s_fcount >= 250) { s_fcount = 0; s_fneed = 0; s_pl = 0; }  // safety: never overflow s_fifo
    return;
  }
  if (s_fcount >= s_fneed) {
    uint8_t op = s_fifo[0] >> 24;
    if (op == 0xA0) {                            // CPU->VRAM: set up the pixel stream
      s_xfer_x = s_fifo[1] & 0x3FF; s_xfer_y = (s_fifo[1] >> 16) & 0x1FF;
      s_xfer_w = ((s_fifo[2] & 0x3FF) ? (s_fifo[2] & 0x3FF) : 1024);
      s_xfer_h = (((s_fifo[2] >> 16) & 0x1FF) ? ((s_fifo[2] >> 16) & 0x1FF) : 512);
      s_xfer_px = 0; s_xfer = 1;
      if (gpu_vk_enabled()) gpu_vk_dirty(s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h);   // mirror upload to VK
      clutwatch_xfer("A0", s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h);
      if (getenv("PSXPORT_UPLOADLOG")) {
        extern uint32_t g_dma_src;
        fprintf(stderr, "[upload] f%d A0 dest=(%d,%d) %dx%d src=0x%08X\n",
                s_frame, s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h, 0x80000000u | g_dma_src);
      }
      if (texwatch_overlap(s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h)) {
        extern uint32_t g_dma_src;
        uint32_t src = 0x80000000u | g_dma_src;
        fprintf(stderr, "[texwatch] f%d A0 dest=(%d,%d) %dx%d src=0x%08X srcbytes:",
                s_frame, s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h, src);
        for (int k = 0; k < 12; k++) fprintf(stderr, " %02X", mem_r8(g_dma_src + k));
        fprintf(stderr, "\n");
      }
    } else if (op == 0x80) {                     // VRAM->VRAM copy
      int sx = s_fifo[1] & 0x3FF, sy = (s_fifo[1] >> 16) & 0x1FF;
      int dx = s_fifo[2] & 0x3FF, dy = (s_fifo[2] >> 16) & 0x1FF;
      int w2 = s_fifo[3] & 0x3FF, h2 = (s_fifo[3] >> 16) & 0x1FF;
      for (int y = 0; y < h2; y++) for (int x = 0; x < w2; x++) *vram(dx + x, dy + y) = *vram(sx + x, sy + y);
      if (gpu_vk_enabled()) gpu_vk_dirty(dx, dy, w2, h2);   // mirror VRAM->VRAM copy to VK
      clutwatch_xfer("80copy", dx, dy, w2, h2);
      if (texwatch_overlap(dx, dy, w2, h2)) {
        fprintf(stderr, "[texwatch] f%d 80copy src=(%d,%d) dest=(%d,%d) %dx%d node=0x%08X words=%08X,%08X,%08X,%08X\n",
                s_frame, sx, sy, dx, dy, w2, h2, s_cur_node, s_fifo[0], s_fifo[1], s_fifo[2], s_fifo[3]);
        // Dump RAM + the OT node neighbourhood the first time the atlas-clobbering copy fires, so the
        // malformed node and the chain that reaches it can be examined offline.
        if (getenv("PSXPORT_CLOBBERDUMP")) { static int done = 0; if (!done++) {
          extern uint8_t g_ram[]; uint32_t na = s_cur_node & 0x1FFFFF;
          fprintf(stderr, "[clobber] OT root madr=0x%08X node@0x%08X neighbourhood:\n", 0x80000000u|g_ot_madr, s_cur_node);
          for (int k = -8; k <= 16; k++) fprintf(stderr, "  [%+d] 0x%08X: %08X\n", k,
                  0x80000000u | ((na + k*4) & 0x1FFFFF), mem_r32(0x80000000u | ((na + k*4) & 0x1FFFFF)));
          FILE* mf = fopen(getenv("PSXPORT_CLOBBERDUMP"), "wb");
          if (mf) { fwrite(g_ram, 1, 0x200000, mf); fclose(mf);
                    fprintf(stderr, "[clobber] RAM dumped -> %s\n", getenv("PSXPORT_CLOBBERDUMP")); } } }
      }
    } else if (op != 0xC0) {
      gp0_exec();
    }
    s_fcount = 0; s_fneed = 0;
  }
}

// GP1 display/control commands.
void gpu_gp1(uint32_t w) {
  uint8_t op = w >> 24;
  if (getenv("PSXPORT_GP1LOG"))
    fprintf(stderr, "[gp1] f%d %02X %06X\n", s_frame, op, w & 0xFFFFFF);
  switch (op) {
    case 0x05: s_disp_x = w & 0x3FF; s_disp_y = (w >> 10) & 0x1FF; break;          // display area start
    case 0x07:  // vertical display range (scanlines). In 480i the field is shown twice (two VRAM
      // lines per scanline), so the displayed VRAM height is (y1-y0)*2 — without the doubling the
      // bottom of a 480-line framebuffer is clipped (the SCEA "Presents" line, journal later-46).
      s_disp_vy0 = w & 0x3FF; s_disp_vy1 = (w >> 10) & 0x3FF;
      { int n = s_disp_vy1 - s_disp_vy0; if (n <= 0) n = 240; s_disp_h = s_disp_480i ? n * 2 : n; }
      break;
    case 0x08:  // display mode: horizontal res (bits0-1, bit6=368), interlace (bit5), VRes 480 (bit2)
      s_disp_w = ((w & 3) == 0) ? 256 : ((w & 3) == 1) ? 320 : ((w & 3) == 2) ? 512 : 640;
      s_disp_480i = ((w & 0x20) && (w & 0x04)) ? 1 : 0;
      { int n = s_disp_vy1 - s_disp_vy0; if (n <= 0) n = 240; s_disp_h = s_disp_480i ? n * 2 : n; }
      break;
    default: break;
  }
}

// Optional live window (PSXPORT_GPU_WINDOW=1). Headless builds without SDL just no-op.
// The output is fit to the screen at a fixed 4:3 display aspect with letterbox/pillarbox bars —
// NEVER stretched. PSX always scans its framebuffer (whatever the horizontal res: 256/320/512/640)
// out to the same 4:3 screen area, so mapping disp_w×disp_h into a 4:3 rect reproduces the correct
// pixel aspect and keeps 2D art / FMVs un-stretched regardless of window size. (This is the display
// scaler, independent of the — currently blocked — widescreen GEOMETRY tier; we do not widen here.)
#ifdef PSXPORT_SDL
static SDL_Window* s_win; static SDL_Renderer* s_ren; static SDL_Texture* s_tex;
static int s_tex_w, s_tex_h, s_win_on = -1;
static int win_enabled(void) {
  // Check the VALUE, not mere presence: run.sh always SETS PSXPORT_GPU_WINDOW (to "0" headless),
  // and getenv("...")!=NULL is truthy for "0" too — so a presence test opened a window even with
  // PSXPORT_NOWINDOW=1 (the "still running headed" bug). Match gpu_pace_subframe: atoi(w)!=0.
  if (s_win_on < 0) { const char* w = getenv("PSXPORT_GPU_WINDOW"); s_win_on = (w && atoi(w) != 0) ? 1 : 0; }
  return s_win_on;
}
static void ensure_window(void) {
  if (!s_win) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");   // linear filter for a smooth upscale
    // Default: borderless fullscreen at the desktop resolution ("adapt to screen"). PSXPORT_WINDOWED=1
    // opens a resizable 3x window instead (handy for dev). The 4:3 fit below applies to both.
    int windowed = getenv("PSXPORT_WINDOWED") && atoi(getenv("PSXPORT_WINDOWED")) != 0;
    Uint32 flags = windowed ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN_DESKTOP;
    s_win = SDL_CreateWindow("Tomba! 2 (native PC port)", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, s_disp_w * 3, s_disp_h * 3, flags);
    // No PRESENTVSYNC: the manual pacer (gpu_pace_subframe) is the single timing authority, so the
    // loop runs at the right wall-clock rate (and audio stays realtime) on ANY monitor refresh rate.
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
  }
  if (!s_tex || s_tex_w != s_disp_w || s_tex_h != s_disp_h) {
    if (s_tex) SDL_DestroyTexture(s_tex);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                              s_disp_w, s_disp_h);
    s_tex_w = s_disp_w; s_tex_h = s_disp_h;
  }
}
// Blit one display-sized region [sx,sy .. +disp_w,disp_h] of `src` (s_vram or s_interp) to the window,
// fit to a 4:3 rect with black bars — never stretched (correct PSX pixel aspect for 2D / FMV / any res).
int  gpu_vk_enabled(void);                                   // gpu_vk.c — Vulkan present backend (M0)
void gpu_vk_present(const uint16_t*, int, int, int, int);
static void blit_src(const uint16_t* src, int sx, int sy) {
  if (!win_enabled()) return;
  if (gpu_vk_enabled()) { gpu_vk_present(src, sx, sy, s_disp_w, s_disp_h); return; }  // HW path
  ensure_window();
  static uint32_t buf[VRAM_W * VRAM_H];
  for (int y = 0; y < s_disp_h; y++)
    for (int x = 0; x < s_disp_w; x++) {
      uint16_t p = src[((sy + y) & 511) * VRAM_W + ((sx + x) & 1023)];
      buf[y * s_disp_w + x] = 0xFF000000u | (((p >> 10) & 31) << 19) | (((p >> 5) & 31) << 11) | ((p & 31) << 3);
    }
  SDL_UpdateTexture(s_tex, NULL, buf, s_disp_w * 4);
  int ow = 0, oh = 0; SDL_GetRendererOutputSize(s_ren, &ow, &oh);
  SDL_Rect dst;
  if (ow * 3 >= oh * 4) { dst.h = oh; dst.w = oh * 4 / 3; }   // screen wider than 4:3 -> pillarbox
  else                  { dst.w = ow; dst.h = ow * 3 / 4; }   // screen taller than 4:3 -> letterbox
  dst.x = (ow - dst.w) / 2; dst.y = (oh - dst.h) / 2;
  SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
  SDL_RenderClear(s_ren); SDL_RenderCopy(s_ren, s_tex, NULL, &dst); SDL_RenderPresent(s_ren);
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) exit(0);
    if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) exit(0);
  }
}
static void present_window(void) { blit_src(s_vram, s_disp_x, s_disp_y); }   // the live front buffer
// wide60: present the previous real frame straight from VRAM (read-only), and the interpolated frame
// from the separate s_interp buffer. Both blit a display-sized region at the given VRAM origin.
void gpu_w60_blit_vram(int dx, int dy)   { blit_src(s_vram,   dx, dy); }
void gpu_w60_blit_interp(int dx, int dy) { blit_src(s_interp, dx, dy); }
#else
static void present_window(void) {}
void gpu_w60_blit_vram(int dx, int dy)   { (void)dx; (void)dy; }
void gpu_w60_blit_interp(int dx, int dy) { (void)dx; (void)dy; }
#endif

// wide60 headless validation: dump a display-sized region of a buffer to a PPM (P6). Used by the
// synth dumptest to compare prev / interpolated / current frames offline.
static void shot_buf(const uint16_t* src, int dx, int dy, const char* path) {
  FILE* f = fopen(path, "wb"); if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", s_disp_w, s_disp_h);
  for (int y = 0; y < s_disp_h; y++)
    for (int x = 0; x < s_disp_w; x++) {
      uint16_t p = src[((dy + y) & 511) * VRAM_W + ((dx + x) & 1023)];
      uint8_t rgb[3] = { (uint8_t)((p & 31) << 3), (uint8_t)(((p >> 5) & 31) << 3), (uint8_t)(((p >> 10) & 31) << 3) };
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
}
void gpu_w60_shot_vram(int dx, int dy, const char* path)   { shot_buf(s_vram,   dx, dy, path); }
void gpu_w60_shot_interp(int dx, int dy, const char* path) { shot_buf(s_interp, dx, dy, path); }

// wide60: redirect the rasterizer's framebuffer writes to the separate s_interp buffer, clear the
// target display region, and set the draw env. The interpolated display list is then rasterized via
// the normal gpu_w60_draw_* path (textures still read from VRAM). end_interp restores VRAM as target.
void gpu_w60_begin_interp(int off_x, int off_y, int cx0, int cy0, int cx1, int cy1) {
  s_fb_base = s_interp;
  for (int y = cy0; y <= cy1; y++)
    for (int x = cx0; x <= cx1; x++) s_interp[(y & 511) * VRAM_W + (x & 1023)] = 0;
  s_off_x = off_x; s_off_y = off_y;
  s_da_x0 = cx0; s_da_y0 = cy0; s_da_x1 = cx1; s_da_y1 = cy1;
}
void gpu_w60_end_interp(void) { s_fb_base = s_vram; }

// Frame pacing: the native game loop (ov_game_main) runs UNTHROTTLED — at thousands of fps.
// That's right for headless tests but unplayable windowed. When a window is up we throttle to
// the game's own pace: DAT_1f800235 is the engine's vblank quota (vblanks at 60 Hz per displayed
// frame; =2 => 30 fps, Tomba2's logic rate). PSXPORT_NOPACE disables (fast-forward); headless
// (no window) is never paced so tests stay fast. SDL timing keeps it portable (a window implies
// SDL is up). Called ONCE per native game-frame from ov_frame_update — NOT from gpu_present,
// which the boot stub also drives many times per frame (pacing those would stall the boot).
// Pace 1/`parts` of a logic frame: parts=1 → one full logic frame (30fps faithful path); parts=2 →
// half a logic frame (wide60 presents twice per logic frame for 60fps). The shared `next` accumulator
// advances by exactly one logic frame's worth per logic frame either way, so audio stays realtime.
void gpu_pace_subframe(int parts) {
#ifdef PSXPORT_SDL
  static int on = -1;
  if (on < 0) {
    const char* w = getenv("PSXPORT_GPU_WINDOW");   // NB: run.sh sets this to "0" headless, so
    on = (w && atoi(w) != 0 && !getenv("PSXPORT_NOPACE")) ? 1 : 0;  // check the VALUE, not presence
  }
  if (!on) return;
  if (parts < 1) parts = 1;
  uint8_t mem_r8(uint32_t);
  int quota = mem_r8(0x1F800235u); if (quota < 1) quota = 2;   // vblanks per frame (default 30fps)
  double interval = quota * 1000.0 / 60.0 / parts;             // ms for this sub-frame
  static double next = -1;
  double now = (double)SDL_GetTicks();
  if (next < 0) next = now;
  next += interval;
  if (next > now) SDL_Delay((unsigned)(next - now));
  else if (now - next > interval) next = now;                  // resync after a hitch (no debt)
#else
  (void)parts;
#endif
}
void gpu_pace_frame(void) { gpu_pace_subframe(1); }

// Present: copy the displayed VRAM region to an RGB buffer. PSXPORT_GPU_DUMP=dir dumps PPMs;
// PSXPORT_GPU_WINDOW=1 shows a live SDL window.
static int s_frame = 0;
// REPL `shot <path>`: write the currently-displayed VRAM region to a PPM so I can SEE where the
// interactive driver is (title / menu / attract / gameplay) instead of guessing from stage numbers.
void gpu_native_shot(const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "[shot] cannot open %s\n", path); return; }
  fprintf(f, "P6\n%d %d\n255\n", s_disp_w, s_disp_h);
  for (int y = 0; y < s_disp_h; y++)
    for (int x = 0; x < s_disp_w; x++) {
      uint16_t p = *vram(s_disp_x + x, s_disp_y + y);
      uint8_t rgb[3] = { (uint8_t)((p & 31) << 3), (uint8_t)(((p >> 5) & 31) << 3), (uint8_t)(((p >> 10) & 31) << 3) };
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
  fprintf(stderr, "[shot] f%d -> %s (%dx%d disp@%d,%d)\n", s_frame, path, s_disp_w, s_disp_h, s_disp_x, s_disp_y);
}
// gpu_present_ex: the per-frame present + bookkeeping. `do_blit` blits the live front buffer to the
// window; wide60 passes 0 (it owns presentation: it blits the previous real frame + the interpolated
// frame itself) but still wants the bookkeeping (watchdog, s_frame++, diagnostics).
void gpu_present_ex(int do_blit) {
  void watchdog_pet(void);
  watchdog_pet();             // frame-progress heartbeat (see watchdog.c)
  trace_flush();              // PSXPORT_GPUTRACE: write this frame's GP0 trace (no-op unless armed)
  if (getenv("PSXPORT_VRAMSCAN")) {
    int minx=99999,miny=99999,maxx=-1,maxy=-1; long nz=0;
    for (int y=0;y<512;y++) for (int x=0;x<1024;x++) if (*vram(x,y)&0x7FFF) {
      nz++; if(x<minx)minx=x; if(x>maxx)maxx=x; if(y<miny)miny=y; if(y>maxy)maxy=y; }
    fprintf(stderr, "[vramscan] f%d disp@(%d,%d) %dx%d  nonblack=%ld bbox=(%d,%d)-(%d,%d)\n",
            s_frame, s_disp_x, s_disp_y, s_disp_w, s_disp_h, nz, minx, miny, maxx, maxy);
  }
  if (do_blit) present_window();
  { void ws_sx_dump(const char*);   // widescreen RE (later-55): dump GTE screen-X histogram
    if (getenv("PSXPORT_WS_SXHIST") && s_frame > 0 && (s_frame % 500) == 0) {
      char t[32]; snprintf(t, sizeof t, "f%d", s_frame); ws_sx_dump(t); } }
  // PSXPORT_PROVAT="x,y[:frame]" — at present time, report (in DISPLAY space, so the double buffer
  // is irrelevant) which primitive last wrote each pixel in a 7x7 box around (x,y), with how many
  // frames ago it was drawn. A wrong pixel whose writer is the current frame = actively drawn (the
  // listed prim is the culprit); whose writer is many frames old = STALE, revealed through a gap.
  { const char* pa = getenv("PSXPORT_PROVAT");
    if (pa) {
      int qx = -1, qy = -1, qf = -1; sscanf(pa, "%d,%d", &qx, &qy);
      const char* col = strchr(pa, ':'); if (col) qf = atoi(col + 1);
      if (qx >= 0 && (qf < 0 ? (s_frame % 200 == 0) : s_frame == qf)) {
        fprintf(stderr, "[provat] f%d display (%d,%d) +/-3  (disp@%d,%d)\n",
                s_frame, qx, qy, s_disp_x, s_disp_y);
        for (int dy = -3; dy <= 3; dy++) for (int dx = -3; dx <= 3; dx++) {
          int vx = s_disp_x + qx + dx, vy = s_disp_y + qy + dy;
          uint16_t p = *vram(vx, vy);
          uint32_t gid = s_prov[(vy & 511) * VRAM_W + (vx & 1023)];
          ProvMeta* m = &s_provmeta[gid % PROVRING];
          int valid = (m->gid == gid && gid != 0);
          fprintf(stderr, "  (%+d,%+d) vram(%d,%d)=%04X rgb(%d,%d,%d)", dx, dy, vx, vy, p,
                  (p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3);
          if (!gid) fprintf(stderr, "  <never written>\n");
          else if (!valid) fprintf(stderr, "  gid=%u <evicted: drawn long ago = STALE>\n", gid);
          else fprintf(stderr, "  gid=%u age=%dframes op=%02X tex=%d mode=%d semi=%d clut=(%d,%d) tp=(%d,%d) "
                       "primcol=(%d,%d,%d) node=%08X v0=(%d,%d) uv0=(%d,%d)\n",
                       gid, (int)((uint32_t)s_frame - m->frame), m->op, m->tex, m->mode, m->semi,
                       m->clut_x, m->clut_y, m->tp_x, m->tp_y, m->r, m->g, m->b, m->node,
                       m->x0, m->y0, m->u0, m->v0);
        }
      }
    } }
  { const char* vd = getenv("PSXPORT_VRAMDUMP_AT");   // "frame:path" — dump our 1024x512x16 VRAM
    if (vd) { int fr = atoi(vd); const char* col = strchr(vd, ':');
      if (col && s_frame == fr) { FILE* vf = fopen(col + 1, "wb");
        if (vf) { fwrite(s_vram, 2, VRAM_W * VRAM_H, vf); fclose(vf);
                  fprintf(stderr, "[gpu] VRAM dump f%d -> %s\n", s_frame, col + 1); } } } }
  if (getenv("PSXPORT_STAGETL") && (s_frame % 200) == 0)
    fprintf(stderr, "[stagetl] gpu f%d task0entry=%08X\n", s_frame, mem_r32(0x801fe00c));
  const char* dir = getenv("PSXPORT_GPU_DUMP");
  if (g_log) fprintf(stderr, "[gpu] frame %d: %ld prims, %ld gp0words, %ld dma2, disp %dx%d @ (%d,%d)\n",
                     s_frame, s_prims, s_gp0_words, s_dma2, s_disp_w, s_disp_h, s_disp_x, s_disp_y);
  // PSXPORT_VRAMDUMP="frame:path" — dump our full 1024x512x16 VRAM at `frame` (raw u16, no header),
  // matching the oracle's PSXPORT_VRAMDUMP (main.cpp) so the texture/CLUT ATLAS can be diffed across
  // engines at a scene-aligned frame (the atlas is uploaded once at scene load = static per scene).
  { static int vf = -2; static char vp[256];
    if (vf == -2) { const char* e = getenv("PSXPORT_VRAMDUMP"); vf = -1;
      if (e) { const char* col = strchr(e, ':'); if (col) { vf = atoi(e); snprintf(vp, sizeof vp, "%s", col + 1); } } }
    if (vf >= 0 && s_frame == vf) { FILE* f = fopen(vp, "wb");
      if (f) { fwrite(s_vram, 2, (size_t)VRAM_W * VRAM_H, f); fclose(f);
               fprintf(stderr, "[vramdump] f%d -> %s (1024x512x16)\n", s_frame, vp); } } }
  if (dir) {
    if (s_frame == 0) { char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir); int r = system(cmd); (void)r; }
    char path[512]; snprintf(path, sizeof path, "%s/f%05d.ppm", dir, s_frame);
    FILE* f = fopen(path, "wb");
    if (f) {
      fprintf(f, "P6\n%d %d\n255\n", s_disp_w, s_disp_h);
      for (int y = 0; y < s_disp_h; y++)
        for (int x = 0; x < s_disp_w; x++) {
          uint16_t p = *vram(s_disp_x + x, s_disp_y + y);
          uint8_t rgb[3] = { (uint8_t)((p & 31) << 3), (uint8_t)(((p >> 5) & 31) << 3), (uint8_t)(((p >> 10) & 31) << 3) };
          fwrite(rgb, 1, 3, f);
        }
      fclose(f);
    }
  }
  { void gpu_vk_dump(int,int,int,int,int); gpu_vk_dump(s_disp_x, s_disp_y, s_disp_w, s_disp_h, s_frame); }  // PSXPORT_VK_SHOT
  { void gpu_vk_frame_end(const uint16_t*, int); gpu_vk_frame_end(s_vram, s_frame); }  // VK: diff + batch reset
  { void gte_probe_dump(const char*); static int gp = -1;   // PSXPORT_GTEPROBE=frame: dump GTE/lighting RE probe once
    if (gp == -2) {} else { if (gp == -1) { const char* e = getenv("PSXPORT_GTEPROBE"); gp = e ? atoi(e) : -2; }
      if (gp >= 0 && s_frame == gp) { char t[24]; snprintf(t,sizeof t,"f%d",s_frame); gte_probe_dump(t); gp = -2; } } }
  // Native lighting engine config (one-shot, env-driven). PSXPORT_LIGHT: 0=off 1=directional 2=normal-viz.
  // LIGHT_DIR="x,y,z" (view space, default upper-left toward camera), LIGHT_AMB / LIGHT_DIFF scalars,
  // FOG=1 + FOG_NEAR/FOG_FAR/FOG_RGB for native distance fog. See docs/engine_re.md (lighting model).
  { void gpu_vk_set_light(const float*); static int done = 0;
    if (!done) { done = 1;
      float L[12] = {0,0,0,0, 1,0,0,0, 0,0,0,0};
      const char* m = getenv("PSXPORT_LIGHT");
      if (m) { float dx=-0.4f,dy=-0.6f,dz=-0.7f, amb=0.55f, diff=0.6f;
        const char* d = getenv("PSXPORT_LIGHT_DIR"); if (d) sscanf(d, "%f,%f,%f", &dx,&dy,&dz);
        const char* a = getenv("PSXPORT_LIGHT_AMB"); if (a) amb = atof(a);
        const char* df = getenv("PSXPORT_LIGHT_DIFF"); if (df) diff = atof(df);
        L[0]=dx; L[1]=dy; L[2]=dz; L[3]=(float)atoi(m); L[4]=amb; L[5]=diff;
      }
      const char* fg = getenv("PSXPORT_FOG");
      if (fg && atoi(fg)) { float n=200, f=2000, fr=0,fgc=0,fb=0;
        const char* nn=getenv("PSXPORT_FOG_NEAR"); if(nn) n=atof(nn);
        const char* ff=getenv("PSXPORT_FOG_FAR");  if(ff) f=atof(ff);
        const char* rg=getenv("PSXPORT_FOG_RGB");  if(rg) sscanf(rg,"%f,%f,%f",&fr,&fgc,&fb);
        L[6]=n; L[7]=f; L[8]=fr; L[9]=fgc; L[10]=fb; L[11]=1;
      }
      gpu_vk_set_light(L);
    } }
  { void pgxp_frame_reset(void); pgxp_frame_reset(); }   // drop this frame's PGXP subpixel cache
  s_frame++; s_prims = 0; s_gp0_words = 0; s_dma2 = 0;
}
void gpu_present(void) { gpu_present_ex(1); }

void gpu_native_init(void) {
  if (getenv("PSXPORT_GPU_LOG")) g_log = 1;
  if (getenv("PSXPORT_REDDBG")) s_reddbg = 1;
  const char* cw = getenv("PSXPORT_CLUTWATCH");
  if (cw) { s_cw_x = 880; s_cw_y = 507; int x, y; if (sscanf(cw, "%d,%d", &x, &y) == 2) { s_cw_x = x; s_cw_y = y; } }
}

// Read-only VRAM inspection accessor (raw 16-bit 555+mask word). Used by the offline GPU-QA
// harness to assert exact rasterized/blended values; harmless in production (read-only).
uint16_t gpu_vram_peek(int x, int y) { return *vram(x, y); }

// Primitives drawn since the last gpu_present() (reset each present). The native frame loop uses
// this to avoid flipping the double buffer to a buffer it didn't draw this frame (menu-load flicker).
int gpu_prims_since_present(void) { return (int)s_prims; }

// Bulk VRAM load/save (1024x512x16). Used by the offline GP0 differ harness (tools/gpu_differ):
// seed s_vram with a captured initial VRAM, replay a GP0 word stream via gpu_gp0(), then read back
// the rasterized result for a pixel-exact compare against Beetle on the identical input.
void gpu_vram_load(const uint16_t* src) { memcpy(s_vram, src, sizeof(s_vram)); }
void gpu_vram_save(uint16_t* dst)       { memcpy(dst, s_vram, sizeof(s_vram)); }

// Provenance query at an ABSOLUTE VRAM coord (the differ replays into the back buffer at
// off=(0,256), so query e.g. vram y = display y + 256 — no double-buffer confound, unlike the
// live-run PROVAT). Requires PSXPORT_PROVAT to be set so put_px_b stamped s_prov during replay.
void gpu_prov_dump(int vx, int vy) {
  uint16_t p = *vram(vx, vy);
  uint32_t gid = s_prov[(vy & 511) * VRAM_W + (vx & 1023)];
  ProvMeta* m = &s_provmeta[gid % PROVRING];
  fprintf(stderr, "[prov] vram(%d,%d)=%04X rgb(%d,%d,%d) ", vx, vy, p,
          (p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3);
  if (!gid) { fprintf(stderr, "<never written>\n"); return; }
  if (m->gid != gid) { fprintf(stderr, "gid=%u <evicted>\n", gid); return; }
  fprintf(stderr, "gid=%u op=%02X tex=%d texmode=%d semi=%d blend=%d clut=(%d,%d) tp=(%d,%d) "
          "primcol=(%d,%d,%d) v0=(%d,%d) uv0=(%d,%d)\n",
          gid, m->op, m->tex, m->mode, m->semi, m->blend, m->clut_x, m->clut_y, m->tp_x, m->tp_y,
          m->r, m->g, m->b, m->x0, m->y0, m->u0, m->v0);
}

// --- Native scene accounting (graphics OWNERSHIP, later-99) -------------------------------------
// A read-only walk of the SAME ordering table DrawOTag DMAs, classifying every primitive into the
// engine-meaningful categories — so the port can ACCOUNT for each draw instead of blindly rasterizing
// the GP0 byte stream. Especially: VRAM->VRAM copies (MoveImage = reflection/fade buffers), VRAM fills,
// and full-screen flat/semi overlays (the fade tiles) + draw-area/offset env state. PSXPORT_SCENEDUMP=N.
static int gp0_cmd_len(uint8_t op, uint32_t c0) {
  if (op >= 0x20 && op <= 0x3F) {                 // polygon
    int nv = (op & 0x08) ? 4 : 3, per = 1 + ((op & 0x04) ? 1 : 0) + ((op & 0x10) ? 1 : 0);
    return 1 + nv * per - ((op & 0x10) ? 1 : 0);  // gouraud reuses cmd word for v0 colour
  }
  if (op >= 0x40 && op <= 0x5F) return 0;          // line / poly-line: variable (terminated) — skip len
  if (op >= 0x60 && op <= 0x7F) {                  // rect/sprite
    int t = (op & 0x04) ? 1 : 0, sz = (op >> 3) & 3;
    return 1 + 1 + t + (sz == 0 ? 1 : 0);          // cmd + xy + (uv) + (wh if size 0)
  }
  if (op == 0x02) return 3;                        // fill
  if (op >= 0x80 && op <= 0x9F) return 4;          // VRAM->VRAM copy
  if (op >= 0xA0 && op <= 0xBF) return 3;          // CPU->VRAM (header; data follows separately)
  if (op >= 0xC0 && op <= 0xDF) return 3;          // VRAM->CPU
  if (op >= 0xE1 && op <= 0xE6) return 1;          // env
  return 1;
}
static void gpu_scene_dump(uint32_t madr) {
  uint32_t addr = madr & 0x1FFFFC;
  int ax0 = 0, ay0 = 0, ax1 = 1023, ay1 = 511, ox = 0, oy = 0;     // tracked draw-area + offset (env)
  int npoly = 0, nrect = 0, nline = 0, nfill = 0, ncopy = 0, nup = 0, nenv = 0;
  fprintf(stderr, "[scene] f%d OT@0x%08X — classified display list:\n", s_frame, 0x80000000u | addr);
  for (int g = 0; g < 0x10000; g++) {
    uint32_t hdr = mem_r32(addr); int n = hdr >> 24;
    int i = 0;
    while (i < n) {                                  // a node may pack several GP0 commands (DR_* env)
      uint32_t c = mem_r32(addr + 4 + i * 4); uint8_t op = c >> 24;
      int len = gp0_cmd_len(op, c); if (len <= 0) { break; }   // variable (lines) — stop scanning node
      uint32_t w1 = (i + 1 < n) ? mem_r32(addr + 4 + (i + 1) * 4) : 0;
      uint32_t w2 = (i + 2 < n) ? mem_r32(addr + 4 + (i + 2) * 4) : 0;
      if (op == 0xE3) { ax0 = c & 0x3FF; ay0 = (c >> 10) & 0x1FF; nenv++; }
      else if (op == 0xE4) { ax1 = c & 0x3FF; ay1 = (c >> 10) & 0x1FF; nenv++; }
      else if (op == 0xE5) { ox = ((int)(c & 0x7FF) << 21) >> 21; oy = ((int)((c >> 11) & 0x7FF) << 21) >> 21; nenv++; }
      else if (op >= 0xE1 && op <= 0xE6) nenv++;
      else if (op == 0x02) { nfill++;
        fprintf(stderr, "  FILL  rgb=(%d,%d,%d) at(%d,%d) %dx%d\n", c&0xFF,(c>>8)&0xFF,(c>>16)&0xFF,
                w1&0x3FF,(w1>>16)&0x1FF, w2&0x3FF,(w2>>16)&0x1FF); }
      else if (op >= 0x80 && op <= 0x9F) { ncopy++;
        uint32_t w3 = (i+3<n)?mem_r32(addr+4+(i+3)*4):0;
        fprintf(stderr, "  COPY  VRAM->VRAM src(%d,%d) -> dst(%d,%d) %dx%d  [reflection/fade-buffer]\n",
                w1&0x3FF,(w1>>16)&0x1FF, w2&0x3FF,(w2>>16)&0x1FF, w3&0x3FF,(w3>>16)&0x1FF); }
      else if (op >= 0xA0 && op <= 0xBF) nup++;
      else if (op >= 0x20 && op <= 0x3F) { npoly++;
        int semi = (op>>1)&1, tex=(op>>2)&1;
        // poly v0 xy = word after cmd (mono) — flag big/semi flat overlays (fade candidates)
        int vx = (int)(w1 & 0x7FF); if (vx>=0x400) vx-=0x800; int vy=(int)((w1>>16)&0x7FF); if(vy>=0x400) vy-=0x800;
        if (semi && !tex) fprintf(stderr, "  POLY  semi flat rgb=(%d,%d,%d) v0=(%d,%d) off=(%d,%d) clip[%d,%d-%d,%d]  [fade/overlay?]\n",
                c&0xFF,(c>>8)&0xFF,(c>>16)&0xFF, vx,vy, ox,oy, ax0,ay0,ax1,ay1); }
      else if (op >= 0x60 && op <= 0x7F) { nrect++;
        int semi=(op>>1)&1, w=(((op>>3)&3)==0)?(int)(w2&0x3FF):(((op>>3)&3)==1?1:(((op>>3)&3)==2?8:16));
        int rx=(int)(w1&0x7FF); if(rx>=0x400) rx-=0x800; int ry=(int)((w1>>16)&0x7FF); if(ry>=0x400) ry-=0x800;
        if (w >= 256) fprintf(stderr, "  RECT  %s rgb=(%d,%d,%d) at(%d,%d) w~%d off=(%d,%d)  [large/overlay?]\n",
                semi?"semi":"opaque", c&0xFF,(c>>8)&0xFF,(c>>16)&0xFF, rx,ry, w, ox,oy); }
      else if (op >= 0x40 && op <= 0x5F) { nline++; break; }
      i += len;
    }
    uint32_t next = hdr & 0xFFFFFF;
    if (next == 0xFFFFFF || next == 0) break;
    addr = next & 0x1FFFFC;
  }
  fprintf(stderr, "[scene] f%d totals: poly=%d rect=%d line=%d fill=%d vramcopy=%d upload=%d env=%d\n",
          s_frame, npoly, nrect, nline, nfill, ncopy, nup, nenv);
}

// DMA channel 2 (GPU): walk an ordering-table linked list from `madr`, feeding each node's
// GP0 words to the parser. Header word: bits[24..31]=word count, bits[0..23]=next node addr
// (0xFFFFFF = end).
void gpu_dma2_linked_list(uint32_t madr) {
  { static int sd = -2; if (sd == -2) { const char* e = getenv("PSXPORT_SCENEDUMP"); sd = e ? atoi(e) : -1; }
    if (sd >= 0 && s_frame == sd) gpu_scene_dump(madr); }
  s_dma2++;
  g_ot_madr = madr & 0x1FFFFC;
  uint32_t addr = madr & 0x1FFFFC;
  // PSXPORT_OTDBG: on a chain that fails to terminate within an OT's worth of nodes (cyclic =
  // malformed), dump its first 40 nodes once for diagnosis. (Empty OTs are ~0x800 link-only nodes
  // that DO terminate at the sentinel; a true cycle never terminates.)
  if (getenv("PSXPORT_OTDBG")) {
    static int dumped = 0;
    uint32_t a = madr & 0x1FFFFC; int term = 0;
    for (int k = 0; k < 4096; k++) {
      uint32_t next = mem_r32(a) & 0xFFFFFF;
      if (next == 0xFFFFFF || next == 0) { term = 1; break; }
      a = next & 0x1FFFFC;
    }
    if (!term && !dumped++) {
      a = madr & 0x1FFFFC;
      fprintf(stderr, "[otdbg] MALFORMED OT from madr=0x%08X:\n", 0x80000000u | (madr & 0x1FFFFC));
      for (int k = 0; k < 40; k++) {
        uint32_t hdr = mem_r32(a); uint32_t next = hdr & 0xFFFFFF; int n = hdr >> 24;
        fprintf(stderr, "  [%2d] @0x%08X hdr=0x%08X (n=%d) -> 0x%08X\n",
                k, 0x80000000u | a, hdr, n, 0x80000000u | (next & 0x1FFFFC));
        if (next == 0xFFFFFF || next == 0) break;
        a = next & 0x1FFFFC;
      }
    }
  }
  // A valid ordering table is bounded (its entry count + linked primitives). A far larger node
  // count means a malformed/cyclic next-pointer chain — cap it (so it can't wedge) and log the
  // first offending case for diagnosis instead of spinning to the old 1M guard.
  int guard;
  for (guard = 0; guard < 0x10000; guard++) {
    uint32_t hdr = mem_r32(addr);
    int n = hdr >> 24;
    s_cur_node = 0x80000000u | addr;
    for (int i = 0; i < n; i++) gpu_gp0(mem_r32(addr + 4 + i * 4));
    uint32_t next = hdr & 0xFFFFFF;
    if (next == 0xFFFFFF || next == 0) break;
    addr = next & 0x1FFFFC;
  }
  if (guard >= 0x10000) {
    static int warned = 0;
    if (!warned++) fprintf(stderr, "[gpu] WARN: OT traversal hit %d-node cap from madr=0x%08X "
                           "(last addr=0x%08X) — malformed/cyclic ordering table\n",
                           guard, madr, addr);
  }
}
// DMA channel 2 block mode: `count` words from `madr` (to/from GP0). to_gpu=1 -> GP0 writes.
uint32_t g_dma_src;   // last block-DMA source (UPLOADLOG: which RAM fed a CPU->VRAM upload)
void gpu_dma2_block(uint32_t madr, int count, int to_gpu) {
  s_dma2++;
  uint32_t addr = madr & 0x1FFFFC;
  g_dma_src = addr;
  for (int i = 0; i < count; i++) { if (to_gpu) gpu_gp0(mem_r32(addr)); addr += 4; }
}
