// One window frame with a panel per title: backdrops, fitted pictures, logos and dividers.
#pragma once

#include "panel_logo.h"
#include "picker_layout.h"

#include "pane_composite.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

class Core;
class GpuDevice;

namespace psx::host {

// One panel's contribution to a frame.
struct PanelSource {
  Core *session = nullptr; // null while the panel has no picture of its own
  int pictureWidth = 1;    // the picture's own aspect, in pixels
  int pictureHeight = 1;
  // The title's own logo, decoded once from its own disc; null until its session has booted far
  // enough for the guest to have drawn it, which is the truth rather than a placeholder wordmark.
  PanelLogo *logo = nullptr;
};

class PickerComposite {
public:
  PickerComposite(GpuDevice &device, Core *hostCore);
  ~PickerComposite();
  PickerComposite(const PickerComposite &) = delete;
  PickerComposite &operator=(const PickerComposite &) = delete;

  // Every panel's present image is built at the selected panel's widest size, so the sessions are
  // told the image size before any of them steps.
  void setSurface(const PickerLayout &layout, int width, int height);
  int surfaceWidth() const {
    return m_surfaceW;
  }
  int surfaceHeight() const {
    return m_surfaceH;
  }
  // The present-image size a panel session should build its picture at.
  int paneImageWidth() const {
    return m_paneImageW;
  }
  int paneImageHeight() const {
    return m_paneImageH;
  }

  // Build the frame: every panel's backdrop, its picture, its title's own logo, and the dividers.
  void present(const PickerLayout &layout, std::span<const PanelSource> panels, int selected);

  // What the player sees of the selector, in either leg.
  void captureShot(const char *path);

private:
  psxport::PaneCompositor m_compositor;
  GpuDevice &m_device;
  std::vector<psxport::Pane> m_panes;
  // Held back until every panel is drawn; see the pass order in the .cpp.
  std::vector<psxport::Pane> m_dividers;
  int m_surfaceW = 0, m_surfaceH = 0;
  int m_paneImageW = 0, m_paneImageH = 0;
};

} // namespace psx::host
