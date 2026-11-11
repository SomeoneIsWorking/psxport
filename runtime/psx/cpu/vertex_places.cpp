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
    keep(loadedMask_);
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
  loadedMask_ |= 1u << reg;
}

void VertexPlaces::sortFaces(const FaceSort &sort) {
  if (std::ranges::find(faceSorts_, sort.function, &FaceSort::function) != faceSorts_.end()) {
    return;
  }
  faceSorts_.push_back(sort);
  faceLoadMask_ |= opcodeBit(sort.faceLoad);
}

void VertexPlaces::faceLoaded(const LoadedWord &load) {
  const auto sort = std::ranges::find_if(faceSorts_, [&](const FaceSort &each) {
    return each.function == load.function && each.faceLoad == load.opcode;
  });
  if (sort == faceSorts_.end()) {
    return;
  }
  keepFace();
  face_ = {.function = load.function, .nearerSlots = static_cast<float>(sort->slots(load.word)), .bin = sort->bin};
}

NotedPlace VertexPlaces::inFace(const NotedPlace &noted) const {
  NotedPlace moved = noted;
  if (moved.place.z <= 0.0f) {
    return moved;
  }
  const float nearer = face_.nearerSlots * (face_.bin + moved.place.back);
  // A face sorted to or behind the eye has no depth, so it draws in slot order.
  moved.place.z = std::max(moved.place.z - nearer, 0.0f);
  // What the guest draws after the face meets it at its own depth.
  moved.place.back += nearer;
  return moved;
}

void VertexPlaces::keepFace() {
  if (rollback_.armed() && faceKeptRun_ != rollback_.run()) {
    keep(face_);
    faceKeptRun_ = rollback_.run();
  }
}

void VertexPlaces::carry(std::uint32_t reg, std::uint32_t word, const NotedPlace &noted) {
  if (!tracking_ || reg == kZero) {
    return;
  }
  keepCarried();
  carried_[reg] = {.word = word, .noted = noted};
  carriedMask_ |= 1u << reg;
  loadedMask_ &= ~(1u << reg);
}

void VertexPlaces::drop(std::uint32_t reg) {
  if (!carries(reg)) {
    return;
  }
  keepCarried();
  carriedMask_ &= ~(1u << reg);
}

} // namespace psx::cpu
