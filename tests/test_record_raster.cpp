// test_record_raster — GP0 streams executed by the GPU device, then replayed from its FrameRecord by
// RecordRasterizer on a headless Vulkan device; all of VRAM must match pixel for pixel (at S > 1, block for pixel).
//
// Bounds the streams respect, each a gpu.c behaviour the rasterizer does not reproduce:
//   - gpu.c's texture cache is invalidated only by copy, upload, read and texpage changes, so a draw
//     or fill into a texture page can be sampled stale. Draws and fills stay at x < 512; textures and
//     CLUTs sampled by draws come from x >= 512.
//   - A draw area below VRAM row 511 is clipped at 511; gpu.c wraps it. Draw areas stay within 511.
//   - Texel rows are fetched & 511; gpu.c reads past VRAM. Texture pages stay within VRAM.

#include "record_raster_harness.h"

using namespace record_raster_harness;

static void test_fills(void) {
  Stream s = baseState();
  s.add({rgb(0x02, 0xF8, 0x80, 0x08), xy(16, 16), xy(64, 32)});
  s.add({rgb(0x02, 0x13, 0x57, 0x9B), xy(37, 70), xy(33, 9)});     // x and width round to 16
  s.add({rgb(0x02, 0x40, 0x40, 0x40), xy(1008, 500), xy(48, 24)}); // wraps both axes
  CHECK_REPLAY(s);
}

static void test_copies(void) {
  Stream s = baseState();
  s.add({0x80000000u, xy(600, 10), xy(20, 300), xy(40, 30)});
  s.add({0x80000000u, xy(100, 100), xy(103, 102), xy(150, 40)}); // overlapping, moving right/down
  s.add({0x80000000u, xy(203, 202), xy(200, 200), xy(130, 20)}); // overlapping, moving left/up
  s.add({0x80000000u, xy(1000, 500), xy(300, 300), xy(50, 30)}); // source wraps
  s.mask(true, false);
  s.add({0x80000000u, xy(0, 0), xy(400, 0), xy(32, 32)});
  s.mask(false, true);
  s.add({0x80000000u, xy(700, 300), xy(400, 0), xy(64, 64)});
  CHECK_REPLAY(s);
}

static void test_uploads(void) {
  Stream s = baseState();
  s.upload(10, 10, 33, 7, 1);
  s.upload(1010, 505, 30, 20, 2); // wraps
  s.mask(true, true);
  s.upload(20, 12, 16, 16, 3);
  CHECK_REPLAY(s);
}

// At a scale that is not a power of two, an upload and a sprite sampling it fill every pixel of each block.
static void test_a_scaled_upload_and_a_read_of_it_fill_whole_blocks(void) {
  Stream s = baseState();
  s.upload(600, 300, 40, 24, 11);
  s.add({drawMode(page(9, 1, 0, 2), false, false, false)});
  s.add({rgb(0x65, 0, 0, 0), xy(10, 10), uv(24, 44, 0), xy(40, 24)});
  s.upload(1010, 505, 30, 20, 2);
  for (int scale : {3, 5}) {
    const Outcome outcome = replay(s, {}, scale);
    CHECK(outcome.replayed);
    CHECK_EQ(outcome.mismatched, 0);
  }
}

static void test_flat_and_gouraud_polygons(void) {
  for (int semi = 0; semi < 4; semi++) {
    for (int dither = 0; dither < 2; dither++) {
      Stream s = baseState();
      s.add({drawMode(page(8, 0, semi, 0), dither != 0, false, false)});
      s.add({rgb(0x20, 200, 100, 50), xy(10, 10), xy(200, 40), xy(60, 180)});
      s.add({rgb(0x22, 90, 180, 250), xy(30, 30), xy(230, 60), xy(80, 200)});
      s.add({rgb(0x28, 30, 60, 90), xy(250, 10), xy(400, 20), xy(240, 120), xy(420, 150)});
      s.add({rgb(0x2A, 255, 255, 255), xy(260, 30), xy(410, 40), xy(250, 140), xy(430, 170)});
      s.add({rgb(0x30, 255, 0, 0), xy(20, 220), rgb(0, 0, 255, 0), xy(220, 260), rgb(0, 0, 0, 255), xy(90, 400)});
      s.add({rgb(0x32, 10, 200, 30), xy(40, 240), rgb(0, 250, 5, 128), xy(240, 280), rgb(0, 7, 9, 250), xy(110, 420)});
      s.add({rgb(0x38, 0, 0, 0),
             xy(280, 220),
             rgb(0, 255, 128, 0),
             xy(500, 230),
             rgb(0, 0, 128, 255),
             xy(270, 380),
             rgb(0, 255, 255, 255),
             xy(505, 400)});
      s.add({rgb(0x3A, 64, 32, 16),
             xy(300, 240),
             rgb(0, 16, 32, 64),
             xy(480, 250),
             rgb(0, 100, 0, 100),
             xy(290, 360),
             rgb(0, 1, 2, 3),
             xy(470, 390)});
      CHECK_REPLAY(s);
    }
  }
}

static void test_textured_polygons(void) {
  for (int mode = 0; mode < 3; mode++) {
    for (int semi = 0; semi < 4; semi++) {
      Stream s = baseState();
      const std::uint32_t texture = page(9, 0, semi, mode);
      const std::uint32_t clut = clutAt(512, 480 + semi);
      s.add({drawMode(texture, true, false, false)});
      s.add({rgb(0x24, 128, 128, 128),
             xy(10, 10),
             uv(0, 0, clut),
             xy(200, 30),
             uv(200, 10, texture),
             xy(40, 200),
             uv(20, 250, 0)});
      s.add({rgb(0x25, 0, 0, 0),
             xy(220, 10),
             uv(5, 5, clut),
             xy(420, 20),
             uv(80, 5, texture),
             xy(230, 90),
             uv(5, 80, 0)});
      s.add({rgb(0x26, 200, 60, 90),
             xy(260, 100),
             uv(0, 0, clut),
             xy(500, 140),
             uv(255, 0, texture),
             xy(280, 260),
             uv(0, 255, 0)});
      s.add({rgb(0x2C, 90, 100, 200),
             xy(10, 260),
             uv(0, 0, clut),
             xy(130, 260),
             uv(64, 0, texture),
             xy(10, 380),
             uv(0, 64, 0),
             xy(130, 380),
             uv(64, 64, 0)});
      s.add({rgb(0x2F, 0, 0, 0),
             xy(150, 270),
             uv(64, 0, clut),
             xy(250, 290),
             uv(0, 0, texture),
             xy(140, 400),
             uv(64, 64, 0),
             xy(260, 410),
             uv(0, 64, 0)});
      s.add({rgb(0x34, 255, 0, 0),
             xy(280, 300),
             uv(10, 10, clut),
             rgb(0, 0, 255, 0),
             xy(500, 320),
             uv(100, 20, texture),
             rgb(0, 0, 0, 255),
             xy(300, 500),
             uv(30, 120, 0)});
      s.add({rgb(0x3E, 40, 80, 160),
             xy(20, 420),
             uv(0, 0, clut),
             rgb(0, 160, 80, 40),
             xy(200, 410),
             uv(127, 0, texture),
             rgb(0, 255, 255, 255),
             xy(30, 505),
             uv(0, 63, 0),
             rgb(0, 0, 0, 0),
             xy(210, 500),
             uv(127, 63, 0)});
      CHECK_REPLAY(s);
    }
  }
}

// CTR's additive gouraud textured fade triangles: one vertex bright, two black, drawn under a row offset.
static void test_steep_gouraud_textured_semi_triangles(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(14, 0, 1, 0);
  const std::uint32_t clut = clutAt(912, 254);
  s.area(0, 296, 511, 511);
  s.offset(0, 296);
  s.add({drawMode(texture, true, false, false)});
  s.add({rgb(0x36, 0, 0, 0),
         xy(-20, 82),
         uv(191, 48, clut),
         rgb(0, 253, 255, 255),
         xy(42, 90),
         uv(191, 95, texture),
         rgb(0, 0, 0, 0),
         xy(9, 68),
         uv(144, 48, 0)});
  s.add({rgb(0x36, 0, 0, 0),
         xy(2, 97),
         uv(144, 48, clut),
         rgb(0, 0, 0, 0),
         xy(-20, 82),
         uv(144, 95, texture),
         rgb(0, 253, 255, 255),
         xy(42, 90),
         uv(191, 95, 0)});
  CHECK_REPLAY(s);
}

// gpu.c fetches texels through a cache that only an upload, copy or read invalidates, and a draw reads the
// pixels it wrote earlier; the rasterizer's snapshot read must still end on the device's picture.
static void test_a_draw_that_samples_its_own_pixels(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(0, 0, 0, 2);
  s.add({drawMode(texture, false, false, false)});
  s.add({rgb(0x25, 128, 128, 128), xy(4, 4), uv(12, 9, 0), xy(70, 6), uv(90, 7, texture), xy(30, 80), uv(30, 75, 0)});
  s.add({rgb(0x2D, 128, 128, 128),
         xy(10, 100),
         uv(0, 98, 0),
         xy(90, 100),
         uv(80, 98, texture),
         xy(10, 150),
         uv(5, 120, 0),
         xy(90, 150),
         uv(85, 130, 0)});
  CHECK_REPLAY(s);
}

static void test_a_draw_over_a_cached_texture_is_sampled_stale(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(0, 0, 0, 2);
  s.add({drawMode(texture, false, false, false)});
  const auto sample = [&](int x, int y) {
    s.add({rgb(0x25, 128, 128, 128),
           xy(x, y),
           uv(0, 0, 0),
           xy(x + 60, y),
           uv(60, 0, texture),
           xy(x, y + 60),
           uv(0, 60, 0)});
  };
  sample(300, 300);
  s.add({rgb(0x20, 200, 30, 90), xy(0, 0), xy(120, 0), xy(0, 120)});
  sample(300, 400);
  CHECK_REPLAY(s);
}

// gpu.c SetTPage invalidates its texture cache when the page changes, from E1 or from a textured polygon's own
// texpage word; a texture read after that sees current VRAM and needs no device-resolved pixels.
static void test_a_texpage_change_invalidates_the_cache_through_e1(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(0, 0, 0, 2);
  const std::uint32_t other = page(2, 0, 0, 2);
  s.add({drawMode(texture, false, false, false)});
  s.add({rgb(0x20, 200, 30, 90), xy(0, 0), xy(120, 0), xy(0, 120)});
  s.add({drawMode(other, false, false, false)});
  s.add({drawMode(texture, false, false, false)});
  s.add({rgb(0x25, 128, 128, 128),
         xy(300, 300),
         uv(0, 0, 0),
         xy(360, 300),
         uv(60, 0, texture),
         xy(300, 360),
         uv(0, 60, 0)});
  const Outcome outcome = replay(s);
  CHECK(outcome.replayed);
  CHECK_EQ(outcome.mismatched, 0);
  CHECK_EQ(outcome.uploads, 0u);
}

static void test_a_texpage_change_invalidates_the_cache_through_a_polygon(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(0, 0, 0, 2);
  const std::uint32_t other = page(2, 0, 0, 2);
  s.add({drawMode(texture, false, false, false)});
  s.add({rgb(0x20, 200, 30, 90), xy(0, 0), xy(120, 0), xy(0, 120)});
  s.add({rgb(0x25, 128, 128, 128),
         xy(300, 300),
         uv(0, 0, 0),
         xy(360, 300),
         uv(60, 0, other),
         xy(300, 360),
         uv(0, 60, 0)});
  s.add({rgb(0x25, 128, 128, 128),
         xy(300, 400),
         uv(0, 0, 0),
         xy(360, 400),
         uv(60, 0, texture),
         xy(300, 460),
         uv(0, 60, 0)});
  const Outcome outcome = replay(s);
  CHECK(outcome.replayed);
  CHECK_EQ(outcome.mismatched, 0);
  CHECK_EQ(outcome.uploads, 0u);
}

static void test_texture_window(void) {
  Stream s = baseState();
  const std::uint32_t texture = page(10, 0, 0, 1);
  s.add({drawMode(texture, false, false, false)});
  // Mask 3 (24 texels), offset 2 on x; mask 1, offset 1 on y.
  s.add({0xE2000000u | 3u | (1u << 5) | (2u << 10) | (1u << 15)});
  s.add({rgb(0x2C, 128, 128, 128),
         xy(10, 10),
         uv(0, 0, clutAt(512, 400)),
         xy(300, 10),
         uv(255, 0, texture),
         xy(10, 300),
         uv(0, 255, 0),
         xy(300, 300),
         uv(255, 255, 0)});
  s.add({rgb(0x64, 128, 128, 128), xy(320, 10), uv(3, 7, clutAt(512, 401)), xy(150, 120)});
  CHECK_REPLAY(s);
}

static void test_sprites(void) {
  for (int flip = 0; flip < 4; flip++) {
    Stream s = baseState();
    const std::uint32_t texture = page(11, 1, 1, flip % 3);
    s.add({drawMode(texture, true, (flip & 1) != 0, (flip & 2) != 0)});
    s.add({rgb(0x60, 50, 100, 150), xy(5, 5), xy(37, 19)});
    s.add({rgb(0x62, 250, 10, 100), xy(20, 10), xy(40, 30)});
    s.add({rgb(0x68, 255, 255, 255), xy(100, 3)});
    s.add({rgb(0x70, 0, 255, 0), xy(110, 3)});
    s.add({rgb(0x78, 0, 0, 255), xy(120, 3)});
    s.add({rgb(0x64, 128, 128, 128), xy(10, 60), uv(13, 200, clutAt(528, 300)), xy(120, 90)});
    s.add({rgb(0x65, 0, 0, 0), xy(140, 60), uv(250, 250, clutAt(528, 301)), xy(80, 70)}); // u, v wrap
    s.add({rgb(0x66, 60, 200, 128), xy(240, 60), uv(0, 0, clutAt(528, 302)), xy(100, 100)});
    s.add({rgb(0x74, 200, 100, 50), xy(10, 200), uv(9, 9, clutAt(528, 303))});
    s.add({rgb(0x7C, 128, 128, 128), xy(30, 200), uv(31, 17, clutAt(528, 304))});
    s.add({rgb(0x6C, 255, 128, 64), xy(60, 200), uv(1, 2, clutAt(528, 305))});
    s.offset(-20, -15); // clipped at the draw area's top-left
    s.add({rgb(0x64, 128, 128, 128), xy(0, 0), uv(100, 100, clutAt(528, 306)), xy(64, 64)});
    CHECK_REPLAY(s);
  }
}

static void test_lines(void) {
  for (int semi = 0; semi < 4; semi++) {
    Stream s = baseState();
    s.add({drawMode(page(8, 0, semi, 0), semi % 2 == 0, false, false)});
    s.add({rgb(0x40, 255, 128, 0), xy(10, 10), xy(300, 100)});
    s.add({rgb(0x42, 0, 128, 255), xy(300, 10), xy(10, 120)});
    s.add({rgb(0x40, 9, 99, 199), xy(50, 300), xy(50, 300)});
    s.add({rgb(0x50, 255, 0, 0), xy(20, 200), rgb(0, 0, 0, 255), xy(480, 260)});
    s.add({rgb(0x52, 0, 255, 0), xy(200, 500), rgb(0, 255, 0, 255), xy(210, 150)});
    s.add({rgb(0x48, 200, 200, 200), xy(100, 400), xy(150, 350), xy(250, 450), xy(400, 380), 0x55555555u});
    s.add({rgb(0x5A, 255, 255, 0),
           xy(300, 300),
           rgb(0, 0, 255, 255),
           xy(350, 500),
           rgb(0, 255, 0, 255),
           xy(500, 320),
           0x50005000u});
    s.offset(-30, 600); // vertices wrap at 11 bits
    s.add({rgb(0x40, 77, 177, 77), xy(40, -580), xy(470, -500)});
    CHECK_REPLAY(s);
  }
}

static void test_clip_offset_and_mask(void) {
  Stream s = baseState();
  s.area(40, 30, 300, 200);
  s.offset(25, -10);
  s.add({rgb(0x28, 120, 50, 200), xy(-100, -100), xy(500, -80), xy(-90, 400), xy(480, 420)});
  s.mask(true, false);
  s.add({rgb(0x20, 255, 255, 0), xy(0, 0), xy(200, 20), xy(30, 180)});
  s.mask(false, true);
  s.add({rgb(0x30, 0, 255, 255), xy(10, 10), rgb(0, 255, 0, 0), xy(260, 40), rgb(0, 0, 0, 255), xy(40, 200)});
  s.add({rgb(0x60, 9, 9, 9), xy(100, 50), xy(100, 100)});
  CHECK_REPLAY(s);
}

static void test_interlaced_row_skip(void) {
  Stream s;
  s.area(0, 0, 511, 511);
  s.offset(0, 0);
  // Drawing to the display area disallowed (E1 bit 10 clear) in 480-line interlace: one field only.
  s.add({0xE1000000u | page(8, 0, 0, 0)});
  s.add({rgb(0x28, 200, 30, 30), xy(0, 0), xy(320, 0), xy(0, 480), xy(320, 480)});
  s.add({rgb(0x60, 30, 200, 30), xy(330, 10), xy(100, 100)});
  s.add({rgb(0x40, 30, 30, 200), xy(330, 200), xy(500, 300)});
  CHECK_REPLAY(s, {0x08000000u | 0x24u | 0x01u});
}

// Seeded random primitives under random state; the bounds in the file comment are kept.
static void test_random_streams(void) {
  std::mt19937 random(1234);
  const auto pick = [&](int low, int high) {
    return std::uniform_int_distribution<int>(low, high)(random);
  };
  for (int round = 0; round < 6; round++) {
    Stream s = baseState();
    for (int n = 0; n < 400; n++) {
      const int kind = pick(0, 9);
      const std::uint32_t texture = page(pick(8, 15), pick(0, 1), pick(0, 3), pick(0, 2));
      const std::uint32_t clut = clutAt(pick(32, 63) * 16, pick(0, 511));
      const auto vertex = [&] {
        return xy(pick(-80, 560), pick(-60, 560));
      };
      const auto colour = [&](std::uint32_t command) {
        return rgb(command, pick(0, 255), pick(0, 255), pick(0, 255));
      };
      if (kind == 0) {
        s.add({drawMode(texture, pick(0, 1) != 0, pick(0, 1) != 0, pick(0, 1) != 0)});
        s.mask(pick(0, 3) == 0, pick(0, 3) == 0);
        const int x0 = pick(0, 200);
        const int y0 = pick(0, 200);
        s.area(x0, y0, pick(x0, 511), pick(y0, 511));
        s.offset(pick(-64, 64), pick(-64, 64));
        s.add({0xE2000000u | (pick(0, 3) == 0 ? static_cast<std::uint32_t>(random() & 0xFFFFFu) : 0u)});
      } else if (kind <= 3) {
        const std::uint32_t command = 0x20u | static_cast<std::uint32_t>(pick(0, 31) & 0x1F);
        const bool gouraud = (command & 0x10u) != 0;
        const bool textured = (command & 0x04u) != 0;
        const int count = (command & 0x08u) != 0 ? 4 : 3;
        for (int v = 0; v < count; v++) {
          if (v == 0 || gouraud) {
            s.add({colour(v == 0 ? command : 0)});
          }
          s.add({vertex()});
          if (textured) {
            s.add({uv(pick(0, 255), pick(0, 255), v == 0 ? clut : v == 1 ? texture : 0)});
          }
        }
      } else if (kind <= 5) {
        const std::uint32_t command = 0x60u | static_cast<std::uint32_t>(pick(0, 31) & 0x1F);
        s.add({colour(command), vertex()});
        if ((command & 0x04u) != 0) {
          s.add({uv(pick(0, 255), pick(0, 255), clut)});
        }
        if ((command & 0x18u) == 0) {
          s.add({xy(pick(0, 300), pick(0, 300))});
        }
      } else if (kind == 6) {
        const std::uint32_t command = 0x40u | static_cast<std::uint32_t>(pick(0, 31) & 0x1A);
        const bool gouraud = (command & 0x10u) != 0;
        const int points = (command & 0x08u) != 0 ? pick(2, 6) : 2;
        for (int p = 0; p < points; p++) {
          if (p == 0 || gouraud) {
            s.add({colour(p == 0 ? command : 0)});
          }
          s.add({vertex()});
        }
        if ((command & 0x08u) != 0) {
          s.add({0x55555555u});
        }
      } else if (kind == 7) {
        s.add({colour(0x02), xy(pick(0, 400), pick(0, 500)), xy(pick(0, 96), pick(0, 100))});
      } else if (kind == 8) {
        s.add({0x80000000u,
               xy(pick(0, 1023), pick(0, 511)),
               xy(pick(0, 1023), pick(0, 511)),
               xy(pick(1, 96), pick(1, 64))});
      } else {
        s.upload(pick(0, 1023), pick(0, 511), pick(1, 40), pick(1, 20), static_cast<std::uint32_t>(random()));
      }
    }
    CHECK_REPLAY(s);
  }
}

int main(void) {
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);
  if (!SDL_Init(SDL_INIT_VIDEO) ||
      (gDevice = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr)) == nullptr) {
    fprintf(stderr, "    FAIL no headless Vulkan device: %s\n", SDL_GetError());
    return 1;
  }
  RUN(fills);
  RUN(copies);
  RUN(uploads);
  RUN(a_scaled_upload_and_a_read_of_it_fill_whole_blocks);
  RUN(flat_and_gouraud_polygons);
  RUN(textured_polygons);
  RUN(steep_gouraud_textured_semi_triangles);
  RUN(a_draw_that_samples_its_own_pixels);
  RUN(a_draw_over_a_cached_texture_is_sampled_stale);
  RUN(a_texpage_change_invalidates_the_cache_through_e1);
  RUN(a_texpage_change_invalidates_the_cache_through_a_polygon);
  RUN(texture_window);
  RUN(sprites);
  RUN(lines);
  RUN(clip_offset_and_mask);
  RUN(interlaced_row_skip);
  RUN(random_streams);
  SDL_DestroyGPUDevice(gDevice);
  SDL_Quit();
  return pt_summary();
}
