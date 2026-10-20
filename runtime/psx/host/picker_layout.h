// Where each panel is, how wide it is this frame, where its picture goes, and where its seams cut.
#pragma once

#include "pixel_size.h"
#include "viewport_layout.h"

#include <vector>

namespace psx::host {

// A logo's vertical centre, as a fraction of the panel's height.
inline constexpr double logoCentrePerHeight = 0.2;

// A catalog longer than this is refused rather than laid out at zero width.
inline constexpr int kMaxPickerPanels = 8;

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
  int averageWidth() const {
    return ((seams.rightTopX - seams.leftTopX) + (seams.rightBottomX - seams.leftBottomX)) / 2;
  }
  // Upright, covering every point the seams enclose: neighbours overlap across their shared seam by
  // up to one slant, and the mask decides which owns each pixel there.
  psx::present::PaneRect cover{0, 0, 0, 0};
  // Centred horizontally, its centre at `logoCentrePerHeight` of the panel's height.
  psx::present::PaneRect logoRect(int logoWidth, int logoHeight) const;
  int slantX = 0; // the top of a seam sits this many pixels left of its bottom
};

// A panel is geometry; what is shown in it is a session's business.
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

  // A resize re-derives every rectangle from the current shares, so the animation continues.
  void setSurface(int width, int height);

  // True while any panel is still settling.
  bool advance();

  int panelCount() const {
    return mPanelCount;
  }
  PanelLayout panel(int index) const;

  // The size panel `index`'s picture is built at: its cover once the layout has settled, so a moving
  // panel shows its picture 1:1 rather than rebuilt at every width it passes through.
  psx::present::PixelSize imageSize(int index) const;

  static constexpr int maxPanels() {
    return kMaxPickerPanels;
  }

private:
  // The shares the selection calls for; advance() walks each panel there.
  void retarget(int selected);
  PanelLayout layoutAt(int index, const std::vector<float> &shares) const;

  int mPanelCount;
  float mSelectedShare;
  float mSlantPerWidth;
  float mResponse;
  int mSurfaceW = 0, mSurfaceH = 0;
  // The animation is the difference between the current share and its target.
  std::vector<float> mShare;
  std::vector<float> mTargetShare;
};

} // namespace psx::host
