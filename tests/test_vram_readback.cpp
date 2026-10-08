// test_vram_readback — GP0(0xC0), VRAM->CPU readback, through the two routes a guest drains it: the
// GPUREAD register (0x1F801810) and DMA channel 2 in the VRAM->CPU direction.
//
// The headline case is the round trip spider1 issue 0007 broke: save a rect to RAM with C0+DMA2,
// restore it with A0+DMA2 from the same buffer, and require the GPU device's VRAM to be unchanged.
// Every case counts what it compared, so a case that compared nothing cannot pass.

#include "game.h"
#include "gpu_device.h"
#include "testutil.h"

#include <memory>

// ---- helpers ----------------------------------------------------------------------------------

// One Game per test process (2 MB RAM + 1 MB VRAM as members — keep it off the stack).
static Game *gam() {
  static const std::unique_ptr<Game> game = std::make_unique<Game>();
  return game.get();
}

static uint32_t coord_word(int x, int y) {
  return (uint32_t)(x & 0x3FF) | ((uint32_t)(y & 0x1FF) << 16);
}
static uint32_t size_word(int w, int h) {
  return (uint32_t)(w & 0x3FF) | ((uint32_t)(h & 0x1FF) << 16);
}

// A recognisable, position-dependent halfword so a mis-addressed read is visible rather than lucky.
static uint16_t pat(int x, int y) {
  return (uint16_t)(((x * 7 + y * 131) & 0x7FFF) | 0x8000);
}

static uint16_t device_pixel(Game *g, int x, int y) {
  const auto vram = g->core.gpuDevice.vram();
  return vram[(size_t)(y & (psx::gpu::kDeviceVramHeight - 1)) * psx::gpu::kDeviceVramWidth +
              (size_t)(x & (psx::gpu::kDeviceVramWidth - 1))];
}

// Upload pat() over a rect with GP0(A0) through the command port, as a guest's LoadImage does.
static void fill_vram(Game *g, int x0, int y0, int w, int h) {
  gpu_gp0(&g->core, 0xA0000000u);
  gpu_gp0(&g->core, coord_word(x0, y0));
  gpu_gp0(&g->core, size_word(w, h));
  const int count = w * h;
  for (int i = 0; i < count; i += 2) {
    const uint32_t low = pat(x0 + i % w, y0 + i / w);
    const uint32_t high = i + 1 < count ? pat(x0 + (i + 1) % w, y0 + (i + 1) / w) : 0u;
    gpu_gp0(&g->core, low | (high << 16));
  }
}

// Issue the GP0(0xC0) header through the ordinary command port, exactly as a guest does.
static void gp0_c0(Game *g, int x, int y, int w, int h) {
  gpu_gp0(&g->core, 0xC0000000u);
  gpu_gp0(&g->core, coord_word(x, y));
  gpu_gp0(&g->core, size_word(w, h));
}

// GP0(0xA0) header, then its pixel stream from a guest RAM buffer via DMA2 to_gpu=1.
static void a0_upload_from_ram(Game *g, int x, int y, int w, int h, uint32_t madr, int words) {
  gpu_gp0(&g->core, 0xA0000000u);
  gpu_gp0(&g->core, coord_word(x, y));
  gpu_gp0(&g->core, size_word(w, h));
  gpu_dma2_block(&g->core, madr, words, /*to_gpu=*/1);
}

// DMA2 block transfer in the VRAM->CPU direction through the DMA registers, as the guest does.
static void dma2_read_to_ram(Game *g, uint32_t madr, int words) {
  g->core.mem_w32(0x1F8010A0u, madr);
  g->core.mem_w32(0x1F8010A4u, (uint32_t)words);
  g->core.mem_w32(0x1F8010A8u, 0x01000000u); // start, direction = to RAM, sync = immediate
}

// ---- cases ------------------------------------------------------------------------------------

static void test_save_restore_roundtrip(void) {
  Game *g = gam();
  const int X = 512, Y = 0, W = 16, H = 4; // a CLUT-strip-shaped rect
  const uint32_t BUF = 0x80100000u;
  const int words = W * H / 2;

  fill_vram(g, X, Y, W, H);
  for (int i = 0; i < words; i++) {
    g->core.mem_w32(BUF + 4u * i, 0x33333333u); // allocator poison
  }

  gp0_c0(g, X, Y, W, H);
  dma2_read_to_ram(g, BUF, words);

  int poison = 0;
  for (int i = 0; i < words; i++) {
    if (g->core.mem_r32(BUF + 4u * i) == 0x33333333u) {
      poison++;
    }
  }
  CHECK_EQ(words, 32);
  CHECK_EQ(poison, 0);

  a0_upload_from_ram(g, X, Y, W, H, BUF, words);

  int compared = 0, diff = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      compared++;
      if (device_pixel(g, X + x, Y + y) != pat(X + x, Y + y)) {
        diff++;
      }
    }
  }
  CHECK_EQ(compared, W * H);
  CHECK_EQ(diff, 0);
}

// Two pixels per GPUREAD word, low halfword first, row-major across the rect.
static void test_gpuread_register_path(void) {
  Game *g = gam();
  const int X = 640, Y = 32, W = 8, H = 2;
  fill_vram(g, X, Y, W, H);
  gp0_c0(g, X, Y, W, H);

  int compared = 0, diff = 0;
  for (int i = 0; i < W * H / 2; i++) {
    uint32_t got = g->core.mem_r32(0x1F801810u);
    uint32_t want = (uint32_t)pat(X + (2 * i) % W, Y + (2 * i) / W) |
                    ((uint32_t)pat(X + (2 * i + 1) % W, Y + (2 * i + 1) / W) << 16);
    compared++;
    if (got != want) {
      diff++;
    }
  }
  CHECK_EQ(compared, 8);
  CHECK_EQ(diff, 0);
}

// The readback wraps in X at 1024 and in Y at 512.
static void test_wraparound(void) {
  Game *g = gam();
  const int X = 1020, Y = 511, W = 8, H = 2;
  fill_vram(g, X, Y, W, H);
  gp0_c0(g, X, Y, W, H);

  int compared = 0, diff = 0;
  for (int i = 0; i < W * H / 2; i++) {
    uint32_t got = g->core.mem_r32(0x1F801810u);
    uint32_t want = (uint32_t)pat(X + (2 * i) % W, Y + (2 * i) / W) |
                    ((uint32_t)pat(X + (2 * i + 1) % W, Y + (2 * i + 1) / W) << 16);
    compared++;
    if (got != want) {
      diff++;
    }
  }
  CHECK_EQ(compared, 8);
  CHECK_EQ(diff, 0);
}

// A zero width field means 1024 pixels.
static void test_zero_size_is_max(void) {
  Game *g = gam();
  fill_vram(g, 0, 300, 1024, 1); // pat() never yields 0 (bit 15 is always set)
  gp0_c0(g, 0, 300, 0, 1);
  int matched = 0;
  for (int i = 0; i < 512; i++) {
    const uint32_t want = (uint32_t)pat(2 * i, 300) | ((uint32_t)pat(2 * i + 1, 300) << 16);
    if (g->core.mem_r32(0x1F801810u) == want) {
      matched++;
    }
  }
  CHECK_EQ(matched, 512);
}

int main(void) {
  RUN(save_restore_roundtrip);
  RUN(gpuread_register_path);
  RUN(wraparound);
  RUN(zero_size_is_max);
  return pt_summary();
}
