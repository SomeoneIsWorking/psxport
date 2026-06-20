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
}  // namespace

int main(int argc, char** argv) {
  const auto disc_path = psxport::ResolveDiscPath(argc > 1 ? argv[1] : "");
  const std::string out_dir = argc > 2 ? argv[2] : "scratch/textures";
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
