// gp0_command.h — the guest's GP0 command word, decoded by name.
//
// Every drawable a PSX game submits is a stream of 32-bit GP0 words, and each word is a packed
// command: a one-byte opcode in the high byte, a colour in the low three bytes, and — for the
// primitives — a signed vertex position, a texture coordinate, and a run of flag bits. Reading one
// meant writing `w >> 24`, `w & 0xFF`, `(w >> 8) & 0xFF`, `(int)(w & 0x7FF) - (…)` at every use, and
// a reader had to know that R is in the LOW byte of a BGR word to make sense of any of it.
//
// This owner decodes a command word ONCE and names every field, so a call site reads
// `command.opcode() == Gp0Opcode::Polygon` and `command.colour().red` rather than arithmetic on a
// hex literal. It is a VALUE type over one word: no state, no I/O, no lifetime.
//
// IT IS A DECODE, NOT AN INTERPRETATION. Nothing here decides what a command MEANS for the picture —
// which texpage a primitive binds, how its flags are submitted, whether a packet is complete. Those
// are the compositor's decisions and they stay there. What lives here is only "which bits of the
// guest's word are which named field", so there is exactly one answer to that question.
#ifndef PSXPORT_GP0_COMMAND_H
#define PSXPORT_GP0_COMMAND_H

#include <cstdint>

namespace psx::gpu {

// The high byte of a GP0 word. Only the values the runtime acts on are named; the rest of the 0x00-0xFF
// opcode space is genuinely unimplemented here and is reported as Unimplemented rather than guessed at.
enum class Gp0Opcode : std::uint8_t {
  Nop = 0x00,
  ClearCache = 0x01,
  // 0x20-0x3F: a triangle or quad, textured or flat, gouraud or per-primitive, opaque or semi.
  Polygon = 0x20,
  // 0x40-0x5F: a line, or a poly-LINE when bit 3 is also set. The two share a range, so the range
  // test alone does not say which; `Gp0Command::isPolyLine()` does.
  Line = 0x40,
  // 0x60-0x7F: a rectangle or sprite, variable or one of the three fixed sizes.
  Rectangle = 0x60,
  // Monochrome rectangle, written straight into VRAM. It ignores the draw area and the draw offset by
  // hardware design, which is why it is the one GP0 command that writes pixels the clip cannot stop.
  FillRect = 0x02,
  VramToVramCopy = 0x80,
  CpuToVramUpload = 0xA0,
  VramToCpuRead = 0xC0,
  // The draw-environment commands. Each is ONE word — they never carry a payload — and they are the
  // only commands permitted to change state a later primitive reads.
  SetDrawMode = 0xE1,      // texpage: page X/Y, colour mode, blend, dither
  SetTextureWindow = 0xE2, // the texture-window mask and offset
  SetDrawAreaTopLeft = 0xE3,
  SetDrawAreaBottomRight = 0xE4,
  SetDrawOffset = 0xE5,
  SetMaskBits = 0xE6, // set-mask / check-mask; neither is modelled as a pixel test
  Unimplemented = 0xFF,
};

// A colour word: 0x00BBGGRR. RED IS IN THE LOW BYTE. That ordering is the guest's, it is the opposite
// of the RGB order the host framebuffer uses, and it is the single fact that makes a hand-decoded
// command word read as a channel swap. It is named here once so no call site re-derives it.
struct Gp0Colour {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;

  friend constexpr bool operator==(Gp0Colour lhs, Gp0Colour rhs) {
    return lhs.red == rhs.red && lhs.green == rhs.green && lhs.blue == rhs.blue;
  }
};

// A signed vertex screen position, packed one axis per halfword. Each axis is 11 bits SIGNED, so the
// full range is -1024..+1023 and a guest may legitimately place geometry left of or above VRAM — the
// draw area clips it, and the sign is not an error to be clamped away at decode time.
struct Gp0VertexPos {
  int x = 0;
  int y = 0;
};

// A texture coordinate word: 8-bit U and V, plus a CLUT or texpage selector in the high half that
// means DIFFERENT things for a polygon's first vertex and its second. The runtime binds the CLUT from
// vertex 0 and the texpage from vertex 1 (see the compositor); the field itself is just the halfword.
struct Gp0TextureCoord {
  int u = 0;
  int v = 0;
  // The high halfword, UNINTERPRETED here. Which of CLUT/texpage it is depends on the vertex index, so
  // naming it in this type would be a claim the decode cannot support.
  std::uint16_t selector = 0;
};

// A rectangle in VRAM: the destination of a fill, a VRAM->VRAM copy, a CPU->VRAM upload or a
// VRAM->CPU readback. The four commands pack it across two words in three DIFFERENT ways, so each has
// its own named decode below rather than one shared unpacker pretending they agree.
//
// The width and height fields are NOT interchangeable between commands: a sprite's are 10 and 9 bits
// with no rounding, a transfer's are 10 and 9 with a zero-width meaning "the full axis", and a fill's
// are snapped outward to a 16-pixel boundary. That difference is the reason this type carries no
// decode of its own.
struct Gp0VramRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// One corner of the DRAW AREA clip rectangle (E3 sets the top-left, E4 the bottom-right). Unlike a
// vertex position these are UNSIGNED — a clip corner below zero is not a position a guest can express,
// and clamping one at decode time would silently move the clip instead of refusing it.
struct Gp0VramPos {
  int x = 0;
  int y = 0;
};

// The two mask bits of E6. NEITHER is honoured as a pixel test; see Gp0Command::maskBits() for why the
// second one is a correctness gap rather than a missing feature.
struct Gp0MaskBits {
  // bit 0: mark the destination pixel's mask bit as this primitive draws it.
  bool setMask = false;
  // bit 1: SKIP the write where the destination pixel is already masked. Honoured on hardware, so
  // overlapping primitives the guest drew deliberately composite once. Not modelled here — see
  // docs/issues/0132-gp0-e6-check-mask-unmodelled.md.
  bool checkMask = false;
};

// The texture window of E2: a per-axis texel MASK and a per-axis texel OFFSET. The window restricts
// which texels of the sampled page are visible; it never scales or shifts the image.
struct Gp0TextureWindow {
  int maskX = 0;
  int maskY = 0;
  int offsetX = 0;
  int offsetY = 0;
};

// The flag bits shared by the polygon, line and rectangle opcodes. They occupy the SAME positions in
// all three ranges, which is why they are one type: a reader learns the bit layout once.
struct Gp0PrimitiveFlags {
  // bit 0. Set = draw the texel unmodified; clear = modulate it by the command colour. Present on
  // polygons and rectangles; on a line it is unused.
  bool rawTexel = false;
  // bit 1. Semi-transparent: the prim composites through the texpage's blend mode instead of replacing
  // the destination pixel.
  bool semiTransparent = false;
  // bit 2. Textured. On a line there is no texture and the bit is unused.
  bool textured = false;
  // bit 3. Quad (polygons) / poly-line (lines). On a RECTANGLE this bit is part of the SIZE CODE rather
  // than a flag, so it reads as SET for the three fixed sizes and clear for the variable one — the flag
  // carries no meaning there and must not be consulted. Which of the two it means on a line is decided
  // by the opcode range, never by this bit alone.
  bool quadOrPolyLine = false;
  // bit 4. Gouraud: each vertex carries its own colour instead of the command's.
  bool gouraud = false;
};

// ONE decoded GP0 command word.
class Gp0Command {
public:
  constexpr explicit Gp0Command(std::uint32_t word) : word_(word) {}

  constexpr std::uint32_t word() const {
    return word_;
  }
  // The raw opcode byte, before the range tests below collapse it to a named kind.
  constexpr std::uint8_t opcodeByte() const {
    return static_cast<std::uint8_t>(word_ >> 24);
  }

  // The opcode as its base kind. The three primitive ranges report Polygon / Line / Rectangle; a range
  // member (0x20-0x3F etc.) is NOT a distinct kind, because nothing downstream treats 0x28 differently
  // from 0x20 except through the flags — and the flags are decoded once, below.
  constexpr Gp0Opcode opcode() const {
    return static_cast<Gp0Opcode>(opcodeByte());
  }

  // The named kind, with the three ranges collapsed onto their base opcode and anything unrecognized
  // reported as Unimplemented. `isDrawable()` is the question the compositor actually asks.
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
  // A poly-LINE is a line whose bit 3 is set, and it is VARIABLE length: the guest terminates the
  // vertex list with a sentinel word rather than a count. This is the distinction that matters, because
  // treating a poly-line as a fixed-length line desynchronises the whole GP0 parse.
  constexpr bool isPolyLine() const {
    return isLineOrPolyLine() && flags().quadOrPolyLine;
  }

  // The one-byte, no-payload draw-environment commands. A command is in this set if and only if it is
  // not drawable and not a transfer; the compositor routes on it before touching the FIFO.
  constexpr bool isDrawEnvironmentCommand() const {
    return opcodeByte() >= kDrawEnvironmentLow && opcodeByte() <= kDrawEnvironmentHigh;
  }
  // Nop and clear-cache: accepted and discarded, which is not the same as unimplemented.
  constexpr bool isDiscardedNoOp() const {
    return opcodeByte() == static_cast<std::uint8_t>(Gp0Opcode::Nop) ||
           opcodeByte() == static_cast<std::uint8_t>(Gp0Opcode::ClearCache);
  }
  // The VRAM<->CPU and VRAM<->VRAM transfer headers: a fixed number of words, then (for the CPU-side
  // directions) a pixel stream that continues until the declared rect is full.
  constexpr bool isVramTransfer() const {
    return opcodeByte() == static_cast<std::uint8_t>(Gp0Opcode::VramToVramCopy) ||
           opcodeByte() == static_cast<std::uint8_t>(Gp0Opcode::CpuToVramUpload) ||
           opcodeByte() == static_cast<std::uint8_t>(Gp0Opcode::VramToCpuRead);
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

  // A polygon's vertex count: 4 for a quad, 3 for a triangle. NOT a decision about a poly-line, which
  // has no count at all — see isPolyLine().
  constexpr int polygonVertexCount() const {
    return flags().quadOrPolyLine ? 4 : 3;
  }
  // A rectangle's size selector, 0..3. 0 means "a width/height word follows"; 1, 2 and 3 are the fixed
  // 1x1, 8x8 and 16x16 sizes. See rectangleFixedSize() for the resolved dimensions.
  constexpr int rectangleSizeCode() const {
    return (opcodeByte() >> 3) & 0x3;
  }
  // The fixed side length a size code 1..3 selects, or 0 for code 0 (variable). All three fixed sizes are
  // SQUARE, so one number answers it.
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

  // The colour packed in the low three bytes, in the guest's 0x00BBGGRR order.
  constexpr Gp0Colour colour() const {
    return Gp0Colour{static_cast<std::uint8_t>(word_ & 0xFFu),
                     static_cast<std::uint8_t>((word_ >> 8) & 0xFFu),
                     static_cast<std::uint8_t>((word_ >> 16) & 0xFFu)};
  }
  // The signed vertex position packed one axis per halfword. Both axes are 11-bit SIGNED; the 12th bit
  // of each halfword is not part of the coordinate and is not read here.
  constexpr Gp0VertexPos vertexPos() const {
    return Gp0VertexPos{signedAxis(word_ & 0xFFFFu), signedAxis((word_ >> 16) & 0xFFFFu)};
  }
  // A texture coordinate word. `selector` is the UNINTERPRETED high halfword — see Gp0TextureCoord.
  constexpr Gp0TextureCoord textureCoord() const {
    return Gp0TextureCoord{static_cast<int>(word_ & 0xFFu),
                           static_cast<int>((word_ >> 8) & 0xFFu),
                           static_cast<std::uint16_t>((word_ >> 16) & 0xFFFFu)};
  }
  // A texpage/CLUT/draw-environment payload: the low halfword on its own. E1-E6 all carry their
  // parameters there.
  constexpr std::uint16_t lowHalfWord() const {
    return static_cast<std::uint16_t>(word_ & 0xFFFFu);
  }

  // A DRAW AREA corner (E3 top-left, E4 bottom-right): the same packing for both, so one decode. X is
  // 10 bits, Y is 9 bits, and they are the two axes of the clip rectangle in VRAM — NOT the same 11-bit
  // signed pair a vertex uses, which is why this is a separate named decode rather than vertexPos().
  constexpr Gp0VramPos drawAreaCorner() const {
    return Gp0VramPos{static_cast<int>(word_ & kSpriteWidthMask),
                      static_cast<int>((word_ >> kDrawAreaAxisShift) & kFillRectHeightMask)};
  }

  // The top-left corner of a VRAM RECT, as the sprite, fill, copy and transfer commands pack it: X in
  // bits 0..9, Y in bits 16..24, one per halfword.
  //
  // THIS IS NOT drawAreaCorner(), and the difference is worth a whole command. A draw-area corner
  // starts at bit 10; a rect corner starts at bit 16. Reading one with the other's shift moves every
  // rect by a factor of 64 in Y, which for a texture upload is a wrong PAGE rather than a wrong pixel.
  constexpr Gp0VramPos rectCorner() const {
    return Gp0VramPos{static_cast<int>(word_ & kSpriteWidthMask),
                      static_cast<int>((word_ >> kRectCornerAxisShift) & kFillRectHeightMask)};
  }

  // The DRAW OFFSET (E5): two 11-bit SIGNED axes, packed adjacently rather than one per halfword. It
  // translates every subsequent primitive's coordinates, and it is signed, so a negative offset is a
  // legitimate way to move the whole scene left/up rather than something to clamp.
  constexpr Gp0VertexPos drawOffset() const {
    return Gp0VertexPos{signedAxis(word_ & kVertexAxisMask),
                        signedAxis((word_ >> kDrawOffsetAxisShift) & kVertexAxisMask)};
  }

  // The MASK bits (E6). NEITHER is modelled as a pixel test. Bit 1 matters for correctness and is
  // recorded rather than implemented: with check-mask on, hardware SKIPS a write wherever the
  // destination pixel is already masked, so deliberately overlapping primitives composite ONCE instead
  // of twice. A port that ignores it double-composites every overlap the guest asked to composite once.
  constexpr Gp0MaskBits maskBits() const {
    // Both bits are tested UNSHIFTED against their own mask. Shifting the word down first and then
    // testing against the same mask reads bit 2 for the check-mask — a bit that is not part of the
    // command — and reports check-mask as permanently set for any word with bit 1 clear. The masks are
    // the bit positions, so they are used as bit positions.
    return Gp0MaskBits{(word_ & kMaskSetBit) != 0, (word_ & kMaskCheckBit) != 0};
  }

  // The TEXTURE WINDOW (E2): four 5-bit fields, a mask in texels and an offset, and they mean
  // different things per axis. The window RESTRICTS which texels of a sampled page may be read; it does
  // not scale or offset the image.
  constexpr Gp0TextureWindow textureWindow() const {
    const std::uint32_t payload = word_ & 0xFFFFu;
    return Gp0TextureWindow{static_cast<int>(payload & 0x1Fu),
                            static_cast<int>((payload >> 5) & 0x1Fu),
                            static_cast<int>((payload >> 10) & 0x1Fu),
                            static_cast<int>((payload >> 15) & 0x1Fu)};
  }

  // How many GP0 words this command occupies IN ITS HEADER, counting the command word itself.
  //
  // This is the packet-framing decision, and it is the one that goes wrong silently: a length that is
  // one word short desynchronises every command after it, and the stream then decodes data as commands
  // (a VRAM copy landing on an atlas is the classic symptom). It is stated here as one table over the
  // opcode, not derived at four call sites.
  //
  // TWO KNOWN LIMITS, both kept rather than papered over because the compositor compensates for them
  // and a reader needs to know why:
  //  - A POLY-LINE is variable length and this returns the fixed-line length. The compositor detects
  //    the poly-line sentinel and reframes the packet itself; this number is then only the minimum
  //    before the terminator can appear.
  //  - The CPU<->VRAM directions return their HEADER length. The pixel stream that follows is not
  //    counted, because its length is the declared rect's area and arrives as data, not as more words
  //    of a command.
  static constexpr unsigned packetWordCount(std::uint32_t word) {
    const Gp0Command command(word);
    const std::uint8_t op = command.opcodeByte();
    if (command.isPolygon()) {
      const int vertices = command.polygonVertexCount();
      // Per vertex: the position word, plus a texture-coordinate word when textured. Gouraud adds a
      // colour word for every vertex EXCEPT the first, whose colour is in the command word itself.
      unsigned count = 1u + static_cast<unsigned>(vertices) * (command.flags().textured ? 2u : 1u);
      if (command.flags().gouraud) {
        count += static_cast<unsigned>(vertices - 1);
      }
      return count;
    }
    if (command.isRectangleOrSprite()) {
      // Position, plus a texture-coordinate word when textured, plus a size word for the VARIABLE
      // size only — the three fixed sizes carry no size word.
      unsigned count = 2u + (command.flags().textured ? 1u : 0u) + (command.rectangleSizeCode() == 0 ? 1u : 0u);
      return count;
    }
    if (command.isLineOrPolyLine()) {
      return (command.flags().gouraud) ? 4u : 3u; // the position of vertex 1, plus its colour if any
    }
    switch (command.opcode()) {
    case Gp0Opcode::FillRect:
      return 3u; // colour+command, top-left, size
    case Gp0Opcode::VramToVramCopy:
      return 4u; // command, source top-left, destination top-left, size
    case Gp0Opcode::CpuToVramUpload:
    case Gp0Opcode::VramToCpuRead:
      return 3u; // command, top-left, size — the pixels stream after
    default:
      break;
    }
    return 1u; // draw environment, nop, clear cache, and everything unmodelled
  }

  // A monochrome FILL RECT's region, decoded from its two payload words.
  //
  // TWO GUEST QUIRKS, both of which are the reason this is a named decode rather than inline arithmetic:
  //
  //  - The ORIGIN is taken from 9 bits with bits 3..0 DROPPED (`x = coord & 0x3F0`), so a fill can never
  //    start on an odd x. The width is then rounded UP to the next multiple of 16. Together they mean
  //    the fill is 16-pixel aligned on the left and in extent — a fill the guest sized to 320x240 at
  //    (0,0) covers [0, 320) x [0, 240), but one sized to 300 wide covers 304 columns.
  //  - The size word's width field is 10 bits, and a width of 0 is a legal "fill nothing" — it is NOT
  //    the "whole axis" convention the CPU<->VRAM transfers use. The two conventions coexist in one
  //    opcode space, and reading a fill's width with the transfer's rule would turn a 0-width fill into
  //    a full-axis one.
  //
  // A fill also IGNORES the draw area and the draw offset, by hardware design. That is not a decode
  // fact and is not enforced here; it is the compositor's business and it is the reason a fill cannot be
  // clipped away by a stale draw area.
  static constexpr Gp0VramRect fillRectRegion(std::uint32_t topLeftWord, std::uint32_t sizeWord) {
    Gp0VramRect rect;
    rect.x = static_cast<int>(topLeftWord & kFillRectOriginMask);
    rect.y = static_cast<int>((topLeftWord >> kRectCornerAxisShift) & kFillRectHeightMask);
    rect.width = static_cast<int>((sizeWord & kSpriteWidthMask) + kFillRectAlign) & ~kFillRectAlign;
    rect.height = static_cast<int>((sizeWord >> kRectCornerAxisShift) & kFillRectHeightMask);
    return rect;
  }

  // A rectangle or sprite's region. The size word is present only for the VARIABLE size (code 0); the
  // three fixed sizes are square and carry no word at all, so the width and height are one number.
  static constexpr Gp0VramRect spriteRegion(std::uint32_t topLeftWord, std::uint32_t sizeWord, int fixedSize) {
    Gp0VramRect rect;
    // A sprite's top-left is a VERTEX position (signed, 11 bits per halfword), not a VRAM rect corner
    // (unsigned, 10/9 bits) — they are the same two halfwords with different widths, and a sprite that
    // hangs off the left edge is expressed as a negative here.
    const Gp0VertexPos topLeft = Gp0Command(topLeftWord).vertexPos();
    rect.x = topLeft.x;
    rect.y = topLeft.y;
    if (fixedSize != 0) {
      rect.width = fixedSize;
      rect.height = fixedSize;
    } else {
      rect.width = static_cast<int>(sizeWord & kSpriteWidthMask);
      rect.height = static_cast<int>((sizeWord >> kRectCornerAxisShift) & kFillRectHeightMask);
    }
    return rect;
  }

  // A CPU<->VRAM transfer's region, decoded from its two payload words.
  //
  // A ZERO width or height means THE WHOLE AXIS (1024 wide, 512 tall), not "nothing". The guest's own
  // loader library leans on this constantly — a full-page texture upload is declared as size 0. The two
  // directions MUST agree on this, or a readback of an upload would cover a different rectangle than the
  // upload wrote and silently corrupt exactly the save/restore round trip it exists to serve.
  static constexpr Gp0VramRect transferRegion(std::uint32_t topLeftWord, std::uint32_t sizeWord) {
    const Gp0VramPos corner = Gp0Command(topLeftWord).rectCorner();
    Gp0VramRect rect;
    rect.x = corner.x;
    rect.y = corner.y;
    rect.width = static_cast<int>(sizeWord & kSpriteWidthMask);
    if (rect.width == 0) {
      rect.width = kVramWidth;
    }
    rect.height = static_cast<int>((sizeWord >> kRectCornerAxisShift) & kFillRectHeightMask);
    if (rect.height == 0) {
      rect.height = kVramHeight;
    }
    return rect;
  }

  // The poly-line terminator. A poly-line has no vertex COUNT, so the guest ends its vertex list with a
  // word whose upper and lower halfwords are both 0x5000. Testing the word rather than a count is the
  // whole mechanism; a poly-line read as a fixed-length line never finds it and the parse desynchronises.
  static constexpr bool isPolyLineTerminator(std::uint32_t word) {
    return (word & 0xF000F000u) == 0x50005000u;
  }

  // Where in a poly-line's word stream a terminator is allowed to appear. A vertex's position word is
  // preceded by its colour word when the poly-line is gouraud, so the two layouts sit the terminator at
  // different indices; accepting it anywhere else would truncate a real vertex.
  static constexpr bool isPolyLineTerminatorSlot(unsigned wordIndex, bool gouraud) {
    // gouraud: the command word, then (colour, position) per vertex — positions are the ODD indices
    // from 1, and a colour slot is any even index from 4 on.
    // mono:    the command word, then position words — positions are indices 3 and up.
    return gouraud ? (wordIndex >= 4u && (wordIndex & 1u) == 0u) : (wordIndex >= 3u);
  }

private:
  // The opcode byte range the draw-environment commands occupy (E1..E6). The names above are exact
  // values inside it; this is the range, because the compositor must route on membership before it can
  // dispatch on a specific value.
  static constexpr std::uint8_t kDrawEnvironmentLow = 0xE1;
  static constexpr std::uint8_t kDrawEnvironmentHigh = 0xE6;

  // The flag bit positions, identical across the polygon, line and rectangle opcodes.
  static constexpr std::uint8_t kFlagRawTexel = 0x01;
  static constexpr std::uint8_t kFlagSemiTransparent = 0x02;
  static constexpr std::uint8_t kFlagTextured = 0x04;
  static constexpr std::uint8_t kFlagQuadOrPolyLine = 0x08;
  static constexpr std::uint8_t kFlagGouraud = 0x10;

  // The VRAM geometry fields, shared in width by the sprite, fill and transfer size words.
  static constexpr std::uint32_t kSpriteWidthMask = 0x3FFu;    // 10 bits
  static constexpr std::uint32_t kFillRectHeightMask = 0x1FFu; // 9 bits
  // Where the Y axis of a packed coordinate starts. A RECT corner puts it in the upper halfword (bit 16);
  // a DRAW AREA corner starts it at bit 10 and runs to bit 18. See rectCorner() / drawAreaCorner().
  static constexpr unsigned kRectCornerAxisShift = 16;
  static constexpr unsigned kDrawAreaAxisShift = 10;
  // E5's second offset axis starts one axis-width above the first, because the two are adjacent 11-bit
  // fields rather than one per halfword.
  static constexpr unsigned kDrawOffsetAxisShift = 11;
  // The two E6 mask bits, as bit masks rather than a shifted value, so the decode cannot be read as an
  // off-by-one index.
  static constexpr std::uint32_t kMaskSetBit = 0x1u;
  static constexpr std::uint32_t kMaskCheckBit = 0x2u;
  // A fill's origin mask is 0x3F0, NOT the sprite's 0x3FF: bits 3..0 are dropped, which is what makes a
  // fill 16-pixel aligned. See fillRectRegion().
  static constexpr std::uint32_t kFillRectOriginMask = 0x3F0u;
  static constexpr int kFillRectAlign = 0xF; // the fill's width is rounded up to this multiple
  // A transfer's "0 means the whole axis" sentinels, in pixels.
  static constexpr int kVramWidth = 1024;
  static constexpr int kVramHeight = 512;

  // The one piece of state this type has: the guest's word, kept verbatim so every accessor is a pure
  // view of it and a diagnostic can print the original.
  std::uint32_t word_ = 0;

  constexpr bool isInRange(std::uint8_t low, std::uint8_t high) const {
    const std::uint8_t op = opcodeByte();
    return op >= low && op <= high;
  }
  // One 11-bit signed axis out of a 16-bit halfword. The sign is bit 10 of the axis, not bit 15 of the
  // halfword, so the halfword has to be narrowed to 11 bits before it is sign-extended — sign-extending
  // the halfword instead silently maps every coordinate at or above +1024 to a negative value, which is
  // a screen position and therefore a visible one.
  static constexpr int signedAxis(std::uint32_t halfWord) {
    const std::uint32_t axis = halfWord & kVertexAxisMask;
    return static_cast<int>((axis & kVertexAxisSign) ? axis - kVertexAxisRange : axis);
  }
  static constexpr std::uint32_t kVertexAxisMask = 0x7FFu;
  static constexpr std::uint32_t kVertexAxisSign = 0x400u;
  static constexpr std::uint32_t kVertexAxisRange = 0x800u;
};

} // namespace psx::gpu

#endif // PSXPORT_GP0_COMMAND_H
