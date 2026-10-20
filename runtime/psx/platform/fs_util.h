// Host file I/O; nothing else in the product opens a FILE* or makes directories.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct Fs {
  static bool exists(const std::string &path);

  // Creates the directories for path's PARENT (std::filesystem::create_directories); returns
  // true on success or if they already exist.
  static bool ensureParentDirs(const std::string &path);

  // Creates the parent directories; false with `error` set on failure, leaving no partial file.
  static bool writeFile(const std::string &path, const void *data, size_t size, std::string &error);

  // The whole file; nullopt with `error` set when it cannot be opened or read. An empty file is empty.
  static std::optional<std::vector<uint8_t>> readFile(const std::string &path, std::string &error);

  // First entry in `dir` whose extension case-insensitively equals `ext` (e.g. ".chd"); empty
  // string if none. If several match, returns the first (directory-iteration order) and logs a
  // one-line logger warning naming the chosen one.
  static std::string findFirstWithExtension(const std::string &dir, const std::string &ext);
};
