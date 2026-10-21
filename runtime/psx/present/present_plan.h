// present_plan.h — what the player sees, decided in one pure place.
#pragma once

#include "viewport_layout.h"

#include <algorithm>

namespace psx::present {

// Everything outside the renderer's own state that the presented picture depends on.
struct PresentInputs {
  int sink_w, sink_h; // presented-image size in pixels
  int sx, sy;         // display origin in VRAM
  int disp_w, disp_h; // display size in VRAM; disp_w is the wide width when widescreen is active
  int native_w;       // the game's own 4:3 framebuffer width
  int present_ires;   // 0 = present the native target, >1 = present the ires target at this scale
  int fade_mode;      // 0 none / 1 additive / 2 subtractive
  int fade_r, fade_g, fade_b;
  int disp_rgb24; // 1 when the display register says 24bpp
  // Appended: callers initialise this struct positionally. Width of guest-authored content inside disp_w; 0 = all.
  int content_w = 0;
  // 1 = fill the sink, centre-cropped to its aspect, instead of letterboxing (a pane).
  int fill = 0;
};

struct PresentPlan {
  bool build;         // run the composite pass at all
  int sink_w, sink_h; // the sink this plan was computed for; the renderer never re-reads it
  PaneRect viewport;  // where the picture lands inside the sink
  int disp[4];        // fragment uniform: sampled source rect (scaled when presenting from ires)
  int fade[4];        // fragment uniform: mode, r, g, b
  int fmt[4];         // fragment uniform: fmt[0] = 24bpp flag
  int src_ires;       // 0 = sample the native target, >1 = sample the ires target
};

// The extent follows the configured framebuffer, not one field's primitives, so a paused 3D frame keeps its width.
inline int presentDisplayWidth(bool widePresentation, int wideWidth, int nativeWidth) {
  if (!widePresentation || wideWidth <= nativeWidth) {
    return nativeWidth;
  }
  return wideWidth;
}

struct Rgb8 {
  int red = 0;
  int green = 0;
  int blue = 0;
};

// PresentPlan::fade on one 8-bit colour, as present.frag applies it.
inline Rgb8 fadeRgb(const int fade[4], Rgb8 colour) {
  if (fade[0] == 1) {
    return {std::min(colour.red + fade[1], 255),
            std::min(colour.green + fade[2], 255),
            std::min(colour.blue + fade[3], 255)};
  }
  if (fade[0] == 2) {
    return {std::max(colour.red - fade[1], 0), std::max(colour.green - fade[2], 0), std::max(colour.blue - fade[3], 0)};
  }
  return colour;
}

inline PresentPlan planPresent(const PresentInputs &in) {
  PresentPlan p{};
  // No sink, nothing to compose into: the one case that builds nothing.
  if (in.sink_w <= 0 || in.sink_h <= 0) {
    return p;
  }

  p.build = true;
  p.sink_w = in.sink_w;
  p.sink_h = in.sink_h;

  const int scale = in.present_ires > 1 ? in.present_ires : 1;
  p.src_ires = in.present_ires > 1 ? in.present_ires : 0;
  // A guest-authored picture fills only the columns the guest authored, from the origin; the ires
  // composite is our own raster at the display's width and is never narrowed.
  const int content_w = (p.src_ires > 1 || in.content_w <= 0 || in.content_w >= in.disp_w) ? in.disp_w : in.content_w;
  p.disp[0] = in.sx * scale;
  p.disp[1] = in.sy * scale;
  p.disp[2] = content_w * scale;
  p.disp[3] = in.disp_h * scale;
  p.fade[0] = in.fade_mode;
  p.fade[1] = in.fade_r;
  p.fade[2] = in.fade_g;
  p.fade[3] = in.fade_b;
  // 24bpp is a native VRAM read only: the ires composite is 1555.
  p.fmt[0] = p.src_ires > 1 ? 0 : in.disp_rgb24;
  // Every PSX display width scans into the same 4:3 screen, so the aspect is 4:3 widened by disp_w / native_w.
  const int aspect_w = content_w > 0 ? content_w : 1;
  // An unknown native width degrades to 4:3.
  const int aspect_h = in.native_w > 0 ? in.native_w : aspect_w;
  if (in.fill && in.sink_w > 0 && in.sink_h > 0 && in.disp_h > 0) {
    // The crop is taken from the rendered source, so the compositor does not resample it again.
    p.viewport = PaneRect{0, 0, in.sink_w, in.sink_h};
    const long long pictureW = 4LL * aspect_w;
    const long long pictureH = 3LL * aspect_h;
    if (pictureW * in.sink_h > pictureH * in.sink_w) {
      const int keepWidth = static_cast<int>(aspect_w * pictureH * in.sink_w / (pictureW * in.sink_h));
      p.disp[0] += (aspect_w - keepWidth) / 2 * scale;
      p.disp[2] = keepWidth * scale;
    } else {
      const int keepHeight = static_cast<int>(in.disp_h * pictureW * in.sink_h / (pictureH * in.sink_w));
      p.disp[1] += (in.disp_h - keepHeight) / 2 * scale;
      p.disp[3] = keepHeight * scale;
    }
    return p;
  }
  p.viewport = letterbox(4 * aspect_w, 3 * aspect_h, in.sink_w, in.sink_h);
  return p;
}

} // namespace psx::present
