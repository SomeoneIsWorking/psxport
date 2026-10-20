#include "picker_panes.h"

#include "panel_logo.h"

#include <lucent/log.h>

#include <algorithm>
#include <cmath>

namespace psx::host {
namespace {

// An unselected panel is grey and dimmed.
constexpr float kUnselectedDesaturation = 1.0f;
constexpr float kUnselectedBrightness = 0.78f;

// A divider is half a percent of the surface height (at least one pixel), in the selection's gold.
constexpr float kDividerPerHeight = 0.005f;
constexpr int kDividerMinimumPixels = 1;
constexpr float kDividerRed = 0.76f;
constexpr float kDividerGreen = 0.64f;
constexpr float kDividerBlue = 0.18f;

// Every pane of a panel is cut by the panel's own two seams, so nothing leaks over a neighbour.
void cutBy(const PanelLayout &geometry, psx::render::Pane &pane) {
  pane.seamLeftTop = static_cast<float>(geometry.seams.leftTopX);
  pane.seamLeftBottom = static_cast<float>(geometry.seams.leftBottomX);
  pane.seamRightTop = static_cast<float>(geometry.seams.rightTopX);
  pane.seamRightBottom = static_cast<float>(geometry.seams.rightBottomX);
}

void shade(bool selected, psx::render::Pane &pane) {
  const float brightness = selected ? 1.0f : kUnselectedBrightness;
  pane.desaturation = selected ? 0.0f : kUnselectedDesaturation;
  pane.tintR = brightness;
  pane.tintG = brightness;
  pane.tintB = brightness;
}

} // namespace

std::span<const psx::render::Pane> PickerPanes::build(const PickerLayout &layout,
                                                      std::span<const PanelSource> panels,
                                                      int selected,
                                                      psx::render::RenderDevice &device) {
  panes_.clear();
  dividers_.clear();
  const int surfaceHeight = panels.empty() ? 0 : layout.panel(0).bounds.h;
  const int dividerPixels =
      std::max(kDividerMinimumPixels, static_cast<int>(static_cast<float>(surfaceHeight) * kDividerPerHeight));
  for (std::size_t index = 0; index < panels.size(); ++index) {
    const PanelSource &source = panels[index];
    const bool isSelected = static_cast<int>(index) == selected;
    const PanelLayout geometry = layout.panel(static_cast<int>(index));

    // The picture at its own size, centred on the panel's cover; the seams decide what shows.
    if (source.picture.valid()) {
      psx::render::Pane picture{};
      picture.texture = source.picture.texture;
      picture.width = static_cast<float>(source.picture.width);
      picture.height = static_cast<float>(source.picture.height);
      picture.x = static_cast<float>(geometry.cover.x + (geometry.cover.w - source.picture.width) / 2);
      picture.y = static_cast<float>(geometry.cover.y + (geometry.cover.h - source.picture.height) / 2);
      cutBy(geometry, picture);
      shade(isSelected, picture);
      panes_.push_back(picture);
    }

    // The title's own logo at its own texel size, narrowed only to fit the panel.
    if (source.logo != nullptr && !source.logo->empty()) {
      const int logoWidth = std::clamp(geometry.averageWidth(), 1, source.logo->width());
      const int logoHeight = std::max(
          1,
          static_cast<int>(std::lround(static_cast<double>(logoWidth) * source.logo->height() / source.logo->width())));
      const psx::present::PaneRect box = geometry.logoRect(logoWidth, logoHeight);
      psx::render::Pane logo{};
      logo.texture = source.logo->texture(device);
      logo.x = static_cast<float>(box.x);
      logo.y = static_cast<float>(box.y);
      logo.width = static_cast<float>(box.w);
      logo.height = static_cast<float>(box.h);
      cutBy(geometry, logo);
      shade(isSelected, logo);
      panes_.push_back(logo);
    }

    lucent::debug("picker",
                  "panel {} picture {}x{} logo {} cover {}x{} at {},{}",
                  index,
                  source.picture.width,
                  source.picture.height,
                  source.logo != nullptr ? "yes" : "no",
                  geometry.cover.w,
                  geometry.cover.h,
                  geometry.cover.x,
                  geometry.cover.y);

    if (index + 1 >= panels.size()) {
      continue;
    }
    // The shared seam, half a divider either side of it; the strip spans the full height so the
    // leaning line fits inside it.
    const float seamTop = static_cast<float>(geometry.seams.rightTopX);
    const float seamBottom = static_cast<float>(geometry.seams.rightBottomX);
    const float half = static_cast<float>(dividerPixels) * 0.5f;
    psx::render::Pane divider{};
    divider.solid = true;
    divider.x = std::min(seamTop, seamBottom) - half;
    divider.y = 0.0f;
    divider.width = std::max(seamTop, seamBottom) + half - divider.x;
    divider.height = static_cast<float>(surfaceHeight);
    divider.seamLeftTop = seamTop - half;
    divider.seamLeftBottom = seamBottom - half;
    divider.seamRightTop = seamTop + half;
    divider.seamRightBottom = seamBottom + half;
    divider.tintR = kDividerRed;
    divider.tintG = kDividerGreen;
    divider.tintB = kDividerBlue;
    dividers_.push_back(divider);
  }
  // After every panel, because the next panel's cover overlaps the seam a divider sits on.
  panes_.insert(panes_.end(), dividers_.begin(), dividers_.end());
  return panes_;
}

} // namespace psx::host
