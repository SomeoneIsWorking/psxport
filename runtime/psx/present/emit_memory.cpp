#include "emit_memory.h"

#include "guest_memory_map.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

namespace psx::present {

std::vector<LevelWord> LevelReads::words() const {
  std::vector<LevelWord> words;
  words.reserve(words_.size());
  for (const auto &[address, value] : words_) {
    words.push_back(LevelWord{address, value});
  }
  return words;
}

const HostMemory::Range *HostMemory::find(std::uint32_t address, std::uint32_t &base) const {
  const std::uint32_t at = kseg0Address(address);
  auto next = ranges_.upper_bound(at);
  if (next == ranges_.begin()) {
    return nullptr;
  }
  const auto held = std::prev(next);
  if (at - held->first >= held->second.size()) {
    return nullptr;
  }
  base = held->first;
  return &held->second;
}

HostMemory::Range *HostMemory::find(std::uint32_t address, std::uint32_t &base) {
  return const_cast<Range *>(std::as_const(*this).find(address, base));
}

std::byte *HostMemory::place(std::uint32_t base, std::uint32_t size) {
  std::uint32_t mergedBase = base;
  std::uint32_t mergedEnd = base + size;
  auto first = ranges_.lower_bound(base);
  if (first != ranges_.begin() && std::prev(first)->first + std::prev(first)->second.size() >= base) {
    first = std::prev(first);
  }
  auto last = first;
  while (last != ranges_.end() && last->first <= mergedEnd) {
    mergedBase = std::min(mergedBase, last->first);
    mergedEnd = std::max(mergedEnd, last->first + static_cast<std::uint32_t>(last->second.size()));
    ++last;
  }
  if (first == last || first->first != mergedBase || std::next(first) != last ||
      first->second.size() != mergedEnd - mergedBase) {
    Range merged(mergedEnd - mergedBase);
    for (auto held = first; held != last; ++held) {
      std::copy(held->second.begin(), held->second.end(), merged.begin() + (held->first - mergedBase));
    }
    ranges_.erase(first, last);
    first = ranges_.emplace(mergedBase, std::move(merged)).first;
  }
  return first->second.data() + (base - mergedBase);
}

void HostMemory::provide(std::uint32_t address, std::span<const std::byte> bytes) {
  std::memcpy(place(kseg0Address(address), static_cast<std::uint32_t>(bytes.size())), bytes.data(), bytes.size());
}

void HostMemory::zero(std::uint32_t address, std::uint32_t size) {
  std::memset(place(kseg0Address(address), size), 0, size);
}

void HostMemory::mapGuest(Core &guest, std::uint32_t address, std::uint32_t size) {
  const std::uint32_t base = kseg0Address(address);
  const std::uint32_t end = base + size;
  // The parts no range holds yet, found before the merge hides them.
  std::vector<GuestSpan> gaps;
  std::uint32_t at = base;
  auto held = ranges_.upper_bound(base);
  if (held != ranges_.begin()) {
    held = std::prev(held);
  }
  for (; held != ranges_.end() && held->first < end && at < end; ++held) {
    const std::uint32_t heldEnd = held->first + static_cast<std::uint32_t>(held->second.size());
    if (heldEnd <= at) {
      continue;
    }
    if (held->first > at) {
      gaps.push_back({at, held->first - at});
    }
    at = std::max(at, heldEnd);
  }
  if (at < end) {
    gaps.push_back({at, end - at});
  }
  std::byte *bytes = place(base, size);
  for (const GuestSpan &gap : gaps) {
    std::byte *out = bytes + (gap.address - base);
    if (const std::optional<GuestAddressRange> ram = guest.mappedMainRamRange(gap.address, gap.bytes)) {
      std::memcpy(out, guest.ram + ram->begin, gap.bytes);
    } else {
      for (std::uint32_t i = 0; i < gap.bytes; ++i) {
        out[i] = static_cast<std::byte>(guest.mem_r8(gap.address + i));
      }
    }
    const std::uint32_t gapEnd = gap.address + gap.bytes;
    for (auto word = overlay_.lower_bound(gap.address & ~3u); word != overlay_.end() && word->first < gapEnd;
         ++word) {
      for (std::uint32_t byte = 0; byte < 4u; ++byte) {
        const std::uint32_t target = word->first + byte;
        if (target >= gap.address && target < gapEnd) {
          out[target - gap.address] = static_cast<std::byte>(word->second >> (8u * byte));
        }
      }
    }
  }
}

void HostMemory::overlay(std::span<const LevelWord> words) {
  for (const LevelWord &word : words) {
    overlay_[kseg0Address(word.address) & ~3u] = word.value;
  }
}

const std::uint32_t *HostMemory::overlaid(std::uint32_t wordAddress) const {
  const auto found = overlay_.find(wordAddress);
  return found == overlay_.end() ? nullptr : &found->second;
}

bool HostMemory::read(std::uint32_t address, void *out, std::uint32_t size) const {
  std::uint32_t base = 0;
  const Range *range = find(address, base);
  if (range == nullptr) {
    return false;
  }
  const std::uint32_t offset = kseg0Address(address) - base;
  if (offset + size > range->size()) {
    lucent::error("emit-memory", "a {}-byte read at 0x{:08X} runs past its host range", size, address);
    std::abort();
  }
  std::memcpy(out, range->data() + offset, size);
  return true;
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

void HostMemory::write(std::uint32_t address, const void *in, std::uint32_t size) {
  std::uint32_t base = 0;
  Range *range = find(address, base);
  if (range == nullptr) {
    lucent::error("emit-memory", "a render wrote {} bytes to 0x{:08X}, outside its host ranges", size, address);
    std::abort();
  }
  const std::uint32_t offset = kseg0Address(address) - base;
  if (offset + size > range->size()) {
    lucent::error("emit-memory", "a {}-byte write at 0x{:08X} runs past its host range", size, address);
    std::abort();
  }
  std::memcpy(range->data() + offset, in, size);
}

std::span<const std::byte> HostMemory::view(std::uint32_t address, std::uint32_t size) const {
  std::uint32_t base = 0;
  const Range *range = find(address, base);
  const std::uint32_t offset = range != nullptr ? kseg0Address(address) - base : 0u;
  if (range == nullptr || offset + size > range->size()) {
    lucent::error("emit-memory", "no host range holds {} bytes at 0x{:08X}", size, address);
    std::abort();
  }
  return std::span<const std::byte>(*range).subspan(offset, size);
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
