#pragma once

#include "title_catalog.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

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

  explicit operator bool() const {
    return status == SelectionStatus::Selected;
  }
};

SelectionResult
selectExecutable(std::string_view serial, std::span<const std::uint8_t> bytes, std::span<const TitleIdentity> catalog);
SelectionResult selectExecutableFile(const std::filesystem::path &path, std::span<const TitleIdentity> catalog);

} // namespace psx::host
