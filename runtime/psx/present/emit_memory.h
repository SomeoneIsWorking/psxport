// emit_memory.h — the memory a packet-emitting body reads and writes: the guest's, or host bytes a render runs
// the same body over so the guest stays untouched.
#pragma once

#include "core.h"
#include "projected_vertex.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace psx::present {

// Host bytes standing in for ranges of guest memory, one store under every name of an address. Ranges that
// overlap or touch merge. A read outside every range falls through to the guest; a write outside them aborts. A word a
// vertex was stored into keeps the vertex's place until another write covers it.
class HostMemory {
public:
  HostMemory() = default;
  ~HostMemory() = default;
  // The last-lookup cache points into this object's own ranges.
  HostMemory(const HostMemory &) = delete;
  HostMemory &operator=(const HostMemory &) = delete;
  HostMemory(HostMemory &&) = delete;
  HostMemory &operator=(HostMemory &&) = delete;

  // Drops every range, keeping the storage.
  void clear();
  // A range holding a copy of `bytes`.
  void provide(std::uint32_t address, std::span<const std::byte> bytes);
  // A range of `size` zero bytes.
  void zero(std::uint32_t address, std::uint32_t size);

  // False when `address` lies in no range; aborts when the access runs past the range's end.
  bool read(std::uint32_t address, void *out, std::uint32_t size) const;
  // A read in no range, from the guest.
  void fallThrough(Core &guest, std::uint32_t address, void *out, std::uint32_t size) const;
  void write(std::uint32_t address, const void *in, std::uint32_t size);
  // The vertex's word at `address`, which keeps the vertex's place; aborts outside every range.
  void writeVertex(std::uint32_t address, const gte::ProjectedVertex &vertex);
  // The place of each word from `address`, none where no vertex was stored or no range holds the word.
  void readPlaces(std::uint32_t address, std::span<gte::WordPlace> out) const;
  // The bytes of the range holding [address, address + size); aborts when none does.
  std::span<const std::byte> view(std::uint32_t address, std::uint32_t size) const;

private:
  using Range = std::vector<std::byte>;
  // One entry per word of a range from its base's word, or empty while the range holds no vertex.
  using Places = std::vector<gte::WordPlace>;
  struct Held {
    std::uint32_t base = 0; // kseg0
    Range bytes;
    Places places;
  };
  // [base, base + size) at kseg0 `base`, merged with every range it overlaps or touches, which keep their
  // bytes; returns the merged range's bytes from `base` on.
  std::byte *place(std::uint32_t base, std::uint32_t size);
  // The range holding `address` and its base, or null.
  const Held *find(std::uint32_t address) const;
  Held *find(std::uint32_t address);
  // A buffer of `size` bytes from the spares.
  Range takeBuffer(std::size_t size);
  // `held`'s places, one per word, taken from the spares the first time a vertex is stored there.
  Places &placesOf(Held &held);
  // Drops the places of the words [kseg0, kseg0 + size) touches.
  static void forgetPlaces(Held &held, std::uint32_t kseg0, std::uint32_t size);
  // The range a write of `size` bytes at `address` lands in; aborts outside every range or past its end.
  Held &writableRange(std::uint32_t address, std::uint32_t size);

  std::vector<Held> ranges_; // by base
  std::vector<Range> spares_;
  std::vector<Places> placeSpares_;
  // The range the last lookup found; a body reads one range for long runs. Cleared when ranges change.
  mutable const Held *lastRange_ = nullptr;
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
  // Consecutive words from `address`; one copy when a host range holds the first, which must then hold all.
  void readWords(std::uint32_t address, std::span<std::uint32_t> out) const;
  // The places of the words from `address`: host memory's, or over the guest those `VertexPlaces` keeps.
  void readPlaces(std::uint32_t address, std::span<gte::WordPlace> out) const;
  void mem_w8(std::uint32_t address, std::uint8_t value) const {
    write(address, value);
  }
  void mem_w16(std::uint32_t address, std::uint16_t value) const {
    write(address, value);
  }
  void mem_w32(std::uint32_t address, std::uint32_t value) const {
    write(address, value);
  }

  // A vertex into the word at `address`, its place kept by host memory or `VertexPlaces` under the running override.
  void storeVertex(std::uint32_t address, const gte::ProjectedVertex &vertex) const {
    if (host_ != nullptr) {
      host_->writeVertex(address, vertex);
    } else {
      mem_w32(address, vertex.word());
      core_->vertexPlaces.note(address, vertex.word(), {.place = vertex.place(), .site = core_->active_native_address});
    }
  }
  // The function that placed every placed one of `words`, read from `address` on (VertexPlaces); 0 over host memory,
  // where none is placed or where two functions placed them.
  [[nodiscard]] std::uint32_t placingSite(std::uint32_t address, std::span<const std::uint32_t> words) const;
  // The vertex a body stored at `address`, on its word's pixel where no place was kept.
  [[nodiscard]] gte::ProjectedVertex loadVertex(std::uint32_t address) const {
    return loadVertex(address, gte::VertexWord{mem_r32(address)});
  }
  // `word` at the place kept at `address`, whose word may be another encoding of it (an outcode); on its own pixel
  // where no place was kept.
  [[nodiscard]] gte::ProjectedVertex loadVertex(std::uint32_t address, gte::VertexWord word) const {
    gte::WordPlace place;
    readPlaces(address, std::span(&place, 1));
    return place ? gte::ProjectedVertex(word.value, *place) : gte::ProjectedVertex::whole(word.value);
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

} // namespace psx::present
