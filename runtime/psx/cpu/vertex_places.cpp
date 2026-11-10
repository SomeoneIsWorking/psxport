#include "vertex_places.h"

#include <algorithm>

namespace psx::cpu {
namespace {

constexpr std::uint32_t kRamWords = kMainRamBytes / 4u;
constexpr std::uint32_t kSlots = kRamWords + kScratchpadBytes / 4u;
constexpr std::uint32_t kNoSlot = kSlots;

} // namespace

std::uint32_t VertexPlaces::slot(std::uint32_t address) {
  if (const std::optional<std::uint32_t> offset = mainRamOffset(address)) {
    return *offset / 4u;
  }
  const std::uint32_t p = physical(address);
  if (p >= kScratchpadPhysicalBase && p < kScratchpadPhysicalBase + kScratchpadBytes) {
    return kRamWords + (p - kScratchpadPhysicalBase) / 4u;
  }
  return kNoSlot;
}

void VertexPlaces::track(bool on) {
  if (on == tracking_) {
    return;
  }
  tracking_ = on;
  carried_ = {};
  if (!on) {
    std::vector<Entry>().swap(entries_);
    ramFirst_ = kRamWords;
    ramEnd_ = 0;
    scratchpadNoted_ = false;
  }
}

void VertexPlaces::keep(Entry &entry) {
  if (rollback_.armed()) {
    rollback_.note(reinterpret_cast<std::uint8_t *>(&entry), sizeof entry);
  }
}

void VertexPlaces::note(std::uint32_t address, std::uint32_t word, const NotedPlace &noted) {
  const std::uint32_t at = slot(address);
  if (!tracking_ || at == kNoSlot) {
    return;
  }
  if (entries_.empty()) {
    entries_.resize(kSlots);
  }
  keep(entries_[at]);
  entries_[at] = {.word = word, .placed = true, .noted = noted};
  if (at < kRamWords) {
    ramFirst_ = std::min(ramFirst_, at);
    ramEnd_ = std::max(ramEnd_, at + 1u);
  } else {
    scratchpadNoted_ = true;
  }
}

void VertexPlaces::forgetNoted(std::uint32_t address, std::uint32_t bytes) {
  for (std::uint32_t word = address & ~3u; word < address + bytes; word += 4u) {
    if (const std::uint32_t at = slot(word); at != kNoSlot) {
      entries_[at].placed = false;
    }
  }
}

std::optional<NotedPlace> VertexPlaces::at(std::uint32_t address, std::uint32_t word) const {
  if (entries_.empty() || !mayHoldNote(address, 4u)) {
    return std::nullopt;
  }
  const std::uint32_t at = slot(address);
  if (at == kNoSlot) {
    return std::nullopt;
  }
  const Entry &entry = entries_[at];
  return entry.placed && entry.word == word ? std::optional(entry.noted) : std::nullopt;
}

void VertexPlaces::loaded(std::uint32_t reg, std::uint32_t address, std::uint32_t word) {
  if (!tracking_ || reg == kZero) {
    return;
  }
  const std::optional<NotedPlace> noted = at(address, word);
  Entry &carried = carried_[reg];
  if (!noted && !carried.placed) {
    return;
  }
  keep(carried);
  carried = noted ? Entry{.word = word, .placed = true, .noted = *noted} : Entry{};
}

void VertexPlaces::carry(std::uint32_t reg, std::uint32_t word, const NotedPlace &noted) {
  if (!tracking_ || reg == kZero) {
    return;
  }
  keep(carried_[reg]);
  carried_[reg] = {.word = word, .placed = true, .noted = noted};
}

void VertexPlaces::overwrote(std::uint32_t reg) {
  if (!tracking_ || !carried_[reg].placed) {
    return;
  }
  keep(carried_[reg]);
  carried_[reg].placed = false;
}

void VertexPlaces::stored(std::uint32_t reg, std::uint32_t address, std::uint32_t word) {
  const Entry &carried = carried_[reg];
  if (carried.placed && carried.word == word) {
    note(address, word, carried.noted);
  }
}

} // namespace psx::cpu
