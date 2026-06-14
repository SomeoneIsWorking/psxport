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
static uint16_t s_vram[VRAM_W * VRAM_H];
static inline uint16_t* vram(int x, int y) { return &s_vram[(y & 511) * VRAM_W + (x & 1023)]; }

// ---- Draw state (set by GP0 env commands E1..E6) ------------------------------------
static int s_da_x0, s_da_y0, s_da_x1 = 1023, s_da_y1 = 511;  // draw clip area
static int s_off_x, s_off_y;                                 // draw offset
static int s_tp_x, s_tp_y;        // texpage base (64-px / 256-line units -> *64 / *256)
static int s_tp_mode;             // texture color mode: 0=4bpp,1=8bpp,2=15bpp
static int s_tp_blend;            // semi-transparency mode 0..3
static int s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy;  // texture window mask/offset (8px units)
static int s_clut_x, s_clut_y;    // CLUT base (per-primitive, from packet)

// ---- Display control (GP1) ----------------------------------------------------------
static int s_disp_x, s_disp_y;    // VRAM top-left of the displayed region
static int s_disp_w = 320, s_disp_h = 240;

typedef struct { int x, y; uint8_t r, g, b; int u, v; } Vtx;

static int g_log = 0;             // PSXPORT_GPU_LOG
static long s_prims = 0;          // primitives drawn since last present
static long s_gp0_words = 0, s_dma2 = 0;  // diagnostics: GP0 words + DMA2 triggers per frame

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint16_t to555(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
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

static inline void put_px(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (x < s_da_x0 || x > s_da_x1 || y < s_da_y0 || y > s_da_y1) return;
  *vram(x, y) = to555(r, g, b) | 0x8000;  // mask bit set (we don't model mask-test reads)
}

// Rasterize a gouraud/textured triangle (barycentric). `tex` selects textured sampling.
static void tri(Vtx a, Vtx b, Vtx c, int tex, int shade) {
  a.x += s_off_x; a.y += s_off_y; b.x += s_off_x; b.y += s_off_y; c.x += s_off_x; c.y += s_off_y;
  int minx = clampi((a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x)), s_da_x0, s_da_x1);
  int maxx = clampi((a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x)), s_da_x0, s_da_x1);
  int miny = clampi((a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y)), s_da_y0, s_da_y1);
  int maxy = clampi((a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y)), s_da_y0, s_da_y1);
  int area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (area == 0) return;
  for (int y = miny; y <= maxy; y++)
    for (int x = minx; x <= maxx; x++) {
      int w0 = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
      int w1 = (c.x - b.x) * (y - b.y) - (c.y - b.y) * (x - b.x);
      int w2 = (a.x - c.x) * (y - c.y) - (a.y - c.y) * (x - c.x);
      if (area > 0 ? (w0 < 0 || w1 < 0 || w2 < 0) : (w0 > 0 || w1 > 0 || w2 > 0)) continue;
      // barycentric (l1->b weight etc.); use w2,w0,w1 for a,b? recompute clean weights:
      long aa = (long)((b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)); if (aa==0) continue;
      long l0 = (long)((b.x-x)*(c.y-y)-(b.y-y)*(c.x-x));
      long l1 = (long)((c.x-x)*(a.y-y)-(c.y-y)*(a.x-x));
      long l2 = aa - l0 - l1;
      uint8_t r, g, bl;
      if (tex) {
        int u = (int)((l0*a.u + l1*b.u + l2*c.u) / aa);
        int v = (int)((l0*a.v + l1*b.v + l2*c.v) / aa);
        uint16_t t = sample_tex(u, v);
        if (t == 0) continue;                       // transparent texel
        r = (t & 31) << 3; g = ((t >> 5) & 31) << 3; bl = ((t >> 10) & 31) << 3;
        if (shade) { r = r * a.r / 128; g = g * a.g / 128; bl = bl * a.b / 128; }  // texture*color
      } else if (shade) {
        r = (uint8_t)((l0*a.r + l1*b.r + l2*c.r) / aa);
        g = (uint8_t)((l0*a.g + l1*b.g + l2*c.g) / aa);
        bl = (uint8_t)((l0*a.b + l1*b.b + l2*c.b) / aa);
      } else { r = a.r; g = a.g; bl = a.b; }
      put_px(x, y, r, g, bl);
    }
}

// ---- GP0 command FIFO ---------------------------------------------------------------
static uint32_t s_fifo[16];
static int s_fcount, s_fneed;
// VRAM transfer state (GP0 0xA0 CPU->VRAM)
static int s_xfer, s_xfer_x, s_xfer_y, s_xfer_w, s_xfer_h, s_xfer_px;

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
}
static void set_clut(uint16_t cl) { s_clut_x = (cl & 0x3F) * 16; s_clut_y = (cl >> 6) & 0x1FF; }

// Execute a complete GP0 primitive packet held in s_fifo[0..s_fcount).
static void gp0_exec(void) {
  uint32_t c = s_fifo[0];
  uint8_t op = c >> 24;
  if (op >= 0x20 && op <= 0x3F) {            // polygon
    int gouraud = op & 0x10, quad = op & 0x08, textured = op & 0x04, semi = op & 0x02;
    (void)semi;
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
    tri(v[0], v[1], v[2], textured, shade);
    if (quad) tri(v[1], v[2], v[3], textured, shade);
    s_prims++;
  } else if (op >= 0x60 && op <= 0x7F) {     // rectangle / sprite
    int textured = op & 0x04, size = (op >> 3) & 3;
    uint8_t cr = cmd_r(c), cg = cmd_g(c), cb = cmd_b(c);
    int idx = 1;
    uint32_t xy = s_fifo[idx++]; int x = cx(xy), y = cy(xy);
    int u0 = 0, v0 = 0;
    if (textured) { uint32_t uv = s_fifo[idx++]; u0 = uv & 0xFF; v0 = (uv >> 8) & 0xFF; set_clut((uv >> 16) & 0xFFFF); }
    int w, h;
    if (size == 0) { uint32_t wh = s_fifo[idx++]; w = wh & 0x3FF; h = (wh >> 16) & 0x1FF; }
    else { w = h = (size == 1) ? 1 : (size == 2) ? 8 : 16; }
    for (int dy = 0; dy < h; dy++)
      for (int dx = 0; dx < w; dx++) {
        if (textured) {
          uint16_t t = sample_tex(u0 + dx, v0 + dy);
          if (t == 0) continue;
          put_px(x + dx + s_off_x, y + dy + s_off_y, (t & 31) << 3, ((t >> 5) & 31) << 3, ((t >> 10) & 31) << 3);
        } else put_px(x + dx + s_off_x, y + dy + s_off_y, cr, cg, cb);
      }
    s_prims++;
  } else if (op == 0x02) {                   // fill rectangle (in VRAM, ignores clip/offset)
    uint8_t cr = cmd_r(c), cg = cmd_g(c), cb = cmd_b(c);
    uint32_t xy = s_fifo[1], wh = s_fifo[2];
    int x = xy & 0x3F0, y = (xy >> 16) & 0x1FF, w = ((wh & 0x3FF) + 0xF) & ~0xF, h = (wh >> 16) & 0x1FF;
    uint16_t col = to555(cr, cg, cb);
    for (int dy = 0; dy < h; dy++) for (int dx = 0; dx < w; dx++) *vram(x + dx, y + dy) = col;
  } else if (op >= 0x40 && op <= 0x5F) {     // line (flat/gouraud) — draw as thin segments
    // minimal: endpoints only (poly-lines rare in this title); draw a 1px Bresenham
    uint8_t cr = cmd_r(c), cg = cmd_g(c), cb = cmd_b(c);
    uint32_t a = s_fifo[1], b = s_fifo[(op & 0x10) ? 3 : 2];
    int x0 = cx(a), y0 = cy(a), x1 = cx(b), y1 = cy(b);
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) { put_px(x0 + s_off_x, y0 + s_off_y, cr, cg, cb); if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
    s_prims++;
  }
  // env commands (E1..E6) handled in gpu_gp0 directly (single-word).
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
  if (op == 0xA0 || op == 0xC0 || op == 0x80) return 3;  // VRAM xfer headers
  return 1;                                      // env / nop / single-word
}

// One word into the GP0 port (direct write or DMA).
void gpu_gp0(uint32_t w) {
  s_gp0_words++;
  if (s_xfer) {                                  // CPU->VRAM pixel stream (2 px/word)
    for (int k = 0; k < 2; k++) {
      int px = s_xfer_px % s_xfer_w, py = s_xfer_px / s_xfer_w;
      if (py < s_xfer_h) *vram(s_xfer_x + px, s_xfer_y + py) = (k ? (w >> 16) : w) & 0xFFFF;
      s_xfer_px++;
    }
    if (s_xfer_px >= s_xfer_w * s_xfer_h) s_xfer = 0;
    return;
  }
  if (s_fcount == 0) {
    uint8_t op = w >> 24;
    switch (op) {                                // single-word env / state commands
      case 0x00: return;                         // nop
      case 0x01: return;                         // clear cache
      case 0xE1: set_texpage(w & 0xFFFF); return;
      case 0xE2: s_tw_mx = w & 31; s_tw_my = (w >> 5) & 31; s_tw_ox = (w >> 10) & 31; s_tw_oy = (w >> 15) & 31; return;
      case 0xE3: s_da_x0 = w & 0x3FF; s_da_y0 = (w >> 10) & 0x1FF; return;
      case 0xE4: s_da_x1 = w & 0x3FF; s_da_y1 = (w >> 10) & 0x1FF; return;
      case 0xE5: s_off_x = ((int)(w & 0x7FF) << 21) >> 21; s_off_y = ((int)((w >> 11) & 0x7FF) << 21) >> 21; return;
      case 0xE6: return;                         // mask settings (mask-test not modeled)
      default: break;
    }
    s_fneed = gp0_len(w);
  }
  s_fifo[s_fcount++] = w;
  if (s_fcount >= s_fneed) {
    uint8_t op = s_fifo[0] >> 24;
    if (op == 0xA0) {                            // CPU->VRAM: set up the pixel stream
      s_xfer_x = s_fifo[1] & 0x3FF; s_xfer_y = (s_fifo[1] >> 16) & 0x1FF;
      s_xfer_w = ((s_fifo[2] & 0x3FF) ? (s_fifo[2] & 0x3FF) : 1024);
      s_xfer_h = (((s_fifo[2] >> 16) & 0x1FF) ? ((s_fifo[2] >> 16) & 0x1FF) : 512);
      s_xfer_px = 0; s_xfer = 1;
    } else if (op == 0x80) {                     // VRAM->VRAM copy
      int sx = s_fifo[1] & 0x3FF, sy = (s_fifo[1] >> 16) & 0x1FF;
      int dx = s_fifo[2] & 0x3FF, dy = (s_fifo[2] >> 16) & 0x1FF;
      int w2 = s_fifo[3] & 0x3FF, h2 = (s_fifo[3] >> 16) & 0x1FF;
      for (int y = 0; y < h2; y++) for (int x = 0; x < w2; x++) *vram(dx + x, dy + y) = *vram(sx + x, sy + y);
    } else if (op != 0xC0) {
      gp0_exec();
    }
    s_fcount = 0; s_fneed = 0;
  }
}

// GP1 display/control commands.
void gpu_gp1(uint32_t w) {
  uint8_t op = w >> 24;
  switch (op) {
    case 0x05: s_disp_x = w & 0x3FF; s_disp_y = (w >> 10) & 0x1FF; break;          // display area start
    case 0x07: { int y0 = w & 0x3FF, y1 = (w >> 10) & 0x3FF; s_disp_h = (y1 - y0) ? (y1 - y0) : 240; break; }
    case 0x08: s_disp_w = ((w & 3) == 0) ? 256 : ((w & 3) == 1) ? 320 : ((w & 3) == 2) ? 512 : 640; break;
    default: break;
  }
}

// Optional live window (PSXPORT_GPU_WINDOW=1). Headless builds without SDL just no-op.
#ifdef PSXPORT_SDL
static SDL_Window* s_win; static SDL_Renderer* s_ren; static SDL_Texture* s_tex;
static int s_tex_w, s_tex_h, s_win_on = -1;
static void present_window(void) {
  if (s_win_on < 0) s_win_on = getenv("PSXPORT_GPU_WINDOW") ? 1 : 0;
  if (!s_win_on) return;
  if (!s_win) {
    SDL_Init(SDL_INIT_VIDEO);
    s_win = SDL_CreateWindow("Tomba! 2 (native PC port)", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, s_disp_w * 3, s_disp_h * 3, SDL_WINDOW_RESIZABLE);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
  }
  if (!s_tex || s_tex_w != s_disp_w || s_tex_h != s_disp_h) {
    if (s_tex) SDL_DestroyTexture(s_tex);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                              s_disp_w, s_disp_h);
    s_tex_w = s_disp_w; s_tex_h = s_disp_h;
  }
  static uint32_t buf[VRAM_W * VRAM_H];
  for (int y = 0; y < s_disp_h; y++)
    for (int x = 0; x < s_disp_w; x++) {
      uint16_t p = *vram(s_disp_x + x, s_disp_y + y);
      buf[y * s_disp_w + x] = 0xFF000000u | (((p >> 10) & 31) << 19) | (((p >> 5) & 31) << 11) | ((p & 31) << 3);
    }
  SDL_UpdateTexture(s_tex, NULL, buf, s_disp_w * 4);
  SDL_RenderClear(s_ren); SDL_RenderCopy(s_ren, s_tex, NULL, NULL); SDL_RenderPresent(s_ren);
  SDL_Event e; while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) exit(0);
}
#else
static void present_window(void) {}
#endif

// Present: copy the displayed VRAM region to an RGB buffer. PSXPORT_GPU_DUMP=dir dumps PPMs;
// PSXPORT_GPU_WINDOW=1 shows a live SDL window.
static int s_frame = 0;
void gpu_present(void) {
  present_window();
  const char* dir = getenv("PSXPORT_GPU_DUMP");
  if (g_log) fprintf(stderr, "[gpu] frame %d: %ld prims, %ld gp0words, %ld dma2, disp %dx%d @ (%d,%d)\n",
                     s_frame, s_prims, s_gp0_words, s_dma2, s_disp_w, s_disp_h, s_disp_x, s_disp_y);
  if (dir) {
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
  s_frame++; s_prims = 0; s_gp0_words = 0; s_dma2 = 0;
}

void gpu_native_init(void) { if (getenv("PSXPORT_GPU_LOG")) g_log = 1; }

// DMA channel 2 (GPU): walk an ordering-table linked list from `madr`, feeding each node's
// GP0 words to the parser. Header word: bits[24..31]=word count, bits[0..23]=next node addr
// (0xFFFFFF = end).
void gpu_dma2_linked_list(uint32_t madr) {
  s_dma2++;
  uint32_t addr = madr & 0x1FFFFC;
  for (int guard = 0; guard < 0x100000; guard++) {
    uint32_t hdr = mem_r32(addr);
    int n = hdr >> 24;
    for (int i = 0; i < n; i++) gpu_gp0(mem_r32(addr + 4 + i * 4));
    uint32_t next = hdr & 0xFFFFFF;
    if (next == 0xFFFFFF || next == 0) break;
    addr = next & 0x1FFFFC;
  }
}
// DMA channel 2 block mode: `count` words from `madr` (to/from GP0). to_gpu=1 -> GP0 writes.
void gpu_dma2_block(uint32_t madr, int count, int to_gpu) {
  s_dma2++;
  uint32_t addr = madr & 0x1FFFFC;
  for (int i = 0; i < count; i++) { if (to_gpu) gpu_gp0(mem_r32(addr)); addr += 4; }
}
