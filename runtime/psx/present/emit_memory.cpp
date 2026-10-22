#include "emit_memory.h"

#include "guest_memory_map.h"

#include <lucent/log.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <tuple>
#include <utility>

namespace psx::present {

void LevelReads::note(std::uint32_t wordAddress, std::uint32_t value) {
  if ((words_.size() + 1) * 2 > slots_.size()) {
    grow();
  }
  const std::size_t mask = slots_.size() - 1;
  for (std::size_t at = startSlot(wordAddress);; at = (at + 1) & mask) {
    Slot &slot = slots_[at];
    if (slot.stamp != stamp_) {
      slot = {stamp_, static_cast<std::uint32_t>(words_.size())};
      words_.push_back({wordAddress, value});
      return;
    }
    if (words_[slot.word].address == wordAddress) {
      return;
    }
  }
}

void LevelReads::clear() {
  words_.clear();
  if (++stamp_ == 0) {
    std::fill(slots_.begin(), slots_.end(), Slot{});
    stamp_ = 1;
  }
}

std::size_t LevelReads::startSlot(std::uint32_t wordAddress) const {
  // The product's high bits, which every address bit reaches; word addresses keep their low two zero.
  return (wordAddress * 0x9E3779B1u) >> (32 - std::countr_zero(slots_.size()));
}

void LevelReads::grow() {
  slots_.assign(std::max<std::size_t>(64, slots_.size() * 2), Slot{});
  stamp_ = 1;
  const std::size_t mask = slots_.size() - 1;
  for (std::uint32_t word = 0; word < words_.size(); ++word) {
    std::size_t at = startSlot(words_[word].address);
    while (slots_[at].stamp == stamp_) {
      at = (at + 1) & mask;
    }
    slots_[at] = {stamp_, word};
  }
}

void abortKeptScratchInUse() {
  lucent::error("emit-memory", "a kept scratch object was taken while another use held it");
  std::abort();
}

namespace {

std::uint32_t wordOf(std::uint32_t kseg0) {
  return kseg0 & ~3u;
}

// The word holding `kseg0`, counted from the word holding `base`.
std::size_t wordIndex(std::uint32_t base, std::uint32_t kseg0) {
  return (wordOf(kseg0) - wordOf(base)) / 4u;
}

// The words a span of `bytes` at `base` touches.
std::size_t wordCount(std::uint32_t base, std::size_t bytes) {
  return wordIndex(base, base + static_cast<std::uint32_t>(bytes) + 3u);
}

} // namespace

void HostMemory::clear() {
  for (Held &held : ranges_) {
    spares_.push_back(std::move(held.bytes));
    if (held.places.capacity() != 0) {
      placeSpares_.push_back(std::move(held.places));
    }
  }
  ranges_.clear();
  lastRange_ = nullptr;
  overlay_.clear();
  keepsLevelReads_ = false;
  levelReads_.clear();
}

HostMemory::Range HostMemory::takeBuffer(std::size_t size) {
  // The smallest spare that holds `size`, else the largest, which grows.
  auto best = spares_.end();
  for (auto spare = spares_.begin(); spare != spares_.end(); ++spare) {
    if (best == spares_.end()) {
      best = spare;
    } else if (spare->capacity() >= size) {
      if (best->capacity() < size || spare->capacity() < best->capacity()) {
        best = spare;
      }
    } else if (best->capacity() < size && spare->capacity() > best->capacity()) {
      best = spare;
    }
  }
  Range buffer;
  if (best != spares_.end()) {
    buffer = std::move(*best);
    spares_.erase(best);
  }
  buffer.assign(size, std::byte{0});
  return buffer;
}

const HostMemory::Held *HostMemory::find(std::uint32_t address) const {
  const std::uint32_t at = kseg0Address(address);
  if (lastRange_ != nullptr && at - lastRange_->base < lastRange_->bytes.size()) {
    return lastRange_;
  }
  const auto next = std::upper_bound(ranges_.begin(), ranges_.end(), at, [](std::uint32_t a, const Held &held) {
    return a < held.base;
  });
  if (next == ranges_.begin()) {
    return nullptr;
  }
  const auto held = std::prev(next);
  if (at - held->base >= held->bytes.size()) {
    return nullptr;
  }
  lastRange_ = &*held;
  return lastRange_;
}

HostMemory::Held *HostMemory::find(std::uint32_t address) {
  return const_cast<Held *>(std::as_const(*this).find(address));
}

HostMemory::Places &HostMemory::placesOf(Held &held) {
  if (held.places.empty()) {
    if (!placeSpares_.empty()) {
      held.places = std::move(placeSpares_.back());
      placeSpares_.pop_back();
    }
    held.places.assign(wordCount(held.base, held.bytes.size()), std::nullopt);
  }
  return held.places;
}

std::byte *HostMemory::place(std::uint32_t base, std::uint32_t size) {
  lastRange_ = nullptr;
  std::uint32_t mergedBase = base;
  std::uint32_t mergedEnd = base + size;
  auto first = std::lower_bound(ranges_.begin(), ranges_.end(), base, [](const Held &held, std::uint32_t a) {
    return held.base < a;
  });
  if (first != ranges_.begin() && std::prev(first)->base + std::prev(first)->bytes.size() >= base) {
    first = std::prev(first);
  }
  auto last = first;
  while (last != ranges_.end() && last->base <= mergedEnd) {
    mergedBase = std::min(mergedBase, last->base);
    mergedEnd = std::max(mergedEnd, last->base + static_cast<std::uint32_t>(last->bytes.size()));
    ++last;
  }
  if (first == last || first->base != mergedBase || std::next(first) != last ||
      first->bytes.size() != mergedEnd - mergedBase) {
    Held merged{mergedBase, takeBuffer(mergedEnd - mergedBase), {}};
    for (auto held = first; held != last; ++held) {
      std::copy(held->bytes.begin(), held->bytes.end(), merged.bytes.begin() + (held->base - mergedBase));
      if (!held->places.empty()) {
        Places &places = placesOf(merged);
        std::copy(held->places.begin(), held->places.end(), places.begin() + wordIndex(mergedBase, held->base));
        placeSpares_.push_back(std::move(held->places));
      }
      spares_.push_back(std::move(held->bytes));
    }
    first = ranges_.erase(first, last);
    first = ranges_.insert(first, std::move(merged));
  }
  return first->bytes.data() + (base - mergedBase);
}

void HostMemory::forgetPlaces(Held &held, std::uint32_t kseg0, std::uint32_t size) {
  if (held.places.empty() || size == 0) {
    return;
  }
  const auto from = held.places.begin() + static_cast<std::ptrdiff_t>(wordIndex(held.base, kseg0));
  const auto to = held.places.begin() + static_cast<std::ptrdiff_t>(wordIndex(held.base, kseg0 + size + 3u));
  std::fill(from, to, std::nullopt);
}

void HostMemory::provide(std::uint32_t address, std::span<const std::byte> bytes) {
  const auto size = static_cast<std::uint32_t>(bytes.size());
  std::memcpy(place(kseg0Address(address), size), bytes.data(), bytes.size());
  forgetPlaces(*find(address), kseg0Address(address), size);
}

void HostMemory::zero(std::uint32_t address, std::uint32_t size) {
  std::memset(place(kseg0Address(address), size), 0, size);
  forgetPlaces(*find(address), kseg0Address(address), size);
}

void HostMemory::mapGuest(Core &guest, std::uint32_t address, std::uint32_t size) {
  const std::uint32_t base = kseg0Address(address);
  const std::uint32_t end = base + size;
  // The parts no range holds yet, found before the merge hides them.
  gaps_.clear();
  std::uint32_t at = base;
  auto held = std::upper_bound(ranges_.begin(), ranges_.end(), base, [](std::uint32_t a, const Held &h) {
    return a < h.base;
  });
  if (held != ranges_.begin()) {
    held = std::prev(held);
  }
  for (; held != ranges_.end() && held->base < end && at < end; ++held) {
    const std::uint32_t heldEnd = held->base + static_cast<std::uint32_t>(held->bytes.size());
    if (heldEnd <= at) {
      continue;
    }
    if (held->base > at) {
      gaps_.push_back({at, held->base - at});
    }
    at = std::max(at, heldEnd);
  }
  if (at < end) {
    gaps_.push_back({at, end - at});
  }
  std::byte *bytes = place(base, size);
  for (const GuestSpan &gap : gaps_) {
    std::byte *out = bytes + (gap.address - base);
    if (const std::optional<GuestAddressRange> ram = guest.mappedMainRamRange(gap.address, gap.bytes)) {
      std::memcpy(out, guest.ram + ram->begin, gap.bytes);
    } else {
      for (std::uint32_t i = 0; i < gap.bytes; ++i) {
        out[i] = static_cast<std::byte>(guest.mem_r8(gap.address + i));
      }
    }
    const std::uint32_t gapEnd = gap.address + gap.bytes;
    auto word = std::ranges::lower_bound(overlay_, gap.address & ~3u, {}, &LevelWord::address);
    for (; word != overlay_.end() && word->address < gapEnd; ++word) {
      for (std::uint32_t byte = 0; byte < 4u; ++byte) {
        const std::uint32_t target = word->address + byte;
        if (target >= gap.address && target < gapEnd) {
          out[target - gap.address] = static_cast<std::byte>(word->value >> (8u * byte));
        }
      }
    }
  }
}

void HostMemory::overlay(std::span<const std::byte> packedWords) {
  overlayNotes_.clear();
  for (const LevelWord &word : overlay_) {
    overlayNotes_.push_back({word.address, static_cast<std::uint32_t>(overlayNotes_.size()), word.value});
  }
  for (std::size_t at = 0; at + sizeof(LevelWord) <= packedWords.size(); at += sizeof(LevelWord)) {
    LevelWord word;
    std::memcpy(&word, packedWords.data() + at, sizeof(LevelWord));
    overlayNotes_.push_back(
        {kseg0Address(word.address) & ~3u, static_cast<std::uint32_t>(overlayNotes_.size()), word.value});
  }
  std::sort(overlayNotes_.begin(), overlayNotes_.end(), [](const OverlayNote &a, const OverlayNote &b) {
    return std::tie(a.address, a.order) < std::tie(b.address, b.order);
  });
  overlay_.clear();
  for (std::size_t i = 0; i < overlayNotes_.size(); ++i) {
    if (i + 1 == overlayNotes_.size() || overlayNotes_[i + 1].address != overlayNotes_[i].address) {
      overlay_.push_back({overlayNotes_[i].address, overlayNotes_[i].value});
    }
  }
}

const std::uint32_t *HostMemory::overlaid(std::uint32_t wordAddress) const {
  const auto found = std::ranges::lower_bound(overlay_, wordAddress, {}, &LevelWord::address);
  return found == overlay_.end() || found->address != wordAddress ? nullptr : &found->value;
}

bool HostMemory::read(std::uint32_t address, void *out, std::uint32_t size) const {
  const Held *held = find(address);
  if (held == nullptr) {
    return false;
  }
  const std::uint32_t offset = kseg0Address(address) - held->base;
  if (offset + size > held->bytes.size()) {
    lucent::error("emit-memory", "a {}-byte read at 0x{:08X} runs past its host range", size, address);
    std::abort();
  }
  std::memcpy(out, held->bytes.data() + offset, size);
  return true;
}

void HostMemory::readPlaces(std::uint32_t address, std::span<gte::WordPlace> out) const {
  const Held *held = find(address);
  if (held == nullptr || held->places.empty()) {
    std::fill(out.begin(), out.end(), std::nullopt);
    return;
  }
  const std::size_t first = wordIndex(held->base, kseg0Address(address));
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = first + i < held->places.size() ? held->places[first + i] : std::nullopt;
  }
}

void HostMemory::fallThrough(Core &guest, std::uint32_t address, void *out, std::uint32_t size) const {
  const std::uint32_t word = kseg0Address(address) & ~3u;
  const std::uint32_t *overlay = overlaid(word);
  if (keepsLevelReads_) {
    levelReads_.note(word, overlay != nullptr ? *overlay : guest.mem_r32(word));
  }
  if (overlay != nullptr) {
    std::memcpy(out, reinterpret_cast<const std::byte *>(overlay) + (address & 3u), size);
  } else if (size == 1) {
    const std::uint8_t value = guest.mem_r8(address);
    std::memcpy(out, &value, size);
  } else if (size == 2) {
    const std::uint16_t value = guest.mem_r16(address);
    std::memcpy(out, &value, size);
  } else {
    const std::uint32_t value = guest.mem_r32(address);
    std::memcpy(out, &value, size);
  }
}

HostMemory::Held &HostMemory::writableRange(std::uint32_t address, std::uint32_t size) {
  Held *held = find(address);
  if (held == nullptr) {
    lucent::error("emit-memory", "a render wrote {} bytes to 0x{:08X}, outside its host ranges", size, address);
    std::abort();
  }
  if (kseg0Address(address) - held->base + size > held->bytes.size()) {
    lucent::error("emit-memory", "a {}-byte write at 0x{:08X} runs past its host range", size, address);
    std::abort();
  }
  return *held;
}

void HostMemory::write(std::uint32_t address, const void *in, std::uint32_t size) {
  Held &range = writableRange(address, size);
  const std::uint32_t at = kseg0Address(address);
  std::memcpy(range.bytes.data() + (at - range.base), in, size);
  forgetPlaces(range, at, size);
}

void HostMemory::writeVertex(std::uint32_t address, const gte::ProjectedVertex &vertex) {
  const std::uint32_t word = vertex.word();
  Held &range = writableRange(address, sizeof(word));
  const std::uint32_t at = kseg0Address(address);
  std::memcpy(range.bytes.data() + (at - range.base), &word, sizeof(word));
  placesOf(range)[wordIndex(range.base, at)] = vertex.place();
}

std::span<const std::byte> HostMemory::view(std::uint32_t address, std::uint32_t size) const {
  const Held *held = find(address);
  const std::uint32_t offset = held != nullptr ? kseg0Address(address) - held->base : 0u;
  if (held == nullptr || offset + size > held->bytes.size()) {
    lucent::error("emit-memory", "no host range holds {} bytes at 0x{:08X}", size, address);
    std::abort();
  }
  return std::span<const std::byte>(held->bytes).subspan(offset, size);
}

void EmitMemory::readWords(std::uint32_t address, std::span<std::uint32_t> out) const {
  if (host_ != nullptr && host_->read(address, out.data(), static_cast<std::uint32_t>(out.size_bytes()))) {
    return;
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = mem_r32(address + 4u * static_cast<std::uint32_t>(i));
  }
}

void EmitMemory::readPlaces(std::uint32_t address, std::span<gte::WordPlace> out) const {
  if (host_ != nullptr) {
    host_->readPlaces(address, out);
  } else {
    std::fill(out.begin(), out.end(), std::nullopt);
  }
}

void EmitMemory::noteRead(std::uint32_t address) const {
  const std::uint32_t word = kseg0Address(address) & ~3u;
  for (const GuestSpan &range : working_) {
    if (word - kseg0Address(range.address) < range.bytes) {
      return;
    }
  }
  reads_->note(word, core_->mem_r32(word));
}

} // namespace psx::present
