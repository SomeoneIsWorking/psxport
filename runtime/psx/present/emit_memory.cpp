// emit_memory.cpp — HostMemory. See emit_memory.h.
#include "emit_memory.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

namespace psx::present {
namespace {

// Guest addresses reach the same byte through KUSEG, KSEG0 and KSEG1.
constexpr std::uint32_t kPhysicalMask = 0x1FFFFFFFu;

std::uint32_t physical(std::uint32_t address) {
  return address & kPhysicalMask;
}

} // namespace

const HostMemory::Range *HostMemory::find(std::uint32_t address, std::uint32_t &base) const {
  const std::uint32_t at = physical(address);
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

void HostMemory::add(std::uint32_t address, std::vector<std::byte> bytes) {
  const std::uint32_t base = physical(address);
  const auto next = ranges_.lower_bound(base);
  const bool overlapsNext = next != ranges_.end() && next->first < base + bytes.size();
  const bool overlapsPrevious =
      next != ranges_.begin() && base - std::prev(next)->first < std::prev(next)->second.size();
  if (overlapsNext || overlapsPrevious) {
    lucent::error("emit-memory", "host range at 0x{:08X} overlaps one already held", address);
    std::abort();
  }
  ranges_.emplace(base, std::move(bytes));
}

void HostMemory::provide(std::uint32_t address, std::span<const std::byte> bytes) {
  add(address, std::vector<std::byte>(bytes.begin(), bytes.end()));
}

void HostMemory::zero(std::uint32_t address, std::uint32_t size) {
  add(address, std::vector<std::byte>(size));
}

bool HostMemory::read(std::uint32_t address, void *out, std::uint32_t size) const {
  std::uint32_t base = 0;
  const Range *range = find(address, base);
  if (range == nullptr) {
    return false;
  }
  const std::uint32_t offset = physical(address) - base;
  if (offset + size > range->size()) {
    lucent::error("emit-memory", "a {}-byte read at 0x{:08X} runs past its host range", size, address);
    std::abort();
  }
  std::memcpy(out, range->data() + offset, size);
  return true;
}

void HostMemory::write(std::uint32_t address, const void *in, std::uint32_t size) {
  std::uint32_t base = 0;
  Range *range = find(address, base);
  if (range == nullptr) {
    lucent::error("emit-memory", "a render wrote {} bytes to 0x{:08X}, outside its host ranges", size, address);
    std::abort();
  }
  const std::uint32_t offset = physical(address) - base;
  if (offset + size > range->size()) {
    lucent::error("emit-memory", "a {}-byte write at 0x{:08X} runs past its host range", size, address);
    std::abort();
  }
  std::memcpy(range->data() + offset, in, size);
}

std::span<const std::byte> HostMemory::view(std::uint32_t address, std::uint32_t size) const {
  std::uint32_t base = 0;
  const Range *range = find(address, base);
  const std::uint32_t offset = range != nullptr ? physical(address) - base : 0u;
  if (range == nullptr || offset + size > range->size()) {
    lucent::error("emit-memory", "no host range holds {} bytes at 0x{:08X}", size, address);
    std::abort();
  }
  return std::span<const std::byte>(*range).subspan(offset, size);
}

} // namespace psx::present
