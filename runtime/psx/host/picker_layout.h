// Where each panel is, how wide it is this frame, where its picture goes, and where its seams cut.
#pragma once

#include "viewport_layout.h"

#include <vector>

namespace psx::host {

// One panel's geometry for one frame, in surface pixels.
struct PanelLayout {
  // Full height, between the neighbouring boundaries.
  psx::present::PaneRect bounds{0, 0, 0, 0};
  // Each boundary's column at y = 0 and at the surface's height: mask lines neighbours share. Only
  // the seams between panels lean; the window's own outer edges stand vertical.
  struct Seams {
    int leftTopX = 0, rightTopX = 0;       // at y = 0
    int leftBottomX = 0, rightBottomX = 0; // at y = the surface's height
  };
  Seams seams;
  // Upright, covering every point the seams enclose: neighbours overlap across their shared seam by
  // up to one slant, and the mask decides which owns each pixel there.
  psx::present::PaneRect cover{0, 0, 0, 0};
  int slantX = 0; // the top of a seam sits this many pixels left of its bottom
};

// A panel is geometry; what is shown in it is the selector's business.
class PickerLayout {
public:
  // The selected panel's fraction of the width; the others split the rest evenly. The slant is a
  // fraction of the width.
  struct Motion {
    float selectedShare = 0.0f;
    float slantPerWidth = 0.0f;
    float responsePerFrame = 0.0f; // clamped away from 0 and 1: neither is an animation
  };
  PickerLayout(int panelCount, Motion motion);

  // Every panel moves towards the shares this selection calls for on the next advance().
  void setSelection(int selected);
  // The shares this selection calls for, at once.
  void snap(int selected);
  // The picked panel widens to the whole surface and its seams stand up, so the edge leaves the window.
  void pick(int selected);

  // A resize re-derives every rectangle from the current shares, so the animation continues.
  void setSurface(int width, int height);

  // True while any panel is still settling.
  bool advance();

  int panelCount() const {
    return mPanelCount;
  }
  PanelLayout panel(int index) const;

  // The width a selected panel keeps clear of both its seams, whichever panel it is.
  int selectedClearWidth() const;

private:
  // The shares the selection calls for; advance() walks each panel there.
  void retarget(int selected);
  PanelLayout layoutAt(int index, const std::vector<float> &shares, float slantScale) const;

  int mPanelCount;
  float mSelectedShare;
  float mSlantPerWidth;
  float mResponse;
  int mSurfaceW = 0, mSurfaceH = 0;
  // The animation is the difference between the current share and its target.
  std::vector<float> mShare;
  std::vector<float> mTargetShare;
  // The seams' lean as a fraction of the configured slant: 1, easing to 0 on a pick.
  float mSlantScale = 1.0f;
  float mTargetSlantScale = 1.0f;
};

} // namespace psx::host
