#include "ownership_verdict.h"

#include <lucent/log.h>

#include <algorithm>
#include <bit>
#include <variant>

namespace psx::present {
namespace {

constexpr lucent::Channel verdictChannel{"verdict"};
constexpr std::uint64_t kMix = 0x9E3779B97F4A7C15ull;

std::uint64_t mix(std::uint64_t h, std::uint64_t value) {
  h ^= value + kMix + (h << 6) + (h >> 2);
  return h;
}

// What a primitive puts on screen, without where its packet lived or who owns it.
std::uint64_t pictureHash(const FrameRecord &record, const DrawPrimitive &primitive) {
  std::uint64_t h = mix(0, primitive.command);
  h = mix(h, static_cast<std::uint64_t>(primitive.kind) | (static_cast<std::uint64_t>(primitive.vertexCount) << 8));
  h = mix(h,
          static_cast<std::uint64_t>(primitive.width) | (static_cast<std::uint64_t>(primitive.height) << 16) |
              (static_cast<std::uint64_t>(primitive.flipX) << 32) |
              (static_cast<std::uint64_t>(primitive.flipY) << 33));
  // Vertices and the draw area relative to the draw offset, so a double-buffered draw is the same picture in
  // either buffer.
  const RecordDrawState &s = primitive.state;
  for (int i = 0; i < primitive.vertexCount; i++) {
    const RecordVertex &v = primitive.vertices[static_cast<std::size_t>(i)];
    h = mix(h,
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(v.x - s.offsetX)) << 32) |
                static_cast<std::uint32_t>(v.y - s.offsetY));
    h = mix(h,
            static_cast<std::uint64_t>(v.r) | (static_cast<std::uint64_t>(v.g) << 8) |
                (static_cast<std::uint64_t>(v.b) << 16) | (static_cast<std::uint64_t>(v.u) << 24) |
                (static_cast<std::uint64_t>(v.v) << 32));
    h = mix(h,
            (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(v.subX)) << 32) |
                std::bit_cast<std::uint32_t>(v.subY));
  }
  h = mix(h,
          static_cast<std::uint64_t>((s.clipX0 - s.offsetX) & 0xFFFF) |
              (static_cast<std::uint64_t>((s.clipY0 - s.offsetY) & 0xFFFF) << 16) |
              (static_cast<std::uint64_t>((s.clipX1 - s.offsetX) & 0xFFFF) << 32) |
              (static_cast<std::uint64_t>((s.clipY1 - s.offsetY) & 0xFFFF) << 48));
  h = mix(h,
          static_cast<std::uint64_t>(s.texPageX & 0xFFFF) | (static_cast<std::uint64_t>(s.texPageY & 0xFFFF) << 16) |
              (static_cast<std::uint64_t>(s.texMode & 0xF) << 32) |
              (static_cast<std::uint64_t>(s.blendMode & 0xF) << 36));
  h = mix(h, primitive.clutWord);
  for (const std::uint16_t entry : record.clut(primitive)) {
    h = mix(h, entry);
  }
  return h;
}

} // namespace

void OwnershipVerdict::collect(const FrameRecord &record, const OwnerLedger &ledger, std::vector<Group> &groups) {
  groups.clear();
  std::size_t last = 0;
  for (const RecordEntry &entry : record.entries()) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive == nullptr || primitive->key) {
      continue;
    }
    Group key;
    if (const GuestWriter *writer = ledger.writer(primitive->owner)) {
      key.kind = writer->kind;
      key.code = writer->function != 0 ? writer->function : writer->blockPc;
      key.dispatched = writer->dispatched;
    }
    key.table = primitive->slot ? primitive->slot->table : kNoTable;
    if (const std::optional<RecordKey> scope = ledger.scopeKey(primitive->mixedOwner)) {
      key.amended = true;
      key.producer = scope->producer;
    }
    if (last >= groups.size() || !groups[last].sameKey(key)) {
      const auto found = std::find_if(groups.begin(), groups.end(), [&key](const Group &group) {
        return group.sameKey(key);
      });
      if (found == groups.end()) {
        key.sample = primitive->owner;
        key.sampleAddress = primitive->sourceAddress;
        key.sampleCommand = primitive->command;
        groups.push_back(key);
        last = groups.size() - 1u;
      } else {
        last = static_cast<std::size_t>(found - groups.begin());
      }
    }
    Group &group = groups[last];
    group.hash = mix(group.hash, pictureHash(record, *primitive));
    ++group.count;
    if (primitive->slot) {
      group.bucketLo = std::min(group.bucketLo, primitive->slot->index);
      group.bucketHi = std::max(group.bucketHi, primitive->slot->index);
    }
  }
}

bool OwnershipVerdict::judge(const FrameRecord &from, const FrameRecord &to, const OwnerLedger &ledger) {
  // The last interval's S is this one's S' when the intervals follow each other.
  if (toSequence_ && *toSequence_ == from.sequence()) {
    from_.swap(to_);
  } else {
    collect(from, ledger, from_);
  }
  collect(to, ledger, to_);
  toSequence_ = to.sequence();
  findings_.clear();
  bool still = true;
  for (const Group &group : to_) {
    const auto earlier = std::find_if(from_.begin(), from_.end(), [&group](const Group &candidate) {
      return candidate.sameKey(group);
    });
    if (earlier != from_.end() && earlier->count == group.count && earlier->hash == group.hash) {
      continue;
    }
    still = false;
    findings_.push_back({.reason = group.amended                     ? OwnerReason::Overwrote
                                   : group.kind == WriterKind::Guest ? OwnerReason::UnownedMoving
                                                                     : OwnerReason::Port,
                         .writer = group.sample,
                         .producer = group.producer,
                         .table = group.table,
                         .bucketLo = group.bucketLo == 0xFFFFFFFFu ? 0u : group.bucketLo,
                         .bucketHi = group.bucketHi,
                         .primitives = group.count,
                         .sampleAddress = group.sampleAddress,
                         .sampleCommand = group.sampleCommand});
  }
  // A group S' drew and S does not vanishes at the interval's start unless the interval is held.
  for (const Group &group : from_) {
    if (std::none_of(to_.begin(), to_.end(), [&group](const Group &candidate) {
          return candidate.sameKey(group);
        })) {
      still = false;
      findings_.push_back({.reason = group.amended                     ? OwnerReason::Overwrote
                                     : group.kind == WriterKind::Guest ? OwnerReason::UnownedMoving
                                                                       : OwnerReason::Port,
                           .writer = group.sample,
                           .producer = group.producer,
                           .table = group.table,
                           .bucketLo = group.bucketLo == 0xFFFFFFFFu ? 0u : group.bucketLo,
                           .bucketHi = group.bucketHi,
                           .primitives = 0,
                           .sampleAddress = group.sampleAddress,
                           .sampleCommand = group.sampleCommand});
    }
  }
  // An owned packet a writer stored into keeps its key, but its render cannot know what the writer changed: the
  // interval is held.
  for (const RecordEntry &entry : to.entries()) {
    const auto *primitive = std::get_if<DrawPrimitive>(&entry);
    if (primitive == nullptr || !primitive->key || !OwnerLedger::isWriter(primitive->mixedOwner)) {
      continue;
    }
    still = false;
    const auto same = std::find_if(findings_.begin(), findings_.end(), [primitive](const OwnerFinding &finding) {
      return finding.reason == OwnerReason::Overwrote && finding.writer == primitive->mixedOwner &&
             finding.producer == primitive->key->producer;
    });
    if (same != findings_.end()) {
      ++same->primitives;
      continue;
    }
    findings_.push_back({.reason = OwnerReason::Overwrote,
                         .writer = primitive->mixedOwner,
                         .producer = primitive->key->producer,
                         .table = primitive->slot ? primitive->slot->table : kNoTable,
                         .bucketLo = primitive->slot ? primitive->slot->index : 0u,
                         .bucketHi = primitive->slot ? primitive->slot->index : 0u,
                         .primitives = 1,
                         .sampleAddress = primitive->sourceAddress,
                         .sampleCommand = primitive->command});
  }
  lucent::debug(verdictChannel,
                "record={}->{}: {} unowned group(s), {} finding(s), still={}",
                from.sequence(),
                to.sequence(),
                to_.size(),
                findings_.size(),
                still);
  return still;
}

} // namespace psx::present
