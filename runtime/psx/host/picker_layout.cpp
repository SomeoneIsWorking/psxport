#include "picker_layout.h"

#include <algorithm>
#include <cmath>

namespace psx::host {
namespace {

// Below this a panel's share has arrived. Compared with == the animation would either never settle
// (0.1 * 0.75 forever) or settle instantly.
constexpr float kSettled = 0.0005f;

} // namespace

PickerLayout::PickerLayout(int panelCount, Motion motion)
    : mPanelCount(std::clamp(panelCount, 0, kMaxPickerPanels)),
      mSelectedShare(std::clamp(motion.selectedShare, 0.0f, 1.0f)), mSlantPerWidth(motion.slantPerWidth),
      mResponse(std::clamp(motion.responsePerFrame, 0.01f, 1.0f)) {
  retarget();
  mShare = mTargetShare; // a layout starts settled; the first advance() has nothing to animate
}

void PickerLayout::setSelection(int selected) {
  if (mPanelCount <= 0) {
    return;
  }
  mSelected = std::clamp(selected, 0, mPanelCount - 1);
  retarget();
}

void PickerLayout::retarget() {
  if (mPanelCount <= 0) {
    mTargetShare.clear();
    return;
  }
  if (mPanelCount == 1) {
    mTargetShare.assign(1, 1.0f);
    return;
  }
  // The shares sum to one: the selected panel's, and the rest split evenly.
  mTargetShare.assign(static_cast<std::size_t>(mPanelCount),
                      (1.0f - mSelectedShare) / static_cast<float>(mPanelCount - 1));
  mTargetShare[static_cast<std::size_t>(mSelected)] = mSelectedShare;
}

void PickerLayout::setSurface(int width, int height) {
  mSurfaceW = std::max(width, 0);
  mSurfaceH = std::max(height, 0);
}

bool PickerLayout::advance() {
  if (mPanelCount <= 0) {
    return false;
  }
  bool settling = false;
  for (int i = 0; i < mPanelCount; ++i) {
    const std::size_t index = static_cast<std::size_t>(i);
    const float remaining = mTargetShare[index] - mShare[index];
    if (std::fabs(remaining) <= kSettled) {
      mShare[index] = mTargetShare[index];
      continue;
    }
    // A fixed fraction of what is left, so a long travel does not snap and a short one crawl.
    mShare[index] += remaining * mResponse;
    settling = true;
  }
  return settling;
}

float PickerLayout::share(int index) const {
  if (index < 0 || index >= mPanelCount) {
    return 0.0f;
  }
  return mShare[static_cast<std::size_t>(index)];
}

PanelLayout PickerLayout::panel(int index) const {
  PanelLayout out;
  if (mPanelCount <= 0 || index < 0 || index >= mPanelCount || mSurfaceW <= 0 || mSurfaceH <= 0) {
    return out;
  }
  // Each panel starts where the one before it ended, so a panel's right edge IS its neighbour's
  // left edge by construction.
  float edge = 0.0f;
  for (int i = 0; i < index; ++i) {
    edge += mShare[static_cast<std::size_t>(i)];
  }
  const float start = edge;
  const float width = mShare[static_cast<std::size_t>(index)];
  const int x = static_cast<int>(std::lround(start * static_cast<float>(mSurfaceW)));
  // The last panel ends on the surface edge whatever rounding the shares carry.
  const int right = index == mPanelCount - 1
                        ? mSurfaceW
                        : static_cast<int>(std::lround((start + width) * static_cast<float>(mSurfaceW)));
  out.bounds = PanelRect{x, 0, std::max(right - x, 0), mSurfaceH};
  // A fraction of the width, not the height: the slant is a horizontal displacement, and a
  // fraction of the height makes it invisible on a wide window.
  out.slantX = static_cast<int>(std::lround(mSlantPerWidth * static_cast<float>(mSurfaceW)));
  // At the tiling edge the seam is half a slant left at the top and half a slant right at the
  // bottom. The row's two outer edges are the window's own and stand vertical, or each panel's
  // corner falls outside the surface it is meant to fill.
  const int half = (out.slantX + 1) / 2;
  const bool first = index == 0;
  const bool last = index + 1 == mPanelCount;
  out.seams.leftTopX = first ? 0 : x - half;
  out.seams.rightTopX = last ? mSurfaceW : right - half;
  out.seams.leftBottomX = first ? 0 : x - half + out.slantX;
  out.seams.rightBottomX = last ? mSurfaceW : right - half + out.slantX;
  // The upright rectangle the pane is drawn in must enclose every point the two seams pass
  // through; neighbours overlap across their shared seam by up to one slant, and the mask makes
  // each pixel belong to exactly one of them.
  const int coverLeft = std::min(out.seams.leftTopX, out.seams.leftBottomX);
  const int coverRight = std::max(out.seams.rightTopX, out.seams.rightBottomX);
  out.cover = PanelRect{coverLeft, 0, std::max(coverRight - coverLeft, 0), mSurfaceH};
  // There is nothing else in a panel to leave a band for.
  return out;
}

} // namespace psx::host
