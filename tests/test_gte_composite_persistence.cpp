// A Gte render path replays the guest's ordering table into the PC rasterizer. The pixels still obey
// PSX framebuffer persistence: while the guest draws page A, the display scans the completed page B.
// Clearing the whole PC composite before drawing page A therefore erases the picture being scanned.
//
// X4 exposed the defect with an exact alternating pair. Immediately before present 180 the queued
// geometry occupied VRAM y=[0,256], while the display scanned sy=240. The next queue occupied
// y=[240,496], while the next present scanned sy=0. Both pages contained valid textured sprites; the
// software raster path showed 53,631 non-black pixels, while Gte showed none because every rebuild
// cleared both pages. This pure policy test guards the ownership distinction without needing a disc or
// GPU: Gte preserves an already-built composite, but the cold ownership build still initializes it.

#include "../runtime/recomp/gpu_vk_present_policy.h"
#include "testutil.h"

namespace {

void test_gte_preserves_the_completed_front_page() {
  CHECK(!preserve_composite_backdrop(/*guestVramIsPicture=*/false,
                                     /*swRasterIsPicture=*/false,
                                     /*guestGeometryPath=*/true,
                                     /*rebuildForOwnership=*/true));
  CHECK(preserve_composite_backdrop(/*guestVramIsPicture=*/false,
                                    /*swRasterIsPicture=*/false,
                                    /*guestGeometryPath=*/true,
                                    /*rebuildForOwnership=*/false));
}

void test_native_renderer_still_clears_each_rebuilt_frame() {
  CHECK(!preserve_composite_backdrop(/*guestVramIsPicture=*/false,
                                     /*swRasterIsPicture=*/false,
                                     /*guestGeometryPath=*/false,
                                     /*rebuildForOwnership=*/false));
}

void test_picture_backdrops_remain_independent_owners() {
  CHECK(preserve_composite_backdrop(/*guestVramIsPicture=*/true,
                                    /*swRasterIsPicture=*/false,
                                    /*guestGeometryPath=*/false,
                                    /*rebuildForOwnership=*/true));
  CHECK(preserve_composite_backdrop(/*guestVramIsPicture=*/false,
                                    /*swRasterIsPicture=*/true,
                                    /*guestGeometryPath=*/false,
                                    /*rebuildForOwnership=*/true));
}

} // namespace

int main() {
  RUN(gte_preserves_the_completed_front_page);
  RUN(native_renderer_still_clears_each_rebuilt_frame);
  RUN(picture_backdrops_remain_independent_owners);
  return pt_summary();
}
