#include "title_selection.h"

#include <lucent/content.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

namespace psx::host {
namespace {

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

std::string hexadecimal(std::uint32_t value) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << value;
  return stream.str();
}

std::size_t indexOf(std::span<const TitleIdentity> catalog, const TitleIdentity &identity) {
  return static_cast<std::size_t>(&identity - catalog.data());
}

SelectionResult mismatch(std::span<const TitleIdentity> catalog, const TitleIdentity &identity, std::string detail) {
  return {SelectionStatus::IdentityMismatch,
          &identity,
          std::string(identity.serial) + " identity mismatch: " + std::move(detail),
          indexOf(catalog, identity)};
}

} // namespace

SelectionResult
selectExecutable(std::string_view serial, std::span<const std::uint8_t> bytes, std::span<const TitleIdentity> catalog) {
  const auto found = std::ranges::find(catalog, serial, &TitleIdentity::serial);
  if (found == catalog.end()) {
    return {SelectionStatus::UnsupportedSerial, nullptr, "unsupported executable serial " + std::string(serial)};
  }
  const TitleIdentity &expected = *found;
  const std::size_t index = indexOf(catalog, expected);
  if (bytes.size() != expected.fileSize) {
    return mismatch(catalog,
                    expected,
                    "expected " + std::to_string(expected.fileSize) + " bytes, got " + std::to_string(bytes.size()));
  }
  constexpr std::array<std::uint8_t, 8> kPsxExeMagic{'P', 'S', '-', 'X', ' ', 'E', 'X', 'E'};
  if (bytes.size() < 0x800u || !std::ranges::equal(kPsxExeMagic, bytes.first(8))) {
    return {SelectionStatus::InvalidExecutable, &expected, std::string(expected.serial) + " is not a PS-X EXE", index};
  }

  struct HeaderFact {
    std::size_t offset;
    std::uint32_t expected;
    std::string_view name;
  };
  const std::array facts{
      HeaderFact{0x10u, expected.entry, "entry"},
      HeaderFact{0x14u, expected.globalPointer, "global pointer"},
      HeaderFact{0x18u, expected.textAddress, "text address"},
      HeaderFact{0x1Cu, expected.textSize, "text size"},
      HeaderFact{0x30u, expected.stackAddress, "stack address"},
      HeaderFact{0x34u, expected.stackOffset, "stack offset"},
  };
  for (const HeaderFact &fact : facts) {
    const std::uint32_t actual = readU32(bytes, fact.offset);
    if (actual != fact.expected) {
      return mismatch(catalog,
                      expected,
                      std::string(fact.name) + " expected " + hexadecimal(fact.expected) + ", got " +
                          hexadecimal(actual));
    }
  }

  const std::string digest = lucent::content::sha256_hex(lucent::content::sha256(std::as_bytes(bytes)));
  if (digest != expected.sha256) {
    return mismatch(catalog, expected, "SHA-256 expected " + std::string(expected.sha256) + ", got " + digest);
  }
  return {SelectionStatus::Selected,
          &expected,
          std::string(expected.serial) + " selected " + std::string(expected.displayName),
          index};
}

SelectionResult selectExecutableFile(const std::filesystem::path &path, std::span<const TitleIdentity> catalog) {
  const std::string serial = path.filename().string();
  const auto found = std::ranges::find(catalog, serial, &TitleIdentity::serial);
  if (found == catalog.end()) {
    return {SelectionStatus::UnsupportedSerial, nullptr, "unsupported executable serial " + serial};
  }
  const std::size_t index = indexOf(catalog, *found);

  std::error_code sizeError;
  const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
  if (sizeError) {
    return {SelectionStatus::MissingExecutable,
            &*found,
            "cannot read " + path.string() + ": " + sizeError.message(),
            index};
  }
  if (size != found->fileSize) {
    return mismatch(
        catalog, *found, "expected " + std::to_string(found->fileSize) + " bytes, got " + std::to_string(size));
  }

  std::vector<std::uint8_t> bytes(found->fileSize);
  std::ifstream input(path, std::ios::binary);
  if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    return {SelectionStatus::MissingExecutable, &*found, "cannot read " + path.string(), index};
  }
  return selectExecutable(serial, bytes, catalog);
}

} // namespace psx::host
