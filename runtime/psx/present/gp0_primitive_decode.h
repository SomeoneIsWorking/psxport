// gp0_primitive_decode.h — a GP0 draw packet's words to a record primitive, shared by the record tap
// and by producer renders that build packets in host memory.
#pragma once

#include "frame_record.h"
#include "gp0_command.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace psx::gpu {

present::RecordVertex colourVertex(std::uint32_t colourWord);
// Sprite corners wrap at 11 bits, as gpu.c's sign extension does.
int sext11(int value);
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
// A textured polygon's texpage attribute into texture page, texture mode and blend mode.
void applyTexPageAttribute(present::RecordDrawState &state, std::uint16_t attribute);

// One polygon or sprite packet as written, before the draw offset: positions, colours, UVs, and for a
// textured polygon its texpage and CLUT attributes. A sprite's or line's texture page and blend mode are the
// draw environment's.
// Nullopt for any other command, and for a poly-line.
std::optional<present::DrawPrimitive> decodePacketPrimitive(std::span<const std::uint32_t> words);

} // namespace psx::gpu
