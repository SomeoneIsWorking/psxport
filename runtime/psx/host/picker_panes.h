// The title selector's picture: each panel a tile with its title's art at the selector's one texel scale, and the
// dividers.
#pragma once

#include "picker_layout.h"
#include "render_pane.h"

#include <span>
#include <vector>

namespace psx::render {
class RenderDevice;
} // namespace psx::render

namespace psx::host {

class PanelArt;

class PickerPanes {
public:
  // The panes for one frame, drawn in order; valid until the next build. `art` has one entry per panel, null for a
  // panel with no art.
  std::span<const psx::render::Pane> build(const PickerLayout &layout,
                                           std::span<const PanelArt *const> art,
                                           int selected,
                                           psx::render::RenderDevice &device);

private:
  std::vector<psx::render::Pane> panes_;
  std::vector<psx::render::Pane> dividers_;
};

} // namespace psx::host
