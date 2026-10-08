// gp0_primitive_decode.cpp — GP0 draw packet decoding.
#include "gp0_primitive_decode.h"

namespace psx::gpu {

present::RecordVertex colourVertex(std::uint32_t colourWord) {
  const Gp0Colour colour = Gp0Command(colourWord).colour();
  present::RecordVertex vertex;
  vertex.r = colour.red;
  vertex.g = colour.green;
  vertex.b = colour.blue;
  return vertex;
}

int sext11(int value) {
  return ((value & 0x7FF) ^ 0x400) - 0x400;
}

present::RecordVertex polygonVertex(std::span<const std::uint32_t> words,
                                    std::size_t &index,
                                    const Gp0PrimitiveFlags &flags,
                                    const present::RecordVertex &first,
                                    bool isFirst) {
  present::RecordVertex vertex = (isFirst || flags.gouraud) ? colourVertex(words[index++]) : first;
  const Gp0VertexPos position = Gp0Command(words[index++]).vertexPos();
  vertex.x = position.x;
  vertex.y = position.y;
  if (flags.textured) {
    const Gp0TextureCoord coord = Gp0Command(words[index++]).textureCoord();
    vertex.u = static_cast<std::uint8_t>(coord.u);
    vertex.v = static_cast<std::uint8_t>(coord.v);
  }
  return vertex;
}

void applyTexPageAttribute(present::RecordDrawState &state, std::uint16_t attribute) {
  state.texPageX = (attribute & 0xF) * 64;
  state.texPageY = ((attribute >> 4) & 1) * 256;
  state.blendMode = (attribute >> 5) & 3;
  state.texMode = (attribute >> 7) & 3;
}

std::optional<present::DrawPrimitive> decodePacketPrimitive(std::span<const std::uint32_t> words) {
  if (words.empty() || words.size() < Gp0Command::packetWordCount(words[0])) {
    return std::nullopt;
  }
  const Gp0Command command(words[0]);
  const Gp0PrimitiveFlags flags = command.flags();
  present::DrawPrimitive primitive;
  primitive.textured = flags.textured;
  primitive.gouraud = flags.gouraud;
  primitive.semiTransparent = flags.semiTransparent;
  primitive.modulate = flags.textured && !flags.rawTexel;
  if (command.isPolygon()) {
    primitive.kind = present::PrimitiveKind::Polygon;
    primitive.vertexCount = command.polygonVertexCount();
    std::size_t index = 0;
    for (int v = 0; v < primitive.vertexCount; v++) {
      primitive.vertices[static_cast<std::size_t>(v)] =
          polygonVertex(words, index, flags, primitive.vertices[0], v == 0);
    }
    if (flags.textured) {
      // Words: command, xy0, uv0 + CLUT, [colour1], xy1, uv1 + texpage.
      const std::size_t uv0 = 2u;
      const std::size_t stride = flags.gouraud ? 3u : 2u;
      primitive.clutWord = Gp0Command(words[uv0]).textureCoord().selector & 0x7FFFu;
      applyTexPageAttribute(primitive.state, Gp0Command(words[uv0 + stride]).textureCoord().selector);
    }
    return primitive;
  }
  if (command.isRectangleOrSprite()) {
    primitive.kind = present::PrimitiveKind::Sprite;
    primitive.vertexCount = 1;
    present::RecordVertex vertex = colourVertex(words[0]);
    const Gp0VertexPos position = Gp0Command(words[1]).vertexPos();
    vertex.x = position.x;
    vertex.y = position.y;
    std::size_t index = 2;
    if (flags.textured) {
      const Gp0TextureCoord coord = Gp0Command(words[index++]).textureCoord();
      vertex.u = static_cast<std::uint8_t>(coord.u);
      vertex.v = static_cast<std::uint8_t>(coord.v);
      primitive.clutWord = coord.selector & 0x7FFFu;
    }
    const int fixedSize = command.rectangleFixedSize();
    const Gp0VramRect region = Gp0Command::spriteRegion(words[1], fixedSize == 0 ? words[index] : 0u, fixedSize);
    primitive.width = region.width;
    primitive.height = region.height;
    primitive.vertices[0] = vertex;
    return primitive;
  }
  return std::nullopt;
}

} // namespace psx::gpu
