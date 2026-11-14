// frame_record.cpp — FrameRecord storage.
#include "frame_record.h"

#include <lucent/log.h>

#include <algorithm>
#include <bit>
#include <utility>

namespace psx::present {

std::span<const std::uint16_t> FrameRecord::clut(const DrawPrimitive &primitive) const {
  if (primitive.clutOffset == kNoClut) {
    return {};
  }
  const std::size_t count = primitive.state.texMode == 0 ? 16u : 256u;
  if (primitive.clutOffset + count > clutPool_.size()) {
    return {};
  }
  return std::span<const std::uint16_t>(clutPool_).subspan(primitive.clutOffset, count);
}

std::span<const std::uint16_t> FrameRecord::pixels(const VramUpload &upload) const {
  const std::size_t count = static_cast<std::size_t>(upload.width) * static_cast<std::size_t>(upload.height);
  if (upload.pixelOffset + count > uploadPixels_.size()) {
    return {};
  }
  return std::span<const std::uint16_t>(uploadPixels_).subspan(upload.pixelOffset, count);
}

void FrameRecord::reset(std::uint64_t sequence, bool complete) {
  sequence_ = sequence;
  complete_ = complete;
  entries_.clear();
  clutPool_.clear();
  uploadPixels_.clear();
  slotStarts_.clear();
  tableExits_.clear();
  chainRuns_.clear();
  drawEntryField_.reset();
  windows_ = {};
}

void FrameRecord::append(const RecordEntry &entry) {
  if (!complete_) {
    return;
  }
  if (entries_.size() >= kMaxEntries) {
    lucent::warn("record", "record {} reached {} entries; it is marked incomplete", sequence_, kMaxEntries);
    markIncomplete();
    return;
  }
  entries_.push_back(entry);
}

std::optional<int> FrameRecord::undrawnRowParity() const {
  for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
    int parity = -1;
    if (const auto *primitive = std::get_if<DrawPrimitive>(&*entry)) {
      parity = primitive->state.skipRowParity;
    } else if (const auto *fill = std::get_if<VramFill>(&*entry)) {
      parity = fill->skipRowParity;
    } else {
      continue;
    }
    return parity >= 0 ? std::optional<int>(parity) : std::nullopt;
  }
  return std::nullopt;
}

RecordEntry *FrameRecord::last() {
  return entries_.empty() ? nullptr : &entries_.back();
}

void FrameRecord::dropLast() {
  if (!entries_.empty()) {
    entries_.pop_back();
  }
}

std::uint32_t FrameRecord::appendClut(std::span<const std::uint16_t> entries) {
  if (!complete_) {
    return kNoClut;
  }
  if (clutPool_.size() + entries.size() > kMaxPoolHalfwords) {
    lucent::warn(
        "record", "record {} CLUT pool reached {} halfwords; it is marked incomplete", sequence_, kMaxPoolHalfwords);
    markIncomplete();
    return kNoClut;
  }
  const auto offset = static_cast<std::uint32_t>(clutPool_.size());
  clutPool_.insert(clutPool_.end(), entries.begin(), entries.end());
  return offset;
}

void FrameRecord::appendUpload(VramUpload upload, std::span<const std::uint16_t> pixels) {
  if (!complete_) {
    return;
  }
  if (uploadPixels_.size() + pixels.size() > kMaxPoolHalfwords) {
    lucent::warn(
        "record", "record {} upload pool reached {} halfwords; it is marked incomplete", sequence_, kMaxPoolHalfwords);
    markIncomplete();
    return;
  }
  upload.pixelOffset = static_cast<std::uint32_t>(uploadPixels_.size());
  uploadPixels_.insert(uploadPixels_.end(), pixels.begin(), pixels.end());
  append(upload);
}

void FrameRecord::markIncomplete() {
  if (!complete_) {
    return;
  }
  complete_ = false;
  // An incomplete record is never replayed, so its contents need not be kept.
  entries_.clear();
  entries_.shrink_to_fit();
  clutPool_.clear();
  clutPool_.shrink_to_fit();
  uploadPixels_.clear();
  uploadPixels_.shrink_to_fit();
  slotStarts_.clear();
  slotStarts_.shrink_to_fit();
  tableExits_.clear();
  tableExits_.shrink_to_fit();
  chainRuns_.clear();
  chainRuns_.shrink_to_fit();
}

void FrameRecord::beginSlot(OtSlot slot, bool descending, const RecordDrawState &state) {
  if (!complete_) {
    return;
  }
  slotStarts_.push_back({slot, entries_.size(), descending, state});
}

void FrameRecord::exitTable(std::uint16_t table, const RecordDrawState &state) {
  if (!complete_) {
    return;
  }
  tableExits_.push_back({table, entries_.size(), state});
}

void FrameRecord::beginChain(std::uint32_t head, const RecordDrawState &atStart) {
  if (!complete_) {
    return;
  }
  chainRuns_.push_back({head, entries_.size(), entries_.size(), atStart});
}

void FrameRecord::endChain() {
  if (!complete_ || chainRuns_.empty()) {
    return;
  }
  chainRuns_.back().endEntry = entries_.size();
}

void FrameRecord::splice(const FrameRecord &base, std::span<ChainSplice> splices) {
  assignPools(base);
  drawEntryField_ = base.drawEntryField_;
  const std::span<const RecordEntry> from = base.entries();
  std::size_t kept = 0;
  for (ChainSplice &splice : splices) {
    entries_.insert(entries_.end(),
                    from.begin() + static_cast<std::ptrdiff_t>(kept),
                    from.begin() + static_cast<std::ptrdiff_t>(splice.run.firstEntry));
    splice.at = entries_.size();
    entries_.insert(entries_.end(), splice.walked.begin(), splice.walked.end());
    kept = splice.run.endEntry;
  }
  entries_.insert(entries_.end(), from.begin() + static_cast<std::ptrdiff_t>(kept), from.end());
}

void FrameRecord::replaceEntries(std::span<const RecordEntry> entries) {
  if (!complete_) {
    return;
  }
  entries_.assign(entries.begin(), entries.end());
  slotStarts_.clear();
  tableExits_.clear();
  chainRuns_.clear();
}

void FrameRecord::assignPools(const FrameRecord &other) {
  sequence_ = other.sequence_;
  complete_ = other.complete_;
  windows_ = other.windows_;
  entries_.clear();
  clutPool_.assign(other.clutPool_.begin(), other.clutPool_.end());
  uploadPixels_.assign(other.uploadPixels_.begin(), other.uploadPixels_.end());
  slotStarts_.clear();
  tableExits_.clear();
  chainRuns_.clear();
}

namespace {

using psx::platform::BlobReader;
using psx::platform::BlobWriter;

void writeFloat(BlobWriter &out, float value) {
  out.u32(std::bit_cast<std::uint32_t>(value));
}

float readFloat(BlobReader &in) {
  return std::bit_cast<float>(in.u32());
}

void writeSlot(BlobWriter &out, const std::optional<OtSlot> &slot) {
  out.boolean(slot.has_value());
  out.u16(slot ? slot->table : 0);
  out.u32(slot ? slot->index : 0);
}

std::optional<OtSlot> readSlot(BlobReader &in) {
  const bool present = in.boolean();
  const OtSlot slot{.table = in.u16(), .index = in.u32()};
  return present ? std::optional(slot) : std::nullopt;
}

void writeState(BlobWriter &out, const RecordDrawState &state) {
  for (const int value : {state.clipX0,
                          state.clipY0,
                          state.clipX1,
                          state.clipY1,
                          state.offsetX,
                          state.offsetY,
                          state.texPageX,
                          state.texPageY,
                          state.texMode,
                          state.blendMode,
                          state.windowMaskX,
                          state.windowMaskY,
                          state.windowOffsetX,
                          state.windowOffsetY,
                          state.skipRowParity}) {
    out.i32(value);
  }
  out.boolean(state.maskSet);
  out.boolean(state.maskCheck);
}

RecordDrawState readState(BlobReader &in) {
  RecordDrawState state;
  for (int *value : {&state.clipX0,
                     &state.clipY0,
                     &state.clipX1,
                     &state.clipY1,
                     &state.offsetX,
                     &state.offsetY,
                     &state.texPageX,
                     &state.texPageY,
                     &state.texMode,
                     &state.blendMode,
                     &state.windowMaskX,
                     &state.windowMaskY,
                     &state.windowOffsetX,
                     &state.windowOffsetY,
                     &state.skipRowParity}) {
    *value = in.i32();
  }
  state.maskSet = in.boolean();
  state.maskCheck = in.boolean();
  return state;
}

void writeVertex(BlobWriter &out, const RecordVertex &vertex) {
  out.i32(vertex.x);
  out.i32(vertex.y);
  for (const std::uint8_t value : {vertex.r, vertex.g, vertex.b, vertex.u, vertex.v}) {
    out.u8(value);
  }
  for (const float value : {vertex.subX, vertex.subY, vertex.depth, vertex.back}) {
    writeFloat(out, value);
  }
  out.u32(vertex.model);
}

RecordVertex readVertex(BlobReader &in) {
  RecordVertex vertex;
  vertex.x = in.i32();
  vertex.y = in.i32();
  for (std::uint8_t *value : {&vertex.r, &vertex.g, &vertex.b, &vertex.u, &vertex.v}) {
    *value = in.u8();
  }
  for (float *value : {&vertex.subX, &vertex.subY, &vertex.depth, &vertex.back}) {
    *value = readFloat(in);
  }
  vertex.model = in.u32();
  return vertex;
}

void writeEntry(BlobWriter &out, const DrawPrimitive &primitive) {
  out.u8(static_cast<std::uint8_t>(primitive.kind));
  out.i32(primitive.vertexCount);
  for (const bool flag : {primitive.textured,
                          primitive.gouraud,
                          primitive.semiTransparent,
                          primitive.modulate,
                          primitive.flipX,
                          primitive.flipY,
                          primitive.ui}) {
    out.boolean(flag);
  }
  out.i32(primitive.width);
  out.i32(primitive.height);
  for (const RecordVertex &vertex : primitive.vertices) {
    writeVertex(out, vertex);
  }
  out.u32(primitive.clutOffset);
  out.u16(primitive.clutWord);
  writeState(out, primitive.state);
  out.u32(primitive.sourceAddress);
  out.u32(primitive.packet);
  out.u32(primitive.site);
  writeSlot(out, primitive.slot);
  out.u32(primitive.owner);
  out.u8(primitive.command);
  out.boolean(primitive.outlineFont.has_value());
  out.u8(primitive.outlineFont.value_or(0));
}

DrawPrimitive readPrimitive(BlobReader &in) {
  DrawPrimitive primitive;
  primitive.kind = static_cast<PrimitiveKind>(in.u8());
  primitive.vertexCount = in.i32();
  for (bool *flag : {&primitive.textured,
                     &primitive.gouraud,
                     &primitive.semiTransparent,
                     &primitive.modulate,
                     &primitive.flipX,
                     &primitive.flipY,
                     &primitive.ui}) {
    *flag = in.boolean();
  }
  primitive.width = in.i32();
  primitive.height = in.i32();
  for (RecordVertex &vertex : primitive.vertices) {
    vertex = readVertex(in);
  }
  primitive.clutOffset = in.u32();
  primitive.clutWord = in.u16();
  primitive.state = readState(in);
  primitive.sourceAddress = in.u32();
  primitive.packet = in.u32();
  primitive.site = in.u32();
  primitive.slot = readSlot(in);
  primitive.owner = in.u32();
  primitive.command = in.u8();
  const bool outlined = in.boolean();
  const std::uint8_t outlineFont = in.u8();
  primitive.outlineFont = outlined ? std::optional(outlineFont) : std::nullopt;
  return primitive;
}

void writeEntry(BlobWriter &out, const VramFill &fill) {
  for (const int value : {fill.x, fill.y, fill.width, fill.height, fill.skipRowParity}) {
    out.i32(value);
  }
  out.u16(fill.value);
}

VramFill readFill(BlobReader &in) {
  VramFill fill;
  for (int *value : {&fill.x, &fill.y, &fill.width, &fill.height, &fill.skipRowParity}) {
    *value = in.i32();
  }
  fill.value = in.u16();
  return fill;
}

void writeEntry(BlobWriter &out, const VramCopy &copy) {
  for (const int value : {copy.srcX, copy.srcY, copy.dstX, copy.dstY, copy.width, copy.height}) {
    out.i32(value);
  }
  out.boolean(copy.maskSet);
  out.boolean(copy.maskCheck);
  for (const float value :
       {copy.place.srcX, copy.place.srcY, copy.place.dstX, copy.place.dstY, copy.place.width, copy.place.height}) {
    writeFloat(out, value);
  }
  writeSlot(out, copy.slot);
}

VramCopy readCopy(BlobReader &in) {
  VramCopy copy;
  for (int *value : {&copy.srcX, &copy.srcY, &copy.dstX, &copy.dstY, &copy.width, &copy.height}) {
    *value = in.i32();
  }
  copy.maskSet = in.boolean();
  copy.maskCheck = in.boolean();
  for (float *value : {&copy.place.srcX,
                       &copy.place.srcY,
                       &copy.place.dstX,
                       &copy.place.dstY,
                       &copy.place.width,
                       &copy.place.height}) {
    *value = readFloat(in);
  }
  copy.slot = readSlot(in);
  return copy;
}

void writeEntry(BlobWriter &out, const VramUpload &upload) {
  for (const int value : {upload.x, upload.y, upload.width, upload.height}) {
    out.i32(value);
  }
  out.boolean(upload.maskSet);
  out.boolean(upload.maskCheck);
  out.u32(upload.pixelOffset);
  out.u32(upload.sourceAddress);
}

VramUpload readUpload(BlobReader &in) {
  VramUpload upload;
  for (int *value : {&upload.x, &upload.y, &upload.width, &upload.height}) {
    *value = in.i32();
  }
  upload.maskSet = in.boolean();
  upload.maskCheck = in.boolean();
  upload.pixelOffset = in.u32();
  upload.sourceAddress = in.u32();
  return upload;
}

void writeEntry(BlobWriter &out, const ReadBackTexture &texture) {
  for (const int value : {texture.readX,
                          texture.readY,
                          texture.readWidth,
                          texture.readHeight,
                          texture.pageX,
                          texture.pageY,
                          texture.pageWidth,
                          texture.pageHeight,
                          texture.texMode}) {
    out.i32(value);
  }
  for (const std::uint8_t value : {texture.red, texture.green, texture.blue, texture.shift}) {
    out.u8(value);
  }
}

ReadBackTexture readReadBack(BlobReader &in) {
  ReadBackTexture texture;
  for (int *value : {&texture.readX,
                     &texture.readY,
                     &texture.readWidth,
                     &texture.readHeight,
                     &texture.pageX,
                     &texture.pageY,
                     &texture.pageWidth,
                     &texture.pageHeight,
                     &texture.texMode}) {
    *value = in.i32();
  }
  for (std::uint8_t *value : {&texture.red, &texture.green, &texture.blue, &texture.shift}) {
    *value = in.u8();
  }
  return texture;
}

std::optional<RecordEntry> readEntry(BlobReader &in) {
  switch (in.u8()) {
  case 0:
    return readPrimitive(in);
  case 1:
    return readFill(in);
  case 2:
    return readCopy(in);
  case 3:
    return readUpload(in);
  case 4:
    return readReadBack(in);
  default:
    return std::nullopt;
  }
}

void writeMargins(BlobWriter &out, HorizontalMargins margins) {
  out.i32(margins.left);
  out.i32(margins.right);
}

HorizontalMargins readMargins(BlobReader &in) {
  const int left = in.i32();
  const int right = in.i32();
  return {left, right};
}

// A count the reader can hold: within `limit` and no larger than the bytes left, each item taking at least one.
bool readCount(BlobReader &in, std::size_t limit, std::size_t &count) {
  count = in.u32();
  return in.ok() && count <= limit && count <= in.size() - in.offset();
}

} // namespace

void FrameRecord::save(BlobWriter &out) const {
  out.u64(sequence_);
  out.boolean(complete_);
  out.boolean(drawEntryField_.has_value());
  out.u32(drawEntryField_.value_or(0));
  writeMargins(out, windows_.view);
  writeMargins(out, windows_.drawn);
  out.u32(static_cast<std::uint32_t>(entries_.size()));
  for (const RecordEntry &entry : entries_) {
    out.u8(static_cast<std::uint8_t>(entry.index()));
    std::visit(
        [&out](const auto &value) {
          writeEntry(out, value);
        },
        entry);
  }
  for (const std::vector<std::uint16_t> *pool : {&clutPool_, &uploadPixels_}) {
    out.u32(static_cast<std::uint32_t>(pool->size()));
    out.array(pool->data(), pool->size());
  }
  out.u32(static_cast<std::uint32_t>(slotStarts_.size()));
  for (const SlotStart &start : slotStarts_) {
    writeSlot(out, start.slot);
    out.u64(start.entry);
    out.boolean(start.descending);
    writeState(out, start.state);
  }
  out.u32(static_cast<std::uint32_t>(tableExits_.size()));
  for (const TableExit &exit : tableExits_) {
    out.u16(exit.table);
    out.u64(exit.entry);
    writeState(out, exit.state);
  }
  out.u32(static_cast<std::uint32_t>(chainRuns_.size()));
  for (const ChainRun &run : chainRuns_) {
    out.u32(run.head);
    out.u64(run.firstEntry);
    out.u64(run.endEntry);
    writeState(out, run.atStart);
  }
}

bool FrameRecord::load(BlobReader &in, std::string &error) {
  reset(0, true);
  const auto refuse = [this, &error](std::string reason) {
    reset(0, true);
    error = "frame record: " + std::move(reason);
    return false;
  };
  sequence_ = in.u64();
  complete_ = in.boolean();
  const bool hasEntry = in.boolean();
  const std::uint32_t entryField = in.u32();
  drawEntryField_ = hasEntry ? std::optional(entryField) : std::nullopt;
  windows_.view = readMargins(in);
  windows_.drawn = readMargins(in);
  std::size_t count = 0;
  if (!readCount(in, kMaxEntries, count)) {
    return refuse("its entry count does not decode");
  }
  entries_.reserve(count);
  for (std::size_t i = 0; i < count; i++) {
    const std::optional<RecordEntry> entry = readEntry(in);
    if (!entry || !in.ok()) {
      return refuse("entry " + std::to_string(i) + " of " + std::to_string(count) + " does not decode");
    }
    entries_.push_back(*entry);
  }
  for (std::vector<std::uint16_t> *pool : {&clutPool_, &uploadPixels_}) {
    if (!readCount(in, kMaxPoolHalfwords, count)) {
      return refuse("a pool's size does not decode");
    }
    pool->resize(count);
    if (!in.array(pool->data(), pool->size())) {
      return refuse("a pool's halfwords do not decode");
    }
  }
  if (!readCount(in, kMaxEntries, count)) {
    return refuse("its slot starts do not decode");
  }
  for (std::size_t i = 0; i < count; i++) {
    SlotStart start;
    const std::optional<OtSlot> slot = readSlot(in);
    start.slot = slot.value_or(OtSlot{});
    start.entry = in.u64();
    start.descending = in.boolean();
    start.state = readState(in);
    slotStarts_.push_back(start);
  }
  if (!readCount(in, kMaxEntries, count)) {
    return refuse("its table exits do not decode");
  }
  for (std::size_t i = 0; i < count; i++) {
    TableExit exit;
    exit.table = in.u16();
    exit.entry = in.u64();
    exit.state = readState(in);
    tableExits_.push_back(exit);
  }
  if (!readCount(in, kMaxEntries, count)) {
    return refuse("its chain runs do not decode");
  }
  for (std::size_t i = 0; i < count; i++) {
    ChainRun run;
    run.head = in.u32();
    run.firstEntry = in.u64();
    run.endEntry = in.u64();
    run.atStart = readState(in);
    chainRuns_.push_back(run);
  }
  if (!in.ok()) {
    return refuse("it is truncated");
  }
  return validate(error);
}

bool FrameRecord::validate(std::string &error) {
  const std::size_t entries = entries_.size();
  const auto outside = [entries](std::size_t entry) {
    return entry > entries;
  };
  for (const RecordEntry &entry : entries_) {
    if (const auto *primitive = std::get_if<DrawPrimitive>(&entry);
        primitive != nullptr &&
        (primitive->kind > PrimitiveKind::Sprite || primitive->vertexCount < 1 || primitive->vertexCount > 4 ||
         (primitive->clutOffset != kNoClut && clut(*primitive).empty()))) {
      error = "frame record: a primitive's kind, vertex count or CLUT does not decode";
      reset(0, true);
      return false;
    }
    if (const auto *upload = std::get_if<VramUpload>(&entry);
        upload != nullptr && (upload->width < 0 || upload->height < 0 ||
                              pixels(*upload).size() !=
                                  static_cast<std::size_t>(upload->width) * static_cast<std::size_t>(upload->height))) {
      error = "frame record: an upload's pixels lie past the pool";
      reset(0, true);
      return false;
    }
  }
  const bool slotsInside = std::none_of(slotStarts_.begin(), slotStarts_.end(), [&](const SlotStart &start) {
    return outside(start.entry);
  });
  const bool exitsInside = std::none_of(tableExits_.begin(), tableExits_.end(), [&](const TableExit &exit) {
    return outside(exit.entry);
  });
  const bool runsInside = std::none_of(chainRuns_.begin(), chainRuns_.end(), [&](const ChainRun &run) {
    return run.firstEntry > run.endEntry || outside(run.endEntry);
  });
  if (!slotsInside || !exitsInside || !runsInside) {
    error = "frame record: a slot start, table exit or chain run lies past its entries";
    reset(0, true);
    return false;
  }
  return true;
}

} // namespace psx::present
