// outline_text.h — a title's bitmap font drawn from outline fonts at the target's resolution (plan 0207).
//
// The title names its fonts (OutlineFont: a page and its glyph rectangles) and, for each packet its text drawers
// filled with glyphs, the font it drew from (OutlineGlyphs). Each sealed frame that drew such packets brings the
// atlas up to date with those fonts' glyphs in VRAM and the pad family in the player's hand: every glyph gets its
// codepoint's outline, fitted to the box of its own bitmap glyph, as a multi-channel distance field (msdfgen), and
// keeps the bitmap's CLUT indices for its colours. The record rasterizer samples the atlas for those packets'
// primitives (kOpOutline) instead of VRAM, so a glyph keeps its ordering-table slot, its widescreen place and its
// replay.
#pragma once

#include "frame_record.h"
#include "vram_pixel.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace psx::input {
class GlyphSet;
}

namespace psx::present {

enum class OutlineFace : std::uint8_t {
  Text,   // the title's text face (OutlineFont::textFace)
  Prompt, // PromptFont, the face of the pad glyphs
};

// One glyph of a font page: the rectangle its text drawer samples, in texels of the page, and what it shows:
// `codepoint` of `face`, or the glyph of PSX button `button` (kButton*) for the pad the player holds.
struct OutlineGlyph {
  std::uint16_t u = 0;
  std::uint16_t v = 0;
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  char32_t codepoint = 0;
  OutlineFace face = OutlineFace::Text;
  std::uint16_t button = 0;
};

// Equal cells across a page from (u, v), `columns` a row, numbered row-major: one way to name a font's glyphs.
struct OutlineGrid {
  int u = 0;
  int v = 0;
  int cellWidth = 0;
  int cellHeight = 0;
  int columns = 0;

  [[nodiscard]] constexpr OutlineGlyph glyph(int cell, char32_t codepoint, OutlineFace face = OutlineFace::Text) const {
    return {.u = static_cast<std::uint16_t>(u + ((cell % columns) * cellWidth)),
            .v = static_cast<std::uint16_t>(v + ((cell / columns) * cellHeight)),
            .width = static_cast<std::uint16_t>(cellWidth),
            .height = static_cast<std::uint16_t>(cellHeight),
            .codepoint = codepoint,
            .face = face};
  }
  [[nodiscard]] constexpr OutlineGlyph button(int cell, std::uint16_t psxButton) const {
    OutlineGlyph named = glyph(cell, 0, OutlineFace::Prompt);
    named.button = psxButton;
    return named;
  }
};

// A bitmap font of a title as its text drawer samples it.
struct OutlineFont {
  psx::gpu::TexPage page; // indexed
  std::span<const OutlineGlyph> glyphs;
  const char *textFace = ""; // asset file under assets/rml
  double weight = 0;         // the text face's wght axis, 0 for its default
  // Whether an outline keeps its proportions at its bitmap glyph's height, centred across the glyph's box and no
  // wider than it, rather than filling the box: for a bitmap font whose glyph shapes the face does not share (serifs).
  bool proportional = false;
  // The CLUT indices of the drop shadow the bitmap glyphs carry, and the texels it lies right of and below its glyph.
  // A glyph holding only those indices has no shadow.
  std::span<const int> shadowIndices;
  int shadowX = 0;
  int shadowY = 0;
};

// The fonts a title's text drawers sample, at most this many (record_raster_setup.h kOpOutlineFontShift).
inline constexpr std::size_t kMaxOutlineFonts = 4;

// A title's naming of the packets that drew its fonts' glyphs.
class OutlineGlyphs {
public:
  virtual ~OutlineGlyphs() = default;
  // Asked as the draw that linked it has just run: the font (an index into GameRuntime::outlineFonts) whose glyph the
  // ordering-table packet at `packet` draws, or none.
  [[nodiscard]] virtual std::optional<std::uint8_t> glyphFont(std::uint32_t packet) const = 0;
};

// Where one font lies in the atlas: its page texels from `originX`, `originY` (in texels of the page's mode across
// VRAM), `width` x `height`, at atlas texel (0, atlasY).
struct OutlineAtlasFont {
  int originX = 0;
  int originY = 0;
  int width = 0;
  int height = 0;
  int atlasY = 0;
};

// What record.frag samples for kOpOutline. `fields` is `scale` pixels a texel over the fonts' glyphs: RGB a
// multi-channel distance field of each glyph, A a distance field of its shadow. `indices` is one pixel a texel, R and
// G the CLUT indices of its row of the bitmap glyph and of its shadow, B a button glyph's slot in `buttons` plus one,
// A 255 where a named glyph's field lies; a texel without one draws from VRAM.
struct OutlineAtlas {
  int scale = 0;
  int width = 0; // texels
  int height = 0;
  std::array<OutlineAtlasFont, kMaxOutlineFonts> fonts{};
  std::array<std::uint16_t, 4> buttons{}; // each PSX button (kButton*) the fonts' glyphs show, in font and glyph order
  std::vector<std::uint8_t> fields;       // RGBA8
  std::vector<std::uint8_t> indices;      // RGBA8
  std::uint64_t generation = 0;
};

class OutlineText {
public:
  OutlineText();
  ~OutlineText();
  OutlineText(const OutlineText &) = delete;
  OutlineText &operator=(const OutlineText &) = delete;

  // A sealed frame drew glyphs of the fonts `drawn` flags (bit i: fonts[i]): the atlas follows their glyphs in `vram`
  // and the pad family of `pads`.
  void drew(std::span<const OutlineFont> fonts,
            unsigned drawn,
            std::span<const std::uint16_t> vram,
            const psx::input::GlyphSet &pads);
  // Null until a font's atlas is built.
  [[nodiscard]] const OutlineAtlas *atlas() const;

private:
  class Builder;
  std::unique_ptr<Builder> builder_;
};

// The face colour of each of `atlas`'s button slots in the family of `pads`, as a VRAM halfword without its
// semi-transparency bit; 0 where the game's own colour stands.
std::array<std::uint32_t, 4> buttonFaceColours(const OutlineAtlas &atlas, const psx::input::GlyphSet &pads);

// Tags the primitives among a sealed frame's `entries` whose packets `glyphs` names with their font; when any, `text`'s
// atlas follows those fonts' glyphs in `vram` and the pad family of `pads`. Nothing without both fonts and naming.
void markGlyphs(std::span<const OutlineFont> fonts,
                const OutlineGlyphs *glyphs,
                std::span<RecordEntry> entries,
                OutlineText &text,
                std::span<const std::uint16_t> vram,
                const psx::input::GlyphSet &pads);

} // namespace psx::present
