#pragma once

#include "testutil.h"

#include <filesystem>
#include <string>

namespace psx::test {

// PSXPORT_SOURCE_DIR comes from tests/CMakeLists.txt; __FILE__ is relative under ccache base_dir.
inline std::filesystem::path sourceRoot() {
  return {PSXPORT_SOURCE_DIR};
}

inline std::filesystem::path sourcePath(const std::filesystem::path &relative) {
  return sourceRoot() / relative;
}

// A psxport file by its repository-relative path; empty when it cannot be read.
inline std::string readRepoFile(const std::string &relative) {
  return readFile(sourcePath(relative).string());
}

} // namespace psx::test
