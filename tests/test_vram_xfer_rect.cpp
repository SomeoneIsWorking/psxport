// GP0 A0 (CPU->VRAM) and C0 (VRAM->CPU) share vram_xfer_rect().  A disagreement
// here corrupts save/modify/restore flows while each direction can still look correct alone.
#include "testutil.h"
#include "../runtime/recomp/gpu_native_internal.h"

static void test_decodes_coordinate_fields(void) {
  const VramRect r = vram_xfer_rect(0x01FF03FFu, 0x01FE03FDu);
  CHECK_EQ(r.x, 1023);
  CHECK_EQ(r.y, 511);
  CHECK_EQ(r.w, 1021);
  CHECK_EQ(r.h, 510);
}

static void test_masks_coordinate_bits_without_affecting_size(void) {
  const VramRect r = vram_xfer_rect(0xFFFFFC01u, 0xABCD8203u);
  CHECK_EQ(r.x, 1);
  CHECK_EQ(r.y, 511);
  CHECK_EQ(r.w, 515);
  CHECK_EQ(r.h, 461);
}

// Zero is not an empty transfer: the GP0 encoding uses it for the full dimension.
static void test_zero_size_means_full_vram(void) {
  const VramRect r = vram_xfer_rect(0u, 0u);
  CHECK_EQ(r.x, 0);
  CHECK_EQ(r.y, 0);
  CHECK_EQ(r.w, VRAM_W);
  CHECK_EQ(r.h, VRAM_H);
}

static void test_zero_width_and_height_are_independent(void) {
  const VramRect width = vram_xfer_rect(0x00400080u, 0x00070000u);
  const VramRect height = vram_xfer_rect(0x00C00300u, 0x00000123u);
  CHECK_EQ(width.x, 128);
  CHECK_EQ(width.y, 64);
  CHECK_EQ(width.w, VRAM_W);
  CHECK_EQ(width.h, 7);
  CHECK_EQ(height.x, 768);
  CHECK_EQ(height.y, 192);
  CHECK_EQ(height.w, 291);
  CHECK_EQ(height.h, VRAM_H);
}

int main(void) {
  RUN(decodes_coordinate_fields);
  RUN(masks_coordinate_bits_without_affecting_size);
  RUN(zero_size_means_full_vram);
  RUN(zero_width_and_height_are_independent);
  return pt_summary();
}
