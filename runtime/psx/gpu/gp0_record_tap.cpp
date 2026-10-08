// gp0_record_tap.cpp — GP0 framing as gpu.c's ProcessFIFO does it, decoded into FrameRecord entries.
#include "gp0_record_tap.h"

#include "gp0_command.h"
#include "gp0_primitive_decode.h"

#include <utility>

namespace psx::gpu {
namespace {

constexpr std::uint32_t kInvalidClutTag = 0xFFFFFFFFu;

unsigned vertexWords(const Gp0Command &command) {
  const Gp0PrimitiveFlags flags = command.flags();
  return 1u + (flags.textured ? 1u : 0u) + (flags.gouraud ? 1u : 0u);
}

// Words gpu.c consumes before it dispatches a command; a quad dispatches its first triangle early.
unsigned firstDispatchWords(std::uint32_t word) {
  const Gp0Command command(word);
  const auto full = Gp0Command::packetWordCount(word);
  if (command.isPolygon() && command.polygonVertexCount() == 4) {
    return full - vertexWords(command);
  }
  return full;
}

} // namespace

Gp0RecordTap::Gp0RecordTap() : record_(nextSequence_++, true) {}

void Gp0RecordTap::resetFraming() {
  mode_ = Mode::Command;
  words_.clear();
  need_ = 0;
  uploadRemaining_ = 0;
  uploadPixels_.clear();
}

void Gp0RecordTap::desync() {
  record_.markIncomplete();
  feedback_.invalidateAll();
  synced_ = false;
  resetFraming();
}

void Gp0RecordTap::resyncIfIdle(const DeviceProbe &probe) {
  if (probe.command == DeviceCommandState::None && probe.fifoDepth == 0) {
    synced_ = true;
    resetFraming();
  }
}

void Gp0RecordTap::onGp0(std::uint32_t word,
                         std::uint32_t sourceAddress,
                         const std::optional<present::RecordKey> &packetKey,
                         const DeviceProbe &probe) {
  if (!synced_) {
    resyncIfIdle(probe);
    return;
  }
  expectedDispatches_ = 0;
  switch (mode_) {
  case Mode::Read:
    // gpu.c queues words behind an unfinished VRAM read and runs them later.
    desync();
    return;
  case Mode::Upload:
    consumeUploadWord(word);
    break;
  case Mode::PolyLine:
    consumePolyLineWord(word, probe);
    break;
  case Mode::QuadTail:
    words_.push_back(word);
    if (words_.size() == need_) {
      finishQuad(probe);
    }
    break;
  case Mode::Command:
    if (words_.empty()) {
      packetSource_ = sourceAddress;
      packetKey_ = packetKey;
      need_ = firstDispatchWords(word);
    }
    words_.push_back(word);
    if (words_.size() == need_) {
      executeCommand(probe);
    }
    break;
  }
  verify(probe);
}

void Gp0RecordTap::onDeviceStep(const DeviceProbe &probe) {
  if (!synced_) {
    resyncIfIdle(probe);
    return;
  }
  if (mode_ == Mode::Read && probe.command != DeviceCommandState::Read) {
    mode_ = Mode::Command;
  }
  expectedDispatches_ = 0;
  verify(probe);
}

void Gp0RecordTap::onSoftReset(const DeviceProbe &probe) {
  feedback_.invalidate();
  feedback_.texturePage(probe.state, probe.texDisable);
}

void Gp0RecordTap::onCommandReset(const DeviceProbe &probe) {
  if (mode_ == Mode::Upload && !uploadPixels_.empty()) {
    // The device kept the pixels it already wrote; the record cannot say which.
    record_.markIncomplete();
  }
  resetFraming();
  clutTag_ = kInvalidClutTag;
  if (!synced_) {
    resyncIfIdle(probe);
  }
}

void Gp0RecordTap::onStateReplaced(const DeviceProbe &probe) {
  record_.markIncomplete();
  feedback_.invalidateAll();
  feedback_.texturePage(probe.state, probe.texDisable);
  resetFraming();
  clutTag_ = kInvalidClutTag;
  synced_ = false;
  resyncIfIdle(probe);
}

present::FrameRecord Gp0RecordTap::seal() {
  present::FrameRecord sealed = std::move(record_);
  if (mode_ == Mode::Upload && !uploadPixels_.empty()) {
    sealed.markIncomplete();
  }
  // A quad's second triangle cannot be replayed without its first.
  record_ = present::FrameRecord(nextSequence_++, synced_ && mode_ != Mode::QuadTail);
  clutTag_ = kInvalidClutTag;
  clutOffset_ = present::kNoClut;
  lastKey_.reset();
  return sealed;
}

void Gp0RecordTap::beginPacket() {
  lastKey_.reset();
}

void Gp0RecordTap::enterSlot(const std::optional<present::OtSlot> &slot, bool descending) {
  if (slot == slot_) {
    return;
  }
  slot_ = slot;
  if (slot) {
    record_.beginSlot(*slot, descending);
  }
}

bool Gp0RecordTap::hasPendingWork() const {
  return !record_.empty() || !record_.complete();
}

std::optional<present::RecordKey> Gp0RecordTap::nextPrimitiveKey() {
  if (!packetKey_) {
    return std::nullopt;
  }
  present::RecordKey key = *packetKey_;
  if (lastKey_ && lastKey_->producer == key.producer && lastKey_->object == key.object &&
      lastKey_->element == key.element) {
    key.part = lastKey_->part + 1;
  }
  lastKey_ = key;
  return key;
}

void Gp0RecordTap::verify(const DeviceProbe &probe) {
  bool agrees = !probe.wordDropped && probe.dispatched == expectedDispatches_;
  const auto queued = static_cast<unsigned>(words_.size());
  switch (mode_) {
  case Mode::Command:
    agrees = agrees && probe.command == DeviceCommandState::None && probe.fifoDepth == queued;
    break;
  case Mode::QuadTail:
    agrees = agrees && probe.command == DeviceCommandState::Quad && probe.fifoDepth == queued;
    break;
  case Mode::PolyLine:
    agrees = agrees && probe.command == DeviceCommandState::PolyLine && probe.fifoDepth == queued;
    break;
  case Mode::Upload:
    agrees = agrees && probe.command == DeviceCommandState::Upload && probe.fifoDepth == 0;
    break;
  case Mode::Read:
    agrees = agrees && probe.command == DeviceCommandState::Read;
    break;
  }
  if (!agrees) {
    desync();
    resyncIfIdle(probe);
  }
}

std::uint32_t Gp0RecordTap::captureClut(const DeviceProbe &probe) {
  if (probe.state.texMode >= 2) {
    return present::kNoClut;
  }
  if (probe.clutTag != clutTag_ || clutOffset_ == present::kNoClut) {
    const std::size_t count = probe.state.texMode == 1 ? 256u : 16u;
    clutOffset_ = record_.appendClut(probe.clut.first(count));
    clutTag_ = probe.clutTag;
  }
  return clutOffset_;
}

void Gp0RecordTap::executeCommand(const DeviceProbe &probe) {
  const Gp0Command command(words_[0]);
  expectedDispatches_ = 1;
  feedback_.texturePage(probe.state, probe.texDisable);
  if (command.isPolygon()) {
    recordPolygon(probe);
    if (command.polygonVertexCount() == 4) {
      quadOpcode_ = words_[0];
      mode_ = Mode::QuadTail;
      need_ = vertexWords(command);
    } else {
      settleLast(probe);
    }
    words_.clear();
    return;
  }
  if (command.isLineOrPolyLine()) {
    const bool gouraud = command.flags().gouraud;
    present::RecordVertex from = colourVertex(words_[0]);
    const Gp0VertexPos p0 = Gp0Command(words_[1]).vertexPos();
    from.x = p0.x + probe.state.offsetX;
    from.y = p0.y + probe.state.offsetY;
    present::RecordVertex to = gouraud ? colourVertex(words_[2]) : from;
    const Gp0VertexPos p1 = Gp0Command(words_[gouraud ? 3 : 2]).vertexPos();
    to.x = p1.x + probe.state.offsetX;
    to.y = p1.y + probe.state.offsetY;
    recordLineSegment(from, to, gouraud, command.flags().semiTransparent, probe);
    if (command.isPolyLine()) {
      mode_ = Mode::PolyLine;
      polyLinePoint_ = to;
      polyLineOpcode_ = words_[0];
      polyLineSource_ = packetSource_;
      polyLineKey_ = packetKey_;
      need_ = gouraud ? 2u : 1u;
    }
    words_.clear();
    return;
  }
  if (command.isRectangleOrSprite()) {
    recordSprite(probe);
    words_.clear();
    return;
  }
  if (command.opcodeByte() == static_cast<std::uint8_t>(Gp0Opcode::FillRect)) {
    const Gp0VramRect rect = Gp0Command::fillRectRegion(words_[1], words_[2]);
    const Gp0Colour colour = command.colour();
    present::VramFill fill;
    fill.x = rect.x;
    fill.y = rect.y;
    fill.width = rect.width;
    fill.height = rect.height;
    fill.value =
        static_cast<std::uint16_t>((colour.red >> 3) | ((colour.green >> 3) << 5) | ((colour.blue >> 3) << 10));
    fill.skipRowParity = probe.state.skipRowParity;
    feedback_.written({fill.x, fill.y, fill.x + fill.width, fill.y + fill.height});
    record_.append(fill);
    words_.clear();
    return;
  }
  if (command.isVramCopy()) {
    const Gp0VramPos source = Gp0Command(words_[1]).rectCorner();
    const Gp0VramRect target = Gp0Command::transferRegion(words_[2], words_[3]);
    present::VramCopy copy;
    copy.srcX = source.x;
    copy.srcY = source.y;
    copy.dstX = target.x;
    copy.dstY = target.y;
    copy.width = target.width;
    copy.height = target.height;
    copy.maskSet = probe.state.maskSet;
    copy.maskCheck = probe.state.maskCheck;
    feedback_.invalidate();
    record_.append(copy);
    words_.clear();
    return;
  }
  if (command.isCpuToVramUpload()) {
    const Gp0VramRect target = Gp0Command::transferRegion(words_[1], words_[2]);
    upload_ = present::VramUpload{};
    upload_.x = target.x;
    upload_.y = target.y;
    upload_.width = target.width;
    upload_.height = target.height;
    upload_.maskSet = probe.state.maskSet;
    upload_.maskCheck = probe.state.maskCheck;
    upload_.sourceAddress = packetSource_;
    uploadRemaining_ = static_cast<std::uint32_t>(target.width) * static_cast<std::uint32_t>(target.height);
    uploadPixels_.clear();
    feedback_.invalidate();
    mode_ = Mode::Upload;
    words_.clear();
    return;
  }
  if (command.isVramToCpuRead()) {
    feedback_.invalidate();
    mode_ = probe.command == DeviceCommandState::Read ? Mode::Read : Mode::Command;
    words_.clear();
    return;
  }
  if (command.opcode() == Gp0Opcode::ClearCache) {
    clutTag_ = kInvalidClutTag;
    feedback_.invalidate();
  }
  words_.clear();
}

void Gp0RecordTap::recordPolygon(const DeviceProbe &probe) {
  const Gp0Command command(words_[0]);
  const Gp0PrimitiveFlags flags = command.flags();
  present::DrawPrimitive primitive;
  primitive.kind = present::PrimitiveKind::Polygon;
  primitive.vertexCount = 3;
  primitive.textured = flags.textured;
  primitive.gouraud = flags.gouraud;
  primitive.semiTransparent = flags.semiTransparent;
  primitive.modulate = flags.textured && !flags.rawTexel;
  primitive.state = probe.state;
  primitive.sourceAddress = packetSource_;
  std::size_t index = 0;
  for (int v = 0; v < 3; v++) {
    present::RecordVertex vertex = polygonVertex(words_, index, flags, primitive.vertices[0], v == 0);
    vertex.x += probe.state.offsetX;
    vertex.y += probe.state.offsetY;
    primitive.vertices[static_cast<std::size_t>(v)] = vertex;
  }
  if (flags.textured) {
    primitive.clutOffset = captureClut(probe);
    primitive.clutWord = static_cast<std::uint16_t>(probe.clutTag & 0x7FFFu);
  }
  primitive.key = nextPrimitiveKey();
  primitive.slot = slot_;
  record_.append(primitive);
}

void Gp0RecordTap::finishQuad(const DeviceProbe &probe) {
  const Gp0Command command(quadOpcode_);
  const Gp0PrimitiveFlags flags = command.flags();
  expectedDispatches_ = 1;
  present::RecordEntry *last = record_.last();
  auto *primitive = last != nullptr ? std::get_if<present::DrawPrimitive>(last) : nullptr;
  if (primitive != nullptr && primitive->kind == present::PrimitiveKind::Polygon && primitive->vertexCount == 3) {
    std::size_t index = 0;
    present::RecordVertex vertex = polygonVertex(words_, index, flags, primitive->vertices[0], false);
    vertex.x += probe.state.offsetX;
    vertex.y += probe.state.offsetY;
    primitive->vertices[3] = vertex;
    primitive->vertexCount = 4;
    settleLast(probe);
  }
  mode_ = Mode::Command;
  words_.clear();
}

void Gp0RecordTap::recordLineSegment(const present::RecordVertex &from,
                                     const present::RecordVertex &to,
                                     bool gouraud,
                                     bool semi,
                                     const DeviceProbe &probe) {
  present::DrawPrimitive primitive;
  primitive.kind = present::PrimitiveKind::Line;
  primitive.vertexCount = 2;
  primitive.gouraud = gouraud;
  primitive.semiTransparent = semi;
  primitive.vertices[0] = from;
  primitive.vertices[1] = to;
  primitive.state = probe.state;
  primitive.sourceAddress = packetSource_;
  primitive.key = nextPrimitiveKey();
  primitive.slot = slot_;
  record_.append(primitive);
  settleLast(probe);
}

void Gp0RecordTap::consumePolyLineWord(std::uint32_t word, const DeviceProbe &probe) {
  if (words_.empty() && Gp0Command::isPolyLineTerminator(word)) {
    mode_ = Mode::Command;
    return;
  }
  words_.push_back(word);
  if (words_.size() < need_) {
    return;
  }
  const Gp0Command command(polyLineOpcode_);
  const bool gouraud = command.flags().gouraud;
  present::RecordVertex to = gouraud ? colourVertex(words_[0]) : polyLinePoint_;
  const Gp0VertexPos position = Gp0Command(words_[gouraud ? 1 : 0]).vertexPos();
  to.x = position.x + probe.state.offsetX;
  to.y = position.y + probe.state.offsetY;
  expectedDispatches_ = 1;
  packetSource_ = polyLineSource_;
  packetKey_ = polyLineKey_;
  recordLineSegment(polyLinePoint_, to, gouraud, command.flags().semiTransparent, probe);
  polyLinePoint_ = to;
  words_.clear();
}

void Gp0RecordTap::recordSprite(const DeviceProbe &probe) {
  std::optional<present::DrawPrimitive> decoded = decodePacketPrimitive(words_);
  if (!decoded) {
    desync();
    return;
  }
  present::DrawPrimitive &primitive = *decoded;
  primitive.state = probe.state;
  primitive.sourceAddress = packetSource_;
  present::RecordVertex &vertex = primitive.vertices[0];
  vertex.x = sext11(vertex.x + probe.state.offsetX);
  vertex.y = sext11(vertex.y + probe.state.offsetY);
  if (primitive.textured) {
    primitive.flipX = probe.spriteFlipX;
    primitive.flipY = probe.spriteFlipY;
    primitive.clutOffset = captureClut(probe);
    primitive.clutWord = static_cast<std::uint16_t>(probe.clutTag & 0x7FFFu);
  }
  primitive.key = nextPrimitiveKey();
  primitive.slot = slot_;
  record_.append(primitive);
  settleLast(probe);
}

void Gp0RecordTap::settleLast(const DeviceProbe &probe) {
  present::RecordEntry *last = record_.last();
  const auto *primitive = last != nullptr ? std::get_if<present::DrawPrimitive>(last) : nullptr;
  if (primitive == nullptr || !feedback_.drawn(*primitive)) {
    return;
  }
  const RecordRect rect = drawBounds(*primitive);
  if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
    return;
  }
  present::VramUpload upload;
  upload.x = rect.x0;
  upload.y = rect.y0;
  upload.width = rect.x1 - rect.x0;
  upload.height = rect.y1 - rect.y0;
  upload.sourceAddress = primitive->sourceAddress;
  std::vector<std::uint16_t> pixels;
  pixels.reserve(static_cast<std::size_t>(upload.width) * static_cast<std::size_t>(upload.height));
  for (int y = rect.y0; y < rect.y1; y++) {
    const auto row =
        probe.vram.subspan(static_cast<std::size_t>(y) * kRecordVramWidth + static_cast<std::size_t>(rect.x0),
                           static_cast<std::size_t>(upload.width));
    pixels.insert(pixels.end(), row.begin(), row.end());
  }
  record_.dropLast();
  record_.appendUpload(upload, pixels);
}

void Gp0RecordTap::consumeUploadWord(std::uint32_t word) {
  for (int half = 0; half < 2 && uploadRemaining_ > 0; half++) {
    uploadPixels_.push_back(static_cast<std::uint16_t>(half == 0 ? word & 0xFFFFu : word >> 16));
    uploadRemaining_--;
  }
  if (uploadRemaining_ == 0) {
    record_.appendUpload(upload_, uploadPixels_);
    uploadPixels_.clear();
    mode_ = Mode::Command;
  }
}

} // namespace psx::gpu
