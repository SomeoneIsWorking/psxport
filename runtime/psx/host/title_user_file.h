// One file in a title's user-data directory (`<user data>/<slug>/<name>`), replaced whole.
#pragma once

#include <filesystem>
#include <functional>
#include <ios>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace psx::host {

struct TitleIdentity;

class TitleUserFile {
public:
  TitleUserFile(const TitleIdentity &title, std::string_view name);

  // Absent when the platform gives the title no user-data directory.
  const std::optional<std::filesystem::path> &path() const {
    return file_;
  }
  // Writes `write`'s output beside the file and renames it over the old one; the reason when it cannot.
  std::optional<std::string> replace(std::ios::openmode mode, const std::function<void(std::ostream &)> &write) const;

private:
  std::optional<std::filesystem::path> file_;
};

} // namespace psx::host
