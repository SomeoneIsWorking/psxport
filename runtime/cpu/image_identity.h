#pragma once

#include "guest_program_image.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace psx::cpu {

struct ImageIdentity {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;

  friend constexpr bool operator==(ImageIdentity lhs, ImageIdentity rhs) {
    return lhs.id == rhs.id && lhs.generation == rhs.generation;
  }
};

struct NativeKey {
  ImageIdentity image;
  std::uint32_t address = 0;

  friend constexpr bool operator==(NativeKey lhs, NativeKey rhs) {
    return lhs.image == rhs.image && lhs.address == rhs.address;
  }
};

class ImageCatalog {
public:
  ImageIdentity activate(std::string_view name, GuestAddressRange range, std::uint64_t contentIdentity);
  bool deactivate(ImageIdentity identity);
  std::optional<ImageIdentity> resolve(std::uint32_t guestAddress) const;
  std::size_t activeCount() const;

private:
  struct Entry {
    std::string name;
    GuestAddressRange range;
    std::uint64_t contentIdentity = 0;
    ImageIdentity identity;
    bool active = false;
  };

  std::vector<Entry> entries_;
  std::uint64_t nextId_ = 1;
  std::uint64_t nextGeneration_ = 1;
};

} // namespace psx::cpu
