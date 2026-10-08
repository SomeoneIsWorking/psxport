#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

class GameRuntime;

namespace psx::host {

// One catalog title. What a selection means: serial plus file identity, never the label.
struct TitleIdentity {
  std::string_view displayName;
  std::string_view serial;
  std::string_view slug; // directory under the provisioning root: <root>/<slug>/<serial>
  std::size_t fileSize;
  std::string_view sha256;
  // PS-X EXE header cross-check.
  std::uint32_t entry;
  std::uint32_t globalPointer;
  std::uint32_t textAddress;
  std::uint32_t textSize;
  std::uint32_t stackAddress;
  std::uint32_t stackOffset;
};

// What a multi-title product supplies. Index i is catalog entry i everywhere.
class TitleCatalog {
public:
  virtual ~TitleCatalog() = default;
  virtual std::string_view productName() const = 0; // the window title
  virtual std::span<const TitleIdentity> titles() const = 0;
  // Installed before that title's Game exists.
  virtual GameRuntime &runtime(std::size_t index) const = 0;
};

} // namespace psx::host
