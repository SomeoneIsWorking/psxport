#include "state_file.h"

#include "state_blob.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace psx::state {
namespace {

const char *const kFrameworkSectionNames[] = {
    section::kCpu.data(),
    section::kRam.data(),
    section::kScratchpad.data(),
    section::kGte.data(),
    section::kGpu.data(),
    section::kBeetleSpu.data(),
    section::kBeetleMdec.data(),
    section::kCdc.data(),
    section::kTiming.data(),
    section::kDma.data(),
    section::kHle.data(),
    section::kSio.data(),
    section::kCard.data(),
    section::kCd.data(),
    section::kTitle.data(),
};
static_assert(std::size(kFrameworkSectionNames) == 15, "every namespace section must be listed once");

} // namespace

const std::vector<std::string> &frameworkSectionNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    out.reserve(std::size(kFrameworkSectionNames));
    for (const char *name : kFrameworkSectionNames) {
      out.emplace_back(name);
    }
    return out;
  }();
  return names;
}

bool isFrameworkSectionName(std::string_view name) {
  const auto &names = frameworkSectionNames();
  return std::find(names.begin(), names.end(), name) != names.end();
}

bool StateImage::add(std::string_view name, std::span<const std::uint8_t> payload) {
  if (name.empty() || name.size() >= kSectionNameBytes) {
    return false;
  }
  if (std::find(mNames.begin(), mNames.end(), name) != mNames.end()) {
    return false; // a duplicate name would make the restored machine depend on scan order
  }
  if (payload.size() > 0xFFFFFFFFull) {
    return false;
  }
  if (mNames.empty()) {
    BlobWriter header;
    header.bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(kStateMagic), 8));
    header.u32(kFormatVersion);
    // The section count is patched below, so write a placeholder the patch step overwrites.
    header.u32(0);
    mBytes = header.take();
  }
  std::array<char, kSectionNameBytes> padded{};
  std::memcpy(padded.data(), name.data(), name.size());
  mBytes.insert(mBytes.end(), padded.begin(), padded.end());
  BlobWriter tail;
  tail.u32(static_cast<std::uint32_t>(payload.size()));
  const auto tailBytes = tail.bytesOut();
  mBytes.insert(mBytes.end(), tailBytes.begin(), tailBytes.end());
  mBytes.insert(mBytes.end(), payload.begin(), payload.end());
  mNames.emplace_back(name);
  // Patch the section count in place: bytes [8, 12) are the version, [12, 16) the count.
  BlobWriter count;
  count.u32(static_cast<std::uint32_t>(mNames.size()));
  const auto countBytes = count.bytesOut();
  std::memcpy(mBytes.data() + 12, countBytes.data(), countBytes.size());
  return true;
}

std::vector<std::string> StateImage::names() const {
  return mNames;
}

std::optional<StateFile> StateFile::open(std::vector<std::uint8_t> bytes, OpenError &error) {
  error.reason.clear();
  if (bytes.size() < 16) {
    error.reason = "shorter than the 16-byte header";
    return std::nullopt;
  }
  if (std::memcmp(bytes.data(), kStateMagic, 8) != 0) {
    error.reason = "magic is not PSXSTATE";
    return std::nullopt;
  }
  BlobReader head(std::span<const std::uint8_t>(bytes.data(), 16));
  head.u32(); // skip the magic, already compared above
  head.u32();
  const std::uint32_t version = head.u32();
  const std::uint32_t count = head.u32();
  if (!head.ok()) {
    error.reason = "the header did not decode";
    return std::nullopt;
  }
  if (version != kFormatVersion) {
    error.reason =
        "format version " + std::to_string(version) + ", this build writes " + std::to_string(kFormatVersion);
    return std::nullopt;
  }
  if (count == 0 || count > kMaxSectionCount) {
    error.reason = "section count " + std::to_string(count) + " outside 1.." + std::to_string(kMaxSectionCount);
    return std::nullopt;
  }

  StateFile file;
  file.mBytes = std::move(bytes);
  file.mVersion = version;
  std::size_t offset = 16;
  for (std::uint32_t index = 0; index < count; ++index) {
    if (file.mBytes.size() - offset < kSectionNameBytes + 4) {
      error.reason =
          "section " + std::to_string(index) + " of " + std::to_string(count) + " starts past the end of the image";
      return std::nullopt;
    }
    std::array<char, kSectionNameBytes> raw{};
    std::memcpy(raw.data(), file.mBytes.data() + offset, kSectionNameBytes);
    std::string name(raw.data(), strnlen(raw.data(), kSectionNameBytes));
    if (name.empty()) {
      error.reason = "section " + std::to_string(index) + " has an empty name";
      return std::nullopt;
    }
    if (!isFrameworkSectionName(name)) {
      // The only section a title may add carries the framework's own `title` name; anything else
      // was written by a build that knows state this one does not.
      error.reason = "unknown section '" + name + "'";
      return std::nullopt;
    }
    BlobReader lengthBytes(std::span<const std::uint8_t>(file.mBytes.data() + offset + kSectionNameBytes, 4));
    const std::uint32_t length = lengthBytes.u32();
    offset += kSectionNameBytes + 4;
    if (length > file.mBytes.size() - offset) {
      error.reason = "section '" + name + "' claims " + std::to_string(length) + " bytes but only " +
                     std::to_string(file.mBytes.size() - offset) + " remain";
      return std::nullopt;
    }
    Entry entry;
    entry.name = name;
    entry.offset = static_cast<std::uint32_t>(offset);
    entry.length = length;
    offset += length;
    if (std::find(file.mNames.begin(), file.mNames.end(), name) != file.mNames.end()) {
      error.reason = "section '" + name + "' appears twice";
      return std::nullopt;
    }
    file.mNames.push_back(entry.name);
    file.mEntries.push_back(std::move(entry));
  }
  return file;
}

std::optional<std::span<const std::uint8_t>> StateFile::titlePayload() const {
  return section(section::kTitle);
}

std::optional<std::span<const std::uint8_t>> StateFile::section(std::string_view name) const {
  for (const Entry &entry : mEntries) {
    if (entry.name == name) {
      return std::span<const std::uint8_t>(mBytes.data() + entry.offset, entry.length);
    }
  }
  return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> readFile(const std::string &path, std::string &error) {
  error.clear();
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    error = "cannot open '" + path + "' for reading";
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes;
  std::uint8_t chunk[64 * 1024];
  while (true) {
    const std::size_t got = std::fread(chunk, 1, sizeof(chunk), file);
    if (got > 0) {
      bytes.insert(bytes.end(), chunk, chunk + got);
    }
    if (got < sizeof(chunk)) {
      break;
    }
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) {
    error = "read of '" + path + "' failed part way through";
    return std::nullopt;
  }
  return bytes;
}

bool writeFile(const std::string &path, std::span<const std::uint8_t> bytes, std::string &error) {
  error.clear();
  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    error = "cannot open '" + path + "' for writing";
    return false;
  }
  const std::size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
  const bool flushed = std::fclose(file) == 0;
  if (written != bytes.size() || !flushed) {
    error = "wrote " + std::to_string(written) + " of " + std::to_string(bytes.size()) + " bytes to '" + path + "'";
    return false;
  }
  return true;
}

} // namespace psx::state