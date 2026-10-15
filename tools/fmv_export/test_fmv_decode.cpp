// test_fmv_decode — TDD suite for the C++ .STR decode path (fmv_decode.cpp +
// mdec_beetle.c + mednafen mdec.c), pinned against the Python clean-room oracle
// (tools/fmv_export/str_decode.py). The Python decoder is the independently
// verified reference (22-test suite, all-white golden frame, correct Spider-Man
// FMVs); these tests drive the SAME golden vectors through the real C++ code so a
// decode regression — the 4833/38400-word drain stall that produced garbage frames
// — fails loudly instead of shipping.
//
// Hermetic tests need no disc (synthetic MDEC code streams + oracle-embedded
// expected colors). Disc-gated tests (LOGO.STR frame 1 / frame 31) run when a
// disc path is passed as the sole argument and SKIP otherwise.
//
// build: cmake --build build/clang --target test_fmv_decode
// run:   build/clang/tools/fmv_export/test_fmv_decode [disc.chd]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <optional>
#include <string>
#include <vector>

#include <openssl/sha.h>

#include "c_subsys.h" // mdec_init
#include "disc.h"
#include "fmv_decode.h"

namespace {

// ====================================================================================
// Tiny test framework
// ====================================================================================
int s_pass = 0, s_fail = 0, s_skip = 0;

struct TestInputs {
  const char *disc_path = nullptr;
};

TestInputs s_inputs;

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

void skip(const char *why) {
  fprintf(stderr, "    SKIP: %s\n", why);
  ++s_skip;
}

std::string sha1_hex(const uint8_t *data, size_t n) {
  unsigned char md[SHA_DIGEST_LENGTH];
  SHA1(data, n, md);
  char hex[SHA_DIGEST_LENGTH * 2 + 1];
  for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
    sprintf(hex + std::size_t{2} * i, "%02x", md[i]);
  }
  return hex;
}

// ====================================================================================
// Hermetic MDEC code-stream builders (mirror the oracle's psx::movie::decodeBsFrame output:
// DC word = (QScale<<10)|(10-bit DC), 0xFE00 = EOB, block order Cr,Cb,Y0..Y3)
// ====================================================================================
void push_block(std::vector<uint16_t> &codes, int dc) {
  codes.push_back((uint16_t)((0 << 10) | (dc & 0x3FF)));
  codes.push_back(0xFE00);
}

std::vector<uint16_t> allwhite_codes() {
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

std::vector<uint16_t> flat_mb_codes(int dcy, int dccb, int dccr) {
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
void test_mdec_drain_hermetic() {
  std::vector<uint16_t> codes = allwhite_codes();
  std::vector<uint16_t> px(std::size_t{320} * 240);
  mdec_init();
  int np = psx::movie::decodeMdecToRgb555(codes, {320, 240}, px.data());
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
void test_mdec_flat_color_blocks() {
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
    std::vector<uint16_t> px(std::size_t{16} * 16);
    int np = psx::movie::decodeMdecToRgb555(codes, {16, 16}, px.data());
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
// Disc-gated: STR demux over psx::cd::Disc (mirrors fmv_export.cpp / str_decode.py)
// ====================================================================================

// The BS payload of video frame `frame` of an STR movie.
std::optional<std::vector<uint8_t>> collectFrame(psx::cd::Disc &disc, psx::cd::DiscFile movie, int frame) {
  const uint32_t nsectors = (movie.size + 2047u) / 2048u;
  std::vector<uint8_t> payload;
  int cur_frame = -1;
  int expected_chunks = 0;
  int got_chunks = 0;
  uint8_t raw[2352];
  for (uint32_t sec = 0; sec < nsectors; sec++) {
    if (!disc.readRaw(movie.lba + sec, raw, 2352)) {
      return std::nullopt;
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
      if (framenum == frame) {
        return payload;
      }
      cur_frame = -1;
      expected_chunks = 0;
      got_chunks = 0;
    }
  }
  return std::nullopt;
}

// LOGO.STR frame `frame` from the disc argument; nullopt after recording a skip or a failure.
std::optional<std::vector<uint8_t>> logoFrame(int frame) {
  const char *path = s_inputs.disc_path;
  if (!path || !*path) {
    skip("no disc path argument");
    return std::nullopt;
  }
  psx::cd::Disc disc;
  if (!disc.openImage(path)) {
    fprintf(stderr, "    FAIL cannot open %s\n", path);
    ++s_fail;
    return std::nullopt;
  }
  const std::optional<psx::cd::DiscFile> movie = disc.findFile("MOVIE/LOGO.STR");
  if (!movie) {
    fprintf(stderr, "    FAIL LOGO.STR not found\n");
    ++s_fail;
    return std::nullopt;
  }
  std::optional<std::vector<uint8_t>> payload = collectFrame(disc, *movie, frame);
  if (!payload) {
    fprintf(stderr, "    FAIL demux\n");
    ++s_fail;
  }
  return payload;
}

// Disc-gated: LOGO.STR frame 1 (the proven all-white golden). Asserts the payload
// matches the oracle's SHA1, then the full bs_decode -> MDEC path decodes to all
// 0x7FFF. Previously the drain stall made this garbage.
void test_logo_frame1_white() {
  const std::optional<std::vector<uint8_t>> payload = logoFrame(1);
  if (!payload) {
    return;
  }
  std::string got = sha1_hex(payload->data(), payload->size());
  CHECK(got == "f39c9a049147394cff4a5df2d8418d9302db1763"); // oracle golden (all-white DC-only)

  std::vector<uint16_t> codes(std::size_t{512} * 1024);
  std::vector<uint16_t> px(std::size_t{320} * 240);
  int nc = psx::movie::decodeBsFrame(*payload, {320, 240}, codes);
  CHECK(nc > 0);
  mdec_init();
  int np = psx::movie::decodeMdecToRgb555({codes.data(), static_cast<std::size_t>(nc)}, {320, 240}, px.data());
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
void test_logo_frame31_content() {
  const std::optional<std::vector<uint8_t>> payload = logoFrame(31);
  if (!payload) {
    return;
  }

  std::vector<uint16_t> codes(std::size_t{512} * 1024);
  std::vector<uint16_t> px(std::size_t{320} * 240);
  int nc = psx::movie::decodeBsFrame(*payload, {320, 240}, codes);
  CHECK(nc > 0);
  mdec_init();
  int np = psx::movie::decodeMdecToRgb555({codes.data(), static_cast<std::size_t>(nc)}, {320, 240}, px.data());
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

} // namespace

int main(int argc, char **argv) {
  if (argc > 2) {
    fprintf(stderr, "usage: %s [disc.chd]\n", argv[0]);
    return 2;
  }
  s_inputs.disc_path = argc == 2 ? argv[1] : nullptr;
  RUN(mdec_drain_hermetic);
  RUN(mdec_flat_color_blocks);
  RUN(logo_frame1_white);
  RUN(logo_frame31_content);
  fprintf(stderr, "\n%d passed, %d failed, %d skipped\n", s_pass, s_fail, s_skip);
  return s_fail ? 1 : 0;
}
