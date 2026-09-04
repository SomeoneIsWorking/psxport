// test_present_empty_batch — the rule that decides whether a present rebuilds the composite.
//
// REGRESSION UNDER TEST (Spyro's black boot logos, 2026-08-04). afca817d added an empty-batch
// early-out to GpuVkState::present() so a 30fps guest's idle field re-shows the last composite
// instead of rebuilding a black one. The early-out sits ABOVE upload_vram, and its condition was
// "the geometry batch is empty" alone. An upload-only screen — the guest blits a still into VRAM and
// submits zero primitives — satisfies that condition while being a brand new picture, so the
// composite was never built and the screen showed black for its whole duration.
//
// The predicate must therefore distinguish TWO ways a frame can be new (primitives, or a direct
// displayed-framebuffer write) from activity elsewhere in VRAM. Hermetic by construction: the
// rule is pure integer/bool logic in gpu_vk_present_policy.h, so this needs no GPU, no window, no
// disc image.
//
// NEGATIVE-RESULT DISCIPLINE: this file counts its assertions and prints the denominator, so a pass
// line can never be confused with a run that asserted nothing. It also asserts BOTH directions of
// each input — a predicate that returned REBUILD unconditionally would pass a suite that only ever
// checked the rebuild cases, and would silently undo afca817d.

#include "../runtime/psx/gpu_vk_present_policy.h"
#include <stdio.h>

static int g_checks = 0, g_fail = 0;

static const char *name(PresentRebuild r) {
  switch (r) {
  case PRESENT_REUSE_LAST:
    return "REUSE_LAST";
  case PRESENT_REBUILD_GEOM:
    return "REBUILD_GEOM";
  case PRESENT_REBUILD_VRAM:
    return "REBUILD_VRAM";
  case PRESENT_REBUILD_OWNERSHIP:
    return "REBUILD_OWNERSHIP";
  case PRESENT_REBUILD_COUNT:
    break;
  }
  return "??";
}

static void check(const char *what, PresentRebuild got, PresentRebuild want) {
  g_checks++;
  if (got == want) {
    printf("  ok   %-58s -> %s\n", what, name(got));
    return;
  }
  g_fail++;
  printf("  FAIL %-58s -> %s (expected %s)\n", what, name(got), name(want));
}

int main(void) {
  printf("test_present_empty_batch: the present rebuild rule\n");

  // ---- 1. The regression itself: an upload-only screen. -------------------------------------------
  // Spyro's SCE / Universal logo screens. The guest DMAs a 24bpp still into VRAM and submits not one
  // primitive; every following present must show it. Before the fix this returned REUSE_LAST, and
  // since no composite had ever been built, "the last composite" was the initial black target.
  check("upload-only screen: empty batch, VRAM written",
        present_rebuild_decision(/*batchEmpty=*/true, /*guestVramIsPicture=*/true, /*vramWrites=*/1, /*atLastBuild=*/0),
        PRESENT_REBUILD_VRAM);

  // Several writes between builds (a still is uploaded as many strips) still counts as one change.
  check("upload-only screen: many VRAM writes since the build",
        present_rebuild_decision(true, true, 37, 0),
        PRESENT_REBUILD_VRAM);

  check("off-display texture write does not replace the visible composite",
        present_rebuild_decision(true,
                                 true,
                                 1,
                                 0,
                                 /*rebuildForOwnership=*/false,
                                 /*swRasterIsPicture=*/false,
                                 /*guestDisplayChanged=*/false),
        PRESENT_REUSE_LAST);

  // ---- 2. What afca817d bought, which the fix must NOT give back. ---------------------------------
  // The Spider-Man 30Hz case: the guest built no ordering table for this field AND touched no VRAM.
  // Nothing changed, so re-show — rebuilding here is what produced the measured 0.0% / 99.4% /
  // 0.0% alternation. This is the assertion that stops the fix from becoming "always rebuild".
  check("idle field: empty batch, no VRAM write since the build",
        present_rebuild_decision(true, true, 12, 12),
        PRESENT_REUSE_LAST);

  // The very first present of a run, before anything at all has happened.
  check("cold start: empty batch, zero writes", present_rebuild_decision(true, true, 0, 0), PRESENT_REUSE_LAST);

  // ---- 3. Geometry still dominates. ---------------------------------------------------------------
  check("normal frame: primitives submitted", present_rebuild_decision(false, true, 5, 5), PRESENT_REBUILD_GEOM);
  check("primitives submitted AND VRAM written", present_rebuild_decision(false, true, 6, 5), PRESENT_REBUILD_GEOM);

  // ---- 4. A NATIVE-PRODUCER port is untouched by this change, by construction. ---------------------
  // guestVramIsPicture = GameConfig::preserveVramBackdrop = 0 (Tomba!2). Its guest still writes VRAM
  // — texture/CLUT uploads at least — but those are not the picture: render_geom clears an empty
  // batch to black for such a port, so rebuilding would composite BLACK over a perfectly good frame.
  // With the flag clear the decision must be identical to afca817d's for EVERY write count, which is
  // what makes the blast radius on that port provably nil rather than merely untested.
  check("native producer, VRAM written: still reuse (Tomba!2 unchanged)",
        present_rebuild_decision(true, /*guestVramIsPicture=*/false, 99, 0),
        PRESENT_REUSE_LAST);
  check("native producer, no VRAM write: still reuse", present_rebuild_decision(true, false, 4, 4), PRESENT_REUSE_LAST);
  check("native producer, primitives submitted: rebuild as always",
        present_rebuild_decision(false, false, 9, 0),
        PRESENT_REBUILD_GEOM);

  // ---- 5. THE SOFTWARE RASTERIZER OWNS THE PICTURE (RenderPath::Psx, 2026-08-11). ------------------
  // MEASURED FAILURE THIS ENCODES: spyro on PSXPORT_RENDER_PATH=psx presented 0.0% non-black / 1 colour
  // at presents 700, 1200 and 2010, while a direct s_vram capture at present 700 of the SAME run was
  // 81.5% non-black / 2117 colours. So the rasterizer was drawing and the PRESENT was throwing it away.
  //
  // Why the two inputs above cannot see it: on the software path there is never any VK geometry, so
  // batchEmpty is permanently true, and every gpu_vk_dirty() call site is gated `if (vk_path())`, so the
  // change counter never moves either. Both inputs are structurally pinned, and the decision was
  // REUSE_LAST for the entire run — of a composite that had never been built once.
  //
  // The third input is therefore not a special case bolted on; it is the missing statement that this
  // frame HAS a source. When the software rasterizer owns the picture, s_vram IS the frame at every
  // present, and there is no "the guest did nothing this field" optimisation to model — that
  // optimisation is about the VK composite.
  check("software raster: empty batch, no VRAM write, no geometry — still the picture",
        present_rebuild_decision(/*batchEmpty=*/true,
                                 /*guestVramIsPicture=*/true,
                                 /*vramWrites=*/7,
                                 /*atLastBuild=*/7,
                                 /*rebuildForOwnership=*/false,
                                 /*swRasterIsPicture=*/true),
        PRESENT_REBUILD_VRAM);
  // It must not depend on preserveVramBackdrop: that flag answers "is the GUEST's VRAM the picture",
  // and this path's picture is in s_vram because WE rasterized it there.
  check("software raster: native-producer port flag, still the picture",
        present_rebuild_decision(true,
                                 /*guestVramIsPicture=*/false,
                                 0,
                                 0,
                                 /*rebuildForOwnership=*/false,
                                 /*swRasterIsPicture=*/true),
        PRESENT_REBUILD_VRAM);
  // BOTH DIRECTIONS: with the new input false, every pre-existing decision is bit-identical. This is
  // the assertion that stops the fix from becoming "always rebuild" on the paths that were working.
  check("VK path unchanged: idle field still reuses",
        present_rebuild_decision(true, true, 12, 12, /*rebuildForOwnership=*/false, /*swRasterIsPicture=*/false),
        PRESENT_REUSE_LAST);
  check("VK path unchanged: geometry still dominates",
        present_rebuild_decision(false, true, 5, 5, /*rebuildForOwnership=*/false, /*swRasterIsPicture=*/false),
        PRESENT_REBUILD_GEOM);

  // ---- 6. Changing picture ownership invalidates the persistent composite. ----------------------
  check("cold composite: build under declared ownership",
        present_rebuild_decision(true, false, 0, 0, /*rebuildForOwnership=*/true),
        PRESENT_REBUILD_OWNERSHIP);
  check("guest-to-native transition: discard the guest-backed composite",
        present_rebuild_decision(true, false, 4, 4, /*rebuildForOwnership=*/true),
        PRESENT_REBUILD_OWNERSHIP);
  check("native-to-guest transition: restore the guest-backed composite",
        present_rebuild_decision(true, true, 4, 4, /*rebuildForOwnership=*/true),
        PRESENT_REBUILD_OWNERSHIP);

  // ---- 7. The counter must not wedge. -------------------------------------------------------------
  // Compared with != rather than >, so a wrapped uint32 counter still reads as "changed" instead of
  // silently pinning the frame to REUSE_LAST forever (which is a black screen that never recovers).
  check("write counter wrapped past the recorded build value",
        present_rebuild_decision(true, true, /*vramWrites=*/0u, /*atLastBuild=*/0xFFFFFFFFu),
        PRESENT_REBUILD_VRAM);

  printf("%s: %d/%d checks passed (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_checks - g_fail, g_checks, g_fail);
  return g_fail ? 1 : 0;
}
