// outline_text.h — a title's bitmap font drawn from outline fonts at the target's resolution (plan 0207).
//
// The title names its font page (OutlineFont) and the packets its text drawer filled with font cells
// (OutlineGlyphs). Each sealed frame that drew such packets brings the atlas up to date with the page's cells in
// VRAM and the pad family in the player's hand: every cell gets its codepoint's glyph from the outline face, fitted
// to the box of the cell's own bitmap glyph, as a multi-channel distance field (msdfgen), and keeps the bitmap's CLUT
// indices for its colours. The record rasterizer samples the atlas for those packets' primitives (kOpOutline)
// instead of VRAM, so a glyph keeps its ordering-table slot, its widescreen place and its replay.
#pragma once

#include "frame_record.h"
#include "vram_pixel.h"

#include <cstdint>
#include <memory>
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

// One cell of the page and what it shows: `codepoint` of `face`, or the glyph of PSX button `button` (kButton*) for
// the pad the player holds.
struct OutlineCell {
  std::uint16_t cell = 0; // row-major in the page's grid
  char32_t codepoint = 0;
  OutlineFace face = OutlineFace::Text;
  std::uint16_t button = 0;
};

// A title's bitmap font as its text drawer samples it.
struct OutlineFont {
  psx::gpu::TexPage page; // indexed
  int cellWidth = 0;      // texels
  int cellHeight = 0;
  int columns = 0; // cells per grid row
  std::span<const OutlineCell> cells;
  const char *textFace = ""; // asset file under assets/rml
  double weight = 0;         // the text face's wght axis, 0 for its default
  // The CLUT indices of the drop shadow the bitmap glyphs carry, and the texels it lies right of and below its glyph.
  // A cell holding only those indices has no shadow.
  std::span<const int> shadowIndices;
  int shadowX = 0;
  int shadowY = 0;
};

// A title's naming of the packets that drew its font's cells.
class OutlineGlyphs {
public:
  virtual ~OutlineGlyphs() = default;
  // Asked as the draw that linked it has just run: whether the ordering-table packet at `packet` draws a font cell.
  [[nodiscard]] virtual bool glyphPacket(std::uint32_t packet) const = 0;
};

// What record.frag samples for kOpOutline. `fields` is `scale` pixels a texel over the page's cell rows: RGB a
// multi-channel distance field of each cell's glyph, A a distance field of its shadow. `indices` is one pixel a texel,
// R and G the CLUT indices of its row of the bitmap glyph and of its shadow.
struct OutlineAtlas {
  int scale = 0;
  int width = 0; // texels
  int height = 0;
  int originX = 0; // the first texel, in texels of the page's mode across VRAM
  int originY = 0;
  std::vector<std::uint8_t> fields;  // RGBA8
  std::vector<std::uint8_t> indices; // RGBA8
  std::uint64_t generation = 0;
};

class OutlineText {
public:
  OutlineText();
  ~OutlineText();
  OutlineText(const OutlineText &) = delete;
  OutlineText &operator=(const OutlineText &) = delete;

  // A sealed frame drew `font`'s cells: the atlas follows the page's cells in `vram` and the pad family of `glyphs`.
  void drew(const OutlineFont &font, std::span<const std::uint16_t> vram, const psx::input::GlyphSet &glyphs);
  // Null until a font's atlas is built.
  [[nodiscard]] const OutlineAtlas *atlas() const;

private:
  class Builder;
  std::unique_ptr<Builder> builder_;
};

// Marks the primitives among a sealed frame's `entries` whose packets `glyphs` names; when any, `text`'s atlas follows
// `font`'s cells in `vram` and the pad family of `pads`. Nothing without both a font and its naming.
void markGlyphs(const OutlineFont *font,
                const OutlineGlyphs *glyphs,
                std::span<RecordEntry> entries,
                OutlineText &text,
                std::span<const std::uint16_t> vram,
                const psx::input::GlyphSet &pads);

} // namespace psx::present
