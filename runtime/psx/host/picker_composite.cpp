#include "picker_composite.h"

#include "game.h"
#include "gpu_vk_device.h"

#include <lucent/log.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace psx::host {
namespace {

// An unselected panel is grey, R == G == B; a partial desaturation leaves enough hue to read as
// this one is the game.
constexpr float kUnselectedDesaturation = 1.0f;
constexpr float kUnselectedBrightness = 0.78f;

// The panels tile the window, so there is no backdrop: what the picture cannot cover is cropped
// away, not letterboxed. The slanted divider is the one piece of host furniture left.

// The dividers: a thin authored line, half a percent of the surface height (at least one
// pixel), in the same gold the picker's screen uses for the selection so the line and the name
// agree about which panel is the chosen one.
constexpr float kDividerPerHeight = 0.005f;
constexpr int kDividerMinimumPixels = 1;
constexpr float kDividerRed = 0.76f;
constexpr float kDividerGreen = 0.64f;
constexpr float kDividerBlue = 0.18f;

// A logo drawn over a running demo is furniture ON the picture, so it is fully opaque: it is the
// title's own wordmark, and a wordmark half faded into a demo is not a wordmark.
constexpr float kLogoAlpha = 1.0f;

// The panel's picture is a 240-line guest frame cover-cropped into a window-tall panel, so the
// logo is magnified by the same factor the panel applies to the picture: on the console the guest
// draws that wordmark at the same scale as everything else in the frame. A whole factor is applied
// texel by texel so the logo keeps the game's pixel grid instead of being interpolated.
int pictureMagnification(const PanelLayout &geometry, const SourceCrop &crop, int pictureHeight) {
  // `crop` is in fractions of the picture, so the rows kept is crop.h * pictureHeight.
  const double rows = static_cast<double>(crop.h) * static_cast<double>(pictureHeight);
  if (rows <= 0.0 || geometry.cover.h <= 0) {
    return 1;
  }
  const double scale = static_cast<double>(geometry.cover.h) / rows;
  const int whole = static_cast<int>(std::lround(scale));
  // A panel is never a hundred times its picture; anything past that is a number that went wrong,
  // and magnifying by it would allocate the wrong image rather than draw one.
  if (whole > 16) {
    return 1;
  }
  // Only a scale that IS whole is applied texel by texel; anything else is left to the draw, which
  // is what the picture itself does, so the logo and the picture never disagree about
  // magnification.
  return whole >= 1 && std::abs(scale - whole) < 0.02 ? whole : 1;
}

// A logo wider than the panel would be cut by the seams.
int logoWidthAt(const PanelLogo &logo, int panelWidth) {
  if (logo.empty() || logo.height() <= 0) {
    return 0;
  }
  if (panelWidth <= 0) {
    return logo.width();
  }
  return std::max(1, std::min(panelWidth, logo.width()));
}

} // namespace

PickerComposite::PickerComposite(GpuDevice &device, Core *hostCore)
    : m_compositor(device, hostCore), m_device(device) {}

PickerComposite::~PickerComposite() = default;

void PickerComposite::setSurface(const PickerLayout &layout, int width, int height) {
  m_surfaceW = std::max(width, 0);
  m_surfaceH = std::max(height, 0);
  m_paneImageW = layout.maxPanelWidth(m_surfaceW);
  m_paneImageH = m_surfaceH;
}

void PickerComposite::present(const PickerLayout &layout, std::span<const PanelSource> panels, int selected) {
  m_panes.clear();
  m_dividers.clear();
  // One entry per panel, then one divider per boundary.
  m_panes.reserve(panels.size() * 2);
  m_dividers.reserve(panels.size());
  const int dividerPixels = std::max(kDividerMinimumPixels, static_cast<int>(m_surfaceH * kDividerPerHeight));
  for (std::size_t index = 0; index < panels.size(); ++index) {
    const PanelSource &source = panels[index];
    const bool isSelected = static_cast<int>(index) == selected;
    const PanelLayout geometry = layout.panel(static_cast<int>(index), source.pictureWidth, source.pictureHeight);

    // Every pane in a panel — picture and logo — is cut by the same two lines, in the layout's
    // surface pixels, so nothing inside a panel can leak over a neighbour.
    const auto seamsOf = [&geometry](psxport::Pane &pane) {
      pane.seamLeftTop = static_cast<float>(geometry.seams.leftTopX);
      pane.seamLeftBottom = static_cast<float>(geometry.seams.leftBottomX);
      pane.seamRightTop = static_cast<float>(geometry.seams.rightTopX);
      pane.seamRightBottom = static_cast<float>(geometry.seams.rightBottomX);
    };

    // The pane is the panel's own rectangle, full height, with no backdrop and no letterbox.
    // How much is cropped is the layout's number, which is what keeps the aspect while
    // filling the shape.
    psxport::Pane pane{};
    pane.core = source.session;
    pane.originX = static_cast<float>(geometry.cover.x);
    pane.originY = static_cast<float>(geometry.cover.y);
    pane.axisUX = static_cast<float>(geometry.cover.w);
    pane.axisUY = 0.0f;
    pane.axisVX = 0.0f;
    pane.axisVY = static_cast<float>(geometry.cover.h);
    const SourceCrop crop = geometry.sourceCrop(source.pictureWidth, source.pictureHeight);
    pane.sourceU = crop.u;
    pane.sourceV = crop.v;
    pane.sourceW = crop.w;
    pane.sourceH = crop.h;
    seamsOf(pane);
    // A per-pane uniform in the pane shader, so it costs nothing on the host.
    pane.desaturation = isSelected ? 0.0f : kUnselectedDesaturation;
    const float brightness = isSelected ? 1.0f : kUnselectedBrightness;
    pane.tintR = brightness;
    pane.tintG = brightness;
    pane.tintB = brightness;
    m_panes.push_back(pane);

    // The title's own logo, centred on the panel; where it sits vertically is the layout's.
    // It takes the panel's own grey when unselected: a colour logo over a grey panel would
    // name the chosen title on the wrong column.
    if (source.logo != nullptr && !source.logo->empty()) {
      const int magnification = pictureMagnification(geometry, crop, source.pictureHeight);
      // Cached inside the logo, so the per-frame ask costs one magnification, not one a frame.
      const PanelLogo *const drawn = &source.logo->magnifiedNearest(magnification);
      const int logoWidth = logoWidthAt(*drawn, geometry.averageWidth());
      const int logoHeight =
          std::max(1,
                   static_cast<int>(std::lround(static_cast<double>(logoWidth) * static_cast<double>(drawn->height()) /
                                                static_cast<double>(drawn->width()))));
      const PanelRect logoBox = geometry.logoRect(logoWidth, logoHeight);
      psxport::Pane logo{};
      logo.texture = drawn->texture(m_device);
      logo.originX = static_cast<float>(logoBox.x);
      logo.originY = static_cast<float>(logoBox.y);
      logo.axisUX = static_cast<float>(logoBox.w);
      logo.axisUY = 0.0f;
      logo.axisVX = 0.0f;
      logo.axisVY = static_cast<float>(logoBox.h);
      // The logo is the panel's own image scaled to the panel's own width, so it is drawn from the
      // whole logo and never from a crop of it.
      logo.desaturation = isSelected ? 0.0f : kUnselectedDesaturation;
      logo.tintR = brightness;
      logo.tintG = brightness;
      logo.tintB = brightness;
      logo.alpha = kLogoAlpha;
      seamsOf(logo);
      lucent::debug("picker",
                    "logo panel {} image {}x{} magnified {}x drawn {}x{} at {},{} cover {}x{} at {},{}",
                    index,
                    source.logo->width(),
                    source.logo->height(),
                    magnification,
                    logoBox.w,
                    logoBox.h,
                    logoBox.x,
                    logoBox.y,
                    geometry.cover.w,
                    geometry.cover.h,
                    geometry.cover.x,
                    geometry.cover.y);
      m_panes.push_back(logo);
    }

    if (index + 1 >= panels.size()) {
      continue;
    }
    // Recorded, not appended; see the pass order below.
    const float seamTop = static_cast<float>(geometry.seams.rightTopX);
    const float seamBottom = static_cast<float>(geometry.seams.rightBottomX);
    const float half = static_cast<float>(dividerPixels) * 0.5f;
    psxport::Pane divider{};
    divider.solid = true;
    // The rectangle must enclose the mask or the mask erases the pane, so this leaning line's
    // strip spans the surface's full height.
    const float bandLeft = std::min(seamTop, seamBottom) - half;
    const float bandRight = std::max(seamTop, seamBottom) + half;
    divider.originX = bandLeft;
    divider.originY = 0.0f;
    divider.axisUX = bandRight - bandLeft;
    divider.axisUY = 0.0f;
    divider.axisVX = 0.0f;
    divider.axisVY = static_cast<float>(m_surfaceH);
    // The line is the shared seam, half a divider's width either side of it: two neighbours cut by
    // the same numbers, so it is drawn once and both agree where it is.
    divider.seamLeftTop = seamTop - half;
    divider.seamLeftBottom = seamBottom - half;
    divider.seamRightTop = seamTop + half;
    divider.seamRightBottom = seamBottom + half;
    divider.tintR = kDividerRed;
    divider.tintG = kDividerGreen;
    divider.tintB = kDividerBlue;
    m_dividers.push_back(divider);
  }

  // A divider sits on a seam the next panel also covers, so drawing it inline means the panel it
  // separates paints over it.
  for (const psxport::Pane &divider : m_dividers) {
    m_panes.push_back(divider);
  }
  m_dividers.clear();

  // Black bands at a panel's top and bottom are either the guest's own rows or this
  // rectangle being wrong, and this line is which.
  if (lucent::detail::channel_enabled("picker")) {
    for (std::size_t index = 0; index < panels.size(); ++index) {
      const PanelSource &source = panels[index];
      const GpuVkState::PresentedImage image =
          source.session != nullptr ? source.session->game->gpu_vk.lastFilledPresented() : GpuVkState::PresentedImage{};
      const PanelLayout geometry = layout.panel(static_cast<int>(index), source.pictureWidth, source.pictureHeight);
      lucent::debug("picker",
                    "panel {} image {}x{} viewport {}x{} at +{}+{} into {}x{} at +{}+{} held={}",
                    index,
                    image.width,
                    image.height,
                    image.viewport.w,
                    image.viewport.h,
                    image.viewport.x,
                    image.viewport.y,
                    geometry.cover.w,
                    geometry.cover.h,
                    geometry.cover.x,
                    geometry.cover.y,
                    image.valid() ? "yes" : "no");
    }
  }

  m_compositor.composite(m_panes);
}

void PickerComposite::captureShot(const char *path) {
  m_compositor.presentShot(path);
}

} // namespace psx::host