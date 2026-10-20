#include "title_selection.h"

#include "disc_file.h"
#include "psx_exe_image.h"

#include <lucent/content.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <utility>

namespace psx::host {
namespace {

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
  const psx::cpu::PsxExeParseResult parsed = psx::cpu::parsePsxExeImage(bytes);
  if (!parsed) {
    return {SelectionStatus::InvalidExecutable,
            &expected,
            std::string(expected.serial) + " is not a PS-X EXE: " + parsed.detail,
            index};
  }
  const psx::cpu::PsxExeImage &header = *parsed.image;

  struct HeaderFact {
    std::uint32_t actual;
    std::uint32_t expected;
    std::string_view name;
  };
  const std::array facts{
      HeaderFact{header.entry, expected.entry, "entry"},
      HeaderFact{header.globalPointer, expected.globalPointer, "global pointer"},
      HeaderFact{header.textAddress, expected.textAddress, "text address"},
      HeaderFact{header.textBytes, expected.textSize, "text size"},
      HeaderFact{header.stackBase, expected.stackAddress, "stack address"},
      HeaderFact{header.stackOffset, expected.stackOffset, "stack offset"},
  };
  for (const HeaderFact &fact : facts) {
    const std::uint32_t actual = fact.actual;
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
  SelectionResult selection = selectExecutable(serial, bytes, catalog);
  if (selection) {
    selection.bytes = std::move(bytes);
  }
  return selection;
}

SelectionResult selectDiscExecutable(psx::cd::Disc &disc, const TitleIdentity &title) {
  const std::string discPath(title.discExecutablePath);
  std::vector<std::uint8_t> bytes;
  if (discPath.empty() || !psx::cd::readDiscFile(disc, discPath.c_str(), bytes)) {
    return {SelectionStatus::MissingExecutable, &title, "cannot read '" + discPath + "' from the disc"};
  }
  SelectionResult selection = selectExecutable(title.serial, bytes, std::span(&title, 1));
  if (selection) {
    selection.bytes = std::move(bytes);
  }
  return selection;
}

} // namespace psx::host
