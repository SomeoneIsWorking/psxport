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

OwnerLedger::OwnerLedger(Core &core) : core_(core), words_(kMainRamWords, kNoOwner), writerSlots_(kWriterSlots, 0u) {
  writers_.reserve(kMaxWriters);
}

void OwnerLedger::noteStore(GuestSpan store) {
  const std::optional<std::uint32_t> first = mainRamWord(store.address);
  const std::optional<std::uint32_t> last = mainRamWord(store.address + store.bytes - 1u);
  if (!first || !last || *last < *first) {
    return;
  }
  std::fill(words_.begin() + *first, words_.begin() + *last + 1, port(WriterKind::Guest));
}

OwnerHandle OwnerLedger::ownerOf(std::uint32_t address) const {
  const std::optional<std::uint32_t> word = mainRamWord(address);
  return word ? words_[*word] : kNoOwner;
}

OwnerHandle OwnerLedger::port(WriterKind kind) {
  const GuestWriter running{.kind = kind,
                            .blockPc = core_.writerLatch.blockPc(),
                            .function = core_.writerLatch.function(),
                            .dispatched = core_.callAttribution.top()};
  const std::uint64_t revision = core_.imageCatalog().revision();
  if (lastHandle_ != kNoOwner && lastRevision_ == revision && sameCode(running, last_)) {
    return lastHandle_;
  }
  last_ = running;
  lastRevision_ = revision;
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
      return index;
    }
    if (!resolved) {
      resolved = catalog.resolve(running.blockPc).value_or(cpu::ImageIdentity{});
    }
    if (known.image == *resolved) {
      known.revision = revision;
      return index;
    }
  }
  if (writers_.size() == kMaxWriters) {
    if (!overflowReported_) {
      overflowReported_ = true;
      lucent::warn("owners", "{} writers interned; later writers are reported as the first", kMaxWriters);
    }
    return 1u;
  }
  GuestWriter added = running;
  added.image = resolved ? *resolved : catalog.resolve(running.blockPc).value_or(cpu::ImageIdentity{});
  added.revision = revision;
  writers_.push_back(added);
  writerSlots_[at] = static_cast<std::uint32_t>(writers_.size());
  return static_cast<std::uint32_t>(writers_.size());
}

const GuestWriter *OwnerLedger::writer(OwnerHandle handle) const {
  return handle != kNoOwner && handle <= writers_.size() ? &writers_[handle - 1u] : nullptr;
}

void OwnerLedger::clear() {
  std::fill(words_.begin(), words_.end(), kNoOwner);
  core_.writerLatch.clear();
  lastHandle_ = kNoOwner;
}

} // namespace psx::present
