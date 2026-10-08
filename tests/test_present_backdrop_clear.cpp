// test_present_backdrop_clear — may render_geom clear the composite when no native primitive was
// submitted this frame?
//
// On RenderPath::Device the presented buffer is the GPU device's VRAM and no native primitive is ever
// submitted, so render_geom's empty-batch clear would wipe the picture it just uploaded. The rule
// must not depend on GameConfig::preserveVramBackdrop, which is the port's statement about the guest's
// VRAM under the native renderer.
//
// Hermetic: the rule is pure bool logic in gpu_vk_present_policy.h. No GPU, no window, no disc.
//
// NEGATIVE-RESULT DISCIPLINE: assertion count printed as a denominator, and BOTH directions of every
// input are asserted — a predicate hardcoded to "never clear" would pass a suite that only checked
// the preserve cases, and would reintroduce issue 0029's raw-VRAM reveal on the native path.

#include "gpu_vk_present_policy.h"
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
  // RenderPath::Device on a port whose native producers own the frame (preserveVramBackdrop = 0).
  // The batch is ALWAYS empty on this path, so without the device input this is indistinguishable
  // from "nothing to show".
  check("device picture owns the frame, port does NOT preserve guest VRAM",
        vram_backdrop_is_picture(/*guestVramIsPicture=*/false, /*deviceIsPicture=*/true),
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
  check("both: guest VRAM is picture AND the device picture is shown", vram_backdrop_is_picture(true, true), true);

  printf("test_present_backdrop_clear: %d/%d assertion(s) passed\n", g_checks - g_fail, g_checks);
  if (g_checks == 0) {
    printf("REFUSED: asserted nothing\n");
    return 2;
  }
  return g_fail ? 1 : 0;
}
