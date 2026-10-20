#include "fs_util.h"

#include <lucent/log.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

bool Fs::exists(const std::string &path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

bool Fs::ensureParentDirs(const std::string &path) {
  fs::path p(path);
  fs::path parent = p.parent_path();
  if (parent.empty()) {
    return true; // no directory component — nothing to create
  }
  std::error_code ec;
  fs::create_directories(parent, ec);
  return !ec;
}

bool Fs::writeFile(const std::string &path, const void *data, size_t size, std::string &error) {
  if (!ensureParentDirs(path)) {
    error = "cannot create the directory for '" + path + "'";
    return false;
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    error = "cannot open '" + path + "' for writing";
    return false;
  }
  if (size) {
    f.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
  }
  bool ok = static_cast<bool>(f);
  f.close();
  ok = ok && static_cast<bool>(f);
  if (!ok) {
    std::error_code ec;
    fs::remove(path, ec);
    error = "writing " + std::to_string(size) + " bytes to '" + path + "' failed";
    return false;
  }
  return true;
}

std::optional<std::vector<uint8_t>> Fs::readFile(const std::string &path, std::string &error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    error = "cannot open '" + path + "' for reading";
    return std::nullopt;
  }
  const std::streamsize n = f.tellg();
  if (n < 0) {
    error = "cannot size '" + path + "'";
    return std::nullopt;
  }
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(static_cast<size_t>(n));
  if (n && !f.read(reinterpret_cast<char *>(buf.data()), n)) {
    error = "read of '" + path + "' failed part way through";
    return std::nullopt;
  }
  return buf;
}

std::string Fs::findFirstWithExtension(const std::string &dir, const std::string &ext) {
  auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return (char)std::tolower(c);
    });
    return s;
  };
  std::string wantExt = lower(ext);
  std::error_code ec;
  std::string found;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    if (lower(entry.path().extension().string()) != wantExt) {
      continue;
    }
    if (found.empty()) {
      found = entry.path().string();
      continue;
    }
    lucent::warn("fs", "multiple {} candidates in {}; using {}", ext, dir, found);
    break;
  }
  return found;
}
