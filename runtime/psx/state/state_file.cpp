#include "state_file.h"

#include "blob_codec.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace psx::state {
namespace {

bool isFrameworkSectionName(std::string_view name) {
  return name == section::kTitle || std::ranges::find(section::kRequired, name) != section::kRequired.end();
}

} // namespace

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
    psx::platform::BlobWriter header;
    header.bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(kStateMagic), 8));
    header.u32(kFormatVersion);
    // The section count is patched below, so write a placeholder the patch step overwrites.
    header.u32(0);
    mBytes = header.take();
  }
  std::array<char, kSectionNameBytes> padded{};
  std::memcpy(padded.data(), name.data(), name.size());
  mBytes.insert(mBytes.end(), padded.begin(), padded.end());
  psx::platform::BlobWriter tail;
  tail.u32(static_cast<std::uint32_t>(payload.size()));
  const auto tailBytes = tail.bytesOut();
  mBytes.insert(mBytes.end(), tailBytes.begin(), tailBytes.end());
  mBytes.insert(mBytes.end(), payload.begin(), payload.end());
  mNames.emplace_back(name);
  // Patch the section count in place: bytes [8, 12) are the version, [12, 16) the count.
  psx::platform::BlobWriter count;
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
  psx::platform::BlobReader head(std::span<const std::uint8_t>(bytes.data(), 16));
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
    psx::platform::BlobReader lengthBytes(std::span<const std::uint8_t>(file.mBytes.data() + offset + kSectionNameBytes, 4));
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

} // namespace psx::state