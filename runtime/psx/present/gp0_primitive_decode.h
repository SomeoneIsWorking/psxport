// gp0_primitive_decode.h — a GP0 draw packet's words to a record primitive, shared by the record tap
// and by producer renders that build packets in host memory.
#pragma once

#include "frame_record.h"
#include "gp0_command.h"
#include "projected_vertex.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace psx::gpu {

present::RecordVertex colourVertex(std::uint32_t colourWord);
// The polygon vertex at `words[index]`, advancing `index`; a flat polygon's later vertices take
// `first`'s colour. Position is as written, before the draw offset.
present::RecordVertex polygonVertex(std::span<const std::uint32_t> words,
                                    std::size_t &index,
                                    const Gp0PrimitiveFlags &flags,
                                    const present::RecordVertex &first,
                                    bool isFirst);
// The two ends of a line packet (not a poly-line), position as written, before the draw offset; a flat line's
// ends share the first colour.
void decodeLineEnds(std::span<const std::uint32_t> words, present::RecordVertex &from, present::RecordVertex &to);
// A texpage attribute into texture page, texture mode and blend mode.
void applyTexPageAttribute(present::RecordDrawState &state, std::uint16_t attribute);

// A packet's words, and the place of the vertex stored into each: one per word, or none at all for words that
// carry no place (the guest's GP0 stream).
struct PacketView {
  std::span<const std::uint32_t> words;
  std::span<const gte::WordPlace> places;
};

// A packet a producer builds in its own storage, keeping the place of each vertex stored into it.
template <std::size_t Count> class PacketBuffer {
public:
  PacketBuffer() = default;
  // `words` copied in, none of them a placed vertex.
  explicit PacketBuffer(std::span<const std::uint32_t, Count> words) {
    std::ranges::copy(words, words_.begin());
  }
  void store(std::size_t index, std::uint32_t word) {
    words_[index] = word;
    places_[index].reset();
  }
  void storeVertex(std::size_t index, const gte::ProjectedVertex &vertex) {
    words_[index] = vertex.word();
    places_[index] = vertex.place();
  }
  [[nodiscard]] std::uint32_t word(std::size_t index) const {
    return words_[index];
  }
  [[nodiscard]] PacketView view() const {
    return {words_, places_};
  }

private:
  std::array<std::uint32_t, Count> words_{};
  std::array<gte::WordPlace, Count> places_{};
};

// The vertex of a poly-line point after `previous`: its colour from the word at `colourWord` (a gouraud line's),
// or `previous`'s, and its position and place from the word at `positionWord`.
present::RecordVertex polyLinePoint(PacketView packet,
                                    std::size_t colourWord,
                                    std::size_t positionWord,
                                    const present::RecordVertex &previous);
// The first two points of a line packet with their places.
void decodePlacedLineEnds(PacketView packet, present::RecordVertex &from, present::RecordVertex &to);
// A line primitive of `command` from `from` to `to`, as the record tap records each segment.
present::DrawPrimitive
lineSegment(const Gp0Command &command, const present::RecordVertex &from, const present::RecordVertex &to);

// Each segment of a poly-line packet as written, before the draw offset, first to last, through
// `segment(DrawPrimitive)`; false, having emitted the segments before it, when the packet ends before its
// terminator.
template <class Segment> bool decodePolyLine(PacketView packet, Segment &&segment) {
  const std::span<const std::uint32_t> words = packet.words;
  const Gp0Command command(words[0]);
  const bool gouraud = command.flags().gouraud;
  const std::size_t stride = gouraud ? 2u : 1u;
  const std::size_t third = gouraud ? 4u : 3u; // the word after the first two points
  if (words.size() < third) {
    return false;
  }
  present::RecordVertex from;
  present::RecordVertex to;
  decodePlacedLineEnds(packet, from, to);
  segment(lineSegment(command, from, to));
  for (std::size_t index = third; index < words.size(); index += stride) {
    if (Gp0Command::isPolyLineTerminator(words[index])) {
      return true;
    }
    if (index + stride > words.size()) {
      return false;
    }
    const present::RecordVertex next = polyLinePoint(packet, index, index + stride - 1u, to);
    segment(lineSegment(command, to, next));
    to = next;
  }
  return false;
}

// One polygon or sprite packet as written, before the draw offset: positions, colours, UVs, and for a
// textured polygon its texpage and CLUT attributes. A sprite's or line's texture page and blend mode are the
// draw environment's. A vertex stored with a place lies that far past its whole pixel (subX/subY).
// Nullopt for any other command, and for a poly-line.
std::optional<present::DrawPrimitive> decodePacketPrimitive(PacketView packet);

} // namespace psx::gpu

namespace psx::present {

// The draw mode GP0(E1) packets set, in submission order. A textured polygon carries its own texpage;
// every other primitive is drawn under the last mode set.
class DrawModeTracker {
public:
  // True when `words` is a GP0(E1) packet, which then sets the mode.
  bool take(std::span<const std::uint32_t> words);
  void apply(DrawPrimitive &primitive) const;

private:
  std::uint16_t attribute_ = 0;
};

} // namespace psx::present
