#include "vertex_places.h"

namespace psx::cpu {

void VertexPlaces::track(bool on) {
  if (on == tracking_) {
    return;
  }
  tracking_ = on;
  carriedMask_ = 0;
  if (!on) {
    std::vector<std::uint64_t>().swap(placed_);
    std::vector<Stored>().swap(stored_);
    std::vector<std::uint32_t>().swap(keptRun_);
  }
}

void VertexPlaces::note(std::uint32_t address, std::uint32_t word, const NotedPlace &noted) {
  const std::uint32_t at = slot(address);
  if (!tracking_ || at == kNoSlot) {
    return;
  }
  if (placed_.empty()) {
    placed_.resize((kSlots + 63u) / 64u);
    keptRun_.resize(placed_.size());
    stored_.resize(kSlots);
  }
  keep(stored_[at]);
  stored_[at] = {.word = word, .noted = noted};
  keepBits(at / 64u);
  placed_[at / 64u] |= std::uint64_t{1} << (at % 64u);
}

void VertexPlaces::keepBits(std::uint32_t index) {
  if (rollback_.armed() && keptRun_[index] != rollback_.run()) {
    keep(placed_[index]);
    keptRun_[index] = rollback_.run();
  }
}

void VertexPlaces::keepCarried() {
  if (rollback_.armed() && carriedKeptRun_ != rollback_.run()) {
    keep(carried_);
    keep(carriedMask_);
    carriedKeptRun_ = rollback_.run();
  }
}

void VertexPlaces::unplace(std::uint32_t at) {
  keepBits(at / 64u);
  placed_[at / 64u] &= ~(std::uint64_t{1} << (at % 64u));
}

void VertexPlaces::forgetSpan(std::uint32_t address, std::uint32_t bytes) {
  for (std::uint32_t word = address & ~3u; word < address + bytes; word += 4u) {
    if (const std::uint32_t at = slot(word); at != kNoSlot && placed(at)) {
      unplace(at);
    }
  }
}

std::optional<NotedPlace> VertexPlaces::at(std::uint32_t address, std::uint32_t word) const {
  if (!placedAt(address)) {
    return std::nullopt;
  }
  const Stored &stored = stored_[slot(address)];
  return stored.word == word ? std::optional(stored.noted) : std::nullopt;
}

void VertexPlaces::loadedPlaced(std::uint32_t reg, const std::optional<NotedPlace> &noted, std::uint32_t word) {
  if (!noted) {
    drop(reg);
    return;
  }
  carry(reg, word, *noted);
}

void VertexPlaces::carry(std::uint32_t reg, std::uint32_t word, const NotedPlace &noted) {
  if (!tracking_ || reg == kZero) {
    return;
  }
  keepCarried();
  carried_[reg] = {.word = word, .noted = noted};
  carriedMask_ |= 1u << reg;
}

void VertexPlaces::drop(std::uint32_t reg) {
  if (!carries(reg)) {
    return;
  }
  keepCarried();
  carriedMask_ &= ~(1u << reg);
}

} // namespace psx::cpu
