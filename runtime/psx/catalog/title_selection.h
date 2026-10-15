#pragma once

#include "title_catalog.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace psx::cd {
class Disc;
}

namespace psx::host {

enum class SelectionStatus : std::uint8_t {
  Selected,
  UnsupportedSerial,
  MissingExecutable,
  InvalidExecutable,
  IdentityMismatch,
};

struct SelectionResult {
  SelectionStatus status;
  const TitleIdentity *identity;
  std::string detail;
  std::size_t index = 0; // catalog position of `identity`; meaningful when identity is set
  // selectExecutableFile only: the bytes it authenticated, when Selected; what the session admits.
  std::vector<std::uint8_t> bytes;

  explicit operator bool() const {
    return status == SelectionStatus::Selected;
  }
};

SelectionResult
selectExecutable(std::string_view serial, std::span<const std::uint8_t> bytes, std::span<const TitleIdentity> catalog);
SelectionResult selectExecutableFile(const std::filesystem::path &path, std::span<const TitleIdentity> catalog);
// `title`'s `discExecutablePath` read from `disc` and authenticated; `bytes` holds it when Selected.
SelectionResult selectDiscExecutable(psx::cd::Disc &disc, const TitleIdentity &title);

} // namespace psx::host
