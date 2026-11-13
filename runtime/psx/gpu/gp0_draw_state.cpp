// gp0_draw_state.cpp — gpu.c's draw environment commands (Command_DrawMode .. Command_MaskSetting, SetTPage).
#include "gp0_draw_state.h"

#include "gp0_command.h"
#include "gp0_primitive_decode.h"

#include <lucent/log.h>

#include <algorithm>
#include <optional>

namespace psx::gpu {
namespace {

constexpr std::uint32_t kDrawToDisplayBit = 1u << 10;
constexpr std::uint32_t kTexDisableBit = 1u << 11;
constexpr std::uint32_t kSpriteFlipXBit = 1u << 12;
constexpr std::uint32_t kSpriteFlipYBit = 1u << 13;
// gpu.c keeps TexMode 3 and draws it as 15-bit.
constexpr int kDirectTexMode = 2;

bool decodePacket(const CapturedPacket &packet,
                  std::span<const std::uint32_t> words,
                  std::span<const gte::WordPlace> places,
                  Gp0DrawState &state,
                  std::vector<present::RecordEntry> &into) {
  std::size_t at = 0;
  while (at < words.size()) {
    const Gp0Command head(words[at]);
    const std::span<const std::uint32_t> rest = words.subspan(at);
    const std::span<const gte::WordPlace> restPlaces = places.size() > at ? places.subspan(at) : places.last(0);
    const auto source = static_cast<std::uint32_t>(packet.address + 4u * (at + 1u));
    if (head.isPolyLine()) {
      return decodePolyLine({.words = rest, .places = restPlaces}, [&](present::DrawPrimitive segment) {
        state.apply(segment);
        segment.sourceAddress = source;
        segment.packet = packet.address;
        segment.site = packet.site;
        into.emplace_back(segment);
      });
    }
    const std::size_t count = Gp0Command::packetWordCount(words[at]);
    if (count > rest.size()) {
      return false;
    }
    const std::span<const std::uint32_t> command = rest.first(count);
    if (head.isDrawEnvironmentCommand()) {
      state.execute(command);
    } else if (head.isDrawable()) {
      std::optional<present::DrawPrimitive> primitive =
          decodePacketPrimitive({.words = command, .places = restPlaces.first(std::min(count, restPlaces.size()))});
      if (!primitive) {
        return false;
      }
      state.execute(command);
      state.apply(*primitive);
      primitive->sourceAddress = source;
      primitive->packet = packet.address;
      primitive->site = packet.site;
      into.emplace_back(*primitive);
    } else if (head.opcode() == Gp0Opcode::FillRect) {
      into.emplace_back(decodeFill(command, state.state()));
    } else if (head.isVramCopy()) {
      into.emplace_back(decodeCopy({.words = command, .places = restPlaces.first(std::min(count, restPlaces.size()))},
                                   state.state()));
    } else if (!head.isDiscardedNoOp()) {
      return false;
    }
    at += count;
  }
  return true;
}

} // namespace

std::string describe(const Gp0DrawRegisters &registers) {
  const present::RecordDrawState &s = registers.state;
  return lucent::format("area ({},{})-({},{}) offset ({},{}) page ({},{}) mode {} blend {} window {}/{} {}/{} "
                        "mask set {} check {} skip {} texDisable {}/{} flip {}{} dfe {} displaySkip {}",
                        s.clipX0,
                        s.clipY0,
                        s.clipX1,
                        s.clipY1,
                        s.offsetX,
                        s.offsetY,
                        s.texPageX,
                        s.texPageY,
                        s.texMode,
                        s.blendMode,
                        s.windowMaskX,
                        s.windowMaskY,
                        s.windowOffsetX,
                        s.windowOffsetY,
                        s.maskSet,
                        s.maskCheck,
                        s.skipRowParity,
                        registers.texDisable,
                        registers.texDisableAllowed,
                        registers.spriteFlipX ? "x" : "-",
                        registers.spriteFlipY ? "y" : "-",
                        registers.drawToDisplay,
                        registers.displaySkipParity);
}

Gp0DrawState::Gp0DrawState(const Gp0DrawRegisters &seed) : registers_(seed) {}

Gp0DrawState::Gp0DrawState(const present::RecordDrawState &atStart) {
  registers_.state = atStart;
  registers_.displaySkipParity = atStart.skipRowParity;
}

void Gp0DrawState::execute(std::span<const std::uint32_t> command) {
  if (command.empty()) {
    return;
  }
  const Gp0Command head(command[0]);
  present::RecordDrawState &state = registers_.state;
  switch (head.opcode()) {
  case Gp0Opcode::SetDrawMode:
    setTexPage(head.lowHalfWord());
    registers_.spriteFlipX = (command[0] & kSpriteFlipXBit) != 0;
    registers_.spriteFlipY = (command[0] & kSpriteFlipYBit) != 0;
    registers_.drawToDisplay = (command[0] & kDrawToDisplayBit) != 0;
    updateSkipParity();
    return;
  case Gp0Opcode::SetTextureWindow: {
    const Gp0TextureWindow window = head.textureWindow();
    state.windowMaskX = window.maskX;
    state.windowMaskY = window.maskY;
    state.windowOffsetX = window.offsetX;
    state.windowOffsetY = window.offsetY;
    return;
  }
  case Gp0Opcode::SetDrawAreaTopLeft: {
    const Gp0VramPos corner = head.drawAreaCorner();
    state.clipX0 = corner.x;
    state.clipY0 = corner.y;
    return;
  }
  case Gp0Opcode::SetDrawAreaBottomRight: {
    const Gp0VramPos corner = head.drawAreaCorner();
    state.clipX1 = corner.x;
    state.clipY1 = corner.y;
    return;
  }
  case Gp0Opcode::SetDrawOffset: {
    const Gp0VertexPos offset = head.drawOffset();
    state.offsetX = offset.x;
    state.offsetY = offset.y;
    return;
  }
  case Gp0Opcode::SetMaskBits: {
    const Gp0MaskBits mask = head.maskBits();
    state.maskSet = mask.setMask;
    state.maskCheck = mask.checkMask;
    return;
  }
  default:
    break;
  }
  if (head.isPolygon() && head.flags().textured) {
    setTexPage(polygonTexture(command).texPage);
  }
}

void Gp0DrawState::softReset() {
  registers_ = Gp0DrawRegisters{};
}

void Gp0DrawState::setControl(bool texDisableAllowed, int displaySkipParity) {
  registers_.texDisableAllowed = texDisableAllowed;
  registers_.displaySkipParity = displaySkipParity;
  updateSkipParity();
}

void Gp0DrawState::offset(present::RecordVertex &vertex) const {
  vertex.x += registers_.state.offsetX;
  vertex.y += registers_.state.offsetY;
}

void Gp0DrawState::apply(present::DrawPrimitive &primitive) const {
  primitive.state = registers_.state;
  if (primitive.kind != present::PrimitiveKind::Sprite) {
    for (int v = 0; v < primitive.vertexCount; v++) {
      offset(primitive.vertices[static_cast<std::size_t>(v)]);
    }
    return;
  }
  present::RecordVertex &vertex = primitive.vertices[0];
  vertex.x = Gp0Command::signedAxis(static_cast<std::uint32_t>(vertex.x + registers_.state.offsetX));
  vertex.y = Gp0Command::signedAxis(static_cast<std::uint32_t>(vertex.y + registers_.state.offsetY));
  if (primitive.textured) {
    primitive.flipX = registers_.spriteFlipX;
    primitive.flipY = registers_.spriteFlipY;
  }
}

void Gp0DrawState::setTexPage(std::uint16_t attribute) {
  applyTexPageAttribute(registers_.state, attribute);
  registers_.state.texMode = std::min(registers_.state.texMode, kDirectTexMode);
  if (registers_.texDisableAllowed) {
    registers_.texDisable = (attribute & kTexDisableBit) != 0;
  }
}

void Gp0DrawState::updateSkipParity() {
  registers_.state.skipRowParity = registers_.drawToDisplay ? -1 : registers_.displaySkipParity;
}

bool decodePackets(std::span<const std::uint32_t> words,
                   std::span<const gte::WordPlace> places,
                   std::span<const CapturedPacket> packets,
                   const present::RecordDrawState &start,
                   std::vector<present::RecordEntry> &into) {
  Gp0DrawState state(start);
  for (const CapturedPacket &packet : packets) {
    const std::span<const gte::WordPlace> packetPlaces =
        places.empty() ? places : places.subspan(packet.offset, packet.count);
    if (!decodePacket(packet, words.subspan(packet.offset, packet.count), packetPlaces, state, into)) {
      return false;
    }
  }
  return true;
}

} // namespace psx::gpu
