#pragma once
// video_plan — the RESOLUTION decisions (widescreen framebuffer width, internal-resolution scale),
// as pure functions of the PRESENTATION SINK rather than of a window.
//
// THE DEFECT THIS EXISTS TO KILL. Both decisions used to read `win_h()` / `win_w()` — "the live
// window size, falling back to 320x240 when there is no window". Headless has no window, so it took
// the fallback: a headless run computed an AUTO internal resolution of round(240/240) = 1 where a
// windowed run of the same build computed round(720/240) = 3, and ASPECT_AUTO computed its widened
// framebuffer from a 320x240 aspect instead of the sink's. Headless captures were therefore not the
// user's picture, and that breaks the rule the whole project's evidence rests on:
//
//     "windowed and headless should be equal anyway, it shouldn't change anything in the game.
//      headless just means no window and no audio"  — USER, 2026-08-06
//
// THE FIX IS NOT "ADD A HEADLESS BRANCH". Resolution is an EXPLICIT input — the size of the surface
// the picture is being composed for — and that surface exists in both legs: windowed it is the live
// drawable, headless it is the configured sink (PSXPORT_PRESENT_SINK, defaulting to the window's own
// creation size). gpu_vk.cpp's `sink_size()` already had that leg-independent definition; these two
// decisions were simply reading the wrong thing. So the inputs below name a SINK, not a window, and
// there is no way to ask this header whether a window exists.
//
// Header-only and dependency-light on purpose (mods.h only, for the ASPECT_* enum) so a test can
// include it with nothing linked.
#include "mods.h" // ASPECT_4_3 / _16_9 / _21_9 / _AUTO

// A PSX display field is 240 scanlines. This is the denominator the AUTO internal-resolution scale
// is expressed against: ires=N means N render samples per PSX scanline, so "how many times does the
// sink's height contain a PSX field" is exactly the scale that stops the composite from being
// upscaled. Named because "240.0" open-coded in the divide reads like a magic number and was
// indistinguishable from the window-height fallback that caused the bug.
enum { PRESENT_NATIVE_LINES = 240 };

// The 4:3 framebuffer width every widescreen ratio in this framework was originally expressed for.
// The targets below (428 for 16:9, 560 for 21:9) are the values this framework has always returned
// for a 320-wide game; they are SCALED by (native/320) rather than re-derived, so a 320-wide game is
// bit-identical by construction and a 512-wide game finally widens instead of cropping.
enum { WIDE_REFERENCE_NATIVE_W = 320 };

struct VideoInputs {
  // THE PRESENTATION SINK — the surface the picture is being composed for. Windowed: the live
  // drawable. Headless: the configured sink. NOT "the window": there is deliberately no way to
  // express "there is no window" here.
  int sinkW = 0;
  int sinkH = 0;
  // The game's OWN 4:3 framebuffer width (GP1(0x08) horizontal resolution). <= 0 degrades to 320.
  int nativeW = 0;
  int aspect = ASPECT_4_3; // Mods::aspect
  int modsIres = 1;        // Mods::ires — 0 = AUTO, 1..cap = fixed
  int iresCap = 1;         // the memory-budget cap computed by the caller
  int vramW = 0;           // hard clamp for a widened framebuffer (VRAM_W); <= 0 = no clamp
};

// The framebuffer width the engine renders at for the selected aspect.
inline int video_wide_native_w(const VideoInputs &in) {
  const int native = in.nativeW > 0 ? in.nativeW : WIDE_REFERENCE_NATIVE_W;
  const int vramClamp = in.vramW;
  auto scaled = [native, vramClamp](int for320) {
    if (native == WIDE_REFERENCE_NATIVE_W) {
      return for320; // exact, for every existing consumer
    }
    int w = (int)((double)for320 * native / (double)WIDE_REFERENCE_NATIVE_W + 0.5);
    w &= ~1; // even, as the AUTO path also requires
    return (vramClamp > 0 && w > vramClamp) ? vramClamp : w;
  };
  switch (in.aspect) {
  case ASPECT_16_9:
    return scaled(428);
  case ASPECT_21_9:
    return scaled(560);
  case ASPECT_AUTO: {
    // Match the SINK's aspect — identically in both legs, and identically under SBS (each core
    // renders its full-sink FOV into its own target; the SBS compositor letterboxes that into its
    // half-sink pane, so there is no special case here).
    if (in.sinkH <= 0 || in.sinkW <= 0) {
      return native; // no sink to match: stay 4:3, honestly
    }
    int w = (int)(((double)native * 0.75 * in.sinkW) / (double)in.sinkH + 0.5);
    w &= ~1;
    if (w < native) {
      w = native;
    }
    if (vramClamp > 0 && w > vramClamp) {
      w = vramClamp;
    }
    return w;
  }
  default:
    return native;
  }
}

// The internal-resolution scale. AUTO derives it from the SINK's height, which is what the picture
// is being composed for — not from a window, which may not exist.
inline int video_ires_scale(const VideoInputs &in) {
  const int cap = in.iresCap < 1 ? 1 : in.iresCap;
  int i = in.modsIres;
  if (i == 0) {
    i = (int)(((double)in.sinkH / (double)PRESENT_NATIVE_LINES) + 0.5);
  }
  if (i < 1) {
    i = 1;
  }
  if (i > cap) {
    i = cap;
  }
  return i;
}
