// sbs_pane_layout.h — WHERE each frame goes in the window: the letterbox rule, and the SBS two-pane
// split that puts core A (the port) on the left and core B (the oracle) on the right.
//
// This is pure geometry — no SDL, no GPU, no Game — for two reasons. It is the layer that decides
// which core's picture you are actually looking at, so it is the layer worth pinning with a test
// (tests/test_sbs_pane_layout.cpp), and pinning it needs no window. And the rule is genuinely shared:
// the single-pane present, the native-image present and both SBS panes all letterbox the same way.
//
// The panes come from two DIFFERENT Games (each core renders and reads back its own frame via
// gpu_vk_render_readback), which is why the composite is the free function gpu_vk_present_sbs2 and
// not a GpuVkState method — no single Game's renderer state owns both pictures.
#pragma once

// A viewport rectangle in window pixels. Mirrors the x/y/w/h of SDL_GPUViewport without depending on
// SDL: this header is the layout RULE, the renderer only consumes it.
struct PaneRect {
  int x, y, w, h;
};

// The two SBS panes, by index. A is the port under test, B is the recomp oracle; A is on the LEFT
// because that is the order every SBS diff dump, log line and PPM uses ("A=port left | B=oracle right").
enum { SBS_PANE_A = 0, SBS_PANE_B = 1, SBS_PANE_COUNT = 2 };

// Largest centred rect of aspect aw:ah that fits in an ow x oh box. Precondition: all four positive
// (a zero source dimension is a caller bug, not a case to paper over — gpu_vk_present_sbs2 clamps its
// pane dimensions to >= 1 before it ever gets here).
inline PaneRect pane_letterbox(int aw, int ah, int ow, int oh) {
  int dw, dh;
  if ((long)ow * ah >= (long)oh * aw) {
    dh = oh;
    dw = oh * aw / ah;
  } // box is wider than the image: bars left/right
  else {
    dw = ow;
    dh = ow * ah / aw;
  } // box is taller: bars top/bottom
  return PaneRect{(ow - dw) / 2, (oh - dh) / 2, dw, dh};
}

// Where pane `pane` (SBS_PANE_A / SBS_PANE_B) of a srcW x srcH picture lands in a winW x winH window.
// The window is split into two equal half-width columns and the picture is letterboxed inside its own
// column by its OWN aspect — the two panes may differ (a widescreen port next to a 4:3 oracle), so
// each is fitted independently rather than to a shared aspect.
inline PaneRect sbs_pane_rect(int pane, int srcW, int srcH, int winW, int winH) {
  const int colW = winW / 2;
  PaneRect r = pane_letterbox(srcW, srcH, colW, winH);
  r.x += pane * colW;
  return r;
}
