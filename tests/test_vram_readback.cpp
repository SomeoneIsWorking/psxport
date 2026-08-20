// test_vram_readback — GP0(0xC0), VRAM->CPU readback: the SAVE half of a guest's
// save / modify / restore round-trip over a VRAM region.
//
// WHAT WAS BROKEN (spider1 issue 0007). psxport recognised GP0(0xC0) only as a 3-word FIFO header
// (gpu_native.cpp gp0_len) and then dropped it: no pixels were ever returned to the guest, through
// either of the two paths the PSX offers (the GPUREAD register at 0x1F801810, and DMA channel 2 in
// the VRAM->CPU direction, CHCR bit0 == 0). A guest that SAVES a VRAM rect to RAM and later RESTORES
// it with GP0(0xA0) therefore uploaded whatever its destination buffer already held. In spider1 that
// buffer holds the guest allocator's 0x33333333 poison, and the restore wrote poison over the live
// CLUT strip — every textured surface in the 3D world sampled the constant 0x3333 (pale green) and
// the world rendered untextured. The failure was SILENT by construction: nothing returned nothing.
//
// WHY THIS TEST IS SHAPED AS A ROUND-TRIP. Asserting "GPUREAD returns pixel P" alone would pass on a
// readback that is right in isolation but mis-ordered against the A0 upload's addressing. The bug
// that actually shipped is a round-trip bug, so the headline case (test_save_restore_roundtrip) is
// the round trip: poison a RAM buffer, C0 it full of VRAM, A0 it back, and demand VRAM is bit-identical.
// It fails today for the exact reason the game fails.
//
// HERMETIC: a Game is constructed in-process; no disc, no window, no Vulkan device. `soft_gpu = 1`
// is the framework's existing oracle mode (gpu_native_internal.h) and keeps the GPU off the VK
// backend — it is not a test-only branch. Nothing here presents or rasterises; only the GP0 command
// stream, the GPUREAD register and the DMA2 block engine are exercised.
//
// DENOMINATORS: every case asserts the COUNT of halfwords/words it compared, so a case that
// compared nothing cannot read as a pass (testutil.h also fails a case that asserts nothing).

#include "../runtime/recomp/game.h"
#include "testutil.h"

// ---- helpers ----------------------------------------------------------------------------------

// One Game per test process is plenty (2 MB RAM + 1 MB VRAM as members — keep it off the stack).
static Game *gam() {
  static Game g;
  static bool once = false;
  // Software rasterizer: there is no VK device in a hermetic test. That is now a property of the
  // Core's RENDER PATH (RenderPath::Psx) rather than a GpuState flag — see render_mode.h.
  if (!once) {
    once = true;
    g.core.rsub.mode.setPath(RenderPath::Psx);
  }
  return &g;
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

static void fill_vram(Game *g, int x0, int y0, int w, int h) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      *g->gpu.vram(x0 + x, y0 + y) = pat(x0 + x, y0 + y);
    }
  }
}

// Issue the GP0(0xC0) header through the ordinary command port, exactly as a guest does.
static void gp0_c0(Game *g, int x, int y, int w, int h) {
  gpu_gp0(&g->core, 0xC0000000u);
  gpu_gp0(&g->core, coord_word(x, y));
  gpu_gp0(&g->core, size_word(w, h));
}

// Issue a GP0(0xA0) upload header + its pixel stream from a guest RAM buffer, via DMA2 to_gpu=1 —
// the same route the game uses for the restore half.
static void a0_upload_from_ram(Game *g, int x, int y, int w, int h, uint32_t madr, int words) {
  gpu_gp0(&g->core, 0xA0000000u);
  gpu_gp0(&g->core, coord_word(x, y));
  gpu_gp0(&g->core, size_word(w, h));
  gpu_dma2_block(&g->core, madr, words, /*to_gpu=*/1);
}

// Drive a DMA2 block transfer in the VRAM->CPU direction through the DMA registers, as the guest
// does: MADR, BCR, then CHCR with bit24 (start) and bit0 clear (to RAM), sync mode 0 (immediate).
static void dma2_read_to_ram(Game *g, uint32_t madr, int words) {
  g->core.mem_w32(0x1F8010A0u, madr);
  g->core.mem_w32(0x1F8010A4u, (uint32_t)words);
  g->core.mem_w32(0x1F8010A8u, 0x01000000u); // start, direction = to RAM, sync = immediate
}

// ---- cases ------------------------------------------------------------------------------------

// THE HEADLINE CASE — the bug that shipped. Save a rect with C0+DMA2, restore it with A0+DMA2 from
// the same RAM buffer, and require VRAM to be unchanged. Today the C0 returns nothing, the buffer
// keeps its 0x33333333 poison, and the A0 paints the rect solid 0x3333 — exactly spider1's palettes.
static void test_save_restore_roundtrip(void) {
  Game *g = gam();
  const int X = 512, Y = 0, W = 16, H = 4; // a CLUT-strip-shaped rect
  const uint32_t BUF = 0x80100000u;        // guest RAM destination
  const int words = W * H / 2;

  fill_vram(g, X, Y, W, H);
  for (int i = 0; i < words; i++) {
    g->core.mem_w32(BUF + 4u * i, 0x33333333u); // allocator poison
  }

  gp0_c0(g, X, Y, W, H);
  dma2_read_to_ram(g, BUF, words);

  // The saved buffer must hold the pixels, not the poison.
  int poison = 0;
  for (int i = 0; i < words; i++) {
    if (g->core.mem_r32(BUF + 4u * i) == 0x33333333u) {
      poison++;
    }
  }
  CHECK_EQ(words, 32); // denominator, stated
  CHECK_EQ(poison, 0);

  a0_upload_from_ram(g, X, Y, W, H, BUF, words);

  int compared = 0, diff = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      compared++;
      if (*g->gpu.vram(X + x, Y + y) != pat(X + x, Y + y)) {
        diff++;
      }
    }
  }
  CHECK_EQ(compared, W * H);
  CHECK_EQ(diff, 0);
}

// GPUREAD (0x1F801810 read) is the other path the PSX offers. Two pixels per word, low halfword
// first, row-major across the rect.
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

// Reading PAST the end of the rect must not keep streaming VRAM: the transfer is over, and the
// register falls back to its idle value. (Without this a runaway guest read would walk VRAM.)
static void test_read_past_end_stops(void) {
  Game *g = gam();
  const int X = 700, Y = 100, W = 4, H = 1;
  fill_vram(g, X, Y, W, H);
  gp0_c0(g, X, Y, W, H);
  uint32_t w0 = g->core.mem_r32(0x1F801810u);
  uint32_t w1 = g->core.mem_r32(0x1F801810u);
  CHECK_EQ(w0, (uint32_t)pat(X, Y) | ((uint32_t)pat(X + 1, Y) << 16));
  CHECK_EQ(w1, (uint32_t)pat(X + 2, Y) | ((uint32_t)pat(X + 3, Y) << 16));
  CHECK_EQ(g->core.mem_r32(0x1F801810u), 0u); // rect exhausted
  CHECK_EQ(g->core.mem_r32(0x1F801810u), 0u);
}

// WRAP: the readback must wrap in X at 1024 and in Y at 512 exactly as every other VRAM transfer
// path in gpu_native.cpp does (`vram()` masks &1023 / &511). Matched, not invented.
static void test_wraparound_matches_other_paths(void) {
  Game *g = gam();
  const int X = 1020, Y = 511, W = 8, H = 2; // wraps in both axes
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      *g->gpu.vram(X + x, Y + y) = pat(X + x, Y + y);
    }
  }
  gp0_c0(g, X, Y, W, H);

  int compared = 0, diff = 0;
  for (int i = 0; i < W * H / 2; i++) {
    uint32_t got = g->core.mem_r32(0x1F801810u);
    uint32_t want = (uint32_t)*g->gpu.vram(X + (2 * i) % W, Y + (2 * i) / W) |
                    ((uint32_t)*g->gpu.vram(X + (2 * i + 1) % W, Y + (2 * i + 1) / W) << 16);
    compared++;
    if (got != want) {
      diff++;
    }
  }
  CHECK_EQ(compared, 8);
  CHECK_EQ(diff, 0);
}

// A zero size field means the maximum (1024 / 512), the same convention gp0's A0 arm already uses.
// Asserted on the DECODED rect via how many words the stream yields before it stops.
static void test_zero_size_is_max(void) {
  Game *g = gam();
  fill_vram(g, 0, 300, 1024, 1); // pat() never yields 0 (bit 15 is always set)
  gp0_c0(g, 0, 300, 0, 1);       // width field 0 -> 1024 px
  int nonzero = 0;
  for (int i = 0; i < 600; i++) {
    if (g->core.mem_r32(0x1F801810u) != 0u) {
      nonzero++;
    }
  }
  CHECK_EQ(nonzero, 512); // exactly one 1024-px row, 2 px per word
}

int main(void) {
  RUN(save_restore_roundtrip);
  RUN(gpuread_register_path);
  RUN(read_past_end_stops);
  RUN(wraparound_matches_other_paths);
  RUN(zero_size_is_max);
  return pt_summary();
}
