#include "title_selection.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace {

void writeU32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte) {
    bytes[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
  }
}

std::vector<std::uint8_t> executable() {
  std::vector<std::uint8_t> bytes(0x800u);
  constexpr std::array<std::uint8_t, 8> magic{'P', 'S', '-', 'X', ' ', 'E', 'X', 'E'};
  std::ranges::copy(magic, bytes.begin());
  writeU32(bytes, 0x10u, 0x80010100u);
  writeU32(bytes, 0x18u, 0x80010000u);
  writeU32(bytes, 0x1Cu, 0x1000u);
  writeU32(bytes, 0x30u, 0x801FFFF0u);
  return bytes;
}

} // namespace

int main() {
  const std::array catalog{
      psx::host::TitleIdentity{
          .displayName = "selection fixture",
          .serial = "SCUS_000.01",
          .fileSize = 0x800u,
          .sha256 = "02e23f3624a575943098a80ec71a271d1d192128907fd3bd464a2f54b523239a",
          .entry = 0x80010100u,
          .globalPointer = 0u,
          .textAddress = 0x80010000u,
          .textSize = 0x1000u,
          .stackAddress = 0x801FFFF0u,
          .stackOffset = 0u,
      },
      psx::host::TitleIdentity{
          .displayName = "different fixture",
          .serial = "SCUS_000.02",
          .fileSize = 0x800u,
          .sha256 = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
          .entry = 0x80010100u,
          .globalPointer = 0u,
          .textAddress = 0x80010000u,
          .textSize = 0x1000u,
          .stackAddress = 0x801FFFF0u,
          .stackOffset = 0u,
      },
  };

  std::vector<std::uint8_t> bytes = executable();
  const psx::host::SelectionResult selected = psx::host::selectExecutable("SCUS_000.01", bytes, catalog);
  if (!selected || selected.identity == nullptr || selected.index != 0) {
    return 1;
  }
  const psx::host::SelectionResult unsupported = psx::host::selectExecutable("SCUS_999.99", bytes, catalog);
  if (unsupported.status != psx::host::SelectionStatus::UnsupportedSerial) {
    return 2;
  }
  bytes.back() = 1u;
  const psx::host::SelectionResult mutated = psx::host::selectExecutable("SCUS_000.01", bytes, catalog);
  if (mutated.status != psx::host::SelectionStatus::IdentityMismatch) {
    return 3;
  }
  bytes.back() = 0u;
  const psx::host::SelectionResult renamed = psx::host::selectExecutable("SCUS_000.02", bytes, catalog);
  return renamed.status == psx::host::SelectionStatus::IdentityMismatch && renamed.identity != nullptr &&
                 renamed.index == 1
             ? 0
             : 4;
}
