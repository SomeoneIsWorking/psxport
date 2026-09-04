// test_present_backdrop_clear — may render_geom clear the composite when no native primitive was
// submitted this frame?
//
// REGRESSION UNDER TEST (Tomba!2's item menu on PSXPORT_RENDER_PATH=psx, 2026-08-20). This is the
// THIRD instance of one blindness, and the first two are already documented in
// gpu_vk_present_policy.h: on RenderPath::Psx the PSX software rasterizer draws the whole frame into
// s_vram, submits no VK geometry and marks nothing dirty, so every input that asks "did anything
// change" is structurally blind. The present DECISION was fixed for it (swRasterIsPicture), and the
// dirty list was fixed for it (s_dirty.markAll()). render_geom's clear was not.
//
// It asked GameConfig::preserveVramBackdrop instead — which is the port's statement about whether the
// GUEST's VRAM is part of the picture under the NATIVE renderer. Tomba!2 sets it to 0, correctly:
// its native producers own the frame, so leftover guest VRAM is stale and clearing is right. But on
// the software path the picture is in s_vram because WE rasterized it there, so that clear destroys
// our own output — after upload_vram had just uploaded it.
//
// MEASURED, and this is what makes it a defect rather than a theory: at f1120 of
// replays/bugs/ingame-item-menu.pad on the psx path, our VRAM is pixel-identical to the beetle GPU
// oracle's (0 differing pixels of 524,288) and holds the menu at 90.0% coverage inside the guest's
// own 320x240 display rect — while the presented frame is 0/691,200 non-black. Drawn correctly,
// then cleared away.
//
// Hermetic: the rule is pure bool logic in gpu_vk_present_policy.h. No GPU, no window, no disc.
//
// NEGATIVE-RESULT DISCIPLINE: assertion count printed as a denominator, and BOTH directions of every
// input are asserted — a predicate hardcoded to "never clear" would pass a suite that only checked
// the preserve cases, and would reintroduce issue 0029's raw-VRAM reveal on the native path.

#include "../runtime/psx/gpu_vk_present_policy.h"
#include <stdio.h>

static int g_checks = 0, g_fail = 0;

static void check(const char *what, bool got, bool want) {
  g_checks++;
  const char *g = got ? "PRESERVE" : "CLEAR";
  if (got == want) {
    printf("  ok   %-62s -> %s\n", what, g);
    return;
  }
  g_fail++;
  printf("  FAIL %-62s -> %s (expected %s)\n", what, g, want ? "PRESERVE" : "CLEAR");
}

int main(void) {
  printf("test_present_backdrop_clear: may an empty native batch clear the composite?\n");

  // ---- 1. The regression. ------------------------------------------------------------------------
  // RenderPath::Psx on a port whose native producers own the frame (preserveVramBackdrop = 0).
  // The batch is ALWAYS empty on this path, so without the software-rasterizer input this is
  // indistinguishable from "nothing to show" and the menu is cleared to black every frame.
  check("software rasterizer owns the frame, port does NOT preserve guest VRAM",
        vram_backdrop_is_picture(/*guestVramIsPicture=*/false, /*swRasterIsPicture=*/true),
        true);

  // ---- 2. What the clear is FOR, which the fix must not give back. -------------------------------
  // The native path on that same port: zero primitives really does mean nothing to show, and
  // revealing raw PSX VRAM instead of black is the bug the clear exists to prevent.
  check("native renderer owns the frame, nothing submitted", vram_backdrop_is_picture(false, false), false);

  // ---- 3. Issue 0029's case, unchanged. ----------------------------------------------------------
  // A port still running the guest's own drawing: upload-only screens (logos, loading screens, fades)
  // are normal and must survive.
  check("port preserves guest VRAM (upload-only screens are normal)", vram_backdrop_is_picture(true, false), true);

  // ---- 4. Both at once — no interaction, and neither input may veto the other. -------------------
  check("both: guest VRAM is picture AND software rasterizer drew it", vram_backdrop_is_picture(true, true), true);

  printf("test_present_backdrop_clear: %d/%d assertion(s) passed\n", g_checks - g_fail, g_checks);
  if (g_checks == 0) {
    printf("REFUSED: asserted nothing\n");
    return 2;
  }
  return g_fail ? 1 : 0;
}
