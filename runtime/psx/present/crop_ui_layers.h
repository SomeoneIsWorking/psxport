// crop_ui_layers.h — a title's naming of the UI in its frames, which crop widescreen (plan 0203) shows at its 4:3
// size and place over the cropped world. A title that provides one offers Crop.
#pragma once

#include <cstdint>

namespace psx::present {

class CropUiLayers {
public:
  virtual ~CropUiLayers() = default;
  // Asked as the draw that linked it has just run: whether the ordering-table packet at `packet` is UI.
  virtual bool uiPacket(std::uint32_t packet) const = 0;
};

} // namespace psx::present
