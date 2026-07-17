// fs_util.h — shared host-filesystem utilities (std::filesystem wrappers).
// PC-native host I/O goes through here — never hand-rolled FILE*/mkdir/dirent code
// (USER directive 2026-07-14). Pure utility: static members on a struct (like Math::), no
// free functions, no globals.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Fs {
  static bool exists(const std::string& path);

  // Creates the directories for path's PARENT (std::filesystem::create_directories); returns
  // true on success or if they already exist.
  static bool ensureParentDirs(const std::string& path);

  // ensureParentDirs(path) + write `size` bytes via a binary ofstream. Returns false on any
  // failure (open/write/close), removing a partially-written file in that case.
  static bool writeFile(const std::string& path, const void* data, size_t size);

  // Reads the whole file into a byte vector; empty vector on failure (missing file, read error).
  static std::vector<uint8_t> readFile(const std::string& path);

  // First entry in `dir` whose extension case-insensitively equals `ext` (e.g. ".chd"); empty
  // string if none. If several match, returns the first (directory-iteration order) and logs a
  // one-line warning to stderr naming the chosen one.
  static std::string findFirstWithExtension(const std::string& dir, const std::string& ext);
};
