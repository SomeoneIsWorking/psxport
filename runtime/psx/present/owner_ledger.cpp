#include "owner_ledger.h"

#include "core.h"
#include "guest_memory_map.h"

#include <lucent/log.h>

#include <algorithm>

namespace psx::present {
namespace {

constexpr std::size_t kMainRamWords = kMainRamBytes / 4u;

std::optional<std::uint32_t> mainRamWord(std::uint32_t address) {
  const std::optional<std::uint32_t> offset = mainRamOffset(address);
  if (!offset) {
    return std::nullopt;
  }
  return *offset >> 2u;
}

bool sameCode(const GuestWriter &a, const GuestWriter &b) {
  return a.kind == b.kind && a.blockPc == b.blockPc && a.function == b.function && a.dispatched == b.dispatched;
}

std::size_t writerHash(const GuestWriter &writer) {
  std::uint64_t h = writer.blockPc;
  h = h * 0x9E3779B97F4A7C15ull ^ writer.function;
  h = h * 0x9E3779B97F4A7C15ull ^ writer.dispatched;
  h = h * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(writer.kind);
  return static_cast<std::size_t>(h ^ (h >> 29));
}

} // namespace

const char *writerKindName(WriterKind kind) {
  switch (kind) {
  case WriterKind::Guest:
    return "guest";
  case WriterKind::BlockDma:
    return "block-dma";
  case WriterKind::MmioGp0:
    return "mmio-gp0";
  case WriterKind::LoadImage:
    return "load-image";
  }
  return "?";
}

OwnerLedger::OwnerLedger(Core &core)
    : core_(core), words_(kMainRamWords, kNoOwner), overwrote_(kMainRamWords, kNoOwner), scopes_(kScopeSlots),
      writerSlots_(kWriterSlots, 0u) {
  writers_.reserve(kMaxWriters);
}

OwnerHandle OwnerLedger::openScope(const RecordKey &key) {
  const std::uint32_t slot = nextScope_;
  nextScope_ = (nextScope_ + 1u) % kScopeSlots;
  if (nextScope_ == 0) {
    epoch_ = epoch_ == kEpochMask ? 1u : epoch_ + 1u;
  }
  scopes_[slot] = {key, epoch_, opened_++};
  return (epoch_ << 16) | slot;
}

void OwnerLedger::noteStore(GuestSpan store, OwnerHandle scope) {
  const std::optional<std::uint32_t> first = mainRamWord(store.address);
  const std::optional<std::uint32_t> last = mainRamWord(store.address + store.bytes - 1u);
  if (!first || !last || *last < *first) {
    return;
  }
  if (scope != kNoOwner) {
    std::fill(words_.begin() + *first, words_.begin() + *last + 1, scope);
    std::fill(overwrote_.begin() + *first, overwrote_.begin() + *last + 1, kNoOwner);
    return;
  }
  const OwnerHandle writer = runningWriter();
  // A writer storing over a scope's word of the draw list being built amends it.
  for (std::uint32_t word = *first; word <= *last; ++word) {
    const OwnerHandle previous = words_[word];
    if (inDrawList(previous)) {
      overwrote_[word] = previous;
    } else if (!inDrawList(overwrote_[word])) {
      overwrote_[word] = kNoOwner;
    }
    words_[word] = writer;
  }
}

bool OwnerLedger::inDrawList(OwnerHandle handle) const {
  return scopeKey(handle) && scopes_[handle & 0xFFFFu].opened >= listStart_;
}

OwnerHandle OwnerLedger::overwrote(std::uint32_t address) const {
  const std::optional<std::uint32_t> word = mainRamWord(address);
  return word ? overwrote_[*word] : kNoOwner;
}

OwnerHandle OwnerLedger::ownerOf(std::uint32_t address) const {
  const std::optional<std::uint32_t> word = mainRamWord(address);
  return word ? words_[*word] : kNoOwner;
}

OwnerHandle OwnerLedger::runningWriter() {
  return port(WriterKind::Guest);
}

OwnerHandle OwnerLedger::port(WriterKind kind) {
  const GuestWriter running{.kind = kind,
                            .blockPc = core_.writerLatch.blockPc(),
                            .function = core_.writerLatch.function(),
                            .dispatched = core_.callAttribution.top()};
  if (lastHandle_ != kNoOwner && sameCode(running, last_)) {
    return lastHandle_;
  }
  last_ = running;
  lastHandle_ = intern(running);
  return lastHandle_;
}

OwnerHandle OwnerLedger::intern(const GuestWriter &running) {
  const cpu::ImageCatalog &catalog = core_.imageCatalog();
  const std::uint64_t revision = catalog.revision();
  std::optional<cpu::ImageIdentity> resolved;
  std::size_t at = writerHash(running) % kWriterSlots;
  for (;; at = (at + 1u) % kWriterSlots) {
    const std::uint32_t index = writerSlots_[at];
    if (index == 0) {
      break;
    }
    GuestWriter &known = writers_[index - 1u];
    if (!sameCode(known, running)) {
      continue;
    }
    if (known.revision == revision) {
      return kWriterBit | (index - 1u);
    }
    if (!resolved) {
      resolved = catalog.resolve(running.blockPc).value_or(cpu::ImageIdentity{});
    }
    if (known.image == *resolved) {
      known.revision = revision;
      return kWriterBit | (index - 1u);
    }
  }
  if (writers_.size() == kMaxWriters) {
    if (!overflowReported_) {
      overflowReported_ = true;
      lucent::warn("owners", "{} writers interned; later writers are reported as the first", kMaxWriters);
    }
    return kWriterBit;
  }
  GuestWriter added = running;
  added.image = resolved ? *resolved : catalog.resolve(running.blockPc).value_or(cpu::ImageIdentity{});
  added.revision = revision;
  writers_.push_back(added);
  writerSlots_[at] = static_cast<std::uint32_t>(writers_.size());
  return kWriterBit | static_cast<std::uint32_t>(writers_.size() - 1u);
}

std::optional<RecordKey> OwnerLedger::scopeKey(OwnerHandle handle) const {
  if (!isScope(handle)) {
    return std::nullopt;
  }
  const ScopeSlot &slot = scopes_[handle & 0xFFFFu];
  if (slot.epoch != (handle >> 16)) {
    return std::nullopt;
  }
  return slot.key;
}

const GuestWriter *OwnerLedger::writer(OwnerHandle handle) const {
  if (!isWriter(handle)) {
    return nullptr;
  }
  const std::uint32_t index = handle & ~kWriterBit;
  return index < writers_.size() ? &writers_[index] : nullptr;
}

void OwnerLedger::clear() {
  std::fill(words_.begin(), words_.end(), kNoOwner);
  std::fill(overwrote_.begin(), overwrote_.end(), kNoOwner);
  core_.writerLatch.clear();
  lastHandle_ = kNoOwner;
}

} // namespace psx::present
