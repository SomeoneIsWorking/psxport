// The title selector's picture: each panel's own present image 1:1, its title's logo, and the dividers.
#pragma once

#include "picker_layout.h"
#include "render_pane.h"
#include "render_state.h"

#include <span>
#include <vector>

namespace psx::render {
class RenderDevice;
} // namespace psx::render

namespace psx::host {

class PanelLogo;

// One panel's content for a frame.
struct PanelSource {
  psx::render::PresentedImage picture; // invalid while the panel has no picture of its own
  PanelLogo *logo = nullptr;           // null until the title's own logo has been read
};

class PickerPanes {
public:
  // The panes for one frame, drawn in order; valid until the next build.
  std::span<const psx::render::Pane> build(const PickerLayout &layout,
                                           std::span<const PanelSource> panels,
                                           int selected,
                                           psx::render::RenderDevice &device);

private:
  std::vector<psx::render::Pane> panes_;
  std::vector<psx::render::Pane> dividers_;
};

} // namespace psx::host
