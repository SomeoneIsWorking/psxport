#pragma once

#include "image_identity.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

class Core;

namespace psx::cpu {

inline constexpr std::size_t kPsxExeHeaderBytes = 0x800;
inline constexpr std::size_t kPsxExeMaxBytes = kPsxExeHeaderBytes + 0x200000;

struct PsxExeImage {
  std::uint32_t entry = 0;
  std::uint32_t globalPointer = 0;
  std::uint32_t textAddress = 0;
  std::uint32_t textBytes = 0;
  std::uint32_t stackBase = 0;
  std::uint32_t stackOffset = 0;
  std::uint32_t stackPointer = 0;
  GuestAddressRange physicalText;
};

struct PsxExeParseResult {
  std::optional<PsxExeImage> image;
  std::string detail;
  explicit operator bool() const {
    return image.has_value();
  }
};

struct PsxExeLoadResult {
  std::optional<ImageIdentity> identity;
  PsxExeImage image;
  std::string detail;
  explicit operator bool() const {
    return identity.has_value();
  }
};

// Structural validation only. Title identity/authentication belongs to the caller.
// Accepts KUSEG, KSEG0 and KSEG1 aliases of contiguous main RAM; no wraparound.
PsxExeParseResult parsePsxExeImage(std::span<const std::uint8_t> bytes);

// Consumes the same bytes the caller authenticated. On validation failure, Core
// memory, registers, image catalog and code cache are unchanged. Input must not
// alias Core RAM. On success sets PC/GP and, when declared, SP/FP; other registers
// remain unchanged. A zero stack base leaves the caller's stack intact.
PsxExeLoadResult loadPsxExeImage(Core &core, std::span<const std::uint8_t> bytes, std::string_view imageName);

// Complete a top-level PS-X EXE startup after successful image mapping. A
// declared stack was established by loadPsxExeImage; absent one, use the
// conventional boot stack. Overlay callers do not use this startup policy.
void applyPsxExeTopLevelRegisters(Core &core, const PsxExeImage &image);

} // namespace psx::cpu

// File-based startup entry: structural admission only, with default stack and
// top-level return sentinel. Exact title authentication remains the caller's job.
void load_exe(const char *path, Core *core);
