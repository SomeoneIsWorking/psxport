// Where each panel is, how wide it is this frame, where its picture goes, and where its seams cut.
#pragma once

#include <cmath>
#include <vector>

namespace psx::host {

inline constexpr double logoCentrePerHeight = 0.2;

// A bound, not a policy: a catalog longer than this is refused by construction rather than by a
// panel silently landing at zero width.
inline constexpr int kMaxPickerPanels = 8;

// Whole pixels, because that is what the compositor and the window consume.
struct PanelRect {
  int x = 0, y = 0, w = 0, h = 0;
};

// The panel is a different shape from the picture, so the overflow is dropped rather than
// letterboxed.
struct SourceCrop {
  float u = 0.0f, v = 0.0f, w = 1.0f, h = 1.0f;
};

// One panel's geometry for one frame.
struct PanelLayout {
  // Full height, between the neighbouring boundaries.
  PanelRect bounds;
  // Each boundary as the column it sits in at y = 0 and at y = the surface's height.
  //
  // Mask lines, not corners: a panel's picture is an upright rectangle and these decide which of
  // it survives, and two neighbours sharing these numbers is what makes one line between them.
  //
  // Only the seams between panels lean. The window's own outer edges stand vertical: leaning
  // those put a black wedge down each side.
  struct Seams {
    int leftTopX = 0, rightTopX = 0;       // at y = 0
    int leftBottomX = 0, rightBottomX = 0; // at y = the surface's height
  };
  Seams seams;
  // A fit wants one width for a shape that has two.
  int averageWidth() const {
    return ((seams.rightTopX - seams.leftTopX) + (seams.rightBottomX - seams.leftBottomX)) / 2;
  }
  // Upright, covering every point the seams enclose. Wider than the panel by up to one slant,
  // because the two neighbours' rectangles overlap across the shared seam and the mask decides
  // which owns each pixel there.
  PanelRect cover;
  // So the picture fills `cover` with its aspect intact; a degenerate source is shown whole.
  SourceCrop sourceCrop(int pictureWidth, int pictureHeight) const {
    if (pictureWidth <= 0 || pictureHeight <= 0 || cover.h <= 0 || averageWidth() <= 0) {
      return SourceCrop{};
    }
    const double panelAspect = static_cast<double>(averageWidth()) / static_cast<double>(cover.h);
    const double pictureAspect = static_cast<double>(pictureWidth) / static_cast<double>(pictureHeight);
    if (pictureAspect > panelAspect) {
      // A picture WIDER than the panel: the height is the panel's, so the sides are what is lost.
      const double keep = panelAspect / pictureAspect;
      return SourceCrop{static_cast<float>((1.0 - keep) * 0.5), 0.0f, static_cast<float>(keep), 1.0f};
    }
    const double keep = pictureAspect / panelAspect;
    return SourceCrop{0.0f, static_cast<float>((1.0 - keep) * 0.5), 1.0f, static_cast<float>(keep)};
  }
  // Centred horizontally, vertical centre at `logoCentrePerHeight` of the panel's height, so the
  // composition is the same at any window size.
  PanelRect logoRect(int logoWidth, int logoHeight) const {
    if (logoWidth <= 0 || logoHeight <= 0 || cover.h <= 0) {
      return PanelRect{cover.x, cover.y, 0, 0};
    }
    const int centreY = static_cast<int>(std::lround(static_cast<double>(cover.h) * logoCentrePerHeight));
    return PanelRect{cover.x + (cover.w - logoWidth) / 2, centreY - logoHeight / 2, logoWidth, logoHeight};
  }
  int slantX = 0; // the top of a seam sits this many pixels LEFT of its bottom
};

// A panel is geometry; what is shown in it is a session's business.
class PickerLayout {
public:
  // Clamped away from 0 and 1: neither is an animation.
  PickerLayout(int panelCount, float selectedShare, float unselectedShare, float slantPerWidth, float responsePerFrame);

  // Every other panel narrows towards its own share on the next advance().
  void setSelection(int selected);
  int selection() const {
    return mSelected;
  }

  // A resize re-derives every rectangle from the current shares, so the animation continues
  // where it was instead of restarting.
  void setSurface(int width, int height);

  // True while any panel is still settling, which is the only question a caller has about the
  // animation: is it over?
  bool advance();

  int panelCount() const {
    return mPanelCount;
  }
  // `pictureWidth`/`pictureHeight` are the aspect of what will be shown (1x1 for a square
  // source); a non-positive source covers the panel's own shape.
  PanelLayout panel(int index, int pictureWidth, int pictureHeight) const;

  // For a diagnostic; the layout itself never reads it back.
  float share(int index) const;

  // The selected panel's own share once the animation has arrived. A host needs one image size for
  // every session, and resizing it per frame would rebuild each picture for a width about to
  // change anyway.
  int maxPanelWidth(int surfaceWidth) const;

  // A caller with more titles than this must say so rather than hand over a silently truncated
  // count.
  static constexpr int maxPanels() {
    return kMaxPickerPanels;
  }

private:
  // The share the selection calls for; advance() is what walks each panel there.
  void retarget();
  int mPanelCount;
  float mSelectedShare;
  float mUnselectedShare;
  float mSlantPerWidth;
  float mResponse;
  int mSelected = 0;
  int mSurfaceW = 0, mSurfaceH = 0;
  // The animation is the difference between the current share and its target.
  std::vector<float> mShare;
  std::vector<float> mTargetShare;
};

} // namespace psx::host
