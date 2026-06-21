// tex_export: dump EVERY Tomba!2 texture set from the disc, offline (emulator OFF). Generalizes
// menu_bg_export — instead of compositing the title, it decodes each set in CD/TOMBA2.IDX/IMG with the
// PC-owned asset codec (unpack descriptors + LZ decompress, mirror of game_tomba2.cpp / tex_reconstruct.c)
// and writes, per set, the VRAM footprint that set places as a PNG (raw 16bpp RGB555 view) plus a manifest
// of its descriptors {x,y,stride,field}. Lets you BROWSE every texture (e.g. find the walking-dust effect
// page) and read off its texpage/CLUT coords — no game run, no GP0, no oracle.
//
// NB most textures are paletted (4/8bpp index data); the raw 16bpp view shows the INDEX bytes, not the
// final colors (apply the prim's CLUT to see true color). Structure/placement is still legible — enough to
// locate a texture and confirm its data is intact (a UV/texpage bug looks fine here; a decode bug looks broken).
//
// Build: add_executable in tools/CMakeLists.txt (links chdr-static + psxport_common + z), cmake --build.
// Run:   tex_export [disc.chd] [out_dir]      (out_dir default scratch/textures/)
//
// FRAMES MODE — lay a sprite SHEET's animation frames into ONE labeled, TRUE-COLOR (CLUT-decoded) PNG:
//   tex_export --frames --tp X,Y --bpp {4|8|15} --clut X,Y --uv U,V --size W,H [--step DU,DV]
//              --n NFRAMES [--set S] [--out scratch/screenshots/dust_sheet.png]
//   Decodes the disc-reconstructed VRAM (full atlas, or only --set S) exactly like the renderer's
//   sample_tex: 4bpp/8bpp index -> CLUT row -> RGB555 -> RGB. Frames are laid left-to-right starting
//   at texpage-local (U,V), advancing (DU,DV) per frame (default DU=W,DV=0 = horizontal sheet). Each
//   cell is labeled F0,F1,...; a RED '#' marks any frame whose U range crosses a 256-texel texpage
//   boundary (the suspected #8/#9 dust-bar onset). U is NOT masked to 255 (matches SW sample_tex,
//   shows wide-sprite spill into the next page honestly — the shader's `u&=255` is the render bug).
//
//   The WALKING-DUST cel's tpage/CLUT/U/V/size/count are RUNTIME data (per-area "BAV" cel asset,
//   loaded via gen_func_80096590 -> cel base latched at *0x80105CE8; effect slot array @0x800154C8),
//   NOT static in MAIN.EXE. The default --frames invocation renders a TEMPLATE over a populated page.
//
// SCANBAV / BAVSHEET MODES (added 2026-06-21, walking-dust task) — locate + lay out a per-area BAV
// EFFECT CEL (e.g. the walking dust) from the disc / a live dump. See the comment blocks in main():
//   --scanbav [--all]  scan the disc for a BAV descriptor (dust = type 0x70, kind 7, 17 frames).
//   --bavsheet --ram <dump> --vram <vramraw> --desc <addr> [--clut X,Y] [--w N]  lay a loaded BAV
//                      cel's frames into one sheet from a RAM dump + a raw-u16 VRAM dump (REPL `vramraw`).
//   bav_parse / bav_layout reproduce engine_bav.cpp loop-3 (the per-frame VRAM cel word) 0-DIFF vs the
//   live-latched cel records — verified against a runtime dump of the seaside dust (slot 1, 0x801858d4).
// FINDING: these BAV "frames" are NOT a simple sprite strip — each frame is a LARGE packed region (~7k
//   VRAM words) that the dust draws as a CLUSTER OF TEXTURED QUADS (the descriptor's hu<<9 region holds
//   32-byte quad records w/ vertex indices c0..c3). A faithful sprite sheet needs those per-quad UV
//   lists decoded; the linear-span sheet here shows the raw per-frame VRAM region only.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>

#include <libchdr/chd.h>
#include "common/env.h"

namespace {
constexpr uint32_t kRawFrameSize = 2448, kUserDataSize = 2048;

class ChdDisc {
public:
  bool Open(const std::string& path) {
    if (chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &m_chd) != CHDERR_NONE) return false;
    const chd_header* h = chd_get_header(m_chd);
    m_hunk_bytes = h->hunkbytes; m_frames_per_hunk = h->hunkbytes / kRawFrameSize;
    m_hunk_count = h->totalhunks; m_hunk_buf.resize(m_hunk_bytes);
    return m_frames_per_hunk > 0;
  }
  ~ChdDisc() { if (m_chd) chd_close(m_chd); }
  bool ReadSector(uint32_t lba, uint8_t* out) {
    const uint32_t hunk = lba / m_frames_per_hunk, offset = (lba % m_frames_per_hunk) * kRawFrameSize;
    if (hunk >= m_hunk_count) return false;
    if (hunk != m_cached_hunk) {
      if (chd_read(m_chd, hunk, m_hunk_buf.data()) != CHDERR_NONE) return false;
      m_cached_hunk = hunk;
    }
    const uint8_t* raw = m_hunk_buf.data() + offset;
    const uint32_t data_off = (raw[15] == 2) ? 24 : 16;
    std::memcpy(out, raw + data_off, kUserDataSize);
    return true;
  }
private:
  chd_file* m_chd = nullptr; uint32_t m_hunk_bytes = 0, m_frames_per_hunk = 0, m_hunk_count = 0;
  uint32_t m_cached_hunk = UINT32_MAX; std::vector<uint8_t> m_hunk_buf;
};

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | (uint32_t(p[3])<<24); }
int16_t  rd16(const uint8_t* p) { return (int16_t)(p[0] | (p[1]<<8)); }

std::string Norm(std::string n) {
  if (auto sc = n.find(';'); sc != std::string::npos) n.resize(sc);
  for (char& c : n) c = (char)std::toupper((unsigned char)c);
  return n;
}
struct IsoFile { uint32_t lba, size; };
bool FindFile(ChdDisc& disc, const std::string& path, IsoFile* out) {
  uint8_t sec[kUserDataSize];
  if (!disc.ReadSector(16, sec) || std::memcmp(sec + 1, "CD001", 5) != 0) return false;
  uint32_t lba = rd32(sec + 156 + 2), size = rd32(sec + 156 + 10);
  std::vector<std::string> parts;
  for (size_t i = 0, j; i < path.size(); i = j + 1) {
    j = path.find('/', i); if (j == std::string::npos) j = path.size();
    if (j > i) parts.emplace_back(path.substr(i, j - i));
  }
  for (size_t k = 0; k < parts.size(); k++) {
    const bool want_dir = k + 1 < parts.size();
    const uint32_t nsec = (size + kUserDataSize - 1) / kUserDataSize;
    std::vector<uint8_t> buf((size_t)nsec * kUserDataSize); bool found = false;
    for (uint32_t i = 0; i < nsec; i++) disc.ReadSector(lba + i, buf.data() + (size_t)i * kUserDataSize);
    for (uint32_t s = 0; s < buf.size() && !found; s += kUserDataSize)
      for (uint32_t pos = s; pos < s + kUserDataSize;) {
        const uint8_t len = buf[pos]; if (!len) break;
        const uint8_t flags = buf[pos + 25], nlen = buf[pos + 32];
        std::string name((const char*)&buf[pos + 33], nlen);
        const uint32_t e_lba = rd32(&buf[pos + 2]), e_size = rd32(&buf[pos + 10]); pos += len;
        if (nlen == 1 && (name[0] == 0 || name[0] == 1)) continue;
        if ((bool)(flags & 0x02) == want_dir && Norm(name) == Norm(parts[k])) {
          lba = e_lba; size = e_size; found = true; break;
        }
      }
    if (!found) return false;
  }
  out->lba = lba; out->size = size; return true;
}
std::vector<uint8_t> ReadFile(ChdDisc& disc, const IsoFile& f) {
  const uint32_t nsec = (f.size + kUserDataSize - 1) / kUserDataSize;
  std::vector<uint8_t> buf((size_t)nsec * kUserDataSize);
  for (uint32_t i = 0; i < nsec; i++) disc.ReadSector(f.lba + i, buf.data() + (size_t)i * kUserDataSize);
  buf.resize(f.size); return buf;
}

// Enumerate a directory's child files (one level) into {name,lba,size}, for --scanbav.
struct DirEnt { std::string name; uint32_t lba, size; bool dir; };
bool ListDir(ChdDisc& disc, const std::string& path, std::vector<DirEnt>& out) {
  IsoFile d;
  if (path.empty()) { uint8_t sec[kUserDataSize];
    if (!disc.ReadSector(16, sec) || std::memcmp(sec + 1, "CD001", 5) != 0) return false;
    d.lba = rd32(sec + 156 + 2); d.size = rd32(sec + 156 + 10);
  } else if (!FindFile(disc, path, &d)) return false;
  const uint32_t nsec = (d.size + kUserDataSize - 1) / kUserDataSize;
  std::vector<uint8_t> buf((size_t)nsec * kUserDataSize);
  for (uint32_t i = 0; i < nsec; i++) disc.ReadSector(d.lba + i, buf.data() + (size_t)i * kUserDataSize);
  for (uint32_t s = 0; s < buf.size(); s += kUserDataSize)
    for (uint32_t pos = s; pos < s + kUserDataSize;) {
      const uint8_t len = buf[pos]; if (!len) break;
      const uint8_t flags = buf[pos + 25], nlen = buf[pos + 32];
      std::string name((const char*)&buf[pos + 33], nlen);
      const uint32_t e_lba = rd32(&buf[pos + 2]), e_size = rd32(&buf[pos + 10]); pos += len;
      if (nlen == 1 && (name[0] == 0 || name[0] == 1)) continue;
      out.push_back({Norm(name), e_lba, e_size, (bool)(flags & 0x02)});
    }
  return true;
}

// ---- PC-owned asset codec (mirror of game_tomba2.cpp / tex_reconstruct.c) ----
const int32_t OFF_BASE[8]   = { 0, -1,  0, -1, -2, -3,  1,  2 };
const int32_t OFF_FACTOR[8] = { 0,  0, -1, -1, -1, -1, -1, -1 };
uint32_t lz_decompress(uint8_t* out, uint32_t outcap, const uint8_t* src, uint32_t srclen, int32_t stride) {
  int32_t off[8]; for (int i = 0; i < 8; i++) off[i] = OFF_BASE[i] + 2 * (OFF_FACTOR[i] * stride);
  uint32_t si = 0, oi = 0;
  while (si < srclen) {
    uint8_t ctrl = src[si++]; uint32_t len = ctrl >> 3, mode = ctrl & 7u;
    if (mode != 0) {
      int64_t bi = (int64_t)oi + off[mode];
      for (uint32_t k = 0; k < len; k++) { if (oi >= outcap || bi < 0 || bi >= (int64_t)outcap) return oi; out[oi++] = out[bi++]; }
    } else {
      if (len == 0) break;
      for (uint32_t k = 0; k < len; k++) { if (oi >= outcap || si >= srclen) return oi; out[oi++] = src[si++]; }
    }
  }
  return oi;
}

constexpr int VRAM_W = 1024, VRAM_H = 512;
uint16_t g_vram[VRAM_W * VRAM_H];

struct Desc { int x, y, stride, field; };
// Decode one archive into g_vram; collect each descriptor's placement + the overall bbox of touched VRAM.
void apply_archive(const uint8_t* arc, uint32_t len, std::vector<Desc>& descs,
                   int& bx0, int& by0, int& bx1, int& by1) {
  if (len < 0x800) return;
  uint32_t count = rd32(arc);
  const uint8_t* entry = arc + 4; const uint8_t* src = arc + 0x800;
  static uint8_t img[0x40000];
  for (uint32_t i = 0; i < count; i++, entry += 12) {
    int16_t x = rd16(entry+0), y = rd16(entry+2), stride = rd16(entry+4), field = rd16(entry+6);
    uint32_t srclen = rd32(entry + 8);
    if (src + srclen > arc + len) break;
    lz_decompress(img, sizeof img, src, srclen, stride);
    const uint16_t* px = (const uint16_t*)img;
    for (int r = 0; r < field; r++) for (int c = 0; c < stride; c++) {
      int vx = x + c, vy = y + r;
      if (vx >= 0 && vx < VRAM_W && vy >= 0 && vy < VRAM_H) g_vram[vy*VRAM_W + vx] = px[r*stride + c];
    }
    descs.push_back({x, y, stride, field});
    if (stride > 0 && field > 0) {
      if (x < bx0) bx0 = x; if (y < by0) by0 = y;
      if (x + stride > bx1) bx1 = x + stride; if (y + field > by1) by1 = y + field;
    }
    src += srclen;
  }
}
void put_png(const char* path, int w, int h, const uint8_t* rgb);  // fwd

// ---- true-color CLUT decode of the reconstructed g_vram (mirror of GpuState::sample_tex) ----
// A PSX texpage is 256 texels wide for the prim's bit depth: 4bpp packs 4 texels / VRAM word
// (64 words), 8bpp packs 2 (128 words), 15bpp is 1:1 (256 words). The CLUT is a 16- (4bpp) or
// 256- (8bpp) entry palette row of RGB555 at (clut_x, clut_y). This is exactly what the renderer
// does at sample time — so the decoded colors ARE the on-screen colors (the bug is render-side
// UV/texpage handling, not the data; this view confirms the data is intact).
inline uint16_t vram_at(int x, int y) {
  if (x < 0 || x >= VRAM_W || y < 0 || y >= VRAM_H) return 0;
  return g_vram[y * VRAM_W + x];
}
// Sample one texel (u,v) within a texpage at (tpx,tpy), bit-depth `bpp` (4/8/15), CLUT at (clx,cly).
// Returns RGB555 (0 = transparent). u is NOT masked to 255 — matches SW sample_tex (lets a wide
// sprite's U run past the 256-texpage into adjacent VRAM, the very behavior the shader's `u&=255`
// wrongly clobbers). For a STATIC SHEET VIEW we want the data as-laid-out, so no wrap here either.
inline uint16_t sample_texel(int u, int v, int tpx, int tpy, int bpp, int clx, int cly) {
  if (bpp == 15) return vram_at(tpx + u, tpy + v);
  if (bpp == 8) {
    uint16_t w = vram_at(tpx + (u >> 1), tpy + v);
    int idx = (u & 1) ? (w >> 8) : (w & 0xFF);
    return vram_at(clx + idx, cly);
  }
  uint16_t w = vram_at(tpx + (u >> 2), tpy + v);  // 4bpp
  int idx = (w >> ((u & 3) * 4)) & 0xF;
  return vram_at(clx + idx, cly);
}
inline void rgb555_to_rgb(uint16_t c, uint8_t* p) {
  p[0] = (c & 31) << 3; p[1] = ((c >> 5) & 31) << 3; p[2] = ((c >> 10) & 31) << 3;
}

// 5x7 ASCII bitmap font (digits + uppercase + a few symbols) for frame labels. Each glyph is 7 rows
// of 5 bits (MSB-left). Sparse table keyed by char; missing chars draw blank.
struct Glyph { char c; uint8_t rows[7]; };
const Glyph kFont[] = {
  {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
  {'2',{0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}}, {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
  {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
  {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
  {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
  {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
  {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
  {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'#',{0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}},
  {':',{0x00,0x04,0x00,0x00,0x00,0x04,0x00}}, {' ',{0,0,0,0,0,0,0}},
};
void draw_char(uint8_t* rgb, int W, int H, int x, int y, char c, uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t* rows = nullptr;
  for (auto& gl : kFont) if (gl.c == c) { rows = gl.rows; break; }
  if (!rows) return;
  for (int ry = 0; ry < 7; ry++) for (int rx = 0; rx < 5; rx++)
    if (rows[ry] & (1 << (4 - rx))) { int px = x + rx, py = y + ry;
      if (px >= 0 && px < W && py >= 0 && py < H) { uint8_t* p = &rgb[(py * W + px) * 3]; p[0]=r; p[1]=g; p[2]=b; } }
}
void draw_text(uint8_t* rgb, int W, int H, int x, int y, const char* s, uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; s[i]; i++) draw_char(rgb, W, H, x + i * 6, y, s[i], r, g, b);
}

// Lay every animation frame of a sprite sheet into ONE labeled PNG (one row of frames). The sheet is
// `nframes` cels, each `fw` x `fh` texels, the first at texpage-local (u0,v0), advancing by `du`,`dv`
// per frame (a horizontal sheet uses du=fw, dv=0). Decoded TRUE-COLOR through (clut_x,clut_y) at the
// given bit depth. Crossing a 256-texpage boundary is shown HONESTLY: U is not wrapped, so if a later
// frame's U runs past 256 you SEE it sampling adjacent VRAM (the render bug's mechanism, made visible).
void layout_sheet(const char* path, int tpx, int tpy, int bpp, int clut_x, int clut_y,
                  int u0, int v0, int fw, int fh, int du, int dv, int nframes) {
  const int pad = 2, label_h = 9;
  const int cellw = fw + pad, cellh = fh + label_h + pad;
  const int W = nframes * cellw + pad, H = cellh + pad;
  std::vector<uint8_t> rgb((size_t)W * H * 3, 32);  // dark-gray checker bg so transparent shows
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
    if (((x >> 3) ^ (y >> 3)) & 1) { uint8_t* p = &rgb[(y*W+x)*3]; p[0]=p[1]=p[2]=48; }
  for (int f = 0; f < nframes; f++) {
    int ox = pad + f * cellw, oy = pad + label_h;
    int fu = u0 + f * du, fv = v0 + f * dv;
    for (int yy = 0; yy < fh; yy++) for (int xx = 0; xx < fw; xx++) {
      uint16_t c = sample_texel(fu + xx, fv + yy, tpx, tpy, bpp, clut_x, clut_y);
      if (c == 0) continue;  // transparent: leave checker bg
      rgb555_to_rgb(c, &rgb[((size_t)(oy + yy) * W + (ox + xx)) * 3]);
    }
    char lbl[16]; std::snprintf(lbl, sizeof lbl, "F%d", f);
    draw_text(rgb.data(), W, H, ox, pad, lbl, 255, 255, 0);
    // mark the frame whose U crosses a 256 page boundary (the suspected bug onset) in RED.
    if ((fu % 256) > ((fu + fw - 1) % 256)) draw_text(rgb.data(), W, H, ox + fw - 6, pad, "#", 255, 0, 0);
  }
  put_png(path, W, H, rgb.data());
  std::printf("dust sheet -> %s  (%dx%d, %d frames, tp=(%d,%d) bpp=%d clut=(%d,%d) start uv=(%d,%d) %dx%d step=(%d,%d))\n",
              path, W, H, nframes, tpx, tpy, bpp, clut_x, clut_y, u0, v0, fw, fh, du, dv);
}
}  // namespace

namespace {
// ---- BAV cel descriptor (per-area effect-cel asset) — see engine/engine_bav.cpp ----
// A BAV descriptor: +0 (u32) header; (hdr>>8)==0x564142 "BAV", low byte = TYPE (0x70 for the dust
// variant). +4 (u32) kind/bpp selector (<5 -> 4bpp & sizes<<2; else 8bpp & sizes<<3; gates the
// 64/128 cel-record clamp when type==0x70). +18 (u16) "hu". +22 (u8) "bu" = frames-1. +32.. = `clamp`
// 16-byte CEL RECORDS, then (after hu<<9 more bytes) the per-frame UV size table.
//   bavload loop-3 (VRAM layout): cum=0; for each frame i in [0,bu]:
//     cum += (uv_hw[i] << shift);  word = (vram_base_word + cum) >> 3;
//   word is written to cel record (i>>1): +12 for even i, +14 for odd i. This is the engine's INTERNAL
//   packed cel pointer (NOT a GP0 texpage word) — reproduced here purely from the descriptor + a given
//   vram_base, so the offline tool needs none of the runtime-latched values (verified 0-diff vs live).
struct BavDesc {
  bool ok = false; uint32_t off = 0;       // byte offset of the descriptor within its (decoded) blob
  uint32_t type = 0, kind = 0, hu = 0; int frames = 0; int bpp = 0;
};
inline bool bav_match(const uint8_t* p, size_t avail) {
  if (avail < 32) return false;
  if (!((p[1]=='B') && (p[2]=='A') && (p[3]=='V'))) return false;  // (hdr>>8)=="BAV"
  return true;
}
// Parse a BAV descriptor at p (avail bytes follow). Fills frames/bpp and (if dust=true) requires the
// dust signature (type 0x70, kind 7, bu==16 -> 17 frames). Returns false if not a (matching) BAV.
bool bav_parse(const uint8_t* p, size_t avail, BavDesc* d, bool dust_only) {
  if (!bav_match(p, avail)) return false;
  uint32_t hdr = rd32(p);
  d->type = hdr & 0xff; d->kind = rd32(p + 4); d->hu = (uint16_t)(p[18] | (p[19] << 8));
  uint32_t bu = p[22]; d->frames = (int)bu + 1; d->bpp = (d->kind < 5) ? 4 : 8;
  if (dust_only && !(d->type == 0x70 && d->kind == 7 && bu == 16)) return false;
  d->ok = true; return true;
}
// Reproduce bavload loop-3: derive each frame's internal cel-pointer word from the descriptor + a
// vram_base (word addr). out_words[i] = (vram_base + cum_off_i) >> 3 (& 0xffff). Returns total bytes.
uint32_t bav_layout(const uint8_t* desc, uint32_t vram_base, int* out_words) {
  uint32_t kind = rd32(desc + 4); int bu = desc[22]; int shift = (kind < 5) ? 2 : 3;
  uint32_t clamp = (desc[0] == 0x70) ? ((kind < 5) ? 64u : 128u) : 64u;
  const uint8_t* uv = desc + 32 + clamp * 16 + ((uint32_t)((uint16_t)(desc[18] | (desc[19] << 8))) << 9);
  uint32_t cum = 0;
  for (int i = 0; i <= bu; i++) {
    uint32_t v = (uint16_t)(uv[i*2] | (uv[i*2+1] << 8));
    cum += (v << shift);
    out_words[i] = (int)(((vram_base + cum) >> 3) & 0xffff);
  }
  return cum;
}

// Parse "--key val" style options out of argv (returns the value string or fallback).
const char* opt(int argc, char** argv, const char* key, const char* dflt) {
  for (int i = 1; i + 1 < argc; i++) if (!std::strcmp(argv[i], key)) return argv[i + 1];
  return dflt;
}
bool has_flag(int argc, char** argv, const char* key) {
  for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], key)) return true; return false;
}
// Overlay ALL texture sets into g_vram (full atlas) so a frames-mode region can sample any page.
void build_full_vram(const std::vector<uint8_t>& idx, const std::vector<uint8_t>& img, uint32_t nsets) {
  std::memset(g_vram, 0, sizeof g_vram);
  for (uint32_t s = 0; s < nsets; s++) {
    uint32_t h0 = rd32(&idx[s*2048]), h1 = rd32(&idx[s*2048 + 4]);
    if (h1 <= h0 || h1 > img.size()) continue;
    std::vector<Desc> descs; int a=VRAM_W,b=VRAM_H,c=0,d=0;
    apply_archive(&img[h0], h1 - h0, descs, a, b, c, d);
  }
}
}  // namespace

int main(int argc, char** argv) {
  // Positional disc/out_dir are the FIRST args not starting with '-' (so options can precede them).
  const char* pos1 = nullptr; const char* pos2 = nullptr;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {                                   // option
      const bool valueless = !std::strcmp(argv[i],"--frames") || !std::strcmp(argv[i],"--scanbav") ||
                             !std::strcmp(argv[i],"--all") || !std::strcmp(argv[i],"--bavsheet");
      if (!valueless && i + 1 < argc) i++;                      // valued opt: skip its value
      continue;
    }
    if (!pos1) pos1 = argv[i]; else if (!pos2) pos2 = argv[i];
  }
  // ---- BAVSHEET MODE: lay a loaded BAV cel's 17 frames into one labeled sheet, from a RAM + raw-VRAM
  // dump (no disc needed). The dust's pixels are uploaded to a DYNAMIC VRAM slot at area-load and are NOT
  // present in TOMBA2.IMG, so the sheet is built from the LIVE atlas (REPL `vramraw` raw u16 dump). We
  // reproduce engine_bav.cpp loop-3 PURELY from the descriptor in the RAM dump to get each frame's VRAM
  // word-address + size, then CLUT-decode the frame rectangle.
  //   tex_export --bavsheet --ram scratch/raw/dust_ram.bin --vram scratch/raw/dust_vram_raw.bin
  //              --desc 0x801858d4 [--clut X,Y] [--w 256] [--out scratch/screenshots/dust_sheet.png]
  // NB each "frame" of these effect cels is a large packed VRAM region (a cluster of textured quads),
  // not a single sprite — the sheet shows each frame's raw atlas region (W-wide), CLUT-decoded.
  if (has_flag(argc, argv, "--bavsheet")) {
    const char* ramp = opt(argc, argv, "--ram", "scratch/raw/dust_ram.bin");
    const char* vramp = opt(argc, argv, "--vram", "scratch/raw/dust_vram_raw.bin");
    uint32_t descva = (uint32_t)std::strtoul(opt(argc, argv, "--desc", "0x801858d4"), nullptr, 0);
    int clx=0, cly=0; std::sscanf(opt(argc,argv,"--clut","0,0"), "%d,%d", &clx,&cly);
    int sheetw = std::atoi(opt(argc, argv, "--w", "256"));
    const char* outp = opt(argc, argv, "--out", "scratch/screenshots/dust_sheet.png");
    int rc0 = system("mkdir -p scratch/screenshots"); (void)rc0;
    FILE* rf = std::fopen(ramp, "rb"); if (!rf) { std::perror(ramp); return 1; }
    std::vector<uint8_t> ram(0x200000); if (std::fread(ram.data(),1,ram.size(),rf)!=ram.size()){} std::fclose(rf);
    FILE* vf = std::fopen(vramp, "rb"); if (!vf) { std::perror(vramp); return 1; }
    if (std::fread(g_vram,2,VRAM_W*VRAM_H,vf)!=(size_t)VRAM_W*VRAM_H){} std::fclose(vf);
    uint32_t d = descva & 0x1FFFFF;
    const uint8_t* desc = &ram[d];
    BavDesc bd; if (!bav_parse(desc, ram.size()-d, &bd, false)) { std::fprintf(stderr, "no BAV at desc\n"); return 1; }
    // slot's VRAM base: find which slot table entry (0x80105C50[slot]) == descva, read base from 0x80105D78.
    uint32_t vbase = 0; int slot=-1;
    for (int s=0;s<16;s++) if (rd32(&ram[(0x80105C50u&0x1FFFFF)+s*4])==descva) { slot=s;
        vbase = rd32(&ram[(0x80105D78u&0x1FFFFF)+s*4]); break; }
    if (slot<0) { std::fprintf(stderr,"desc not in any loaded BAV slot\n"); return 1; }
    int words[256]; bav_layout(desc, vbase, words);              // latched cel word per frame
    int nf = bd.frames;
    // frame i data span = [wstart[i], wstart[i+1]); wstart[i] = words[i-1]<<3 (words[-1]=vbase), end words[i]<<3.
    // Lay each frame as a sheetw-wide CLUT-decoded tile of its VRAM word region.
    const int pad=2, label_h=9; std::vector<std::vector<uint8_t>> tiles(nf); std::vector<int> th(nf);
    int maxh=1;
    for (int i=0;i<nf;i++) {
      // words[i] = (vbase + sum(sizes[0..i])) >> 3, so words[i]<<3 = end of frame i's VRAM word span;
      // frame i occupies [ (i==0?vbase : words[i-1]<<3), words[i]<<3 ).
      uint32_t a0 = (i==0) ? vbase : ((uint32_t)words[i-1] << 3);
      uint32_t a1 = (uint32_t)words[i] << 3;
      uint32_t nwords = (a1>a0)? a1-a0 : 0;
      // pixels are 8bpp: 2 per word. Lay as a sheetw-wide (texels) tile.
      int twords = sheetw/2; if (twords<1) twords=1;
      int rows = (int)((nwords + twords - 1)/twords); if (rows<1) rows=1; if (rows>2000) rows=2000;
      th[i]=rows; if (rows>maxh) maxh=rows;
      tiles[i].assign((size_t)sheetw*rows*3, 0);
      for (uint32_t k=0;k<nwords;k++) {
        uint32_t wa=a0+k; int vx=(int)(wa%VRAM_W), vy=(int)(wa/VRAM_W);
        if (vy>=VRAM_H) break; uint16_t wd=vram_at(vx,vy);
        for (int half=0; half<2; half++) {
          int idx=half? (wd>>8):(wd&0xff);
          int u=(int)(k*2+half); int rr=u/sheetw, cc=u%sheetw;
          if (rr>=rows) continue;
          uint8_t* p=&tiles[i][((size_t)rr*sheetw+cc)*3];
          if (clx==0 && cly==0) { p[0]=p[1]=p[2]=(uint8_t)idx; }   // index-grayscale view (no CLUT given)
          else { uint16_t c = vram_at(clx+idx, cly); if (c) rgb555_to_rgb(c,p); }
        }
      }
    }
    int cellw=sheetw+pad, cellh=maxh+label_h+pad;
    int W=nf*cellw+pad, H=cellh+pad;
    std::vector<uint8_t> rgb((size_t)W*H*3,32);
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) if (((x>>3)^(y>>3))&1){uint8_t*p=&rgb[(y*W+x)*3];p[0]=p[1]=p[2]=48;}
    for (int i=0;i<nf;i++) {
      int ox=pad+i*cellw, oy=pad+label_h;
      for (int yy=0;yy<th[i];yy++) for (int xx=0;xx<sheetw;xx++){
        uint8_t* s=&tiles[i][((size_t)yy*sheetw+xx)*3];
        if (s[0]|s[1]|s[2]) std::memcpy(&rgb[((size_t)(oy+yy)*W+ox+xx)*3], s, 3);
      }
      char lbl[16]; std::snprintf(lbl,sizeof lbl,"F%d",i);
      draw_text(rgb.data(),W,H,ox,pad,lbl,255,255,0);
    }
    put_png(outp,W,H,rgb.data());
    std::printf("bavsheet -> %s (%dx%d, slot %d, %d frames, vbase word=%05x, clut=(%d,%d), tile w=%d)\n",
                outp,W,H,slot,nf,vbase,clx,cly,sheetw);
    return 0;
  }

  const auto disc_path = psxport::ResolveDiscPath(pos1 ? pos1 : "");
  const std::string out_dir = pos2 ? pos2 : "scratch/textures";
  if (!disc_path) { std::fprintf(stderr, "no disc image (arg/env/drop-in)\n"); return 1; }
  ChdDisc disc;
  if (!disc.Open(*disc_path)) { std::fprintf(stderr, "cannot open CHD %s\n", disc_path->c_str()); return 1; }
  IsoFile idxf, imgf;
  if (!FindFile(disc, "CD/TOMBA2.IDX", &idxf) || !FindFile(disc, "CD/TOMBA2.IMG", &imgf)) {
    std::fprintf(stderr, "TOMBA2.IDX/IMG not found on disc\n"); return 1;
  }
  std::vector<uint8_t> idx = ReadFile(disc, idxf), img = ReadFile(disc, imgf);
  const uint32_t nsets = idxf.size / 2048;
  std::printf("disc %s : %u texture sets\n", disc_path->c_str(), nsets);

  // ---- SCANBAV MODE: locate a BAV cel descriptor on the disc, offline ----
  // `--scanbav [--all]`  scans BIN/*.BIN, CD/TOMBA2.{DAT,IMG} and ZZZ.DAT for the BAV magic ("BAV" at
  // byte+1) — both as RAW bytes and after a best-effort {count,12B-entry,payload@0x800} archive LZ-decode
  // — reporting any hit's file + byte offset + parsed descriptor (type/kind/frames). Without --all only
  // the DUST signature (type 0x70, kind 7, 17 frames) is reported.
  // FINDING (2026-06-21): NO BAV descriptor is present in ANY of these files raw OR via that archive
  // format. The per-area effect cels (the 3 type-0x70 cel atlases incl. the walking dust at runtime
  // 0x801858d4) are stored in a DIFFERENT compressed container that this scanner does not yet decode;
  // the exact disc file+offset is therefore still UNRESOLVED (needs the area-load path traced). The cel
  // pixels are also NOT in TOMBA2.IMG (the dust's live VRAM slot, X=272 Y=184, is uncovered by any
  // TOMBA2.IMG placement) — they are uploaded from the area asset to a dynamic VRAM slot at load.
  if (has_flag(argc, argv, "--scanbav")) {
    const bool all = has_flag(argc, argv, "--all");
    std::vector<std::string> files;
    std::vector<DirEnt> binents;
    if (ListDir(disc, "BIN", binents)) for (auto& e : binents) if (!e.dir) files.push_back("BIN/" + e.name);
    files.push_back("CD/TOMBA2.DAT");
    files.push_back("CD/TOMBA2.IMG");
    files.push_back("ZZZ.DAT");
    auto scan_buf = [&](const char* tag, const std::string& fp, uint32_t lba,
                        const uint8_t* buf, size_t n) {
      for (size_t i = 0; i + 32 <= n; i++) {
        if (buf[i] != 0x42 && buf[i] != 0x70) continue;             // type byte (cheap pre-filter)
        if (!(buf[i+1]=='B' && buf[i+2]=='A' && buf[i+3]=='V')) continue;
        BavDesc d; if (!bav_parse(&buf[i], n - i, &d, !all)) continue;
        std::printf("BAV  %-16s %-7s off=0x%06zx (LBA %u)  type=0x%02x kind=%u frames=%d bpp=%d hu=%u%s\n",
                    fp.c_str(), tag, i, lba, d.type, d.kind, d.frames, d.bpp, d.hu,
                    (d.type==0x70 && d.kind==7 && d.frames==17) ? "   <-- DUST" : "");
      }
    };
    static uint8_t dec[0x80000];
    for (auto& fp : files) {
      IsoFile f; if (!FindFile(disc, fp, &f)) continue;
      std::vector<uint8_t> blob = ReadFile(disc, f);
      scan_buf("raw", fp, f.lba, blob.data(), blob.size());          // 1) raw bytes
      // 2) archive LZ-decode: area BINs are {count, 12B entries{x,y,stride,field,srclen}, payload@0x800}.
      if (blob.size() >= 0x800) {
        uint32_t count = rd32(blob.data());
        if (count && count < 4096) {
          const uint8_t* entry = blob.data() + 4; const uint8_t* src = blob.data() + 0x800;
          for (uint32_t e = 0; e < count && src < blob.data() + blob.size(); e++, entry += 12) {
            int16_t stride = rd16(entry + 4); uint32_t srclen = rd32(entry + 8);
            if (src + srclen > blob.data() + blob.size()) break;
            uint32_t outn = lz_decompress(dec, sizeof dec, src, srclen, stride);
            char tag[16]; std::snprintf(tag, sizeof tag, "lz#%u", e);
            scan_buf(tag, fp, f.lba, dec, outn);
            src += srclen;
          }
        }
      }
    }
    return 0;
  }

  // ---- FRAMES MODE: lay every animation frame of a sprite sheet into one labeled PNG ----
  // `--frames` with: --tp X,Y (texpage origin) --bpp {4|8|15} --clut X,Y --uv U,V (first cel local)
  //   --size W,H (per-frame texels) --step DU,DV (per-frame advance, default DU=W DV=0) --n N (#frames)
  //   --set S (overlay only set S; default = full atlas) --out PATH (default scratch/screenshots/dust_sheet.png)
  // The dust's tpage/clut/uv are RUNTIME cel data (loaded per-area, not static in MAIN.EXE), so the
  // caller supplies them from a live REPL dump; this decodes the disc-reconstructed VRAM honestly.
  if (has_flag(argc, argv, "--frames")) {
    int tpx=0,tpy=0,clx=0,cly=0,u0=0,v0=0,fw=32,fh=32,du=-1,dv=0,n=8,bpp=4,set=-1;
    std::sscanf(opt(argc,argv,"--tp","0,0"), "%d,%d", &tpx,&tpy);
    std::sscanf(opt(argc,argv,"--clut","0,0"), "%d,%d", &clx,&cly);
    std::sscanf(opt(argc,argv,"--uv","0,0"), "%d,%d", &u0,&v0);
    std::sscanf(opt(argc,argv,"--size","32,32"), "%d,%d", &fw,&fh);
    std::sscanf(opt(argc,argv,"--step","-1,0"), "%d,%d", &du,&dv);
    bpp = std::atoi(opt(argc,argv,"--bpp","4"));
    n   = std::atoi(opt(argc,argv,"--n","8"));
    set = std::atoi(opt(argc,argv,"--set","-1"));
    if (du < 0) du = fw;  // default: horizontal sheet
    const char* outp = opt(argc, argv, "--out", "scratch/screenshots/dust_sheet.png");
    int rc0 = system("mkdir -p scratch/screenshots"); (void)rc0;
    if (set >= 0 && (uint32_t)set < nsets) {
      std::memset(g_vram, 0, sizeof g_vram);
      uint32_t h0 = rd32(&idx[set*2048]), h1 = rd32(&idx[set*2048 + 4]);
      if (h1 > h0 && h1 <= img.size()) { std::vector<Desc> d; int a=VRAM_W,b=VRAM_H,c=0,e=0;
        apply_archive(&img[h0], h1 - h0, d, a, b, c, e); }
      std::printf("frames: overlaid set %d only\n", set);
    } else {
      build_full_vram(idx, img, nsets);
      std::printf("frames: overlaid full atlas (%u sets)\n", nsets);
    }
    layout_sheet(outp, tpx, tpy, bpp, clx, cly, u0, v0, fw, fh, du, dv, n);
    return 0;
  }

  std::string mkdir = "mkdir -p '" + out_dir + "'"; int rc = system(mkdir.c_str()); (void)rc;
  std::string manifest_path = out_dir + "/manifest.txt";
  FILE* mf = fopen(manifest_path.c_str(), "w");

  for (uint32_t s = 0; s < nsets; s++) {
    uint32_t h0 = rd32(&idx[s*2048]), h1 = rd32(&idx[s*2048 + 4]);
    if (h1 <= h0 || h1 > img.size()) continue;
    std::memset(g_vram, 0, sizeof g_vram);
    std::vector<Desc> descs; int bx0 = VRAM_W, by0 = VRAM_H, bx1 = 0, by1 = 0;
    apply_archive(&img[h0], h1 - h0, descs, bx0, by0, bx1, by1);
    if (descs.empty() || bx1 <= bx0 || by1 <= by0) continue;
    // dump the touched bbox as a raw 16bpp RGB555 view (index data for paletted pages)
    int w = bx1 - bx0, h = by1 - by0;
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
      uint16_t c = g_vram[(by0 + y) * VRAM_W + (bx0 + x)];
      uint8_t* p = &rgb[((size_t)y * w + x) * 3];
      p[0] = (c & 31) << 3; p[1] = ((c >> 5) & 31) << 3; p[2] = ((c >> 10) & 31) << 3;
    }
    char png[512]; std::snprintf(png, sizeof png, "%s/set_%03u.png", out_dir.c_str(), s);
    put_png(png, w, h, rgb.data());
    if (mf) {
      std::fprintf(mf, "set %3u: archive[%u,%u)  bbox VRAM (%d,%d)..(%d,%d)  %zu images\n",
                   s, h0, h1, bx0, by0, bx1, by1, descs.size());
      for (auto& d : descs)
        std::fprintf(mf, "    img x=%4d y=%4d stride=%4d field=%4d\n", d.x, d.y, d.stride, d.field);
    }
    std::printf("set %3u -> %s  (%dx%d @ %d,%d, %zu imgs)\n", s, png, w, h, bx0, by0, descs.size());
  }
  if (mf) fclose(mf);
  std::printf("manifest: %s\n", manifest_path.c_str());
  return 0;
}

#include <zlib.h>
namespace {
void put_png(const char* path, int w, int h, const uint8_t* rgb) {
  std::vector<uint8_t> raw((size_t)h * (1 + w * 3));
  for (int y = 0; y < h; y++) { raw[(size_t)y*(1+w*3)] = 0;
    std::memcpy(&raw[(size_t)y*(1+w*3)+1], &rgb[(size_t)y*w*3], (size_t)w*3); }
  uLongf clen = compressBound(raw.size()); std::vector<uint8_t> comp(clen);
  compress2(comp.data(), &clen, raw.data(), raw.size(), 9); comp.resize(clen);
  FILE* f = fopen(path, "wb"); if (!f) { perror(path); return; }
  auto be32 = [&](uint32_t v){ uint8_t b[4]={(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v}; fwrite(b,1,4,f); };
  auto chunk = [&](const char* t, const uint8_t* d, uint32_t n){
    be32(n); uint32_t crc = crc32(0, (const Bytef*)t, 4); fwrite(t,1,4,f);
    if (n){ crc = crc32(crc, d, n); fwrite(d,1,n,f);} be32(crc); };
  fwrite("\x89PNG\r\n\x1a\n",1,8,f);
  uint8_t ihdr[13]={(uint8_t)(w>>24),(uint8_t)(w>>16),(uint8_t)(w>>8),(uint8_t)w,
                    (uint8_t)(h>>24),(uint8_t)(h>>16),(uint8_t)(h>>8),(uint8_t)h, 8,2,0,0,0};
  chunk("IHDR", ihdr, 13); chunk("IDAT", comp.data(), comp.size()); chunk("IEND", nullptr, 0);
  fclose(f);
}
}  // namespace
