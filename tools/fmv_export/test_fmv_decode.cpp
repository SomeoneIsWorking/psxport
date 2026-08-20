// test_fmv_decode — TDD suite for the C++ .STR decode path (fmv_decode.cpp +
// mdec_beetle.c + mednafen mdec.c), pinned against the Python clean-room oracle
// (tools/fmv_export/str_decode.py). The Python decoder is the independently
// verified reference (22-test suite, all-white golden frame, correct Spider-Man
// FMVs); these tests drive the SAME golden vectors through the real C++ code so a
// decode regression — the 4833/38400-word drain stall that produced garbage frames
// — fails loudly instead of shipping.
//
// Hermetic tests need no disc (synthetic MDEC code streams + oracle-embedded
// expected colors). Disc-gated tests (LOGO.STR frame 1 / frame 31) run when
// PSXPORT_TOMBA2_DISC or PSXPORT_DISC is set and SKIP otherwise.
//
// build: tools/fmv_export/build.sh also builds tools/fmv_export/test_fmv_decode
// run:   tools/fmv_export/test_fmv_decode
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <libchdr/chd.h>
#include <openssl/sha.h>

#include "c_subsys.h"   // mdec_init
#include "fmv_decode.h" // bs_decode_frame / mdec_decode_to_rgb555 / xa_decode_sector

// Vestigial Beetle savestate hook the vendored mdec.c references (same stub the
// exporter and runtime keep).
extern "C" int MDFNSS_StateAction(void *st, int load, int data_only, void *sf, const char *name) {
  (void)st;
  (void)load;
  (void)data_only;
  (void)sf;
  (void)name;
  return 1;
}

// ====================================================================================
// Tiny test framework
// ====================================================================================
static int s_pass = 0, s_fail = 0, s_skip = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                              \
      ++s_fail;                                                                                                        \
      return;                                                                                                          \
    }                                                                                                                  \
  } while (0)

#define RUN(name)                                                                                                      \
  do {                                                                                                                 \
    fprintf(stderr, "test %s\n", #name);                                                                               \
    int before = s_fail;                                                                                               \
    test_##name();                                                                                                     \
    if (s_fail == before) {                                                                                            \
      ++s_pass;                                                                                                        \
      fprintf(stderr, "  PASS\n");                                                                                     \
    }                                                                                                                  \
  } while (0)

static void skip(const char *why) {
  fprintf(stderr, "    SKIP: %s\n", why);
  ++s_skip;
}

static std::string sha1_hex(const uint8_t *data, size_t n) {
  unsigned char md[SHA_DIGEST_LENGTH];
  SHA1(data, n, md);
  char hex[SHA_DIGEST_LENGTH * 2 + 1];
  for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
    sprintf(hex + i * 2, "%02x", md[i]);
  }
  return hex;
}

// ====================================================================================
// Hermetic MDEC code-stream builders (mirror the oracle's bs_decode_frame output:
// DC word = (QScale<<10)|(10-bit DC), 0xFE00 = EOB, block order Cr,Cb,Y0..Y3)
// ====================================================================================
static void push_block(std::vector<uint16_t> &codes, int dc) {
  codes.push_back((uint16_t)((0 << 10) | (dc & 0x3FF)));
  codes.push_back(0xFE00);
}

static std::vector<uint16_t> allwhite_codes() {
  std::vector<uint16_t> codes; // 300 MBs, 320x240
  for (int mb = 0; mb < 300; mb++) {
    push_block(codes, 0); // Cr (neutral)
    push_block(codes, 0); // Cb (neutral)
    for (int i = 0; i < 4; i++) {
      push_block(codes, 508); // Y0..Y3 (white)
    }
  }
  return codes;
}

static std::vector<uint16_t> flat_mb_codes(int dcy, int dccb, int dccr) {
  std::vector<uint16_t> codes;
  push_block(codes, dccr);
  push_block(codes, dccb);
  for (int i = 0; i < 4; i++) {
    push_block(codes, dcy);
  }
  return codes;
}

// ====================================================================================
// Tests
// ====================================================================================

// Hermetic: a full synthetic 320x240 all-white frame (1800 blocks, DC-only) must
// drain COMPLETELY (38400 words) and decode to 0x7FFF everywhere. Catches the
// input-drop drain stall (only ~37 of 300 macroblocks used to decode).
static void test_mdec_drain_hermetic() {
  std::vector<uint16_t> codes = allwhite_codes();
  std::vector<uint16_t> px(320u * 240u);
  mdec_init();
  int np = mdec_decode_to_rgb555(codes.data(), (int)codes.size(), 320, 240, px.data());
  CHECK(np == 320 * 240);
  for (size_t i = 0; i < px.size(); i++) {
    if (px[i] != 0x7FFF) {
      fprintf(stderr, "    pixel %zu = %04x\n", i, px[i]);
      CHECK(false);
    }
  }
}

// Hermetic: a single macroblock with flat DC per block must produce one flat color
// per block (goldens generated by the Python oracle through the same mdec.c math).
static void test_mdec_flat_color_blocks() {
  struct {
    int y, cb, cr, expect;
  } vec[] = {
      {128, 200, 32, 0x7e35},
      {16, 16, 16, 0x4611},
      {240, 128, 64, 0x7eba},
  };
  mdec_init();
  for (auto &v : vec) {
    std::vector<uint16_t> codes = flat_mb_codes(v.y, v.cb, v.cr);
    std::vector<uint16_t> px(16u * 16u);
    int np = mdec_decode_to_rgb555(codes.data(), (int)codes.size(), 16, 16, px.data());
    CHECK(np == 16 * 16);
    for (size_t i = 0; i < px.size(); i++) {
      if (px[i] != (uint16_t)v.expect) {
        fprintf(stderr, "    DC(%d,%d,%d) pixel %zu = %04x want %04x\n", v.y, v.cb, v.cr, i, px[i], v.expect);
        CHECK(false);
      }
    }
  }
}

// ====================================================================================
// Disc-gated: minimal CHD reader + STR demux (mirror fmv_export.cpp / str_decode.py)
// ====================================================================================
static chd_file *s_chd = nullptr;
static uint32_t s_hbytes = 0, s_fph = 0, s_hcount = 0, s_cached = 0xFFFFFFFFu;
static std::vector<uint8_t> s_hbuf;

static bool disc_open(const char *path) {
  if (chd_open(path, CHD_OPEN_READ, 0, &s_chd) != CHDERR_NONE) {
    return false;
  }
  const chd_header *h = chd_get_header(s_chd);
  s_hbytes = h->hunkbytes;
  s_fph = h->hunkbytes / 2448u;
  s_hcount = h->totalhunks;
  s_hbuf.assign(s_hbytes, 0);
  return s_fph > 0;
}

static bool disc_read_raw(uint32_t lba, uint8_t *out, uint32_t n) {
  if (n > 2352u) {
    n = 2352u;
  }
  uint32_t hunk = lba / s_fph, off = (lba % s_fph) * 2448u;
  if (hunk >= s_hcount) {
    return false;
  }
  if (hunk != s_cached) {
    if (chd_read(s_chd, hunk, s_hbuf.data()) != CHDERR_NONE) {
      return false;
    }
    s_cached = hunk;
  }
  memcpy(out, s_hbuf.data() + off, n);
  return true;
}

static uint32_t rd_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool iso_find_file(const char *path, uint32_t *out_lba, uint32_t *out_size) {
  uint8_t sec[2048];
  if (!disc_read_raw(16, sec, 2048)) {
    return false; // PVD at LBA 16 (data starts at +24)
  }
  uint8_t user[2048];
  memcpy(user, sec + 24, 2048);
  if (user[0] != 1) {
    return false;
  }
  uint32_t dir_lba = rd_le32(user + 156 + 2), dir_size = rd_le32(user + 156 + 10);

  const char *p = path;
  while (*p == '\\' || *p == '/') {
    p++;
  }
  while (*p) {
    char comp[64];
    int cn = 0;
    while (*p && *p != '\\' && *p != '/' && cn < 63) {
      comp[cn++] = *p++;
    }
    comp[cn] = 0;
    while (*p == '\\' || *p == '/') {
      p++;
    }
    int is_last = (*p == 0);

    uint32_t nsec = (dir_size + 2047u) / 2048u, found = 0;
    for (uint32_t s = 0; s < nsec && !found; s++) {
      if (!disc_read_raw(dir_lba + s, sec, 2048)) {
        return false;
      }
      memcpy(user, sec + 24, 2048);
      uint32_t o = 0;
      while (o < 2048) {
        uint32_t rlen = user[o];
        if (rlen == 0) {
          break;
        }
        if (o + rlen > 2048) {
          break;
        }
        int nlen = user[o + 32];
        char nm[128] = {0};
        int i = 0;
        for (; i < nlen && i < 127; i++) {
          if (user[o + 33 + i] == ';') {
            break;
          }
          nm[i] = (char)user[o + 33 + i];
        }
        nm[i] = 0;
        char a[64] = {0};
        for (i = 0; comp[i]; i++) {
          a[i] = (comp[i] >= 'a' && comp[i] <= 'z') ? (char)(comp[i] - 32) : comp[i];
        }
        for (i = 0; nm[i]; i++) {
          nm[i] = (nm[i] >= 'a' && nm[i] <= 'z') ? (char)(nm[i] - 32) : nm[i];
        }
        if (strcmp(a, nm) == 0) {
          if (is_last) {
            *out_lba = rd_le32(user + o + 2);
            *out_size = rd_le32(user + o + 10);
            return true;
          }
          dir_lba = rd_le32(user + o + 2);
          dir_size = rd_le32(user + o + 10);
          found = 1;
          break;
        }
        o += rlen;
      }
    }
    if (!found) {
      return false;
    }
  }
  return false;
}

// Collect the BS payloads for video frames `from`..`to` (inclusive) of an STR movie.
static bool
collect_frames(uint32_t lba, uint32_t size, int from, int to, std::vector<std::vector<uint8_t>> &out_payloads) {
  uint32_t nsectors = (size + 2047u) / 2048u;
  std::vector<uint8_t> payload;
  int cur_frame = -1, expected_chunks = 0, got_chunks = 0;
  uint8_t raw[2352];
  for (uint32_t sec = 0; sec < nsectors; sec++) {
    if (!disc_read_raw(lba + sec, raw, 2352)) {
      return false;
    }
    if (raw[18] & 0x04) {
      continue; // XA-ADPCM audio sector
    }
    const uint8_t *sbuf = raw + 24;
    if ((sbuf[0] | (sbuf[1] << 8)) != 0x0160) {
      continue;
    }
    int chunk_idx = sbuf[4] | (sbuf[5] << 8);
    int nchunks = sbuf[6] | (sbuf[7] << 8);
    int framenum = sbuf[8] | (sbuf[9] << 8) | (sbuf[10] << 16) | (sbuf[11] << 24);
    if (chunk_idx == 0) {
      cur_frame = framenum;
      expected_chunks = nchunks;
      got_chunks = 0;
      payload.clear();
    }
    if (cur_frame != framenum) {
      continue;
    }
    payload.insert(payload.end(), sbuf + 32, sbuf + 32 + 2016);
    got_chunks++;
    if (expected_chunks > 0 && got_chunks >= expected_chunks) {
      if (framenum >= from && framenum <= to) {
        out_payloads.push_back(payload);
      }
      cur_frame = -1;
      expected_chunks = 0;
      got_chunks = 0;
      if (framenum >= to) {
        return true;
      }
    }
  }
  return true;
}

// Disc-gated: LOGO.STR frame 1 (the proven all-white golden). Asserts the payload
// matches the oracle's SHA1, then the full bs_decode -> MDEC path decodes to all
// 0x7FFF. Previously the drain stall made this garbage.
static void test_logo_frame1_white() {
  const char *disc = getenv("PSXPORT_TOMBA2_DISC");
  if (!disc) {
    disc = getenv("PSXPORT_DISC");
  }
  if (!disc || !*disc) {
    skip("no disc (set PSXPORT_TOMBA2_DISC / PSXPORT_DISC)");
    return;
  }
  if (!disc_open(disc)) {
    fprintf(stderr, "    FAIL cannot open %s\n", disc);
    ++s_fail;
    return;
  }
  uint32_t lba = 0, size = 0;
  if (!iso_find_file("MOVIE/LOGO.STR", &lba, &size)) {
    fprintf(stderr, "    FAIL LOGO.STR not found\n");
    ++s_fail;
    return;
  }
  std::vector<std::vector<uint8_t>> payloads;
  if (!collect_frames(lba, size, 1, 1, payloads)) {
    fprintf(stderr, "    FAIL demux\n");
    ++s_fail;
    return;
  }
  CHECK(payloads.size() == 1);
  std::string got = sha1_hex(payloads[0].data(), payloads[0].size());
  CHECK(got == "f39c9a049147394cff4a5df2d8418d9302db1763"); // oracle golden (all-white DC-only)

  std::vector<uint16_t> codes(512 * 1024);
  std::vector<uint16_t> px(320u * 240u);
  int nc = bs_decode_frame(payloads[0].data(), (uint32_t)payloads[0].size(), 320, 240, codes.data(), (int)codes.size());
  CHECK(nc > 0);
  mdec_init();
  int np = mdec_decode_to_rgb555(codes.data(), nc, 320, 240, px.data());
  CHECK(np == 320 * 240);
  for (size_t i = 0; i < px.size(); i++) {
    if (px[i] != 0x7FFF) {
      fprintf(stderr, "    pixel %zu = %04x\n", i, px[i]);
      CHECK(false);
    }
  }
}

// Disc-gated: LOGO.STR frame 31 (first AC-content frame). Asserts the decoded frame
// matches the oracle's unique-color count, top-color histogram, and pixel SHA1.
static void test_logo_frame31_content() {
  const char *disc = getenv("PSXPORT_TOMBA2_DISC");
  if (!disc) {
    disc = getenv("PSXPORT_DISC");
  }
  if (!disc || !*disc) {
    skip("no disc (set PSXPORT_TOMBA2_DISC / PSXPORT_DISC)");
    return;
  }
  if (!disc_open(disc)) {
    fprintf(stderr, "    FAIL cannot open %s\n", disc);
    ++s_fail;
    return;
  }
  uint32_t lba = 0, size = 0;
  if (!iso_find_file("MOVIE/LOGO.STR", &lba, &size)) {
    fprintf(stderr, "    FAIL LOGO.STR not found\n");
    ++s_fail;
    return;
  }
  std::vector<std::vector<uint8_t>> payloads;
  if (!collect_frames(lba, size, 31, 31, payloads)) {
    fprintf(stderr, "    FAIL demux\n");
    ++s_fail;
    return;
  }
  CHECK(payloads.size() == 1);

  std::vector<uint16_t> codes(512 * 1024);
  std::vector<uint16_t> px(320u * 240u);
  int nc = bs_decode_frame(payloads[0].data(), (uint32_t)payloads[0].size(), 320, 240, codes.data(), (int)codes.size());
  CHECK(nc > 0);
  mdec_init();
  int np = mdec_decode_to_rgb555(codes.data(), nc, 320, 240, px.data());
  CHECK(np == 320 * 240);

  // unique-color count + top-5 histogram (oracle: 9fd25626e8138e5b3438bda41cb29250f3d7e88c)
  long hist[65536] = {0};
  int unique = 0;
  for (int v : px) {
    if (++hist[v] == 1) {
      unique++;
    }
  }
  CHECK(unique == 378);
  struct Top {
    int v, c;
  } top[5] = {{0, -1}, {0, -1}, {0, -1}, {0, -1}, {0, -1}};
  for (int v = 0; v < 65536; v++) {
    for (int k = 0; k < 5; k++) {
      if (hist[v] > top[k].c) {
        for (int j = 4; j > k; j--) {
          top[j] = top[j - 1];
        }
        top[k] = {v, (int)hist[v]};
        break;
      }
    }
  }
  int expect_v[5] = {32767, 24063, 24095, 31710, 24031};
  int expect_c[5] = {75782, 129, 28, 22, 19};
  for (int k = 0; k < 5; k++) {
    if (top[k].v != expect_v[k] || top[k].c != expect_c[k]) {
      fprintf(stderr, "    top[%d] = %04x x%d want %04x x%d\n", k, top[k].v, top[k].c, expect_v[k], expect_c[k]);
      CHECK(false);
    }
  }

  std::string got = sha1_hex((const uint8_t *)px.data(), px.size() * 2);
  CHECK(got == "9fd25626e8138e5b3438bda41cb29250f3d7e88c");
}

int main() {
  RUN(mdec_drain_hermetic);
  RUN(mdec_flat_color_blocks);
  RUN(logo_frame1_white);
  RUN(logo_frame31_content);
  fprintf(stderr, "\n%d passed, %d failed, %d skipped\n", s_pass, s_fail, s_skip);
  return s_fail ? 1 : 0;
}
