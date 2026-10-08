#include "psx_exe_image.h"

#include "core.h"
#include "invalidation.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace psx::cpu {
namespace {

constexpr uint32_t kRamBytes = 0x200000;

uint32_t readWord(std::span<const uint8_t> bytes, std::size_t offset) {
  uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<uint32_t>(bytes[offset + index]) << (index * 8);
  }
  return value;
}

std::optional<uint32_t> ramOffset(uint32_t address, bool allowEnd = false) {
  const uint32_t segment = address & 0xe0000000u;
  if (segment != 0 && segment != 0x80000000u && segment != 0xa0000000u) {
    return std::nullopt;
  }
  const uint32_t physical = address & 0x1fffffffu;
  if (physical < kRamBytes || (allowEnd && physical == kRamBytes)) {
    return physical;
  }
  return std::nullopt;
}

} // namespace

PsxExeParseResult parsePsxExeImage(std::span<const uint8_t> bytes) {
  if (bytes.size() < kPsxExeHeaderBytes || bytes.size() > kPsxExeMaxBytes) {
    return {std::nullopt, "PS-X EXE size must contain the 0x800-byte header and fit main RAM"};
  }
  constexpr std::array<uint8_t, 8> magic{'P', 'S', '-', 'X', ' ', 'E', 'X', 'E'};
  if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
    return {std::nullopt, "invalid PS-X EXE magic"};
  }
  PsxExeImage image{};
  image.entry = readWord(bytes, 0x10);
  image.globalPointer = readWord(bytes, 0x14);
  image.textAddress = readWord(bytes, 0x18);
  image.textBytes = readWord(bytes, 0x1c);
  image.stackBase = readWord(bytes, 0x30);
  image.stackOffset = readWord(bytes, 0x34);
  const auto text = ramOffset(image.textAddress);
  if (!text || (image.textAddress & 3u) || image.textBytes < 4 || image.textBytes > kRamBytes - *text) {
    return {std::nullopt, "PS-X EXE text must be aligned and fit contiguous main RAM"};
  }
  if (image.textBytes > bytes.size() - kPsxExeHeaderBytes) {
    return {std::nullopt, "truncated PS-X EXE text payload"};
  }
  image.physicalText = {*text, *text + image.textBytes};
  const auto entry = ramOffset(image.entry);
  if (!entry || (image.entry & 3u) || *entry < *text || *entry > image.physicalText.end - 4) {
    return {std::nullopt, "PS-X EXE entry instruction must lie inside its loaded text"};
  }
  if (image.globalPointer != 0 && !ramOffset(image.globalPointer)) {
    return {std::nullopt, "PS-X EXE global pointer is not null or mapped main RAM"};
  }
  if (image.stackBase == 0) {
    if (image.stackOffset != 0) {
      return {std::nullopt, "PS-X EXE stack offset has no stack base"};
    }
  } else {
    const auto base = ramOffset(image.stackBase, true);
    const uint64_t stack = static_cast<uint64_t>(image.stackBase) + image.stackOffset;
    if (!base || image.stackOffset > kRamBytes - *base || stack > std::numeric_limits<uint32_t>::max() ||
        !ramOffset(static_cast<uint32_t>(stack), true) || (stack & 3u)) {
      return {std::nullopt, "PS-X EXE stack base plus offset must be aligned and inside main RAM"};
    }
    image.stackPointer = static_cast<uint32_t>(stack);
  }
  return {image, {}};
}

PsxExeLoadResult loadPsxExeImage(Core &core, std::span<const uint8_t> bytes, std::string_view imageName) {
  const auto parsed = parsePsxExeImage(bytes);
  if (!parsed) {
    return {std::nullopt, {}, parsed.detail};
  }
  if (imageName.empty()) {
    return {std::nullopt, {}, "PS-X EXE requires an image name"};
  }
  const auto input = reinterpret_cast<std::uintptr_t>(bytes.data());
  const auto ram = reinterpret_cast<std::uintptr_t>(core.ram);
  if (input > std::numeric_limits<std::uintptr_t>::max() - bytes.size() ||
      (input < ram + sizeof(core.ram) && input + bytes.size() > ram)) {
    return {std::nullopt, {}, "PS-X EXE input span aliases destination RAM or overflows its address range"};
  }
  const auto &image = *parsed.image;
  const auto payload = bytes.subspan(kPsxExeHeaderBytes, image.textBytes);
  uint64_t contentIdentity = 1469598103934665603ull;
  for (uint8_t byte : payload) {
    contentIdentity ^= byte;
    contentIdentity *= 1099511628211ull;
  }
  // One complete payload publication, followed by the same central invalidation
  // used by guest stores and DMA. No raw copy can leave an older translation live.
  std::memcpy(core.ram + image.physicalText.begin, payload.data(), payload.size());
  notifyExecutableWrite(core, image.physicalText, ExecutableWriteSource::ModuleLoad);
  const auto identity = core.imageCatalog().activate(imageName, image.physicalText, contentIdentity);
  core.pc = image.entry;
  core.r[28] = image.globalPointer;
  if (image.stackBase != 0) {
    core.r[29] = image.stackPointer;
    core.r[30] = image.stackPointer;
  }
  return {identity, image, {}};
}

} // namespace psx::cpu
