#include "testutil.h"
#include "wide_margin_plan.h"

// At 15bpp a display pixel IS a VRAM halfword, so the plan's rect is the display extension itself.
static void test_spyro_extension_only(void) {
  const WideMarginPlan p = plan_wide_margin(0, 248, 512, 684, 240, /*rgb24=*/false);
  CHECK(p.draw);
  CHECK_EQ(p.x0, 512);
  CHECK_EQ(p.x1, 684);
  CHECK_EQ(p.y0, 248);
  CHECK_EQ(p.y1, 488);
}

static void test_origin_is_preserved(void) {
  const WideMarginPlan p = plan_wide_margin(32, 8, 320, 428, 224, /*rgb24=*/false);
  CHECK(p.draw);
  CHECK_EQ(p.x0, 352);
  CHECK_EQ(p.x1, 460);
  CHECK_EQ(p.y0, 8);
  CHECK_EQ(p.y1, 232);
}

// THE REGRESSION THIS FILE DID NOT COVER (spyro issue 0118). Spyro's Universal boot logo is an
// upload-only guest-VRAM picture in 512x240 24bpp, widened to 684. Built from display widths alone
// the rect is halfwords [512,684), which the 24bpp present samples as display columns [341,456) — a
// black band through the middle of the picture, while the real margin is never covered. A 24bpp
// pixel spans 1.5 halfwords, so the extension starts at halfword 768 and the clamp below is what
// keeps it inside VRAM.
static void test_24bpp_margin_is_in_halfwords(void) {
  const WideMarginPlan p = plan_wide_margin(0, 0, 512, 684, 240, /*rgb24=*/true);
  CHECK(p.draw);
  CHECK_EQ(p.x0, 768);  // 512 display columns * 3/2
  CHECK_EQ(p.x1, 1024); // 684 * 3/2 = 1026, clamped to the width of VRAM
  CHECK_EQ(p.y0, 0);
  CHECK_EQ(p.y1, 240);
  // It must NOT be the 15bpp answer, which is the band that was measured on screen.
  CHECK(p.x0 != 512);
  CHECK(p.x1 != 684);
}

// A 320-wide 24bpp display widened to 428 stays inside VRAM, so the clamp is not what makes the
// conversion right — the conversion is.
static void test_24bpp_narrow_display_needs_no_clamp(void) {
  const WideMarginPlan p = plan_wide_margin(0, 16, 320, 428, 224, /*rgb24=*/true);
  CHECK(p.draw);
  CHECK_EQ(p.x0, 480); // 320 * 3/2
  CHECK_EQ(p.x1, 642); // 428 * 3/2
  CHECK_EQ(p.y0, 16);
  CHECK_EQ(p.y1, 240);
}

// The two depths must genuinely disagree, or the parameter is decorative.
static void test_the_two_depths_differ(void) {
  const WideMarginPlan flat = plan_wide_margin(0, 0, 512, 684, 240, /*rgb24=*/false);
  const WideMarginPlan deep = plan_wide_margin(0, 0, 512, 684, 240, /*rgb24=*/true);
  CHECK(flat.draw && deep.draw);
  CHECK(flat.x0 != deep.x0);
  CHECK(flat.x1 != deep.x1);
}

// A 24bpp extension that begins at or past the end of VRAM has nothing to cover, and must say so
// rather than emit an inverted or zero-width rect for the rasterizer to sort out.
static void test_24bpp_extension_past_vram_is_a_noop(void) {
  CHECK(!plan_wide_margin(0, 0, 683, 684, 240, /*rgb24=*/true).draw); // x0 = 1024 = end of VRAM
  CHECK(!plan_wide_margin(400, 0, 512, 684, 240, /*rgb24=*/true).draw);
}

static void test_4_3_and_invalid_are_noops(void) {
  CHECK(!plan_wide_margin(0, 0, 512, 512, 240, /*rgb24=*/false).draw);
  CHECK(!plan_wide_margin(0, 0, 512, 400, 240, /*rgb24=*/false).draw);
  CHECK(!plan_wide_margin(0, 0, 0, 684, 240, /*rgb24=*/false).draw);
  CHECK(!plan_wide_margin(0, 0, 512, 684, 0, /*rgb24=*/false).draw);
  CHECK(!plan_wide_margin(0, 0, 512, 512, 240, /*rgb24=*/true).draw);
  CHECK(!plan_wide_margin(0, 0, 512, 684, 0, /*rgb24=*/true).draw);
}

// A guest that draws its own margin keeps the host base out of the rows it draws in the displayed
// buffer. Spyro 2 displays (0,0) for 240 rows and draws rows 12..227 there, so only the 12-row
// letterbox above and below gets the base; the rows it drew one composite earlier are left alone.
static void test_guest_drawn_rows_leave_only_the_letterbox(void) {
  const WideMarginPlan margin = plan_wide_margin(0, 0, 512, 684, 240, /*rgb24=*/false);
  const WideMarginBands bands = host_margin_bands(margin, psx::gpu::RowSpan{12, 228});
  CHECK_EQ(bands.count, 2);
  CHECK_EQ(bands.band[0].y0, 0);
  CHECK_EQ(bands.band[0].y1, 12);
  CHECK_EQ(bands.band[1].y0, 228);
  CHECK_EQ(bands.band[1].y1, 240);
  CHECK_EQ(bands.band[1].x0, 512);
  CHECK_EQ(bands.band[1].x1, 684);
}

// With no guest-drawn rows the base is the whole margin, exactly the plan.
static void test_no_guest_rows_is_the_whole_margin(void) {
  const WideMarginPlan margin = plan_wide_margin(0, 228, 512, 684, 240, /*rgb24=*/false);
  const WideMarginBands bands = host_margin_bands(margin, std::nullopt);
  CHECK_EQ(bands.count, 1);
  CHECK_EQ(bands.band[0].y0, 228);
  CHECK_EQ(bands.band[0].y1, 468);
}

// A guest that draws every displayed row leaves the host nothing to cover, and a 4:3 plan never draws.
static void test_fully_drawn_and_4_3_lay_no_base(void) {
  const WideMarginPlan margin = plan_wide_margin(0, 0, 512, 684, 240, /*rgb24=*/false);
  CHECK_EQ(host_margin_bands(margin, psx::gpu::RowSpan{0, 240}).count, 0);
  CHECK_EQ(host_margin_bands(plan_wide_margin(0, 0, 512, 512, 240, false), std::nullopt).count, 0);
}

int main(void) {
  RUN(spyro_extension_only);
  RUN(origin_is_preserved);
  RUN(24bpp_margin_is_in_halfwords);
  RUN(24bpp_narrow_display_needs_no_clamp);
  RUN(the_two_depths_differ);
  RUN(24bpp_extension_past_vram_is_a_noop);
  RUN(4_3_and_invalid_are_noops);
  RUN(guest_drawn_rows_leave_only_the_letterbox);
  RUN(no_guest_rows_is_the_whole_margin);
  RUN(fully_drawn_and_4_3_lay_no_base);
  return pt_summary();
}
