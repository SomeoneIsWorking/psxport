// gpu_native_raster.cpp — the SOFTWARE rasterizer: guest triangles, sprites and lines into VRAM.
//
// Split out of gpu_native.cpp. The concept is the software raster path and nothing else: it samples
// texels through the current texpage and CLUT, blends semi-transparent fragments, applies the ordered
// dither, and walks a triangle's edges in integer fixed point. It is the only path that writes pixels
// on the host side, and it is the reason a guest frame is visible at all when no GPU backend owns the
// picture.
//
// The methods stay members of GpuState — they read and write the per-instance draw state and VRAM, and
// keeping them there is what lets two Cores rasterize independently. What moved is the FILE, so the
// GP0 command parsing that drives it and the rasterizer that consumes it are two readable things
// rather than one unreadable one.
//
// These bodies are byte-identical to the ones they replace. Every bit-exactness note in them (the
// half-LSB barycentric bias, the scanline edge-walk's failure to round, the dither table) is a
// deliberate match to a measured reference and must survive a move unchanged.
#include "c_subsys.h"
#include "cfg.h" // cfg_str — the diagnostics in this file read their armed frames from it
#include "core.h"
#include "gpu_native_internal.h"
#include "vram_pixel.h"

namespace {
// The three blend modes the PSX texpage can select, and the per-channel formula each applies to the
// source over the destination in 5-bit space:
//
//   B/2 + F/2   average            B + F    additive        B - F   subtractive        B + F/4  additive/4
//
// All four saturate to [0,31]. `mode` is the texpage's blend field (s_tp_blend), which a primitive
// carries inline, so this is a per-primitive decision rather than a per-frame one.
constexpr int kBlendAverage = 0;
constexpr int kBlendAdditive = 1;
constexpr int kBlendSubtractive = 2;
// The mode value for "B + F/4". It is what any other value falls through to, and the field is two bits
// wide so no other value is reachable — naming it says so rather than leaving a bare `default:`.
constexpr int kBlendAdditiveQuarter = 3;
// The low 15 bits of a VRAM halfword: the colour, with the draw-mask bit stripped. A blend reads the
// destination's colour and must not blend the mask bit into a channel.
constexpr std::uint16_t kVramColourMask = 0x7FFFu;
// The five-bit saturation clamp every blend result passes through. A subtractive blend of a bright
// source over a dark destination goes NEGATIVE, and an unclamped result would wrap through the colour
// fields into the neighbouring channel.
constexpr int kChannelMax = 31;
constexpr int sat5(int v) {
  return v < 0 ? 0 : v > kChannelMax ? kChannelMax : v;
}

// Blend `source` (already 5-bit per channel) over the destination halfword in one of the four modes.
// `mode` is the texpage blend field; see the table above.
std::uint16_t blend555(std::uint16_t bg, int fr, int fg, int fb, int mode) {
  const int br = bg & 0x1F, bgn = (bg >> 5) & 0x1F, bb = (bg >> 10) & 0x1F;
  int rr, rg, rb;
  switch (mode) {
  case kBlendAverage:
    rr = (br + fr) >> 1;
    rg = (bgn + fg) >> 1;
    rb = (bb + fb) >> 1;
    break;
  case kBlendAdditive:
    rr = sat5(br + fr);
    rg = sat5(bgn + fg);
    rb = sat5(bb + fb);
    break;
  case kBlendSubtractive:
    rr = sat5(br - fr);
    rg = sat5(bgn - fg);
    rb = sat5(bb - fb);
    break;
  case kBlendAdditiveQuarter:
    rr = sat5(br + (fr >> 2));
    rg = sat5(bgn + (fg >> 2));
    rb = sat5(bb + (fb >> 2));
    break;
  default:
    rr = sat5(br + (fr >> 2));
    rg = sat5(bgn + (fg >> 2));
    rb = sat5(bb + (fb >> 2));
    break;
  }
  return static_cast<std::uint16_t>(rr | (rg << 5) | (rb << 10));
}
} // namespace

// The one explicit texture/CLUT sampler. The shipping rasterizer supplies its current state; queue
// diagnostics supply the state captured on an RqItem, so both answers use identical wrap/index rules.
GpuTextureSample GpuState::sample_tex_at(
    int u, int v, int tp_x, int tp_y, int mode, int clut_x, int clut_y, int tw_mx, int tw_my, int tw_ox, int tw_oy) {
  GpuTextureSample sample;
  sample.u = (u & ~(tw_mx * 8)) | ((tw_ox & tw_mx) * 8);
  sample.v = (v & ~(tw_my * 8)) | ((tw_oy & tw_my) * 8);
  if (mode == 2) {
    sample.source_word = *vram(tp_x + sample.u, tp_y + sample.v);
    sample.texel = sample.source_word;
    return sample;
  }
  if (mode == 1) {
    sample.source_word = *vram(tp_x + (sample.u >> 1), tp_y + sample.v);
    sample.palette_index = (sample.u & 1) ? (sample.source_word >> 8) : (sample.source_word & 0xFF);
  } else {
    sample.source_word = *vram(tp_x + (sample.u >> 2), tp_y + sample.v);
    sample.palette_index = (sample.source_word >> ((sample.u & 3) * 4)) & 0xF;
  }
  sample.texel = *vram(clut_x + sample.palette_index, clut_y);
  return sample;
}

// Sample through the current draw state. A zero texel is transparent on the PSX.
uint16_t GpuState::sample_tex(int u, int v) {
  return sample_tex_at(u, v, s_tp_x, s_tp_y, s_tp_mode, s_clut_x, s_clut_y, s_tw_mx, s_tw_my, s_tw_ox, s_tw_oy).texel;
}

// Write one pixel. If `semi` is set, blend the source (r,g,b) over the existing VRAM pixel
// using the current texpage blend mode (s_tp_blend); otherwise overwrite. The mask bit is
// always set on the written pixel (we don't model mask-test reads).
void GpuState::put_px_b(int x, int y, uint8_t r, uint8_t g, uint8_t b, int semi) {
  if (x < s_da_x0 || x > s_da_x1 || y < s_da_y0 || y > s_da_y1) {
    return;
  }
  const uint16_t before = *fb(x, y);
  uint16_t out;
  if (semi) {
    out = blend555(before & kVramColourMask, r >> 3, g >> 3, b >> 3, s_tp_blend);
  } else {
    out = psx::gpu::toVram555(r, g, b);
  }
  if (lucent::channel_on("provchain")) {
    if (!s_provenance_chain_probe.configured) {
      s_provenance_chain_probe.configured = true;
      if (const char *setting = cfg_str("PSXPORT_PROVCHAIN")) {
        sscanf(setting,
               "%d,%d,%d",
               &s_provenance_chain_probe.x,
               &s_provenance_chain_probe.y,
               &s_provenance_chain_probe.from_frame);
      }
    }
    if (s_frame >= s_provenance_chain_probe.from_frame && x == s_provenance_chain_probe.x &&
        y == s_provenance_chain_probe.y) {
      const ProvMeta &meta = s_provmeta[s_prim_gid % PROVRING];
      lucent::debug("provchain",
                    "f{} ({},{}) gid={} node={:08X} op={:02X} semi={} blend={} rgb=({},{},{}) "
                    "before={:04X} after={:04X}",
                    s_frame,
                    x,
                    y,
                    s_prim_gid,
                    meta.node,
                    meta.op,
                    semi,
                    s_tp_blend,
                    r,
                    g,
                    b,
                    before,
                    out | 0x8000u);
    }
  }
  *fb(x, y) = out | 0x8000;
  if (s_prov_on > 0) {
    s_prov[(y & 511) * VRAM_W + (x & 1023)] = s_prim_gid;
  }
}
void GpuState::put_px(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  put_px_b(x, y, r, g, b, 0);
}

// PSX ordered 4x4 dither matrix (applied to 8-bit channels before 5-bit truncation, when
// the texpage dither bit is set, on gouraud + texture-modulated pixels). We add the per-pixel
// bias then clamp to [0,255] so the subsequent >>3 truncation effectively rounds.
static const int s_dither4[4][4] = {
    {-4, 0, -3, 1},
    {2, -2, 3, -1},
    {-3, 1, -4, 0},
    {3, -1, 2, -2},
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
static inline int64_t MakePolyXFPStep(int dx, int dy) { // dy is always > 0 at our call sites
  int64_t dx_ex = (int64_t)dx << 32;
  if (dx_ex < 0) {
    dx_ex -= dy - 1;
  }
  if (dx_ex > 0) {
    dx_ex += dy - 1;
  }
  return dx_ex / dy;
}
static inline int GetPolyXFP_Int(int64_t xfp) {
  return (int)(xfp >> 32);
}

// Shade + write ONE covered pixel of triangle (a,b,c) at integer screen (x,y). Coverage is
// decided by the caller (tri()); this only does the per-pixel math, which stays barycentric
// off the ORIGINAL (unsorted) a,b,c and the doubled signed area `aa` — already validated to
// match Beetle's per-pixel output (modulation/UV-round/dither). `tex`/`shade`/`semi` as tri().
void GpuState::tri_px(Vtx a, Vtx b, Vtx c, int x, int y, int tex, int shade, int semi, int raw, long aa) {
  long l0 = (long)((b.x - x) * (c.y - y) - (b.y - y) * (c.x - x));
  long l1 = (long)((c.x - x) * (a.y - y) - (c.y - y) * (a.x - x));
  long l2 = aa - l0 - l1;
  uint8_t r, g, bl;
  int px_semi = semi; // whether THIS pixel blends
  int dithered = 0;   // PSX dithers gouraud + modulated-texture
  int pt_u = 0, pt_v = 0;
  uint16_t pt_t = 0;                         // PSXPORT_PIXTRACE capture
  int pt_cr = a.r, pt_cg = a.g, pt_cb = a.b; // interpolated modulation color (set below)
  if (tex) {
    // Affine UV, ROUND-TO-NEAREST (not truncate): PSX/Beetle add a +0.5-texel bias before the
    // integer truncation (gpu_polygon.c affine seed `+(1<<(COORD_FBS-1))`), i.e. sample the
    // nearest texel. Truncating instead biases sampling half a texel toward the origin, picking a
    // neighbouring texel at fractional coords (journal later-44 residual). Round in sign-
    // normalized form since `aa` (doubled area) may be negative.
    long su = l0 * a.u + l1 * b.u + l2 * c.u, sv = l0 * a.v + l1 * b.v + l2 * c.v, den = aa;
    if (den < 0) {
      su = -su;
      sv = -sv;
      den = -den;
    }
    int u = (int)((su + den / 2) / den);
    int v = (int)((sv + den / 2) / den);
    uint16_t t = sample_tex(u, v);
    pt_u = u;
    pt_v = v;
    pt_t = t;
    if (t == 0) {
      return; // transparent texel — skip this pixel
    }
    // PSX: a textured pixel blends only when its bit15 is set AND the prim semi bit is set.
    px_semi = semi && (t & 0x8000);
    r = (t & 31) << 3;
    g = ((t >> 5) & 31) << 3;
    bl = ((t >> 10) & 31) << 3;
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
      // ROUNDED, not truncated — beetle seeds its colour DDA with a half-LSB bias exactly as it
      // does for u/v above (gpu_polygon.c:945). Truncating here biased every modulated pixel
      // LOW; see bary_round().
      int cr = bary_round(l0, a.r, l1, b.r, l2, c.r, aa);
      int cg = bary_round(l0, a.g, l1, b.g, l2, c.g, aa);
      int cb = bary_round(l0, a.b, l1, b.b, l2, c.b, aa);
      pt_cr = cr;
      pt_cg = cg;
      pt_cb = cb;
      int rr = r * cr / 128, gg = g * cg / 128, bb = bl * cb / 128;
      r = rr > 255 ? 255 : rr;
      g = gg > 255 ? 255 : gg;
      bl = bb > 255 ? 255 : bb;
      dithered = 1;
    } else {
      pt_cr = pt_cg = pt_cb = 128;
    } // raw: undithered texel, modulation color = neutral
  } else if (shade) {
    // Untextured gouraud: same rounding rule as the modulated path above.
    r = (uint8_t)bary_round(l0, a.r, l1, b.r, l2, c.r, aa);
    g = (uint8_t)bary_round(l0, a.g, l1, b.g, l2, c.g, aa);
    bl = (uint8_t)bary_round(l0, a.b, l1, b.b, l2, c.b, aa);
    dithered = 1;
  } else {
    r = a.r;
    g = a.g;
    bl = a.b;
  }
  if (s_tp_dither && dithered) {
    r = dith(r, x, y);
    g = dith(g, x, y);
    bl = dith(bl, x, y);
  }
  // PSXPORT_PIXTRACE="vx,vy": dump every prim that writes this absolute VRAM pixel (post-offset),
  // with its sampled texel + interpolated color + modulated output — for per-pixel-math diffing
  // against Beetle's gpu_polygon.c (which carries the matching [pixtrace beetle] log).
  {
    static int tx = -2, ty;
    if (tx == -2) {
      const char *e = cfg_str("PSXPORT_PIXTRACE");
      if (e) {
        sscanf(e, "%d,%d", &tx, &ty);
      } else {
        tx = -1;
      }
    }
    if (tx >= 0 && x == tx && y == ty) {
      lucent::info("gpu_native",
                   "[pixtrace ours] ({},{}) tex={} shade={} semi={} px_semi={} blend={} dith={} uv=({},{}) "
                   "texel={:04X} out8=({},{},{}) out5=({},{},{}) vcol=({},{},{})",
                   x,
                   y,
                   tex,
                   shade,
                   semi,
                   px_semi,
                   s_tp_blend,
                   (s_tp_dither && dithered),
                   pt_u,
                   pt_v,
                   pt_t,
                   r,
                   g,
                   bl,
                   r >> 3,
                   g >> 3,
                   bl >> 3,
                   pt_cr,
                   pt_cg,
                   pt_cb);
    }
  }
  // REDDBG: dark-red output anomaly probe (grass blocks). Log the prim's params once.
  if (s_reddbg && tex && r >= 64 && g < 24 && bl < 24 && x >= s_da_x0 && x <= s_da_x1) {
    static int n = 0;
    if (n++ < 6) {
      int uu = (int)((l0 * a.u + l1 * b.u + l2 * c.u) / aa);
      int vv = (int)((l0 * a.v + l1 * b.v + l2 * c.v) / aa);
      lucent::info("reddbg",
                   "@({},{}) out=({},{},{}) tpmode={} clut=({},{}) tp=({},{}) uv=({},{})",
                   x,
                   y,
                   r,
                   g,
                   bl,
                   s_tp_mode,
                   s_clut_x,
                   s_clut_y,
                   s_tp_x,
                   s_tp_y,
                   uu,
                   vv);
      lucent::Line ln;
      ln.add("  palette[16]@({},{}):", s_clut_x, s_clut_y);
      for (int k = 0; k < 16; k++) {
        ln.add(" {:04X}", *vram(s_clut_x + k, s_clut_y));
      }
      ln.flush(lucent::Level::Info, "reddbg");
      ln.add("  texrow@({},{}) words:", s_tp_x + (uu >> 2), s_tp_y + vv);
      for (int k = 0; k < 8; k++) {
        ln.add(" {:04X}", *vram(s_tp_x + (uu >> 2) + k, s_tp_y + vv));
      }
      ln.flush(lucent::Level::Info, "reddbg");
    }
  }
  put_px_b(x, y, r, g, bl, px_semi);
}

// Rasterize a gouraud/textured triangle. `tex` selects textured sampling, `semi` requests
// semi-transparency. Coverage = mednafen's exact integer edge-walk (so it matches the oracle
// pixel-for-pixel); per-pixel shading = tri_px (barycentric off the original a,b,c).
void GpuState::tri(Vtx a, Vtx b, Vtx c, int tex, int shade, int semi, int raw) {
  a.x += s_off_x;
  a.y += s_off_y;
  b.x += s_off_x;
  b.y += s_off_y;
  c.x += s_off_x;
  c.y += s_off_y;
  long aa = (long)((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
  if (aa == 0) {
    return; // degenerate (zero area)
  }

  // --- Exact port of mednafen's DEFINE_DrawTriangle coverage (gpu_polygon.c). Operates on a
  // y-sorted copy of the vertices; shading (tri_px) still uses the original a,b,c order. ---
  int vx[3] = {a.x, b.x, c.x}, vy[3] = {a.y, b.y, c.y};
  unsigned cvtemp; // "core vertex" select (rasterisation order)
  if (vx[1] <= vx[0]) {
    cvtemp = (vx[2] <= vx[1]) ? (1u << 2) : (1u << 1);
  } else if (vx[2] < vx[0]) {
    cvtemp = (1u << 2);
  } else {
    cvtemp = (1u << 0);
  }
#define VSWAP(i, j)                                                                                                    \
  do {                                                                                                                 \
    int t;                                                                                                             \
    t = vx[i];                                                                                                         \
    vx[i] = vx[j];                                                                                                     \
    vx[j] = t;                                                                                                         \
    t = vy[i];                                                                                                         \
    vy[i] = vy[j];                                                                                                     \
    vy[j] = t;                                                                                                         \
  } while (0)
  if (vy[2] < vy[1]) {
    VSWAP(2, 1);
    cvtemp = ((cvtemp >> 1) & 0x2) | ((cvtemp << 1) & 0x4) | (cvtemp & 0x1);
  }
  if (vy[1] < vy[0]) {
    VSWAP(1, 0);
    cvtemp = ((cvtemp >> 1) & 0x1) | ((cvtemp << 1) & 0x2) | (cvtemp & 0x4);
  }
  if (vy[2] < vy[1]) {
    VSWAP(2, 1);
    cvtemp = ((cvtemp >> 1) & 0x2) | ((cvtemp << 1) & 0x4) | (cvtemp & 0x1);
  }
#undef VSWAP
  unsigned core_vertex = cvtemp >> 1;
  if (vy[0] == vy[2]) {
    return; // 0-height after sort
  }

  int64_t base_coord = MakePolyXFP(vx[0]);
  int64_t base_step = MakePolyXFPStep(vx[2] - vx[0], vy[2] - vy[0]);
  int64_t bound_coord_us, bound_coord_ls;
  int right_facing;
  if (vy[1] == vy[0]) {
    bound_coord_us = 0;
    right_facing = (vx[1] > vx[0]);
  } else {
    bound_coord_us = MakePolyXFPStep(vx[1] - vx[0], vy[1] - vy[0]);
    right_facing = (bound_coord_us > base_step);
  }
  bound_coord_ls = (vy[2] == vy[1]) ? 0 : MakePolyXFPStep(vx[2] - vx[1], vy[2] - vy[1]);

  unsigned vo = core_vertex ? 1 : 0;
  unsigned vp = (core_vertex == 2) ? 3 : 0;
  struct {
    int64_t x_coord[2], x_step[2];
    int y_coord, y_bound, dec_mode;
  } tp[2];
  {
    int k = vo;
    tp[k].y_coord = vy[0 ^ vo];
    tp[k].y_bound = vy[1 ^ vo];
    tp[k].x_coord[right_facing] = MakePolyXFP(vx[0 ^ vo]);
    tp[k].x_step[right_facing] = bound_coord_us;
    tp[k].x_coord[!right_facing] = base_coord + (int64_t)(vy[vo] - vy[0]) * base_step;
    tp[k].x_step[!right_facing] = base_step;
    tp[k].dec_mode = (vo != 0);
  }
  {
    int k = vo ^ 1;
    tp[k].y_coord = vy[1 ^ vp];
    tp[k].y_bound = vy[2 ^ vp];
    tp[k].x_coord[right_facing] = MakePolyXFP(vx[1 ^ vp]);
    tp[k].x_step[right_facing] = bound_coord_ls;
    tp[k].x_coord[!right_facing] = base_coord + (int64_t)(vy[1 ^ vp] - vy[0]) * base_step;
    tp[k].x_step[!right_facing] = base_step;
    tp[k].dec_mode = (vp != 0);
  }

  for (int i = 0; i < 2; i++) {
    int yi = tp[i].y_coord, yb = tp[i].y_bound;
    int64_t lc = tp[i].x_coord[0], ls = tp[i].x_step[0];
    int64_t rc = tp[i].x_coord[1], rs = tp[i].x_step[1];
    if (tp[i].dec_mode) {
      while (yi > yb) {
        yi--;
        lc -= ls;
        rc -= rs;
        if (yi < s_da_y0) {
          break;
        }
        if (yi > s_da_y1) {
          continue;
        }
        int xs = GetPolyXFP_Int(lc), xb = GetPolyXFP_Int(rc);
        if (xs < s_da_x0) {
          xs = s_da_x0;
        }
        if (xb > s_da_x1 + 1) {
          xb = s_da_x1 + 1;
        }
        for (int x = xs; x < xb; x++) {
          tri_px(a, b, c, x, yi, tex, shade, semi, raw, aa);
        }
      }
    } else {
      while (yi < yb) {
        if (yi > s_da_y1) {
          break;
        }
        if (yi >= s_da_y0) {
          int xs = GetPolyXFP_Int(lc), xb = GetPolyXFP_Int(rc);
          if (xs < s_da_x0) {
            xs = s_da_x0;
          }
          if (xb > s_da_x1 + 1) {
            xb = s_da_x1 + 1;
          }
          for (int x = xs; x < xb; x++) {
            tri_px(a, b, c, x, yi, tex, shade, semi, raw, aa);
          }
        }
        yi++;
        lc += ls;
        rc += rs;
      }
    }
  }
}
