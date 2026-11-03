// A title's logo as its own title screen drew it, rebuilt from the frame record and VRAM.
#pragma once

#include "logo_image.h"
#include "vram_rect.h"

#include <cstdint>
#include <optional>
#include <span>

namespace psx::present {

class FrameRecord;

// A texture a logo is drawn from: its page origin in VRAM and, when the page also holds other art, the palette
// that only the logo is drawn through.
struct LogoTexture {
  psx::gpu::VramPoint page;
  std::optional<psx::gpu::VramPoint> clut;
};

// Rasterizes the record's textured primitives that sample one of `textures` into one image over their screen
// bounds; texel 0 stays transparent. nullopt when none was drawn or the draws hold too little artwork to be the
// wordmark yet.
std::optional<psx::host::LogoImage>
drawnLogo(const FrameRecord &record, std::span<const std::uint16_t> vram, std::span<const LogoTexture> textures);

} // namespace psx::present
