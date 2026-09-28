// test_gp0_command — the guest's GP0 word, decoded by name, must decode to the same bits it always did.
//
// Every drawable a PSX game submits is a stream of 32-bit GP0 words, each a packed command. Before this
// was a named owner, decoding one meant writing `w >> 24`, `w & 0xFF`, `(int)(w & 0x7FF) - (…)` at every
// use, and a reader had to already know that RED IS IN THE LOW BYTE of a BGR colour word. The failures
// available here are all silent: a wrong channel order is a channel swap, a wrong sign bit is a screen
// position, and a wrong packet length desynchronises every command after it so a later DATA word decodes
// as a VRAM copy — atlas corruption rather than a rendering artefact.
//
// THE OLD CONTRACT IS THE ORACLE. Each case recomputes the inline arithmetic this owner replaced and
// asserts the named accessor agrees, so a refactor that renames without preserving cannot go green.
// Where the OLD code carried a limitation, that limitation is asserted as still-present rather than
// quietly fixed — a readability change that also corrected a decode would hide a behaviour change
// inside a large diff.
//
// Hermetic: every case is a pure function of one or two 32-bit words. No GPU, no disc, no window.
//
// NEGATIVE-RESULT DISCIPLINE: each family is asserted over a SET of inputs and against the reference,
// not on one hand-picked word, so a decode that happens to be right for the common case cannot pass.
#include "../runtime/psx/gp0_command.h"
#include "testutil.h"

#include <cstdint>

namespace {

using psx::gpu::Gp0Command;

// The inline arithmetic the owner replaced, kept as the reference.
std::uint8_t referenceRed(std::uint32_t w) {
  return static_cast<std::uint8_t>(w & 0xFFu);
}
std::uint8_t referenceGreen(std::uint32_t w) {
  return static_cast<std::uint8_t>((w >> 8) & 0xFFu);
}
std::uint8_t referenceBlue(std::uint32_t w) {
  return static_cast<std::uint8_t>((w >> 16) & 0xFFu);
}
int referenceLowAxis(std::uint32_t w) {
  const int v = static_cast<int>(w & 0x7FFu);
  return v >= 0x400 ? v - 0x800 : v;
}
int referenceHighAxis(std::uint32_t w) {
  const int v = static_cast<int>((w >> 16) & 0x7FFu);
  return v >= 0x400 ? v - 0x800 : v;
}
bool referenceIsPolygon(std::uint32_t w) {
  const std::uint8_t op = static_cast<std::uint8_t>(w >> 24);
  return op >= 0x20 && op <= 0x3F;
}
bool referenceIsLine(std::uint32_t w) {
  const std::uint8_t op = static_cast<std::uint8_t>(w >> 24);
  return op >= 0x40 && op <= 0x5F;
}
bool referenceIsSprite(std::uint32_t w) {
  const std::uint8_t op = static_cast<std::uint8_t>(w >> 24);
  return op >= 0x60 && op <= 0x7F;
}
bool referenceIsPolyLine(std::uint32_t w) {
  const std::uint8_t op = static_cast<std::uint8_t>(w >> 24);
  return op >= 0x40 && op <= 0x5F && (op & 0x08) != 0;
}

// The old gp0_len(), verbatim in behaviour.
int referencePacketWordCount(std::uint32_t c) {
  const std::uint8_t op = static_cast<std::uint8_t>(c >> 24);
  if (op >= 0x20 && op <= 0x3F) {
    int n = 1, nv = (op & 8) ? 4 : 3;
    n += nv * (1 + ((op & 4) ? 1 : 0));
    if (op & 0x10) {
      n += nv - 1;
    }
    return n;
  }
  if (op >= 0x60 && op <= 0x7F) {
    int n = 2;
    if (op & 4) {
      n++;
    }
    if (((op >> 3) & 3) == 0) {
      n++;
    }
    return n;
  }
  if (op >= 0x40 && op <= 0x5F) {
    return (op & 0x10) ? 4 : 3;
  }
  if (op == 0x02) {
    return 3;
  }
  if (op == 0x80) {
    return 4;
  }
  if (op == 0xA0 || op == 0xC0) {
    return 3;
  }
  return 1;
}

// A representative colour, used where the colour is not what is under test.
constexpr std::uint32_t kRed = 0x00'00'00'FFu;

// One command word per polygon variant, so every flag combination in the range is covered.
constexpr std::uint32_t kPolygonCommands[] = {
    0x20000000u,
    0x21000000u,
    0x22000000u,
    0x24000000u,
    0x28000000u,
    0x2C000000u,
    0x30000000u,
    0x34000000u,
    0x38000000u,
    0x3C000000u,
    0x2F000000u,
    0x3B000000u,
    0x20000000u | kRed,
};

// One per rectangle variant, including the three FIXED sizes, which carry no size word at all.
constexpr std::uint32_t kSpriteCommands[] = {
    0x60000000u,
    0x64000000u,
    0x68000000u,
    0x70000000u,
    0x78000000u,
    0x6C000000u,
    0x74000000u,
};

// One per line/poly-line variant, flat and gouraud, single and poly.
constexpr std::uint32_t kLineCommands[] = {
    0x40000000u,
    0x42000000u,
    0x50000000u,
    0x48000000u,
    0x58000000u,
    0x58000000u | kRed,
};

} // namespace

// ---- colour: the guest packs 0x00BBGGRR, so RED IS THE LOW BYTE ---------------------------------
static void test_colour_byte_order_matches_the_original_shift(void) {
  constexpr std::uint32_t words[] = {
      0x00000000u, 0x00'00'00'FFu, 0x00'00'FF'00u, 0x00'FF'00'00u, 0x00'FF'FF'FFu, 0x00'12'34'56u, 0x20'AB'CD'EFu};
  for (std::uint32_t w : words) {
    const auto colour = Gp0Command(w).colour();
    CHECK_EQ(colour.red, referenceRed(w));
    CHECK_EQ(colour.green, referenceGreen(w));
    CHECK_EQ(colour.blue, referenceBlue(w));
  }
  // The fact itself, named: a colour of 0x00112233 is RED 0x33, GREEN 0x22, BLUE 0x11. Getting this
  // backwards is a channel permutation, so the picture still renders — as its complement.
  const auto packed = Gp0Command(0x00'11'22'33u).colour();
  CHECK_EQ(packed.red, 0x33u);
  CHECK_EQ(packed.green, 0x22u);
  CHECK_EQ(packed.blue, 0x11u);
}

// ---- vertex position: 11-bit SIGNED axes, one per halfword ---------------------------------------
static void test_vertex_axes_are_eleven_bit_signed(void) {
  constexpr std::uint32_t words[] = {0x00000000u,
                                     0x00000001u,
                                     0x000003FFu,
                                     0x00000400u,
                                     0x000007FFu,
                                     0x00010000u,
                                     0x00014000u,
                                     0x00018000u,
                                     0x0001FFFFu,
                                     0x1234'5678u};
  for (std::uint32_t w : words) {
    const auto pos = Gp0Command(w).vertexPos();
    CHECK_EQ(pos.x, referenceLowAxis(w));
    CHECK_EQ(pos.y, referenceHighAxis(w));
  }
  // The three boundaries that matter, because the sign is bit 10 of the AXIS and not bit 15 of the
  // halfword. Sign-extending the halfword instead maps every coordinate from +1024 upward to a
  // NEGATIVE screen position, which is a visible misdraw rather than a clamp.
  CHECK_EQ(Gp0Command(0x000003FFu).vertexPos().x, 1023);
  CHECK_EQ(Gp0Command(0x00000400u).vertexPos().x, -1024);
  CHECK_EQ(Gp0Command(0x000007FFu).vertexPos().x, -1);
  CHECK_EQ(Gp0Command(0x0400'0000u).vertexPos().y, -1024);
  // The 12th bit of each halfword is not part of the coordinate and must not be read as one.
  CHECK_EQ(Gp0Command(0x00000800u).vertexPos().x, 0);
  CHECK_EQ(Gp0Command(0x0000'0800u).vertexPos().y, 0);
}

// ---- opcode ranges and flags ---------------------------------------------------------------------
static void test_opcode_ranges_match_the_original_comparisons(void) {
  for (std::uint32_t w : kPolygonCommands) {
    CHECK(Gp0Command(w).isPolygon());
    CHECK_EQ(Gp0Command(w).isPolygon(), referenceIsPolygon(w));
    CHECK(!Gp0Command(w).isLineOrPolyLine());
    CHECK(!Gp0Command(w).isRectangleOrSprite());
  }
  for (std::uint32_t w : kSpriteCommands) {
    CHECK(Gp0Command(w).isRectangleOrSprite());
    CHECK_EQ(Gp0Command(w).isRectangleOrSprite(), referenceIsSprite(w));
    CHECK(!Gp0Command(w).isPolygon());
  }
  for (std::uint32_t w : kLineCommands) {
    CHECK(Gp0Command(w).isLineOrPolyLine());
    CHECK_EQ(Gp0Command(w).isLineOrPolyLine(), referenceIsLine(w));
    CHECK(!Gp0Command(w).isPolygon());
  }
  // The range EDGES, because an off-by-one in a range test is a whole class of command silently
  // decoded as another: 0x1F is not a polygon and 0x40 is not a sprite.
  CHECK(!Gp0Command(0x1F'00'00'00u).isDrawable());
  CHECK(Gp0Command(0x20'00'00'00u).isDrawable());
  CHECK(Gp0Command(0x3F'00'00'00u).isDrawable());
  CHECK(!Gp0Command(0x40'00'00'00u).isDrawable() == false); // a line IS drawable
  CHECK(Gp0Command(0x40'00'00'00u).isDrawable());
  CHECK(Gp0Command(0x5F'00'00'00u).isDrawable());
  CHECK(Gp0Command(0x60'00'00'00u).isDrawable());
  CHECK(Gp0Command(0x7F'00'00'00u).isDrawable());
  CHECK(!Gp0Command(0x80'00'00'00u).isDrawable());
  CHECK(!Gp0Command(0x02'00'00'00u).isDrawable());
  CHECK(!Gp0Command(0xE1'00'00'00u).isDrawable());
}

static void test_polygon_flags_sit_at_the_original_bit_positions(void) {
  const auto plain = Gp0Command(0x20000000u).flags();
  CHECK(!plain.gouraud);
  CHECK(!plain.quadOrPolyLine);
  CHECK(!plain.textured);
  CHECK(!plain.semiTransparent);
  CHECK(!plain.rawTexel);
  const auto all = Gp0Command(0x3F000000u).flags();
  CHECK(all.gouraud);
  CHECK(all.quadOrPolyLine);
  CHECK(all.textured);
  CHECK(all.semiTransparent);
  CHECK(all.rawTexel);
  // Each flag on its own, so a decode that reads two bits for one flag cannot pass.
  CHECK(Gp0Command(0x30000000u).flags().gouraud);
  CHECK(!Gp0Command(0x30000000u).flags().quadOrPolyLine);
  CHECK(Gp0Command(0x28000000u).flags().quadOrPolyLine);
  CHECK(!Gp0Command(0x28000000u).flags().gouraud);
  CHECK(Gp0Command(0x24000000u).flags().textured);
  CHECK(!Gp0Command(0x24000000u).flags().semiTransparent);
  CHECK(Gp0Command(0x22000000u).flags().semiTransparent);
  CHECK(Gp0Command(0x21000000u).flags().rawTexel);
}

static void test_polygon_vertex_count_follows_the_quad_bit(void) {
  for (std::uint32_t w : kPolygonCommands) {
    const std::uint8_t op = static_cast<std::uint8_t>(w >> 24);
    CHECK_EQ(Gp0Command(w).polygonVertexCount(), (op & 8) ? 4 : 3);
  }
  CHECK_EQ(Gp0Command(0x20000000u).polygonVertexCount(), 3);
  CHECK_EQ(Gp0Command(0x28000000u).polygonVertexCount(), 4);
}

static void test_rectangle_size_codes_and_their_fixed_sizes(void) {
  // A size code is bits 3-4, and code 0 means "a size word follows" while 1/2/3 are 1x1, 8x8 and 16x16.
  CHECK_EQ(Gp0Command(0x60000000u).rectangleSizeCode(), 0);
  CHECK_EQ(Gp0Command(0x60000000u).rectangleFixedSize(), 0);
  CHECK_EQ(Gp0Command(0x68000000u).rectangleSizeCode(), 1);
  CHECK_EQ(Gp0Command(0x68000000u).rectangleFixedSize(), 1);
  CHECK_EQ(Gp0Command(0x70000000u).rectangleSizeCode(), 2);
  CHECK_EQ(Gp0Command(0x70000000u).rectangleFixedSize(), 8);
  CHECK_EQ(Gp0Command(0x78000000u).rectangleSizeCode(), 3);
  CHECK_EQ(Gp0Command(0x78000000u).rectangleFixedSize(), 16);
}

static void test_poly_line_is_a_line_with_the_quad_bit_set(void) {
  for (std::uint32_t w : kLineCommands) {
    CHECK_EQ(Gp0Command(w).isPolyLine(), referenceIsPolyLine(w));
  }
  CHECK(!Gp0Command(0x40000000u).isPolyLine()); // a plain line: fixed two vertices
  CHECK(Gp0Command(0x48000000u).isPolyLine());  // mono poly-line
  CHECK(Gp0Command(0x58000000u).isPolyLine());  // gouraud poly-line
  CHECK(!Gp0Command(0x50000000u).isPolyLine()); // gouraud SINGLE line
  // The bit means something DIFFERENT per range, which is why the flag is not named "quad" outright.
  CHECK(Gp0Command(0x28000000u).flags().quadOrPolyLine);
  // On a RECTANGLE bit 3 is the low half of the SIZE CODE, not a flag: 0x68 is the 1x1 size, so the
  // bit reads as set and the flag is meaningless. Recorded here because reading it as a "quad" on a
  // sprite would be reading a size code as a shape.
  CHECK(Gp0Command(0x68000000u).flags().quadOrPolyLine);
  CHECK(!Gp0Command(0x60000000u).flags().quadOrPolyLine); // size code 0 — bit 3 clear
  CHECK(Gp0Command(0x78000000u).flags().quadOrPolyLine);  // size code 3 — bit 3 set
}

// ---- packet framing -------------------------------------------------------------------------------
// THE DECISION. A length that is one word short desynchronises the whole GP0 parse from that point on,
// and the visible symptom is a later DATA word decoded as a VRAM copy — atlas corruption, not a
// rendering artefact. The old gp0_len() is the oracle for every case.
static void test_packet_word_count_matches_the_original_table(void) {
  constexpr std::uint32_t every[] = {
      0x00000000u, 0x01000000u, 0x02000000u, 0x10000000u, 0x20000000u, 0x21000000u, 0x22000000u, 0x24000000u,
      0x28000000u, 0x2C000000u, 0x30000000u, 0x34000000u, 0x38000000u, 0x3C000000u, 0x40000000u, 0x42000000u,
      0x48000000u, 0x50000000u, 0x58000000u, 0x60000000u, 0x64000000u, 0x68000000u, 0x6C000000u, 0x70000000u,
      0x74000000u, 0x78000000u, 0x7C000000u, 0x80000000u, 0xA0000000u, 0xC0000000u, 0xE1000000u, 0xE2000000u,
      0xE3000000u, 0xE4000000u, 0xE5000000u, 0xE6000000u, 0xF0000000u, 0xFF000000u,
  };
  for (std::uint32_t w : every) {
    CHECK_EQ(static_cast<int>(Gp0Command::packetWordCount(w)), referencePacketWordCount(w));
  }
  // Worked values, so a reference that agreed by accident on the table above cannot carry the test.
  // cmd + 3 positions                                    = 4
  CHECK_EQ(Gp0Command::packetWordCount(0x20000000u), 4u);
  // cmd + 3 (position, texcoord)                          = 7
  CHECK_EQ(Gp0Command::packetWordCount(0x24000000u), 7u);
  // gouraud triangle: cmd + 3 positions + 2 extra colours = 6
  CHECK_EQ(Gp0Command::packetWordCount(0x30000000u), 6u);
  // gouraud textured triangle: 1 + 6 + 2                 = 9
  CHECK_EQ(Gp0Command::packetWordCount(0x34000000u), 9u);
  // textured quad: 1 + 4*2                              = 9
  CHECK_EQ(Gp0Command::packetWordCount(0x2C000000u), 9u);
  // gouraud quad: 1 + 4 + 3                              = 8
  CHECK_EQ(Gp0Command::packetWordCount(0x38000000u), 8u);
  // variable sprite: cmd + position + size               = 3
  CHECK_EQ(Gp0Command::packetWordCount(0x60000000u), 3u);
  // textured sprite: cmd + position + texcoord + size    = 4
  CHECK_EQ(Gp0Command::packetWordCount(0x64000000u), 4u);
  // a FIXED-size sprite carries no size word              = 2
  CHECK_EQ(Gp0Command::packetWordCount(0x68000000u), 2u);
  CHECK_EQ(Gp0Command::packetWordCount(0x78000000u), 2u);
  // line 3, gouraud line 4, copy 4, fill 3, transfer 3, env 1
  CHECK_EQ(Gp0Command::packetWordCount(0x40000000u), 3u);
  CHECK_EQ(Gp0Command::packetWordCount(0x50000000u), 4u);
  CHECK_EQ(Gp0Command::packetWordCount(0x80000000u), 4u);
  CHECK_EQ(Gp0Command::packetWordCount(0x02000000u), 3u);
  CHECK_EQ(Gp0Command::packetWordCount(0xA0000000u), 3u);
  CHECK_EQ(Gp0Command::packetWordCount(0xC0000000u), 3u);
  CHECK_EQ(Gp0Command::packetWordCount(0xE1000000u), 1u);
}

// THE LIMITATION, ASSERTED AS STILL PRESENT. A poly-line is variable length, so no first word can state
// its length; the count returned is the fixed-LINE length and the compositor reframes the packet on the
// terminator instead. A change that "fixed" this by inventing a count would desynchronise every poly-line
// the guest draws, so the limitation is pinned rather than removed.
static void test_poly_line_reports_the_fixed_line_length(void) {
  // A poly-line is 0x40-0x5F with bit 3, and its count is the line's — 3 flat, 4 gouraud.
  CHECK_EQ(Gp0Command::packetWordCount(0x48000000u), 3u);
  CHECK_EQ(Gp0Command::packetWordCount(0x58000000u), 4u);
  CHECK_EQ(Gp0Command::packetWordCount(0x48000000u), Gp0Command::packetWordCount(0x40000000u));
  CHECK_EQ(Gp0Command::packetWordCount(0x58000000u), Gp0Command::packetWordCount(0x50000000u));
}

static void test_poly_line_terminator_is_the_sentinel_word(void) {
  // A guest ends a poly-line with 0x55555555: both halfwords 0x5000, tested against the 0xF000F000
  // mask. Asserting the real value matters, because the mask is loose — a word that is merely SIMILAR
  // must not terminate the line, or a real vertex is silently dropped.
  CHECK(Gp0Command::isPolyLineTerminator(0x55555555u));
  CHECK(!Gp0Command::isPolyLineTerminator(0x00000000u));
  // The mask looks at the top NIBBLE of each halfword and requires it to be 5, so a word that is merely
  // similar is rejected. The cases below are what a WIDENED test would wrongly accept.
  CHECK(!Gp0Command::isPolyLineTerminator(0xF000F000u)); // all-ones nibbles are not 5
  CHECK(!Gp0Command::isPolyLineTerminator(0x50009000u)); // low halfword's top nibble is 9, not 5
  CHECK(!Gp0Command::isPolyLineTerminator(0x90005000u)); // high halfword's top nibble is 9, not 5
  CHECK(!Gp0Command::isPolyLineTerminator(0x30005000u)); // and 3, not 5
}

static void test_poly_line_terminator_may_only_appear_at_a_vertex_start(void) {
  // A terminator accepted at ANY index truncates a real vertex. The two layouts sit vertex starts at
  // different indices because a gouraud poly-line interleaves a colour word per vertex.
  //
  //   gouraud: cmd,xy0,c1,xy1, then c2/term,xy2,...  -> starts are the EVEN indices from 4
  //   mono:    cmd,xy0,xy1, then xy2/term,xy3,...     -> starts are the indices from 3
  CHECK(!Gp0Command::isPolyLineTerminatorSlot(0, false));
  CHECK(!Gp0Command::isPolyLineTerminatorSlot(1, false));
  CHECK(!Gp0Command::isPolyLineTerminatorSlot(2, false)); // vertex 0's position
  CHECK(Gp0Command::isPolyLineTerminatorSlot(3, false));  // vertex 1's position — the first legal slot
  CHECK(Gp0Command::isPolyLineTerminatorSlot(4, false));
  CHECK(!Gp0Command::isPolyLineTerminatorSlot(0, true)); // the command word is never a vertex start
  CHECK(!Gp0Command::isPolyLineTerminatorSlot(1, true)); // vertex 0's position
  CHECK(!Gp0Command::isPolyLineTerminatorSlot(2, true)); // vertex 1's colour
  CHECK(!Gp0Command::isPolyLineTerminatorSlot(3, true)); // vertex 1's position
  CHECK(Gp0Command::isPolyLineTerminatorSlot(4, true));  // vertex 2's colour — the first legal slot
  CHECK(!Gp0Command::isPolyLineTerminatorSlot(5, true)); // vertex 2's position
  CHECK(Gp0Command::isPolyLineTerminatorSlot(6, true));
}

// ---- the three VRAM region decodes, which deliberately DISAGREE ------------------------------------
// A sprite, a fill and a transfer all name a rectangle, and they do not pack or interpret it the same
// way. Two of the differences are silent misdraws; that is why each has its own named decode.
static void test_fill_rect_is_sixteen_pixel_aligned_and_rounds_up(void) {
  // x keeps only bits 4-9, so a fill can never start on an odd column, and the width rounds UP to a
  // multiple of 16. A 300-wide fill therefore covers 304 columns — which is the hardware's behaviour and
  // not a rounding mistake to "fix" here.
  const auto atOrigin = Gp0Command::fillRectRegion(0x00000000u, (240u << 16) | 320u);
  CHECK_EQ(atOrigin.x, 0);
  CHECK_EQ(atOrigin.y, 0);
  CHECK_EQ(atOrigin.width, 320);
  CHECK_EQ(atOrigin.height, 240);
  const auto odd = Gp0Command::fillRectRegion(0x00000000u, (240u << 16) | 300u);
  CHECK_EQ(odd.width, 304);
  const auto shifted = Gp0Command::fillRectRegion(0x0000'0017u, (10u << 16) | 5u);
  CHECK_EQ(shifted.x, 0x10);   // 0x17 & 0x3F0
  CHECK_EQ(shifted.width, 16); // 5 rounds up to one 16-pixel block
  // A ZERO width is "fill nothing" here — NOT the "whole axis" convention a transfer uses. Reading it
  // with the transfer's rule would turn a zero-width fill into a full-VRAM one.
  CHECK_EQ(Gp0Command::fillRectRegion(0u, 0u).width, 0);
  CHECK_EQ(Gp0Command::fillRectRegion(0u, 0u).height, 0);
}

static void test_transfer_zero_size_means_the_whole_axis(void) {
  // The guest's own loader declares a full-page upload as size 0, and BOTH directions must agree, or a
  // readback would cover a different rectangle than the upload wrote and corrupt the save/restore round
  // trip it exists to serve.
  const auto whole = Gp0Command::transferRegion(0u, 0u);
  CHECK_EQ(whole.width, 1024);
  CHECK_EQ(whole.height, 512);
  const auto page = Gp0Command::transferRegion(0x0080'0080u, (256u << 16) | 256u);
  CHECK_EQ(page.x, 0x80);
  CHECK_EQ(page.y, 0x80);
  CHECK_EQ(page.width, 256);
  CHECK_EQ(page.height, 256);
  // Only ONE axis being zero substitutes; the other keeps its value.
  const auto wideOnly = Gp0Command::transferRegion(0u, 240u << 16);
  CHECK_EQ(wideOnly.width, 1024);
  CHECK_EQ(wideOnly.height, 240);
}

static void test_rect_corner_and_draw_area_corner_use_different_shifts(void) {
  // THE ONE THAT BITES. A rect corner's Y starts at bit 16; a draw-area corner's Y starts at bit 10.
  // Reading one with the other's shift moves every rect by a factor of 64 in Y, which for a texture
  // upload is a wrong PAGE rather than a wrong pixel.
  // The OLD inline arithmetic for each, as the oracle, over a set of words.
  constexpr std::uint32_t words[] = {0x0080'0100u, 0x0000'0000u, 0x1234'5678u, 0x00FF'03FFu, 0xABCD'EF01u};
  bool anyDisagree = false;
  for (std::uint32_t word : words) {
    const auto rect = Gp0Command(word).rectCorner();
    const auto area = Gp0Command(word).drawAreaCorner();
    CHECK_EQ(rect.x, static_cast<int>(word & 0x3FFu));
    CHECK_EQ(rect.y, static_cast<int>((word >> 16) & 0x1FFu));
    CHECK_EQ(area.x, static_cast<int>(word & 0x3FFu));
    CHECK_EQ(area.y, static_cast<int>((word >> 10) & 0x1FFu));
    anyDisagree = anyDisagree || (rect.y != area.y);
  }
  CHECK(anyDisagree); // the two decodes genuinely disagree, which is the whole point
}

static void test_sprite_region_uses_a_signed_vertex_position_for_its_origin(void) {
  // A sprite may hang off the left or top edge, so its origin is a SIGNED 11-bit vertex position — not
  // the unsigned rect corner a transfer uses.
  const auto offLeft = Gp0Command::spriteRegion(0x0000'0400u, 0u, 0);
  CHECK_EQ(offLeft.x, -1024);
  const auto fixed = Gp0Command::spriteRegion(0x0001'0000u, 0xDEADBEEFu, 8);
  CHECK_EQ(fixed.x, 0);
  CHECK_EQ(fixed.y, 1);
  CHECK_EQ(fixed.width, 8); // the size word is IGNORED for a fixed size
  CHECK_EQ(fixed.height, 8);
}

// ---- the draw-environment commands -----------------------------------------------------------------
static void test_draw_environment_commands_are_one_word_each(void) {
  // E1..E6 carry their parameters in the low halfword and never a payload, so the compositor routes on
  // membership BEFORE it can dispatch on a value.
  CHECK(Gp0Command(0xE1000000u).isDrawEnvironmentCommand());
  CHECK(Gp0Command(0xE6000000u).isDrawEnvironmentCommand());
  CHECK(!Gp0Command(0xE0000000u).isDrawEnvironmentCommand());
  CHECK(!Gp0Command(0xE7000000u).isDrawEnvironmentCommand());
  CHECK(!Gp0Command(0x20000000u).isDrawEnvironmentCommand());
  CHECK_EQ(Gp0Command::packetWordCount(0xE3000000u), 1u);
  CHECK_EQ(Gp0Command(0xE3000000u).lowHalfWord(), 0u);
  CHECK_EQ(Gp0Command(0xE312'3456u).lowHalfWord(), 0x3456u);
}

static void test_draw_area_corner_is_ten_and_nine_bits_unsigned(void) {
  // Not a vertex position: a clip corner below zero is not expressible, and clamping one at decode time
  // would silently MOVE the clip instead of refusing it.
  // Y occupies bits 10-18, so 384 is written as 384 << 10.
  const auto tl = Gp0Command(0xE3000000u | (384u << 10) | 1023u).drawAreaCorner();
  CHECK_EQ(tl.x, 1023);
  CHECK_EQ(tl.y, 384);
  // Bits past the two fields are not part of the corner.
  CHECK_EQ(Gp0Command(0xE3FF'FFFFu).drawAreaCorner().x, 1023);
  CHECK_EQ(Gp0Command(0xE3FF'FFFFu).drawAreaCorner().y, 511);
}

static void test_draw_offset_is_two_adjacent_signed_eleven_bit_axes(void) {
  // NOT one axis per halfword: the two are adjacent 11-bit fields. A negative offset is legitimate —
  // it moves the whole scene left/up — so the sign is part of the decode, not clamped away.
  const auto zero = Gp0Command(0xE5000000u).drawOffset();
  CHECK_EQ(zero.x, 0);
  CHECK_EQ(zero.y, 0);
  const auto rightDown = Gp0Command(0xE5000000u | (20u << 11) | 30u).drawOffset();
  CHECK_EQ(rightDown.x, 30);
  CHECK_EQ(rightDown.y, 20);
  const auto leftUp = Gp0Command(0xE5000000u | (0x7FFu << 11) | 0x7FFu).drawOffset();
  CHECK_EQ(leftUp.x, -1);
  CHECK_EQ(leftUp.y, -1);
  const auto farLeft = Gp0Command(0xE5000000u | (0u << 11) | 0x400u).drawOffset();
  CHECK_EQ(farLeft.x, -1024);
  // The old inline form, as the oracle.
  const auto referenceX = (static_cast<int>(0x400u & 0x7FFu) << 21) >> 21;
  CHECK_EQ(farLeft.x, referenceX);
}

static void test_texture_window_is_four_five_bit_fields(void) {
  // Four adjacent 5-bit fields, asserted against the reference shifts over a set of payloads so a decode
  // that reads the wrong field order cannot pass on one value.
  constexpr std::uint32_t payloads[] = {0x0000u, 0xFFFFu, 0x8421u, 0x1234u, 0xABCDu, 0x8000u};
  for (std::uint32_t payload : payloads) {
    const auto window = Gp0Command(0xE2000000u | payload).textureWindow();
    CHECK_EQ(window.maskX, static_cast<int>(payload & 0x1Fu));
    CHECK_EQ(window.maskY, static_cast<int>((payload >> 5) & 0x1Fu));
    CHECK_EQ(window.offsetX, static_cast<int>((payload >> 10) & 0x1Fu));
    CHECK_EQ(window.offsetY, static_cast<int>((payload >> 15) & 0x1Fu));
  }
  const auto named = Gp0Command(0xE2000000u | 0x8421u).textureWindow();
  CHECK_EQ(named.maskX, 0x1u);
  CHECK_EQ(named.maskY, 0x1u);
  CHECK_EQ(named.offsetX, 0x1u);
  CHECK_EQ(named.offsetY, 0x1u);
  // A zero window is the hardware default: no masking at all, not a mask of zero.
  const auto open = Gp0Command(0xE2000000u).textureWindow();
  CHECK_EQ(open.maskX, 0);
  CHECK_EQ(open.offsetX, 0);
}

static void test_mask_bits_are_the_two_named_hardware_bits(void) {
  // NEITHER is honoured as a pixel test. Bit 1 is a real correctness gap rather than a missing
  // feature: with check-mask on, hardware SKIPS a write where the destination is already masked, so
  // deliberately overlapping primitives composite ONCE. See docs/issues/0132-gp0-e6-check-mask-unmodelled.md.
  const auto none = Gp0Command(0xE6000000u).maskBits();
  CHECK(!none.setMask);
  CHECK(!none.checkMask);
  const auto set = Gp0Command(0xE6000001u).maskBits();
  CHECK(set.setMask);
  CHECK(!set.checkMask);
  const auto check = Gp0Command(0xE6000002u).maskBits();
  CHECK(!check.setMask);
  CHECK(check.checkMask);
  const auto both = Gp0Command(0xE6000003u).maskBits();
  CHECK(both.setMask);
  CHECK(both.checkMask);
}

static void test_nop_and_clear_cache_are_accepted_not_unimplemented(void) {
  // A discarded command is not the same as an unrecognised one: the first is honoured by doing nothing,
  // the second is a command this port does not implement. They are named apart for that reason.
  CHECK(Gp0Command(0x00000000u).isDiscardedNoOp());
  CHECK(Gp0Command(0x01000000u).isDiscardedNoOp());
  CHECK(!Gp0Command(0x02000000u).isDiscardedNoOp());
  CHECK(Gp0Command(0x00000000u).opcode() == psx::gpu::Gp0Opcode::Nop);
  CHECK(Gp0Command(0x01000000u).opcode() == psx::gpu::Gp0Opcode::ClearCache);
  CHECK(Gp0Command(0x02000000u).opcode() == psx::gpu::Gp0Opcode::FillRect);
  CHECK(Gp0Command(0x80000000u).opcode() == psx::gpu::Gp0Opcode::VramToVramCopy);
  CHECK(Gp0Command(0xA0000000u).opcode() == psx::gpu::Gp0Opcode::CpuToVramUpload);
  CHECK(Gp0Command(0xC0000000u).opcode() == psx::gpu::Gp0Opcode::VramToCpuRead);
}

static void test_vram_transfer_classification(void) {
  CHECK(Gp0Command(0x80000000u).isVramTransfer());
  CHECK(Gp0Command(0xA0000000u).isVramTransfer());
  CHECK(Gp0Command(0xC0000000u).isVramTransfer());
  CHECK(!Gp0Command(0x02000000u).isVramTransfer()); // a fill writes VRAM but is not a transfer
  CHECK(!Gp0Command(0xE1000000u).isVramTransfer());
}

static void test_texture_coordinate_word_leaves_the_selector_uninterpreted(void) {
  // The high halfword is a CLUT at vertex 0 and a texpage at vertex 1, so the WORD cannot name it. A
  // decode that called it one or the other would be right for half the vertices and wrong for the rest.
  const auto tex = Gp0Command(0x00AB'07C5u).textureCoord();
  CHECK_EQ(tex.u, 0xC5);
  CHECK_EQ(tex.v, 0x07);
  CHECK_EQ(tex.selector, 0x00AB);
}

int main(void) {
  RUN(colour_byte_order_matches_the_original_shift);
  RUN(vertex_axes_are_eleven_bit_signed);
  RUN(opcode_ranges_match_the_original_comparisons);
  RUN(polygon_flags_sit_at_the_original_bit_positions);
  RUN(polygon_vertex_count_follows_the_quad_bit);
  RUN(rectangle_size_codes_and_their_fixed_sizes);
  RUN(poly_line_is_a_line_with_the_quad_bit_set);
  RUN(packet_word_count_matches_the_original_table);
  RUN(poly_line_reports_the_fixed_line_length);
  RUN(poly_line_terminator_is_the_sentinel_word);
  RUN(poly_line_terminator_may_only_appear_at_a_vertex_start);
  RUN(fill_rect_is_sixteen_pixel_aligned_and_rounds_up);
  RUN(transfer_zero_size_means_the_whole_axis);
  RUN(rect_corner_and_draw_area_corner_use_different_shifts);
  RUN(sprite_region_uses_a_signed_vertex_position_for_its_origin);
  RUN(draw_environment_commands_are_one_word_each);
  RUN(draw_area_corner_is_ten_and_nine_bits_unsigned);
  RUN(draw_offset_is_two_adjacent_signed_eleven_bit_axes);
  RUN(texture_window_is_four_five_bit_fields);
  RUN(mask_bits_are_the_two_named_hardware_bits);
  RUN(nop_and_clear_cache_are_accepted_not_unimplemented);
  RUN(vram_transfer_classification);
  RUN(texture_coordinate_word_leaves_the_selector_uninterpreted);
  return pt_summary();
}
