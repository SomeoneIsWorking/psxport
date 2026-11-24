#include "picker_panes.h"

#include "panel_art.h"

#include <algorithm>
#include <cstddef>

namespace psx::host {
namespace {

// An unselected panel is pure greyscale.
constexpr float kUnselectedDesaturation = 1.0f;
constexpr float kUnselectedBrightness = 1.0f;

// A divider is half a percent of the surface height (at least one pixel), in the selection's gold.
constexpr float kDividerPerHeight = 0.005f;
constexpr int kDividerMinimumPixels = 1;
constexpr float kDividerRed = 0.76f;
constexpr float kDividerGreen = 0.64f;
constexpr float kDividerBlue = 0.18f;

// Every panel is a plain tile under its art.
constexpr float kTileRed = 0.11f;
constexpr float kTileGreen = 0.11f;
constexpr float kTileBlue = 0.13f;

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

// The largest whole texel scale at which every panel's art fits the window's height and a selected panel's clear
// width, so selection changes a panel's width and never its art's scale.
int artScale(const PickerLayout &layout, std::span<const PanelArt *const> art, int surfaceHeight) {
  int scale = 0;
  for (const PanelArt *panel : art) {
    if (panel == nullptr) {
      continue;
    }
    const int fits = std::min(surfaceHeight / panel->height(), layout.selectedClearWidth() / panel->width());
    scale = scale == 0 ? fits : std::min(scale, fits);
  }
  return std::max(scale, 1);
}

} // namespace

std::span<const psx::render::Pane> PickerPanes::build(const PickerLayout &layout,
                                                      std::span<const PanelArt *const> art,
                                                      int selected,
                                                      psx::render::RenderDevice &device) {
  panes_.clear();
  dividers_.clear();
  const int surfaceHeight = art.empty() ? 0 : layout.panel(0).bounds.h;
  const int dividerPixels =
      std::max(kDividerMinimumPixels, static_cast<int>(static_cast<float>(surfaceHeight) * kDividerPerHeight));
  const int scale = artScale(layout, art, surfaceHeight);
  for (std::size_t index = 0; index < art.size(); ++index) {
    const PanelArt *picture = art[index];
    const bool isSelected = static_cast<int>(index) == selected;
    const PanelLayout geometry = layout.panel(static_cast<int>(index));

    psx::render::Pane tile{};
    tile.solid = true;
    tile.x = static_cast<float>(geometry.cover.x);
    tile.y = static_cast<float>(geometry.cover.y);
    tile.width = static_cast<float>(geometry.cover.w);
    tile.height = static_cast<float>(geometry.cover.h);
    cutBy(geometry, tile);
    shade(isSelected, tile);
    tile.tintR *= kTileRed;
    tile.tintG *= kTileGreen;
    tile.tintB *= kTileBlue;
    panes_.push_back(tile);

    // Centred on the panel's cover at whole texels, so the nearest sampler keeps it sharp; the seams cut it.
    if (picture != nullptr) {
      const int width = picture->width() * scale;
      const int height = picture->height() * scale;
      psx::render::Pane pane{};
      pane.texture = picture->texture(device);
      pane.x = static_cast<float>(geometry.cover.x + (geometry.cover.w - width) / 2);
      pane.y = static_cast<float>(geometry.cover.y + (geometry.cover.h - height) / 2);
      pane.width = static_cast<float>(width);
      pane.height = static_cast<float>(height);
      cutBy(geometry, pane);
      shade(isSelected, pane);
      panes_.push_back(pane);
    }

    if (index + 1 >= art.size()) {
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
