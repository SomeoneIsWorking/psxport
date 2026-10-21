// gp0_command.h — a GP0 command word decoded by named field; a decode only, no interpretation.
#pragma once

#include "vram_rect.h"

#include <cstdint>

namespace psx::gpu {

// The high byte of a GP0 word; only the values the runtime acts on are named.
enum class Gp0Opcode : std::uint8_t {
  Nop = 0x00,
  ClearCache = 0x01,
  // 0x20-0x3F: a triangle or quad, textured or flat, gouraud or per-primitive, opaque or semi.
  Polygon = 0x20,
  // 0x40-0x5F: a line, or a poly-line when bit 3 is set (see Gp0Command::isPolyLine).
  Line = 0x40,
  // 0x60-0x7F: a rectangle or sprite, variable or one of the three fixed sizes.
  Rectangle = 0x60,
  // Monochrome rectangle into VRAM; ignores the draw area and draw offset.
  FillRect = 0x02,
  VramToVramCopy = 0x80,
  CpuToVramUpload = 0xA0,
  VramToCpuRead = 0xC0,
  // The one-word draw-environment commands.
  SetDrawMode = 0xE1,      // texpage: page X/Y, colour mode, blend, dither
  SetTextureWindow = 0xE2, // the texture-window mask and offset
  SetDrawAreaTopLeft = 0xE3,
  SetDrawAreaBottomRight = 0xE4,
  SetDrawOffset = 0xE5,
  SetMaskBits = 0xE6, // set-mask / check-mask; neither is modelled as a pixel test
  Unimplemented = 0xFF,
};

// A colour word: 0x00BBGGRR, red in the low byte.
struct Gp0Colour {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;

  friend constexpr bool operator==(Gp0Colour lhs, Gp0Colour rhs) {
    return lhs.red == rhs.red && lhs.green == rhs.green && lhs.blue == rhs.blue;
  }
};

// A vertex position, one 11-bit signed axis per halfword (-1024..+1023); the draw area clips it.
struct Gp0VertexPos {
  int x = 0;
  int y = 0;
};

// A texture coordinate word: 8-bit U and V, and a high halfword that is the CLUT on a polygon's
// vertex 0 and the texpage on vertex 1.
struct Gp0TextureCoord {
  int u = 0;
  int v = 0;
  std::uint16_t selector = 0;
};

// A rectangle command's top-left and size payload words; each command decodes them its own way.
struct Gp0RectWords {
  std::uint32_t topLeft = 0;
  std::uint32_t size = 0;
};

struct Gp0VramRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// An unsigned VRAM position: a draw-area corner (E3/E4) or a rect corner.
struct Gp0VramPos {
  int x = 0;
  int y = 0;
};

// The two E6 mask bits; check-mask is not modelled (docs/issues/0132-gp0-e6-check-mask-unmodelled.md).
struct Gp0MaskBits {
  bool setMask = false;   // bit 0
  bool checkMask = false; // bit 1
};

// The E2 texture window: a per-axis texel mask and offset.
struct Gp0TextureWindow {
  int maskX = 0;
  int maskY = 0;
  int offsetX = 0;
  int offsetY = 0;
};

// The opcode flag bits, at the same positions for polygons, lines and rectangles.
struct Gp0PrimitiveFlags {
  bool rawTexel = false;        // bit 0: texel unmodulated
  bool semiTransparent = false; // bit 1
  bool textured = false;        // bit 2
  bool quadOrPolyLine = false;  // bit 3: on a rectangle, part of the size code instead
  bool gouraud = false;         // bit 4
};

// The shape of a polygon opcode.
struct Gp0PolygonShape {
  bool quad = false;
  bool gouraud = false;
  bool textured = false;
  bool semiTransparent = false;
};

// One decoded GP0 command word.
class Gp0Command {
public:
  constexpr explicit Gp0Command(std::uint32_t word) : word_(word) {}

  constexpr std::uint32_t word() const {
    return word_;
  }
  constexpr std::uint8_t opcodeByte() const {
    return static_cast<std::uint8_t>(word_ >> 24);
  }

  constexpr Gp0Opcode opcode() const {
    return static_cast<Gp0Opcode>(opcodeByte());
  }

  constexpr bool isPolygon() const {
    return isInRange(0x20, 0x3F);
  }
  constexpr bool isLineOrPolyLine() const {
    return isInRange(0x40, 0x5F);
  }
  constexpr bool isRectangleOrSprite() const {
    return isInRange(0x60, 0x7F);
  }
  constexpr bool isDrawable() const {
    return isPolygon() || isLineOrPolyLine() || isRectangleOrSprite();
  }
  // A line with bit 3 set: variable length, ended by a terminator word.
  constexpr bool isPolyLine() const {
    return isLineOrPolyLine() && flags().quadOrPolyLine;
  }

  constexpr bool isDrawEnvironmentCommand() const {
    return opcodeByte() >= kDrawEnvironmentLow && opcodeByte() <= kDrawEnvironmentHigh;
  }
  constexpr bool isDiscardedNoOp() const {
    return opcodeByte() == static_cast<std::uint8_t>(Gp0Opcode::Nop) ||
           opcodeByte() == static_cast<std::uint8_t>(Gp0Opcode::ClearCache);
  }
  constexpr bool isVramTransfer() const {
    return isVramCopy() || isCpuToVramUpload() || isVramToCpuRead();
  }
  constexpr bool isVramCopy() const {
    return isInRange(0x80, 0x9F);
  }
  constexpr bool isCpuToVramUpload() const {
    return isInRange(0xA0, 0xBF);
  }
  constexpr bool isVramToCpuRead() const {
    return isInRange(0xC0, 0xDF);
  }

  constexpr Gp0PrimitiveFlags flags() const {
    Gp0PrimitiveFlags f;
    const std::uint8_t op = opcodeByte();
    f.rawTexel = (op & kFlagRawTexel) != 0;
    f.semiTransparent = (op & kFlagSemiTransparent) != 0;
    f.textured = (op & kFlagTextured) != 0;
    f.quadOrPolyLine = (op & kFlagQuadOrPolyLine) != 0;
    f.gouraud = (op & kFlagGouraud) != 0;
    return f;
  }

  constexpr int polygonVertexCount() const {
    return flags().quadOrPolyLine ? 4 : 3;
  }
  // 0: a size word follows; 1, 2, 3: fixed 1x1, 8x8, 16x16.
  constexpr int rectangleSizeCode() const {
    return (opcodeByte() >> 3) & 0x3;
  }
  constexpr int rectangleFixedSize() const {
    switch (rectangleSizeCode()) {
    case 1:
      return 1;
    case 2:
      return 8;
    case 3:
      return 16;
    default:
      return 0;
    }
  }

  constexpr Gp0Colour colour() const {
    return Gp0Colour{static_cast<std::uint8_t>(word_ & 0xFFu),
                     static_cast<std::uint8_t>((word_ >> 8) & 0xFFu),
                     static_cast<std::uint8_t>((word_ >> 16) & 0xFFu)};
  }
  constexpr Gp0VertexPos vertexPos() const {
    return Gp0VertexPos{signedAxis(word_ & 0xFFFFu), signedAxis((word_ >> 16) & 0xFFFFu)};
  }
  constexpr Gp0TextureCoord textureCoord() const {
    return Gp0TextureCoord{static_cast<int>(word_ & 0xFFu),
                           static_cast<int>((word_ >> 8) & 0xFFu),
                           static_cast<std::uint16_t>((word_ >> 16) & 0xFFFFu)};
  }
  // The E1-E6 payload.
  constexpr std::uint16_t lowHalfWord() const {
    return static_cast<std::uint16_t>(word_ & 0xFFFFu);
  }

  // E3/E4: X in bits 0..9, Y in bits 10..18.
  constexpr Gp0VramPos drawAreaCorner() const {
    return Gp0VramPos{static_cast<int>(word_ & kSpriteWidthMask),
                      static_cast<int>((word_ >> kDrawAreaAxisShift) & kFillRectHeightMask)};
  }

  // A fill, copy or transfer corner: X in bits 0..9, Y in bits 16..24.
  constexpr Gp0VramPos rectCorner() const {
    return Gp0VramPos{static_cast<int>(word_ & kSpriteWidthMask),
                      static_cast<int>((word_ >> kRectCornerAxisShift) & kFillRectHeightMask)};
  }

  // E5: two adjacent 11-bit signed axes.
  constexpr Gp0VertexPos drawOffset() const {
    return Gp0VertexPos{signedAxis(word_ & kVertexAxisMask),
                        signedAxis((word_ >> kDrawOffsetAxisShift) & kVertexAxisMask)};
  }

  constexpr Gp0MaskBits maskBits() const {
    return Gp0MaskBits{(word_ & kMaskSetBit) != 0, (word_ & kMaskCheckBit) != 0};
  }

  constexpr Gp0TextureWindow textureWindow() const {
    const std::uint32_t payload = word_ & 0xFFFFu;
    return Gp0TextureWindow{static_cast<int>(payload & 0x1Fu),
                            static_cast<int>((payload >> 5) & 0x1Fu),
                            static_cast<int>((payload >> 10) & 0x1Fu),
                            static_cast<int>((payload >> 15) & 0x1Fu)};
  }

  // The command's words including the command word; a poly-line's minimum, a transfer's header only.
  static constexpr unsigned packetWordCount(std::uint32_t word) {
    const Gp0Command command(word);
    const std::uint8_t op = command.opcodeByte();
    if (command.isPolygon()) {
      const int vertices = command.polygonVertexCount();
      // Gouraud colours for every vertex but the first, whose colour is the command word's.
      unsigned count = 1u + static_cast<unsigned>(vertices) * (command.flags().textured ? 2u : 1u);
      if (command.flags().gouraud) {
        count += static_cast<unsigned>(vertices - 1);
      }
      return count;
    }
    if (command.isRectangleOrSprite()) {
      unsigned count = 2u + (command.flags().textured ? 1u : 0u) + (command.rectangleSizeCode() == 0 ? 1u : 0u);
      return count;
    }
    if (command.isLineOrPolyLine()) {
      return (command.flags().gouraud) ? 4u : 3u;
    }
    if (command.isVramCopy()) {
      return 4u; // command, source top-left, destination top-left, size
    }
    if (command.isCpuToVramUpload() || command.isVramToCpuRead()) {
      return 3u; // command, top-left, size — the pixels stream after
    }
    if (op == static_cast<std::uint8_t>(Gp0Opcode::FillRect)) {
      return 3u; // colour+command, top-left, size
    }
    return 1u; // draw environment, nop, clear cache, and everything unmodelled
  }

  // A fill's X drops bits 0..3 and its width rounds up to 16; a zero width fills nothing.
  static constexpr Gp0VramRect fillRectRegion(Gp0RectWords words) {
    Gp0VramRect rect;
    rect.x = static_cast<int>(words.topLeft & kFillRectOriginMask);
    rect.y = static_cast<int>((words.topLeft >> kRectCornerAxisShift) & kFillRectHeightMask);
    rect.width = static_cast<int>((words.size & kSpriteWidthMask) + kFillRectAlign) & ~kFillRectAlign;
    rect.height = static_cast<int>((words.size >> kRectCornerAxisShift) & kFillRectHeightMask);
    return rect;
  }

  // A sprite's region; `fixedSize` 0 reads the size word.
  static constexpr Gp0VramRect spriteRegion(Gp0RectWords words, int fixedSize) {
    Gp0VramRect rect;
    // Signed like a vertex, unlike a rect corner.
    const Gp0VertexPos topLeft = Gp0Command(words.topLeft).vertexPos();
    rect.x = topLeft.x;
    rect.y = topLeft.y;
    if (fixedSize != 0) {
      rect.width = fixedSize;
      rect.height = fixedSize;
    } else {
      rect.width = static_cast<int>(words.size & kSpriteWidthMask);
      rect.height = static_cast<int>((words.size >> kRectCornerAxisShift) & kFillRectHeightMask);
    }
    return rect;
  }

  // A CPU<->VRAM transfer's region; a zero width or height is the whole axis.
  static constexpr Gp0VramRect transferRegion(Gp0RectWords words) {
    const Gp0VramPos corner = Gp0Command(words.topLeft).rectCorner();
    Gp0VramRect rect;
    rect.x = corner.x;
    rect.y = corner.y;
    rect.width = static_cast<int>(words.size & kSpriteWidthMask);
    if (rect.width == 0) {
      rect.width = kVramWidth;
    }
    rect.height = static_cast<int>((words.size >> kRectCornerAxisShift) & kFillRectHeightMask);
    if (rect.height == 0) {
      rect.height = kVramHeight;
    }
    return rect;
  }

  // The poly-line terminator: both halfwords 0x5xxx.
  static constexpr bool isPolyLineTerminator(std::uint32_t word) {
    return (word & 0xF000F000u) == 0x50005000u;
  }

  // Where a terminator may appear: a gouraud line's colour slots from 4, a flat line's from 3.
  static constexpr bool isPolyLineTerminatorSlot(unsigned wordIndex, bool gouraud) {
    return gouraud ? (wordIndex >= 4u && (wordIndex & 1u) == 0u) : (wordIndex >= 3u);
  }

  // The opcode flag bits.
  static constexpr std::uint8_t kFlagRawTexel = 0x01;
  static constexpr std::uint8_t kFlagSemiTransparent = 0x02;
  static constexpr std::uint8_t kFlagTextured = 0x04;
  static constexpr std::uint8_t kFlagQuadOrPolyLine = 0x08;
  static constexpr std::uint8_t kFlagGouraud = 0x10;

  // An opcode byte in a command word's top byte.
  static constexpr std::uint32_t opcodeWord(std::uint8_t opcode) {
    return static_cast<std::uint32_t>(opcode) << 24;
  }
  static constexpr std::uint8_t polygonOpcode(Gp0PolygonShape shape) {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(Gp0Opcode::Polygon) |
                                     (shape.gouraud ? kFlagGouraud : 0u) | (shape.quad ? kFlagQuadOrPolyLine : 0u) |
                                     (shape.textured ? kFlagTextured : 0u) |
                                     (shape.semiTransparent ? kFlagSemiTransparent : 0u));
  }
  // An ordering-table packet's tag: its command word count in the top byte, the link left clear.
  static constexpr std::uint32_t packetTag(std::uint8_t opcode) {
    return packetWordCount(opcodeWord(opcode)) << 24;
  }
  // An ordering-table packet's bytes, tag word included.
  static constexpr std::uint32_t packetBytes(std::uint8_t opcode) {
    return (packetWordCount(opcodeWord(opcode)) + 1u) * 4u;
  }

  // One 11-bit signed axis out of a halfword: narrowed to 11 bits before its bit 10 sign is extended.
  static constexpr int signedAxis(std::uint32_t halfWord) {
    const std::uint32_t axis = halfWord & kVertexAxisMask;
    return static_cast<int>((axis & kVertexAxisSign) ? axis - kVertexAxisRange : axis);
  }
  static constexpr std::uint32_t kVertexAxisMask = 0x7FFu;
  static constexpr std::uint32_t kVertexAxisSign = 0x400u;
  static constexpr std::uint32_t kVertexAxisRange = 0x800u;

private:
  static constexpr std::uint8_t kDrawEnvironmentLow = 0xE1;
  static constexpr std::uint8_t kDrawEnvironmentHigh = 0xE6;

  static constexpr std::uint32_t kSpriteWidthMask = 0x3FFu;    // 10 bits
  static constexpr std::uint32_t kFillRectHeightMask = 0x1FFu; // 9 bits
  static constexpr unsigned kRectCornerAxisShift = 16;
  static constexpr unsigned kDrawAreaAxisShift = 10;
  static constexpr unsigned kDrawOffsetAxisShift = 11;
  static constexpr std::uint32_t kMaskSetBit = 0x1u;
  static constexpr std::uint32_t kMaskCheckBit = 0x2u;
  static constexpr std::uint32_t kFillRectOriginMask = 0x3F0u;
  static constexpr int kFillRectAlign = 0xF;

  std::uint32_t word_ = 0;

  constexpr bool isInRange(std::uint8_t low, std::uint8_t high) const {
    const std::uint8_t op = opcodeByte();
    return op >= low && op <= high;
  }
};

// The opaque polygon opcodes, by libgpu name.
inline constexpr std::uint8_t kPolyF3 = Gp0Command::polygonOpcode({});
inline constexpr std::uint8_t kPolyF4 = Gp0Command::polygonOpcode({.quad = true});
inline constexpr std::uint8_t kPolyFt3 = Gp0Command::polygonOpcode({.textured = true});
inline constexpr std::uint8_t kPolyFt4 = Gp0Command::polygonOpcode({.quad = true, .textured = true});
inline constexpr std::uint8_t kPolyG3 = Gp0Command::polygonOpcode({.gouraud = true});
inline constexpr std::uint8_t kPolyG4 = Gp0Command::polygonOpcode({.quad = true, .gouraud = true});
inline constexpr std::uint8_t kPolyGt3 = Gp0Command::polygonOpcode({.gouraud = true, .textured = true});
inline constexpr std::uint8_t kPolyGt4 = Gp0Command::polygonOpcode({.quad = true, .gouraud = true, .textured = true});

// The opaque line opcodes, by libgpu name.
inline constexpr std::uint8_t kLineF2 = static_cast<std::uint8_t>(Gp0Opcode::Line);
inline constexpr std::uint8_t kLineG2 = kLineF2 | Gp0Command::kFlagGouraud;

} // namespace psx::gpu
