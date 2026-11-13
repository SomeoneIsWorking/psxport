#include "outline_text.h"

#include "glyph_set.h"
#include "ui_assets.h"

#include <lucent/log.h>
#include <msdfgen-ext.h>
#include <msdfgen.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <string>

namespace psx::present {
namespace {

// Field pixels a texel: one target pixel a texel at 8x, the 4K sink's scale.
constexpr int kFieldScale = 8;
// The distance a field channel spans either side of the edge, field pixels.
constexpr double kFieldRange = 4.0;

// The element of (x, y) in a row-major image `width` wide, `channels` elements a pixel.
constexpr std::size_t offsetOf(int x, int y, int width, int channels = 1) {
  return ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) + static_cast<std::size_t>(x)) *
         static_cast<std::size_t>(channels);
}

// A cell's bitmap glyph: its CLUT indices, the box of its glyph texels (the shadow's excluded), half open, and
// whether it carries the shadow.
struct BitmapCell {
  std::vector<int> indices; // row-major, cellWidth x cellHeight
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  bool shadowed = false;

  bool empty() const {
    return x1 <= x0;
  }
};

// What a cell's field is drawn from; a cell keeps its field while this holds.
struct FieldKey {
  char32_t codepoint = 0;
  OutlineFace face = OutlineFace::Text;
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  bool shadowed = false;

  bool operator==(const FieldKey &) const = default;
};

bool shadowTexel(const OutlineFont &font, int index) {
  return index != 0 && std::ranges::find(font.shadowIndices, index) != font.shadowIndices.end();
}

bool glyphTexel(const OutlineFont &font, int index) {
  return index != 0 && !shadowTexel(font, index);
}

// Whether a texel of `bitmap` is its glyph's ink rather than its shadow.
bool glyphInk(const OutlineFont &font, const BitmapCell &bitmap, int index) {
  return bitmap.shadowed ? glyphTexel(font, index) : index != 0;
}

BitmapCell readCell(const OutlineFont &font, std::span<const std::uint16_t> vram, int cell) {
  const int u0 = (cell % font.columns) * font.cellWidth;
  const int v0 = (cell / font.columns) * font.cellHeight;
  BitmapCell bitmap;
  bitmap.indices.resize(offsetOf(0, font.cellHeight, font.cellWidth));
  const auto halfWordAt = [vram](psx::gpu::VramPoint at) {
    return psx::gpu::vramHalfWord(vram, at);
  };
  bool glyph = false;
  bool shadow = false;
  for (int y = 0; y < font.cellHeight; y++) {
    for (int x = 0; x < font.cellWidth; x++) {
      const int index = psx::gpu::fetchIndex(halfWordAt, font.page, u0 + x, v0 + y);
      bitmap.indices[offsetOf(x, y, font.cellWidth)] = index;
      glyph = glyph || glyphTexel(font, index);
      shadow = shadow || shadowTexel(font, index);
    }
  }
  // A cell of the shadow's indices alone is drawn in them.
  bitmap.shadowed = glyph && shadow;
  bitmap.x0 = font.cellWidth;
  bitmap.y0 = font.cellHeight;
  for (int y = 0; y < font.cellHeight; y++) {
    for (int x = 0; x < font.cellWidth; x++) {
      const int index = bitmap.indices[offsetOf(x, y, font.cellWidth)];
      if (glyphInk(font, bitmap, index)) {
        bitmap.x0 = std::min(bitmap.x0, x);
        bitmap.y0 = std::min(bitmap.y0, y);
        bitmap.x1 = std::max(bitmap.x1, x + 1);
        bitmap.y1 = std::max(bitmap.y1, y + 1);
      }
    }
  }
  return bitmap;
}

// The CLUT index each row of a cell draws `inked` texels in: the font's gradient runs down the cell, so a row takes
// the index most of its such texels hold, and a row without any takes the nearest row's.
template <class Inked> std::vector<int> rowIndices(const OutlineFont &font, const BitmapCell &bitmap, Inked inked) {
  const auto h = static_cast<std::size_t>(font.cellHeight);
  std::vector<int> rows(h, 0);
  for (std::size_t y = 0; y < h; y++) {
    std::array<int, 256> counts{};
    for (int x = 0; x < font.cellWidth; x++) {
      const int index = bitmap.indices[offsetOf(x, static_cast<int>(y), font.cellWidth)];
      if (inked(index)) {
        counts.at(static_cast<std::size_t>(index))++;
      }
    }
    const auto most = std::ranges::max_element(counts);
    rows[y] = *most != 0 ? static_cast<int>(most - counts.begin()) : 0;
  }
  std::vector<int> banded(rows);
  for (std::size_t y = 0; y < h; y++) {
    for (std::size_t d = 1; banded[y] == 0 && d < h; d++) {
      if (y >= d && rows[y - d] != 0) {
        banded[y] = rows[y - d];
      } else if (y + d < h && rows[y + d] != 0) {
        banded[y] = rows[y + d];
      }
    }
  }
  return banded;
}

std::uint8_t fieldByte(float value) {
  return static_cast<std::uint8_t>(std::clamp(value * 256.0f, 0.0f, 255.0f));
}

} // namespace

class OutlineText::Builder {
public:
  Builder() : freetype_(msdfgen::initializeFreetype()) {}
  ~Builder() {
    for (auto &[name, face] : faces_) {
      if (face != nullptr) {
        msdfgen::destroyFont(face);
      }
    }
    if (freetype_ != nullptr) {
      msdfgen::deinitializeFreetype(freetype_);
    }
  }
  Builder(const Builder &) = delete;
  Builder &operator=(const Builder &) = delete;

  void drew(const OutlineFont &font, std::span<const std::uint16_t> vram, const psx::input::GlyphSet &glyphs) {
    std::vector<BitmapCell> bitmaps;
    std::vector<char32_t> codepoints;
    for (const OutlineCell &cell : font.cells) {
      bitmaps.push_back(readCell(font, vram, cell.cell));
      codepoints.push_back(cell.button != 0 ? glyphs.glyph(cell.button) : cell.codepoint);
    }
    const auto sameIndices = [](const BitmapCell &a, const BitmapCell &b) {
      return a.indices == b.indices;
    };
    if (&font == font_ && codepoints == codepoints_ && std::ranges::equal(bitmaps, bitmaps_, sameIndices)) {
      return;
    }
    if (&font != font_) {
      fields_.clear();
    }
    font_ = &font;
    bitmaps_ = std::move(bitmaps);
    codepoints_ = std::move(codepoints);
    build(font);
  }

  const OutlineAtlas *atlas() const {
    return atlas_.generation != 0 ? &atlas_ : nullptr;
  }

private:
  struct CellField {
    FieldKey key;
    std::vector<std::uint8_t> rgba; // the cell's tile, cellWidth x cellHeight texels at kFieldScale
  };

  msdfgen::FontHandle *face(const OutlineFont &font, OutlineFace which) {
    const std::string name = which == OutlineFace::Prompt ? "promptfont.ttf" : font.textFace;
    const auto [at, added] = faces_.try_emplace(name, nullptr);
    if (!added) {
      return at->second;
    }
    psx::ui::AssetSet assets;
    std::string path;
    if (freetype_ == nullptr || !assets.open() || !assets.require(name.c_str(), path)) {
      return nullptr;
    }
    at->second = msdfgen::loadFont(freetype_, path.c_str());
    if (at->second == nullptr) {
      lucent::error("outline", "font {} present but FreeType rejected it", path);
      return nullptr;
    }
    if (which == OutlineFace::Text && font.weight != 0 &&
        !msdfgen::setFontVariationAxis(freetype_, at->second, msdfgen::FontVariationAxis::Tag("wght"), font.weight)) {
      lucent::error("outline", "font {} has no weight axis to set to {}", path, font.weight);
    }
    return at->second;
  }

  // The cell's tile: RGB the glyph's field fitted to the bitmap glyph's box, A the shadow's.
  bool drawField(const OutlineFont &font, int cell, const FieldKey &key, std::vector<std::uint8_t> &rgba) {
    const int w = font.cellWidth * kFieldScale;
    const int h = font.cellHeight * kFieldScale;
    rgba.assign(offsetOf(0, h, w, 4), 0);
    msdfgen::FontHandle *handle = face(font, key.face);
    msdfgen::Shape shape;
    // Glyph 0 is the face's missing-glyph box.
    msdfgen::GlyphIndex glyphIndex;
    if (handle == nullptr || !msdfgen::getGlyphIndex(glyphIndex, handle, msdfgen::unicode_t{key.codepoint}) ||
        glyphIndex.getIndex() == 0 ||
        !msdfgen::loadGlyph(shape, handle, glyphIndex, msdfgen::FONT_SCALING_EM_NORMALIZED) || shape.contours.empty()) {
      lucent::error("outline",
                    "no outline for cell {} (U+{:04X}); it draws nothing",
                    cell,
                    static_cast<std::uint32_t>(key.codepoint));
      return false;
    }
    shape.normalize();
    msdfgen::edgeColoringSimple(shape, 3.0);
    const msdfgen::Shape::Bounds bounds = shape.getBounds();
    // Field pixels measured up from the tile's bottom, as msdfgen's bitmaps run.
    const double left = key.x0 * kFieldScale;
    const double right = key.x1 * kFieldScale;
    const double bottom = h - (key.y1 * kFieldScale);
    const double top = h - (key.y0 * kFieldScale);
    const msdfgen::Vector2 scale((right - left) / (bounds.r - bounds.l), (top - bottom) / (bounds.t - bounds.b));
    const msdfgen::Vector2 translate(left / scale.x - bounds.l, bottom / scale.y - bounds.b);
    const msdfgen::DistanceMapping mapping(msdfgen::Range(kFieldRange / std::min(scale.x, scale.y)));
    // Variable faces keep overlapping contours: the fields take their sign from the nonzero fill, then the MSDF its
    // error correction, as msdfgen's scanline pass does.
    const msdfgen::SDFTransformation placed(msdfgen::Projection(scale, translate), mapping);
    msdfgen::MSDFGeneratorConfig generate;
    generate.errorCorrection.mode = msdfgen::ErrorCorrectionConfig::DISABLED;
    msdfgen::MSDFGeneratorConfig correct;
    correct.errorCorrection.distanceCheckMode = msdfgen::ErrorCorrectionConfig::DO_NOT_CHECK_DISTANCE;
    msdfgen::Bitmap<float, 3> glyph(w, h);
    msdfgen::generateMSDF(glyph, shape, placed, generate);
    msdfgen::distanceSignCorrection(glyph, shape, placed, msdfgen::FILL_NONZERO);
    msdfgen::msdfErrorCorrection(glyph, shape, placed, correct);
    msdfgen::Bitmap<float, 1> shadow(w, h);
    if (key.shadowed) {
      const msdfgen::Vector2 shifted(translate.x + font.shadowX * kFieldScale / scale.x,
                                     translate.y - font.shadowY * kFieldScale / scale.y);
      const msdfgen::SDFTransformation cast(msdfgen::Projection(scale, shifted), mapping);
      msdfgen::generateSDF(shadow, shape, cast);
      msdfgen::distanceSignCorrection(shadow, shape, cast, msdfgen::FILL_NONZERO);
    }
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        std::uint8_t *pixel = &rgba[offsetOf(x, h - 1 - y, w, 4)];
        const float *channels = glyph(x, y);
        pixel[0] = fieldByte(channels[0]);
        pixel[1] = fieldByte(channels[1]);
        pixel[2] = fieldByte(channels[2]);
        pixel[3] = key.shadowed ? fieldByte(*shadow(x, y)) : 0;
      }
    }
    return true;
  }

  void build(const OutlineFont &font) {
    int columns = 0;
    int rows = 0;
    for (const OutlineCell &cell : font.cells) {
      columns = std::max(columns, cell.cell % font.columns + 1);
      rows = std::max(rows, cell.cell / font.columns + 1);
    }
    OutlineAtlas next;
    next.scale = kFieldScale;
    next.width = columns * font.cellWidth;
    next.height = rows * font.cellHeight;
    next.originX = font.page.x << (2 - static_cast<int>(font.page.depth));
    next.originY = font.page.y;
    const int fieldWidth = next.width * kFieldScale;
    next.fields.assign(offsetOf(0, next.height * kFieldScale, fieldWidth, 4), 0);
    next.indices.assign(offsetOf(0, next.height, next.width, 4), 0);
    int drawn = 0;
    for (std::size_t i = 0; i < font.cells.size(); i++) {
      const OutlineCell &cell = font.cells[i];
      const BitmapCell &bitmap = bitmaps_[i];
      if (bitmap.empty()) {
        continue;
      }
      const FieldKey key{codepoints_[i],
                         cell.button != 0 ? OutlineFace::Prompt : cell.face,
                         bitmap.x0,
                         bitmap.y0,
                         bitmap.x1,
                         bitmap.y1,
                         bitmap.shadowed};
      CellField &field = fields_[cell.cell];
      if (field.rgba.empty() || !(field.key == key)) {
        field.key = key;
        if (!drawField(font, cell.cell, key, field.rgba)) {
          continue;
        }
        ++drawn;
      }
      const int tileX = (cell.cell % font.columns) * font.cellWidth;
      const int tileY = (cell.cell / font.columns) * font.cellHeight;
      const int tileWidth = font.cellWidth * kFieldScale;
      for (int y = 0; y < font.cellHeight * kFieldScale; y++) {
        std::copy_n(&field.rgba[offsetOf(0, y, tileWidth, 4)],
                    tileWidth * 4,
                    &next.fields[offsetOf(tileX * kFieldScale, (tileY * kFieldScale) + y, fieldWidth, 4)]);
      }
      const std::vector<int> glyphRows = rowIndices(font, bitmap, [&](int index) {
        return glyphInk(font, bitmap, index);
      });
      const std::vector<int> shadowRows = rowIndices(font, bitmap, [&](int index) {
        return bitmap.shadowed && shadowTexel(font, index);
      });
      for (std::size_t y = 0; y < glyphRows.size(); y++) {
        for (int x = 0; x < font.cellWidth; x++) {
          std::uint8_t *texel = &next.indices[offsetOf(tileX + x, tileY + static_cast<int>(y), next.width, 4)];
          texel[0] = static_cast<std::uint8_t>(glyphRows[y]);
          texel[1] = static_cast<std::uint8_t>(shadowRows[y]);
        }
      }
    }
    next.generation = atlas_.generation + 1;
    atlas_ = std::move(next);
    lucent::info("outline", "atlas {}: {} cells, {} fields drawn", atlas_.generation, font.cells.size(), drawn);
  }

  msdfgen::FreetypeHandle *freetype_ = nullptr;
  std::map<std::string, msdfgen::FontHandle *> faces_;
  const OutlineFont *font_ = nullptr;
  std::vector<BitmapCell> bitmaps_;
  std::vector<char32_t> codepoints_;
  std::map<std::uint16_t, CellField> fields_;
  OutlineAtlas atlas_;
};

OutlineText::OutlineText() : builder_(std::make_unique<Builder>()) {}

OutlineText::~OutlineText() = default;

void OutlineText::drew(const OutlineFont &font,
                       std::span<const std::uint16_t> vram,
                       const psx::input::GlyphSet &glyphs) {
  builder_->drew(font, vram, glyphs);
}

const OutlineAtlas *OutlineText::atlas() const {
  return builder_->atlas();
}

void markGlyphs(const OutlineFont *font,
                const OutlineGlyphs *glyphs,
                std::span<RecordEntry> entries,
                OutlineText &text,
                std::span<const std::uint16_t> vram,
                const psx::input::GlyphSet &pads) {
  if (font == nullptr || glyphs == nullptr) {
    return;
  }
  bool any = false;
  for (RecordEntry &entry : entries) {
    if (auto *primitive = std::get_if<DrawPrimitive>(&entry)) {
      primitive->outline = primitive->textured && glyphs->glyphPacket(primitive->packet);
      any = any || primitive->outline;
    }
  }
  if (any) {
    text.drew(*font, vram, pads);
  }
}

} // namespace psx::present
