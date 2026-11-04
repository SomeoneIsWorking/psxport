#include "vertex_depths.h"

#include <algorithm>

namespace psx::cpu {
namespace {

constexpr std::uint32_t kRamWords = kMainRamBytes / 4u;
constexpr std::uint32_t kSlots = kRamWords + kScratchpadBytes / 4u;
constexpr std::uint32_t kNoSlot = kSlots;

} // namespace

std::uint32_t VertexDepths::slot(std::uint32_t address) {
  if (const std::optional<std::uint32_t> offset = mainRamOffset(address)) {
    return *offset / 4u;
  }
  const std::uint32_t p = physical(address);
  if (p >= kScratchpadPhysicalBase && p < kScratchpadPhysicalBase + kScratchpadBytes) {
    return kRamWords + (p - kScratchpadPhysicalBase) / 4u;
  }
  return kNoSlot;
}

void VertexDepths::track(bool on) {
  if (on == tracking_) {
    return;
  }
  tracking_ = on;
  if (!on) {
    std::vector<Entry>().swap(entries_);
    ramFirst_ = kRamWords;
    ramEnd_ = 0;
    scratchpadNoted_ = false;
  }
}

void VertexDepths::note(std::uint32_t address, std::uint32_t word, float depth) {
  const std::uint32_t at = slot(address);
  if (!tracking_ || at == kNoSlot || (entries_.empty() && depth <= 0.0f)) {
    return;
  }
  if (entries_.empty()) {
    entries_.resize(kSlots);
  }
  entries_[at] = {word, depth};
  if (at < kRamWords) {
    ramFirst_ = std::min(ramFirst_, at);
    ramEnd_ = std::max(ramEnd_, at + 1u);
  } else {
    scratchpadNoted_ = true;
  }
}

void VertexDepths::forgetNoted(std::uint32_t address, std::uint32_t bytes) {
  for (std::uint32_t word = address & ~3u; word < address + bytes; word += 4u) {
    if (const std::uint32_t at = slot(word); at != kNoSlot) {
      entries_[at].depth = 0.0f;
    }
  }
}

float VertexDepths::at(std::uint32_t address, std::uint32_t word) const {
  if (entries_.empty()) {
    return 0.0f;
  }
  const std::uint32_t at = slot(address);
  if (at == kNoSlot) {
    return 0.0f;
  }
  const Entry &entry = entries_[at];
  return entry.word == word ? entry.depth : 0.0f;
}

} // namespace psx::cpu
