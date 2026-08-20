// present_plan.h — WHAT THE PLAYER SEES, decided in one pure place, identically in both legs.
//
// The composite that turns a finished VRAM/ires target into a presented picture — letterbox, fade,
// source selection (native vs high-res), 24bpp decode — is the LAST stage of the renderer, and until
// this header existed it lived entirely inside `GpuVkState::show_composite`, which HEADLESS NEVER
// REACHED: `present()` did `if (s_headless) { submit; return; }` above it. Headless was not "the same
// pipeline with a different final sink", it was a pipeline that STOPPED ONE STAGE EARLY.
//
// That is why this is a correctness header and not a tidiness one. Every capture instrument in the
// project (PSXPORT_SHOT_AT, dump_to, gpu_vk_render_readback) reads back guest VRAM, i.e. the stage
// BEFORE this one, and almost every measurement is taken headless. So each of those numbers was a
// claim about a program the user never runs. Measured cost of that gap, from issue 0005: the intro
// logo read 99.95% non-black / 11395 colours headless while the USER, watching a window on the same
// build, saw black. Both numbers were true. Nothing in the output said they described different
// stages.
//
// THE INVARIANT THIS HEADER EXISTS TO PIN, asserted in tests/test_present_plan.cpp:
//
//     For identical inputs, plan_present(in, headless) and plan_present(in, windowed) differ in
//     EXACTLY ONE field: `to_swapchain`.
//
// `to_swapchain` is the final sink and the only place a leg difference is legitimate. Everything that
// decides what the picture LOOKS like is computed above it, from inputs, with no leg term at all — so
// a future edit cannot re-introduce the split without failing the test.
//
// Pure: no SDL, no GPU, no Game, no globals. The renderer consumes the plan; it does not re-decide.
#pragma once

#include "sbs_pane_layout.h" // PaneRect / pane_letterbox — the shared letterbox rule

// The PSX display height the present aspect is measured against. It is a constant of the DISPLAY,
// not of the framebuffer: `disp_w` widens with widescreen while this does not, which is exactly what
// makes a wide frame present as 512:240 instead of a squeezed 4:3.
enum { PRESENT_DISPLAY_ASPECT_H = 240 };

// Everything outside the renderer's own state that the presented picture depends on. All of it is
// leg-independent by construction: there is no `headless` field here, because none of these values
// may be derived from the leg.
struct PresentInputs {
  int sink_w, sink_h; // presented-image size in pixels (window drawable size, or the headless
                      // sink size). A SIZE is a legitimate leg parameter; a code path is not.
  int sx, sy;         // display origin in VRAM
  int disp_w, disp_h; // display size in VRAM (disp_w is the WIDE width when widescreen is active)
  int native_w;       // the game's OWN native 4:3 framebuffer width (320 Tomba2, 512 spider1/spyro)
  int present_ires;   // 0 = present the native target, >1 = present the ires target at this scale
  int fade_mode;      // 0 none / 1 additive / 2 subtractive (ScreenFade, guest-backed)
  int fade_r, fade_g, fade_b;
  int disp_rgb24; // 1 when the display register says 24bpp
};

// The complete description of one presented frame. Two plans that compare equal produce the same
// picture; that is what makes the both-legs-identical test meaningful.
struct PresentPlan {
  bool build;         // run the composite pass at all? TRUE IN BOTH LEGS whenever the sink is real.
  bool to_swapchain;  // blit the built image into the window swapchain? THE ONLY LEG DIFFERENCE.
  int sink_w, sink_h; // THE sink size this plan was computed for — carried so the renderer cannot
                      // re-read it. The first version let build_present_image call sink_w()/sink_h()
                      // again, so one present sampled the live window size from three independent
                      // places (plus a fourth from the swapchain acquire); mid-resize they disagree
                      // and the letterbox is then computed for a different rectangle than the target
                      // it is drawn into. A plan that does not carry its own sink is not a plan.
  PaneRect viewport;  // where the picture lands inside the sink
  int disp[4];        // fragment uniform: sampled source rect (scaled when presenting from ires)
  int fade[4];        // fragment uniform: mode, r, g, b
  int fmt[4];         // fragment uniform: fmt[0] = 24bpp flag
  int src_ires;       // 0 = sample the native target, >1 = sample the ires target
};

inline bool present_plan_same_picture(const PresentPlan &a, const PresentPlan &b) {
  if (a.build != b.build || a.src_ires != b.src_ires) {
    return false;
  }
  if (a.sink_w != b.sink_w || a.sink_h != b.sink_h) {
    return false;
  }
  if (a.viewport.x != b.viewport.x || a.viewport.y != b.viewport.y || a.viewport.w != b.viewport.w ||
      a.viewport.h != b.viewport.h) {
    return false;
  }
  for (int i = 0; i < 4; i++) {
    if (a.disp[i] != b.disp[i] || a.fade[i] != b.fade[i] || a.fmt[i] != b.fmt[i]) {
      return false;
    }
  }
  return true;
}

inline PresentPlan plan_present(const PresentInputs &in, bool headless) {
  PresentPlan p{};
  // No sink means nothing to compose INTO — a minimised window, or a headless sink configured to
  // zero. This is the one case that builds nothing, and it is leg-independent: it is a statement
  // about the sink's size, which both legs supply the same way.
  if (in.sink_w <= 0 || in.sink_h <= 0) {
    return p; // build = false, everything zeroed
  }

  p.build = true;
  p.to_swapchain = !headless; // <-- the ONE place the leg is allowed to appear
  p.sink_w = in.sink_w;
  p.sink_h = in.sink_h; // resolved ONCE, here, for everything downstream

  const int scale = in.present_ires > 1 ? in.present_ires : 1;
  p.src_ires = in.present_ires > 1 ? in.present_ires : 0;
  p.disp[0] = in.sx * scale;
  p.disp[1] = in.sy * scale;
  p.disp[2] = in.disp_w * scale;
  p.disp[3] = in.disp_h * scale;
  p.fade[0] = in.fade_mode;
  p.fade[1] = in.fade_r;
  p.fade[2] = in.fade_g;
  p.fade[3] = in.fade_b;
  // 24bpp applies to the NATIVE VRAM read only: the ires composite is rendered by our own raster in
  // 1555, so a scaled present is never 24bpp regardless of what the display register says.
  p.fmt[0] = p.src_ires > 1 ? 0 : in.disp_rgb24;
  // ---- THE PRESENTED ASPECT (issue 0008) ------------------------------------------------------
  // On PSX the horizontal framebuffer width (256/320/368/512/640) selects the horizontal SAMPLING
  // RATE, not the shape of the picture: every one of those modes scans into the SAME 4:3 screen
  // area, so pixels are non-square (in 512-wide mode, 0.625:1). The shape is therefore 4:3 for a
  // native frame REGARDLESS of framebuffer width — and wider only when the port has DELIBERATELY
  // widened the frame for the widescreen mod.
  //
  // So the aspect is 4:3 scaled by how much wider than ITS OWN NATIVE WIDTH the framebuffer is:
  //     aspect = (4 / 3) * (disp_w / native_w)   ->   pane_letterbox(4*disp_w, 3*native_w, ...)
  //
  // The previous rule was pane_letterbox(disp_w, 240, ...), and disp_w/240 == (4/3)*(disp_w/320) —
  // i.e. it HARDCODED 320 as every game's native 4:3 width. For a 320-wide game that constant IS
  // the native width, so Tomba2 was correct in both its modes (320:240 = 4:3, 428:240 = 16:9) and
  // the bug was invisible on the reference consumer. A 512-wide game (spider1, spyro) had its
  // picture stretched 1.6x wide, with black bars that read as deliberate letterboxing.
  //
  // psxport had ALREADY fixed this same assumption once, in the widescreen accessor, whose comment
  // says the wide width must scale from the game's own 4:3 width "not from a hardcoded 320" and
  // names Spyro's 512x240 as the case that breaks. That fix reached the WIDE side only; this is the
  // PRESENT side of the identical constant, wearing 240 instead of 320.
  const int aspect_w = in.disp_w > 0 ? in.disp_w : 1;
  // native_w <= 0 means "the game has not told us its native width". Degrade to 4:3 — every PSX
  // display mode is 4:3, so that is the only safe assumption, and it can never divide by zero.
  const int aspect_h = in.native_w > 0 ? in.native_w : aspect_w;
  p.viewport = pane_letterbox(4 * aspect_w, 3 * aspect_h, in.sink_w, in.sink_h);
  return p;
}
