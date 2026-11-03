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

// gpu.c draws the record's textured primitives that sample one of `textures` again, over a cleared scratch
// device, into one image over their screen bounds; a pixel they did not write is transparent. nullopt when
// none was drawn or none wrote a pixel.
std::optional<psx::host::LogoImage>
drawnLogo(const FrameRecord &record, std::span<const std::uint16_t> vram, std::span<const LogoTexture> textures);

} // namespace psx::present
