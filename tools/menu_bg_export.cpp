// menu_bg_export: reconstruct the Tomba!2 TITLE/MENU background image DIRECTLY from the CHD disc, with
// the emulator/game completely OFF — no runtime, no OT, no GP0 replay. It opens the disc, resolves the
// two texture files by NAME (CD/TOMBA2.IDX = per-set header index, CD/TOMBA2.IMG = packed archives),
// decodes the title texture set with the PC-owned asset codecs (unpack descriptors + LZ decompress, the
// SAME logic as engine/game_tomba2.cpp ov_unpack_group / lz_decompress and tools/tex_reconstruct.c), then
// composites the on-screen title exactly as the two title sprites would (8bpp index pages + CLUT lookup),
// and writes a PNG. Proves the menu image is fully reconstructable from disc data alone.
//
// Disc layout (RE'd in ov_load_texgroup): a "texture set" S has a 1-sector header at TOMBA2.IDX + S*2048
// whose first two u32 {h0,h1} give the set's packed archive as the IMG byte range [h0,h1). The archive is
// [count:u32][pad:u32][count x 12B descriptor {x,y,stride,field:s16, srclen:u32}][packed data @ +0x800];
// each image LZ-decompresses to stride*field 16-bit VRAM words placed at VRAM (x,y).
//
// The title's two full-screen background sprites (decoded once from the title GP0 stream; baked here as the
// asset pipeline bakes the LZ table — no game run needed): both 8bpp, CLUT at VRAM (640,511), UV origin
// (0,0); sprite0 = screen x[0,256) from texpage X-base 640, sprite1 = screen x[256,320) from X-base 768 —
// together the 320-wide title. (texpage Y-base 256 for both.) Screen rect top = y=-8, so screen row sy
// samples texture row sy+8.
//
// Build:  add_executable in tools/CMakeLists.txt (links chdr-static + psxport_common), then cmake --build.
// Run:    menu_bg_export [disc.chd] [out.png]   (disc resolved via env/drop-in if omitted)
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

// ---- minimal CHD sector reader (same as tools/discdump.cpp) ----
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
// Resolve a nested path like "CD/TOMBA2.IDX" by descending the ISO9660 tree.
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

// ---- PC-owned asset codec (mirror of game_tomba2.cpp / tex_reconstruct.c) ----
// LZ back-reference offsets: MAIN.EXE .rodata @0x800153C8, 8 x {base, factor}; off[i]=base[i]+2*factor[i]*stride.
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

// Decode one archive (IMG byte range) and place every image into g_vram. When `report` is set, also note
// which descriptors land EXACTLY at the title page (640,256) and the title CLUT row block (640, y in 504..511)
// — exact placement, not bbox overlap, so the one title set is identified unambiguously among the broad pages.
void apply_archive(const uint8_t* arc, uint32_t len, bool* has_title, bool* has_clut) {
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
    if (has_title && x == 640 && y == 256) *has_title = true;
    if (has_clut  && x == 640 && y >= 504 && y <= 511) *has_clut = true;
    src += srclen;
  }
}

// 8bpp texel sample: texpage X-base txw / Y-base ty are VRAM-word coords; CLUT at (cx,cy) word coords.
inline uint16_t sample8(int txw, int ty, int u, int v, int cx, int cy) {
  uint16_t word = g_vram[(ty + v) * VRAM_W + (txw + (u >> 1))];
  uint8_t idx = (u & 1) ? (word >> 8) : (word & 0xFF);
  return g_vram[cy * VRAM_W + (cx + idx)];
}
void put_png(const char* path, int w, int h, const uint8_t* rgb);  // fwd
}  // namespace

int main(int argc, char** argv) {
  const auto disc_path = psxport::ResolveDiscPath(argc > 1 ? argv[1] : "");
  const char* out = argc > 2 ? argv[2] : "scratch/screenshots/menu_bg_offline.png";
  if (!disc_path) { std::fprintf(stderr, "no disc image (arg/env/drop-in)\n"); return 1; }
  ChdDisc disc;
  if (!disc.Open(*disc_path)) { std::fprintf(stderr, "cannot open CHD %s\n", disc_path->c_str()); return 1; }
  std::printf("disc: %s\n", disc_path->c_str());

  IsoFile idxf, imgf;
  if (!FindFile(disc, "CD/TOMBA2.IDX", &idxf) || !FindFile(disc, "CD/TOMBA2.IMG", &imgf)) {
    std::fprintf(stderr, "TOMBA2.IDX/IMG not found on disc\n"); return 1;
  }
  std::printf("TOMBA2.IDX LBA %u (%u B, %u sets)  TOMBA2.IMG LBA %u (%u B)\n",
              idxf.lba, idxf.size, idxf.size / 2048, imgf.lba, imgf.size);
  std::vector<uint8_t> idx = ReadFile(disc, idxf), img = ReadFile(disc, imgf);

  // Scan every set; decode into g_vram and report which set's archive places at VRAM (640,256) = the title
  // page (and (640,~507) = its CLUT block). Apply ONLY title-touching sets so a later set can't overwrite it.
  const uint32_t nsets = idxf.size / 2048;
  int title_set = -1, clut_set = -1;
  for (uint32_t s = 0; s < nsets; s++) {
    uint32_t h0 = rd32(&idx[s*2048]), h1 = rd32(&idx[s*2048 + 4]);
    if (h1 <= h0 || h1 > img.size()) continue;
    bool ht = false, hc = false;
    apply_archive(&img[h0], h1 - h0, &ht, &hc);
    if (ht || hc)
      std::printf("set %2u: archive[%u,%u)%s%s\n", s, h0, h1,
                  ht ? "  <= places (640,256) TITLE PAGE" : "", hc ? "  <= places (640,~507) CLUT" : "");
    if (ht && title_set < 0) title_set = (int)s;
    if (hc && clut_set  < 0) clut_set  = (int)s;
  }
  if (title_set < 0) { std::fprintf(stderr, "no set places exactly at VRAM (640,256) — title page not found\n"); return 1; }

  // Rebuild g_vram with ONLY the title (+CLUT) sets, in set order, so nothing overwrites the title page.
  std::memset(g_vram, 0, sizeof g_vram);
  uint32_t order[2]; int no = 0; order[no++] = (uint32_t)title_set;
  if (clut_set >= 0 && clut_set != title_set) order[no++] = (uint32_t)clut_set;
  for (int k = 0; k < no; k++) {
    uint32_t s = order[k], h0 = rd32(&idx[s*2048]), h1 = rd32(&idx[s*2048 + 4]);
    bool d0 = false, d1 = false; apply_archive(&img[h0], h1 - h0, &d0, &d1);
  }

  // Composite the 320x224 on-screen title exactly as the two title sprites sample it (8bpp + CLUT).
  // sprite0: screen x[0,256) texpage X-base 640 ; sprite1: screen x[256,320) X-base 768. CLUT (640,511).
  const int W = 320, H = 224, CX = 640, CY = 511, TY = 256;
  std::vector<uint8_t> rgb((size_t)W * H * 3);
  for (int sy = 0; sy < H; sy++) for (int sx = 0; sx < W; sx++) {
    int txw = (sx < 256) ? 640 : 768, u = (sx < 256) ? sx : sx - 256, v = sy + 8;  // screen top = y=-8
    uint16_t c = sample8(txw, TY, u, v, CX, CY);
    uint8_t* p = &rgb[((size_t)sy * W + sx) * 3];
    p[0] = (c & 31) << 3; p[1] = ((c >> 5) & 31) << 3; p[2] = ((c >> 10) & 31) << 3;
  }
  put_png(out, W, H, rgb.data());
  std::printf("wrote %s (%dx%d) from set %d%s — game NOT run\n", out, W, H, title_set,
              clut_set >= 0 ? "" : " (CLUT in same set)");
  return 0;
}

// --- tiny zlib-free PNG writer (stored deflate blocks) ---
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
