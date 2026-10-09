// emit_memory.h — the memory a packet-emitting body reads and writes: the guest's, or host bytes a render runs
// the same body over so the guest stays untouched.
#pragma once

#include "core.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <span>
#include <vector>

namespace psx::present {

// Host bytes standing in for ranges of guest memory. A read outside every range falls through to the
// guest (level data the guest does not change while an object lives); a write outside them aborts.
class HostMemory {
public:
  // A range holding a copy of `bytes`; aborts when it overlaps one already held.
  void provide(std::uint32_t address, std::span<const std::byte> bytes);
  // A range of `size` zero bytes.
  void zero(std::uint32_t address, std::uint32_t size);
  // False when `address` lies in no range; aborts when the access runs past the range's end.
  bool read(std::uint32_t address, void *out, std::uint32_t size) const;
  void write(std::uint32_t address, const void *in, std::uint32_t size);
  // The bytes of the range holding [address, address + size); aborts when none does.
  std::span<const std::byte> view(std::uint32_t address, std::uint32_t size) const;

private:
  using Range = std::vector<std::byte>;
  void add(std::uint32_t address, std::vector<std::byte> bytes);
  // The range holding `address` and its physical base, or null.
  const Range *find(std::uint32_t address, std::uint32_t &base) const;
  Range *find(std::uint32_t address, std::uint32_t &base);

  std::map<std::uint32_t, Range> ranges_; // by physical base
};

// The accessors of Core a body uses, over the guest or over a HostMemory.
class EmitMemory {
public:
  EmitMemory(Core &core) : core_(&core) {}
  EmitMemory(Core &core, HostMemory &host) : core_(&core), host_(&host) {}

  Core &core() const {
    return *core_;
  }
  bool hosted() const {
    return host_ != nullptr;
  }

  std::uint8_t mem_r8(std::uint32_t address) const {
    return read<std::uint8_t>(address);
  }
  std::uint16_t mem_r16(std::uint32_t address) const {
    return read<std::uint16_t>(address);
  }
  std::uint32_t mem_r32(std::uint32_t address) const {
    return read<std::uint32_t>(address);
  }
  std::int32_t mem_r8s(std::uint32_t address) const {
    return static_cast<std::int8_t>(mem_r8(address));
  }
  std::int32_t mem_r16s(std::uint32_t address) const {
    return static_cast<std::int16_t>(mem_r16(address));
  }
  void mem_w8(std::uint32_t address, std::uint8_t value) const {
    write(address, value);
  }
  void mem_w16(std::uint32_t address, std::uint16_t value) const {
    write(address, value);
  }
  void mem_w32(std::uint32_t address, std::uint32_t value) const {
    write(address, value);
  }

  // The GTE screen point `reg` into the packet word at `address`; a render leaves the guest's provenance alone.
  void storeGteXy(std::uint32_t address, int reg) const {
    if (host_ != nullptr) {
      mem_w32(address, gte_read_data(static_cast<std::uint32_t>(reg)));
    } else {
      gte_store_xy(core_, address, reg);
    }
  }

private:
  template <class Value> Value read(std::uint32_t address) const {
    if (host_ != nullptr) {
      Value value;
      if (host_->read(address, &value, sizeof(Value))) {
        return value;
      }
    }
    if constexpr (sizeof(Value) == 1) {
      return core_->mem_r8(address);
    } else if constexpr (sizeof(Value) == 2) {
      return core_->mem_r16(address);
    } else {
      return core_->mem_r32(address);
    }
  }
  template <class Value> void write(std::uint32_t address, Value value) const {
    if (host_ != nullptr) {
      host_->write(address, &value, sizeof(Value));
    } else if constexpr (sizeof(Value) == 1) {
      core_->mem_w8(address, value);
    } else if constexpr (sizeof(Value) == 2) {
      core_->mem_w16(address, value);
    } else {
      core_->mem_w32(address, value);
    }
  }

  Core *core_;
  HostMemory *host_ = nullptr;
};

// One element of the innermost open object while a body runs over the guest; nothing in a render, and nothing
// when no object scope is open (the body's stores stay unbound).
class ElementScope {
public:
  ElementScope(const EmitMemory &memory, std::uint32_t element) {
    if (!memory.hosted() && memory.core().emission.isOpen()) {
      guard_ = new (storage_) EmissionScope::Guard(memory.core().emission.element(element));
    }
  }
  ~ElementScope() {
    if (guard_ != nullptr) {
      guard_->~Guard();
    }
  }
  ElementScope(const ElementScope &) = delete;
  ElementScope &operator=(const ElementScope &) = delete;
  ElementScope(ElementScope &&) = delete;
  ElementScope &operator=(ElementScope &&) = delete;

private:
  alignas(EmissionScope::Guard) std::byte storage_[sizeof(EmissionScope::Guard)];
  EmissionScope::Guard *guard_ = nullptr;
};

} // namespace psx::present
