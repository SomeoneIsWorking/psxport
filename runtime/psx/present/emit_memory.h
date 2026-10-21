// emit_memory.h — the memory a packet-emitting body reads and writes: the guest's, or host bytes a render runs
// the same body over so the guest stays untouched.
#pragma once

#include "core.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <vector>

namespace psx::present {

// A guest word as a run first read it.
struct LevelWord {
  std::uint32_t address = 0;
  std::uint32_t value = 0;
};

// The guest words a run read, each with the value it held at the first read. Kept across runs and cleared
// per run, so a steady stream of runs allocates nothing.
class LevelReads {
public:
  void note(std::uint32_t wordAddress, std::uint32_t value);
  // In first-read order.
  std::span<const LevelWord> words() const {
    return words_;
  }
  void clear();

private:
  // Open addressing over words_ indices; a slot is live when its stamp is the current one.
  struct Slot {
    std::uint32_t stamp = 0;
    std::uint32_t word = 0;
  };
  void grow();

  std::vector<LevelWord> words_;
  std::vector<Slot> slots_;
  std::uint32_t stamp_ = 1;
};

// Aborts: a kept scratch object was taken while a use already held it.
[[noreturn]] void abortKeptScratchInUse();

// A scratch object kept across runs, so a steady stream of runs allocates nothing; one run holds it at a time.
template <class Scratch> class Kept {
public:
  // The scratch, cleared, until the use ends; aborts while another use holds it.
  class Use {
  public:
    explicit Use(Kept &kept) : kept_(kept) {
      if (kept_.inUse_) {
        abortKeptScratchInUse();
      }
      kept_.inUse_ = true;
      kept_.scratch_.clear();
    }
    ~Use() {
      kept_.inUse_ = false;
    }
    Use(const Use &) = delete;
    Use &operator=(const Use &) = delete;
    Use(Use &&) = delete;
    Use &operator=(Use &&) = delete;

    Scratch &operator*() const {
      return kept_.scratch_;
    }
    Scratch *operator->() const {
      return &kept_.scratch_;
    }

  private:
    Kept &kept_;
  };

private:
  Scratch scratch_;
  bool inUse_ = false;
};

// Host bytes standing in for ranges of guest memory, one store under every name of an address. Ranges that
// overlap or touch merge. A read outside every range falls through to the guest (level data the guest does
// not change while an object lives), or to an overlaid word; a write outside them aborts.
class HostMemory {
public:
  HostMemory() = default;
  ~HostMemory() = default;
  // The last-lookup cache points into this object's own ranges.
  HostMemory(const HostMemory &) = delete;
  HostMemory &operator=(const HostMemory &) = delete;
  HostMemory(HostMemory &&) = delete;
  HostMemory &operator=(HostMemory &&) = delete;

  // Drops every range, overlay and noted read, keeping the storage.
  void clear();
  // A range holding a copy of `bytes`.
  void provide(std::uint32_t address, std::span<const std::byte> bytes);
  // A range of `size` zero bytes.
  void zero(std::uint32_t address, std::uint32_t size);
  // A range of `size` bytes seeded from the guest (through the overlay) where no range already holds them.
  void mapGuest(Core &guest, std::uint32_t address, std::uint32_t size);
  // Fall-through reads of these words, packed LevelWords, take the overlaid value in place of the guest's; a
  // later value for a word replaces the earlier one.
  void overlay(std::span<const std::byte> packedWords);
  // From now on, every word a read falls through to is noted with the value it gave.
  void keepLevelReads() {
    keepsLevelReads_ = true;
  }
  const LevelReads &levelReads() const {
    return levelReads_;
  }

  // False when `address` lies in no range; aborts when the access runs past the range's end.
  bool read(std::uint32_t address, void *out, std::uint32_t size) const;
  // A read in no range, from the overlay or the guest.
  void fallThrough(Core &guest, std::uint32_t address, void *out, std::uint32_t size) const;
  void write(std::uint32_t address, const void *in, std::uint32_t size);
  // The bytes of the range holding [address, address + size); aborts when none does.
  std::span<const std::byte> view(std::uint32_t address, std::uint32_t size) const;

private:
  using Range = std::vector<std::byte>;
  struct Held {
    std::uint32_t base = 0; // kseg0
    Range bytes;
  };
  struct OverlayNote {
    std::uint32_t address = 0;
    std::uint32_t order = 0;
    std::uint32_t value = 0;
  };
  // [base, base + size) at kseg0 `base`, merged with every range it overlaps or touches, which keep their
  // bytes; returns the merged range's bytes from `base` on.
  std::byte *place(std::uint32_t base, std::uint32_t size);
  // The range holding `address` and its base, or null.
  const Range *find(std::uint32_t address, std::uint32_t &base) const;
  Range *find(std::uint32_t address, std::uint32_t &base);
  const std::uint32_t *overlaid(std::uint32_t wordAddress) const;
  // A buffer of `size` bytes from the spares.
  Range takeBuffer(std::size_t size);

  std::vector<Held> ranges_; // by base
  std::vector<Range> spares_;
  // The range the last lookup found; a body reads one range for long runs. Cleared when ranges change.
  mutable const Range *lastRange_ = nullptr;
  mutable std::uint32_t lastBase_ = 0;
  std::vector<LevelWord> overlay_; // by kseg0 word address, one entry per word
  std::vector<OverlayNote> overlayNotes_;
  std::vector<GuestSpan> gaps_;
  bool keepsLevelReads_ = false;
  mutable LevelReads levelReads_;
};

// The accessors of Core a body uses, over the guest or over a HostMemory. Over the guest it may note each
// first read outside `working`, the ranges the body owns, as the level data it drew from.
class EmitMemory {
public:
  EmitMemory(Core &core) : core_(&core) {}
  EmitMemory(Core &core, HostMemory &host) : core_(&core), host_(&host) {}
  EmitMemory(Core &core, LevelReads &reads, std::span<const GuestSpan> working)
      : core_(&core), reads_(&reads), working_(working) {}

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
  // Consecutive words from `address`; one copy when a host range holds the first, which must then hold all.
  void readWords(std::uint32_t address, std::span<std::uint32_t> out) const;
  void mem_w8(std::uint32_t address, std::uint8_t value) const {
    write(address, value);
  }
  void mem_w16(std::uint32_t address, std::uint16_t value) const {
    write(address, value);
  }
  void mem_w32(std::uint32_t address, std::uint32_t value) const {
    write(address, value);
  }

  // The GTE screen point `reg` into the packet word at `address`.
  void storeGteXy(std::uint32_t address, int reg) const {
    mem_w32(address, core_->gte().readData(static_cast<std::uint32_t>(reg)));
  }

private:
  template <class Value> Value read(std::uint32_t address) const {
    Value value;
    if (host_ != nullptr) {
      if (!host_->read(address, &value, sizeof(Value))) {
        host_->fallThrough(*core_, address, &value, sizeof(Value));
      }
      return value;
    }
    if (reads_ != nullptr) {
      noteRead(address);
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
  void noteRead(std::uint32_t address) const;

  Core *core_;
  HostMemory *host_ = nullptr;
  LevelReads *reads_ = nullptr;
  std::span<const GuestSpan> working_;
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
