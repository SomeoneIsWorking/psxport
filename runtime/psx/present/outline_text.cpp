#include "outline_text.h"

#include "glyph_set.h"
#include "record_geometry.h"
#include "ui_assets.h"

#include <lucent/log.h>
#include <msdfgen-ext.h>
#include <msdfgen.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace psx::present {
namespace {

// Field pixels a texel, whatever the internal scale: a field holds its edges and corners magnified well past this.
constexpr int kFieldScale = 4;
// The distance a field channel spans either side of the edge, field pixels.
constexpr double kFieldRange = 4.0;
// Field pixels round each packed tile, repeating its edge, so a filtered sample never reads a neighbour's field.
constexpr int kFieldGutter = 1;
// The packed fields' width, pixels; tiles fill it in shelves.
constexpr int kFieldShelfWidth = 2048;
// The largest field texture a weak GPU takes, pixels a side.
constexpr int kMaxFieldSide = 8192;

// The element of (x, y) in a row-major image `width` wide, `channels` elements a pixel.
constexpr std::size_t offsetOf(int x, int y, int width, int channels = 1) {
  return ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) + static_cast<std::size_t>(x)) *
         static_cast<std::size_t>(channels);
}

// A glyph's bitmap: its CLUT indices, the box of its glyph texels (the shadow's excluded), half open, and whether it
// carries the shadow.
struct BitmapGlyph {
  int width = 0;
  int height = 0;
  std::vector<int> indices; // row-major, width x height
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  bool shadowed = false;

  bool empty() const {
    return x1 <= x0;
  }
};

// What a glyph's field is drawn from; a glyph keeps its field while this holds.
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
bool glyphInk(const OutlineFont &font, const BitmapGlyph &bitmap, int index) {
  return bitmap.shadowed ? glyphTexel(font, index) : index != 0;
}

BitmapGlyph readGlyph(const OutlineFont &font, std::span<const std::uint16_t> vram, const OutlineGlyph &glyph) {
  BitmapGlyph bitmap;
  bitmap.width = glyph.width;
  bitmap.height = glyph.height;
  bitmap.indices.resize(offsetOf(0, bitmap.height, bitmap.width));
  const auto halfWordAt = [vram](psx::gpu::VramPoint at) {
    return psx::gpu::vramHalfWord(vram, at);
  };
  bool ink = false;
  bool shadow = false;
  for (int y = 0; y < bitmap.height; y++) {
    for (int x = 0; x < bitmap.width; x++) {
      const int index = psx::gpu::fetchIndex(halfWordAt, font.page, glyph.u + x, glyph.v + y);
      bitmap.indices[offsetOf(x, y, bitmap.width)] = index;
      ink = ink || glyphTexel(font, index);
      shadow = shadow || shadowTexel(font, index);
    }
  }
  // A glyph of the shadow's indices alone is drawn in them.
  bitmap.shadowed = ink && shadow;
  bitmap.x0 = bitmap.width;
  bitmap.y0 = bitmap.height;
  for (int y = 0; y < bitmap.height; y++) {
    for (int x = 0; x < bitmap.width; x++) {
      const int index = bitmap.indices[offsetOf(x, y, bitmap.width)];
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

// The CLUT index each row of a glyph draws `inked` texels in: the font's gradient runs down the glyph, so a row takes
// the index most of its such texels hold, and a row without any takes the nearest row's.
template <class Inked> std::vector<int> rowIndices(const BitmapGlyph &bitmap, Inked inked) {
  const auto h = static_cast<std::size_t>(bitmap.height);
  std::vector<int> rows(h, 0);
  for (std::size_t y = 0; y < h; y++) {
    std::array<int, 256> counts{};
    for (int x = 0; x < bitmap.width; x++) {
      const int index = bitmap.indices[offsetOf(x, static_cast<int>(y), bitmap.width)];
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

// The page texels a font's glyphs cover: the box of their rectangles, `originX`/`originY` across VRAM.
OutlineAtlasFont placeOf(const OutlineFont &font) {
  int x0 = std::numeric_limits<int>::max();
  int y0 = std::numeric_limits<int>::max();
  int x1 = 0;
  int y1 = 0;
  for (const OutlineGlyph &glyph : font.glyphs) {
    x0 = std::min<int>(x0, glyph.u);
    y0 = std::min<int>(y0, glyph.v);
    x1 = std::max(x1, glyph.u + glyph.width);
    y1 = std::max(y1, glyph.v + glyph.height);
  }
  if (x1 <= x0 || y1 <= y0) {
    return {};
  }
  return {.originX = (font.page.x << (2 - static_cast<int>(font.page.depth))) + x0,
          .originY = font.page.y + y0,
          .width = x1 - x0,
          .height = y1 - y0};
}

// Flags the glyphs of `font` whose rectangles the texels `primitive` samples overlap; all of them through a texture
// window.
void markDrawn(const OutlineFont &font, const DrawPrimitive &primitive, std::vector<bool> &drawn) {
  const RecordDrawState &state = primitive.state;
  if (state.windowMaskX != 0 || state.windowMaskY != 0) {
    std::fill(drawn.begin(), drawn.end(), true);
    return;
  }
  psx::gpu::RecordRect box = psx::gpu::texelBox(primitive);
  // A polygon's right and bottom coordinates lie on edges the rasterizer leaves undrawn.
  if (primitive.kind != PrimitiveKind::Sprite) {
    box.x1 = std::max(box.x1 - 1, box.x0 + 1);
    box.y1 = std::max(box.y1 - 1, box.y0 + 1);
  }
  // In texels of the font's page from its left edge, as its glyphs' u runs.
  const int depthShift = 2 - static_cast<int>(font.page.depth);
  const int pageU = (state.texPageX << depthShift) - (font.page.x << depthShift);
  const int pageV = state.texPageY - font.page.y;
  const psx::gpu::RecordRect sampled{
      .x0 = pageU + box.x0, .y0 = pageV + box.y0, .x1 = pageU + box.x1, .y1 = pageV + box.y1};
  for (std::size_t i = 0; i < font.glyphs.size(); i++) {
    const OutlineGlyph &glyph = font.glyphs[i];
    const psx::gpu::RecordRect rect{
        .x0 = glyph.u, .y0 = glyph.v, .x1 = glyph.u + glyph.width, .y1 = glyph.v + glyph.height};
    if (psx::gpu::rectsOverlap(sampled, rect)) {
      drawn[i] = true;
    }
  }
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

  void drew(std::span<const OutlineFont> fonts,
            const DrawnGlyphs &drawn,
            std::span<const std::uint16_t> vram,
            const psx::input::GlyphSet &pads) {
    if (fonts.size() > kMaxOutlineFonts) {
      lucent::error("outline", "a title names {} fonts, more than {}", fonts.size(), kMaxOutlineFonts);
      std::abort();
    }
    if (!laidOut(fonts)) {
      places_.clear();
      for (const OutlineFont &font : fonts) {
        places_.push_back(placesOf(font));
      }
      states_ = {};
      layout(fonts);
    }
    bool changed = false;
    for (std::size_t f = 0; f < fonts.size(); f++) {
      const OutlineFont &font = fonts[f];
      FontState &state = states_.at(f);
      if (std::ranges::find(drawn.at(f), true) == drawn.at(f).end()) {
        continue;
      }
      state.wanted.resize(font.glyphs.size());
      for (std::size_t i = 0; i < font.glyphs.size(); i++) {
        state.wanted[i] = state.wanted[i] || drawn.at(f)[i];
      }
      std::vector<BitmapGlyph> bitmaps(font.glyphs.size());
      std::vector<char32_t> codepoints(font.glyphs.size(), psx::input::kNoGlyph);
      for (std::size_t i = 0; i < font.glyphs.size(); i++) {
        if (!state.wanted[i]) {
          continue;
        }
        const OutlineGlyph &glyph = font.glyphs[i];
        bitmaps[i] = readGlyph(font, vram, glyph);
        codepoints[i] =
            (glyph.button == 0                                                     ? glyph.codepoint
             : glyph.padArt && pads.family() == psx::input::PadFamily::PlayStation ? psx::input::kNoGlyph
                                                                                   : pads.glyph(glyph.button));
      }
      const auto sameIndices = [](const BitmapGlyph &a, const BitmapGlyph &b) {
        return a.indices == b.indices;
      };
      if (state.built && codepoints == state.codepoints && std::ranges::equal(bitmaps, state.bitmaps, sameIndices)) {
        continue;
      }
      state.bitmaps = std::move(bitmaps);
      state.codepoints = std::move(codepoints);
      state.built = true;
      draw(font, f);
      changed = true;
    }
    if (changed) {
      atlas_.generation++;
      lucent::info("outline", "atlas {}: {} fonts, {} fields drawn", atlas_.generation, fonts.size(), drawn_);
      drawn_ = 0;
    }
  }

  const OutlineAtlas *atlas() const {
    return atlas_.generation != 0 ? &atlas_ : nullptr;
  }

private:
  struct GlyphField {
    FieldKey key;
    std::vector<std::uint8_t> rgba; // the glyph's tile, width x height texels at kFieldScale
  };

  // A glyph's tile in the packed fields: the field pixel of its top-left texel, inside the gutter.
  struct Tile {
    int x = 0;
    int y = 0;
  };

  struct FontState {
    bool built = false;
    std::vector<BitmapGlyph> bitmaps;
    std::vector<char32_t> codepoints;
    std::map<std::size_t, GlyphField> fields;
    std::vector<Tile> tiles;  // by glyph
    std::vector<bool> wanted; // by glyph: drawn since the atlas was laid out
  };

  // A font's page and glyph rectangles, as the atlas was laid out for them.
  struct FontPlaces {
    psx::gpu::TexPage page;
    std::vector<std::array<std::uint16_t, 4>> rects;

    // Whether `font` still has this page and these rectangles.
    [[nodiscard]] bool holds(const OutlineFont &font) const {
      if (font.page.x != page.x || font.page.y != page.y || font.page.depth != page.depth ||
          font.glyphs.size() != rects.size()) {
        return false;
      }
      for (std::size_t i = 0; i < rects.size(); i++) {
        const OutlineGlyph &glyph = font.glyphs[i];
        if (rects[i] != std::array<std::uint16_t, 4>{glyph.u, glyph.v, glyph.width, glyph.height}) {
          return false;
        }
      }
      return true;
    }
  };

  static FontPlaces placesOf(const OutlineFont &font) {
    FontPlaces places{.page = font.page, .rects = {}};
    for (const OutlineGlyph &glyph : font.glyphs) {
      places.rects.push_back({glyph.u, glyph.v, glyph.width, glyph.height});
    }
    return places;
  }

  // Whether the atlas is laid out for `fonts` as they stand.
  bool laidOut(std::span<const OutlineFont> fonts) const {
    if (fonts.size() != places_.size()) {
      return false;
    }
    for (std::size_t f = 0; f < fonts.size(); f++) {
      if (!places_[f].holds(fonts[f])) {
        return false;
      }
    }
    return true;
  }

  // Each font's place in the index map, one under another, and each glyph's tile in shelves across the fields; every
  // glyph field and index empty.
  void layout(std::span<const OutlineFont> fonts) {
    OutlineAtlas next;
    next.scale = kFieldScale;
    next.fieldWidth = kFieldShelfWidth;
    std::size_t buttons = 0;
    int shelfX = 0;
    int shelfHeight = 0;
    for (std::size_t f = 0; f < fonts.size(); f++) {
      OutlineAtlasFont place = placeOf(fonts[f]);
      place.atlasY = next.height;
      next.fonts.at(f) = place;
      next.width = std::max(next.width, place.width);
      next.height += place.height;
      std::vector<Tile> &tiles = states_.at(f).tiles;
      for (const OutlineGlyph &glyph : fonts[f].glyphs) {
        const int tileWidth = (glyph.width * kFieldScale) + (2 * kFieldGutter);
        const int tileHeight = (glyph.height * kFieldScale) + (2 * kFieldGutter);
        if (shelfX + tileWidth > next.fieldWidth) {
          next.fieldHeight += shelfHeight;
          shelfX = 0;
          shelfHeight = 0;
        }
        tiles.push_back({.x = shelfX + kFieldGutter, .y = next.fieldHeight + kFieldGutter});
        shelfX += tileWidth;
        shelfHeight = std::max(shelfHeight, tileHeight);
      }
      for (const OutlineGlyph &glyph : fonts[f].glyphs) {
        const auto named = next.buttons.begin() + static_cast<std::ptrdiff_t>(buttons);
        if (glyph.button == 0 || std::find(next.buttons.begin(), named, glyph.button) != named) {
          continue;
        }
        if (buttons == next.buttons.size()) {
          lucent::error("outline", "fonts name more than {} buttons", next.buttons.size());
          std::abort();
        }
        next.buttons.at(buttons++) = glyph.button;
      }
    }
    next.fieldHeight += shelfHeight;
    if (next.fieldHeight > kMaxFieldSide || next.width * 2 > kMaxFieldSide) {
      lucent::error("outline",
                    "fonts need a {}x{} field atlas and a {}x{} index map, past {} a side",
                    next.fieldWidth,
                    next.fieldHeight,
                    next.width * 2,
                    next.height,
                    kMaxFieldSide);
      std::abort();
    }
    next.fields.assign(offsetOf(0, next.fieldHeight, next.fieldWidth, 4), 0);
    next.indices.assign(offsetOf(0, next.height, next.width * 2, 4), 0);
    next.generation = atlas_.generation;
    atlas_ = std::move(next);
  }

  msdfgen::FontHandle *face(const OutlineFont &font, OutlineFace which) {
    const std::string name = which == OutlineFace::Prompt   ? "promptfont.ttf"
                             : which == OutlineFace::Symbol ? font.symbolFace
                                                            : font.textFace;
    // Fonts sharing a variable text face at other weights each need their own instance.
    const double weight = which == OutlineFace::Text ? font.weight : 0;
    const auto [at, added] = faces_.try_emplace({name, weight}, nullptr);
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
    if (weight != 0 &&
        !msdfgen::setFontVariationAxis(freetype_, at->second, msdfgen::FontVariationAxis::Tag("wght"), weight)) {
      lucent::error("outline", "font {} has no weight axis to set to {}", path, weight);
    }
    return at->second;
  }

  // The glyph's tile: RGB its outline's field fitted to the bitmap glyph's box, A the shadow's.
  bool
  drawField(const OutlineFont &font, const OutlineGlyph &glyph, const FieldKey &key, std::vector<std::uint8_t> &rgba) {
    const int w = glyph.width * kFieldScale;
    const int h = glyph.height * kFieldScale;
    rgba.assign(offsetOf(0, h, w, 4), 0);
    msdfgen::FontHandle *handle = face(font, key.face);
    msdfgen::Shape shape;
    // Glyph 0 is the face's missing-glyph box.
    msdfgen::GlyphIndex glyphIndex;
    if (handle == nullptr || !msdfgen::getGlyphIndex(glyphIndex, handle, msdfgen::unicode_t{key.codepoint}) ||
        glyphIndex.getIndex() == 0 ||
        !msdfgen::loadGlyph(shape, handle, glyphIndex, msdfgen::FONT_SCALING_EM_NORMALIZED) || shape.contours.empty()) {
      lucent::error("outline",
                    "no outline for the glyph at ({}, {}) (U+{:04X}); it draws nothing",
                    glyph.u,
                    glyph.v,
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
    const double fillX = (right - left) / (bounds.r - bounds.l);
    const double fillY = (top - bottom) / (bounds.t - bounds.b);
    const msdfgen::Vector2 scale(font.proportional ? std::min(fillX, fillY) : fillX, fillY);
    const double inset = ((right - left) - ((bounds.r - bounds.l) * scale.x)) / 2;
    const msdfgen::Vector2 translate((left + inset) / scale.x - bounds.l, bottom / scale.y - bounds.b);
    const msdfgen::DistanceMapping mapping(msdfgen::Range(kFieldRange / std::min(scale.x, scale.y)));
    // The shadow is the glyph moved by its offset, field pixels right and up.
    const int shiftX = key.shadowed ? font.shadowX * kFieldScale : 0;
    const int shiftY = key.shadowed ? -font.shadowY * kFieldScale : 0;
    // Past the ink box and its shadow by more than the range, every field pixel is outside: only the box between is
    // generated.
    const int pad = static_cast<int>(std::ceil(kFieldRange)) + 1;
    const int cropX0 = std::max(0, static_cast<int>(left) + std::min(0, shiftX) - pad);
    const int cropX1 = std::min(w, static_cast<int>(right) + std::max(0, shiftX) + pad);
    const int cropY0 = std::max(0, static_cast<int>(bottom) + std::min(0, shiftY) - pad);
    const int cropY1 = std::min(h, static_cast<int>(top) + std::max(0, shiftY) + pad);
    const msdfgen::Vector2 cropped(translate.x - (cropX0 / scale.x), translate.y - (cropY0 / scale.y));
    const msdfgen::SDFTransformation placed(msdfgen::Projection(scale, cropped), mapping);
    msdfgen::MSDFGeneratorConfig generate;
    generate.errorCorrection.mode = msdfgen::ErrorCorrectionConfig::DISABLED;
    msdfgen::MSDFGeneratorConfig correct;
    correct.errorCorrection.distanceCheckMode = msdfgen::ErrorCorrectionConfig::DO_NOT_CHECK_DISTANCE;
    msdfgen::Bitmap<float, 3> field(cropX1 - cropX0, cropY1 - cropY0);
    msdfgen::generateMSDF(field, shape, placed, generate);
    // Variable faces keep overlapping contours: the fields take their sign from the nonzero fill, then the MSDF its
    // error correction, as msdfgen's scanline pass does.
    msdfgen::distanceSignCorrection(field, shape, placed, msdfgen::FILL_NONZERO);
    msdfgen::msdfErrorCorrection(field, shape, placed, correct);
    const auto inCrop = [&](int x, int y) {
      return x >= cropX0 && x < cropX1 && y >= cropY0 && y < cropY1;
    };
    for (int y = cropY0; y < cropY1; y++) {
      for (int x = cropX0; x < cropX1; x++) {
        std::uint8_t *pixel = &rgba[offsetOf(x, h - 1 - y, w, 4)];
        const float *channels = field(x - cropX0, y - cropY0);
        pixel[0] = fieldByte(channels[0]);
        pixel[1] = fieldByte(channels[1]);
        pixel[2] = fieldByte(channels[2]);
        if (key.shadowed && inCrop(x - shiftX, y - shiftY)) {
          const float *cast = field(x - shiftX - cropX0, y - shiftY - cropY0);
          pixel[3] = fieldByte(msdfgen::median(cast[0], cast[1], cast[2]));
        }
      }
    }
    return true;
  }

  // Font `f`'s place in the atlas from its glyphs as last read.
  void draw(const OutlineFont &font, std::size_t f) {
    FontState &state = states_.at(f);
    const OutlineAtlasFont &place = atlas_.fonts.at(f);
    const int pageX0 = place.originX - (font.page.x << (2 - static_cast<int>(font.page.depth)));
    const int pageY0 = place.originY - font.page.y;
    for (int y = 0; y < place.height; y++) {
      std::fill_n(&atlas_.indices[offsetOf(0, place.atlasY + y, atlas_.width * 2, 4)], place.width * 2 * 4, 0);
    }
    for (std::size_t i = 0; i < font.glyphs.size(); i++) {
      const OutlineGlyph &glyph = font.glyphs[i];
      const BitmapGlyph &bitmap = state.bitmaps[i];
      const Tile tile = state.tiles[i];
      clearTile(glyph, tile);
      const int glyphSlot =
          glyph.button != 0
              ? static_cast<int>(std::ranges::find(atlas_.buttons, glyph.button) - atlas_.buttons.begin()) + 1
              : 0;
      // A glyph without a codepoint draws its bitmap.
      if (bitmap.empty() || state.codepoints[i] == psx::input::kNoGlyph) {
        continue;
      }
      const FieldKey key{state.codepoints[i],
                         glyph.button != 0 ? OutlineFace::Prompt : glyph.face,
                         bitmap.x0,
                         bitmap.y0,
                         bitmap.x1,
                         bitmap.y1,
                         bitmap.shadowed};
      GlyphField &field = state.fields[i];
      if (field.rgba.empty() || !(field.key == key)) {
        field.key = key;
        if (!drawField(font, glyph, key, field.rgba)) {
          field.rgba.clear();
          continue;
        }
        ++drawn_;
      }
      copyTile(glyph, tile, field.rgba);
      const std::vector<int> glyphRows = rowIndices(bitmap, [&](int index) {
        return glyphInk(font, bitmap, index);
      });
      const std::vector<int> shadowRows = rowIndices(bitmap, [&](int index) {
        return bitmap.shadowed && shadowTexel(font, index);
      });
      const int mapX = glyph.u - pageX0;
      const int mapY = place.atlasY + glyph.v - pageY0;
      for (std::size_t y = 0; y < glyphRows.size(); y++) {
        for (int x = 0; x < glyph.width; x++) {
          std::uint8_t *texel =
              &atlas_.indices[offsetOf((mapX + x) * 2, mapY + static_cast<int>(y), atlas_.width * 2, 4)];
          texel[0] = static_cast<std::uint8_t>(glyphRows[y]);
          texel[1] = static_cast<std::uint8_t>(shadowRows[y]);
          texel[2] = static_cast<std::uint8_t>(glyphSlot);
          texel[3] = 255;
          const int fieldX = tile.x + (x * kFieldScale);
          const int fieldY = tile.y + (static_cast<int>(y) * kFieldScale);
          texel[4] = static_cast<std::uint8_t>(fieldX & 0xFF);
          texel[5] = static_cast<std::uint8_t>(fieldX >> 8);
          texel[6] = static_cast<std::uint8_t>(fieldY & 0xFF);
          texel[7] = static_cast<std::uint8_t>(fieldY >> 8);
        }
      }
    }
  }

  // A glyph's tile and its gutter, empty.
  void clearTile(const OutlineGlyph &glyph, Tile tile) {
    const int width = (glyph.width * kFieldScale) + (2 * kFieldGutter);
    for (int y = -kFieldGutter; y < (glyph.height * kFieldScale) + kFieldGutter; y++) {
      std::fill_n(&atlas_.fields[offsetOf(tile.x - kFieldGutter, tile.y + y, atlas_.fieldWidth, 4)], width * 4, 0);
    }
  }

  // A glyph's field into its tile, its edge pixels repeated across the gutter.
  void copyTile(const OutlineGlyph &glyph, Tile tile, std::span<const std::uint8_t> rgba) {
    const int width = glyph.width * kFieldScale;
    const int height = glyph.height * kFieldScale;
    for (int y = -kFieldGutter; y < height + kFieldGutter; y++) {
      const int row = std::clamp(y, 0, height - 1);
      for (int x = -kFieldGutter; x < width + kFieldGutter; x++) {
        const int column = std::clamp(x, 0, width - 1);
        std::copy_n(&rgba[offsetOf(column, row, width, 4)],
                    4,
                    &atlas_.fields[offsetOf(tile.x + x, tile.y + y, atlas_.fieldWidth, 4)]);
      }
    }
  }

  msdfgen::FreetypeHandle *freetype_ = nullptr;
  std::map<std::pair<std::string, double>, msdfgen::FontHandle *> faces_; // by file and weight
  std::vector<FontPlaces> places_;                                        // the fonts the atlas is laid out for
  std::array<FontState, kMaxOutlineFonts> states_;
  int drawn_ = 0;
  OutlineAtlas atlas_;
};

OutlineText::OutlineText() : builder_(std::make_unique<Builder>()) {}

OutlineText::~OutlineText() = default;

void OutlineText::drew(std::span<const OutlineFont> fonts,
                       const DrawnGlyphs &drawn,
                       std::span<const std::uint16_t> vram,
                       const psx::input::GlyphSet &pads) {
  builder_->drew(fonts, drawn, vram, pads);
}

const OutlineAtlas *OutlineText::atlas() const {
  return builder_->atlas();
}

std::array<std::uint32_t, 4> buttonFaceColours(const OutlineAtlas &atlas, const psx::input::GlyphSet &pads) {
  std::array<std::uint32_t, 4> colours{};
  for (std::size_t slot = 0; slot < atlas.buttons.size(); slot++) {
    if (const std::optional<psx::input::FaceColour> face = pads.faceColour(atlas.buttons.at(slot))) {
      colours.at(slot) = psx::gpu::toVram555(face->red, face->green, face->blue);
    }
  }
  return colours;
}

void markGlyphs(std::span<const OutlineFont> fonts,
                const OutlineGlyphs *glyphs,
                std::span<RecordEntry> entries,
                OutlineText &text,
                std::span<const std::uint16_t> vram,
                const psx::input::GlyphSet &pads) {
  if (fonts.empty() || glyphs == nullptr) {
    return;
  }
  DrawnGlyphs drawn;
  bool any = false;
  for (std::size_t f = 0; f < fonts.size(); f++) {
    drawn.at(f).assign(fonts[f].glyphs.size(), false);
  }
  for (RecordEntry &entry : entries) {
    if (auto *primitive = std::get_if<DrawPrimitive>(&entry)) {
      primitive->outlineFont = primitive->textured ? glyphs->glyphFont(primitive->packet) : std::nullopt;
      if (primitive->outlineFont) {
        if (*primitive->outlineFont >= fonts.size()) {
          lucent::error(
              "outline", "packet {:08X} names font {} of {}", primitive->packet, *primitive->outlineFont, fonts.size());
          std::abort();
        }
        markDrawn(fonts[*primitive->outlineFont], *primitive, drawn.at(*primitive->outlineFont));
        any = true;
      }
    }
  }
  if (any) {
    text.drew(fonts, drawn, vram, pads);
  }
}

} // namespace psx::present
