// Sony BIOS libc string/character/memory-compare leaves.
#pragma once

#include <cstdint>

class Core;

namespace psx::hle {

// Forward reproduces memcpy's byte order when the ranges overlap; OverlapSafe is memmove.
enum class CopyOrder : std::uint8_t { Forward, OverlapSafe };
void guestCopy(Core &core, uint32_t dst, uint32_t src, uint32_t n, CopyOrder order);

// Dispatch an A0 libc leaf owned here. Returns true only for a handled leaf and writes guest V0.
bool dispatchLibcString(Core *core, uint32_t fn);

} // namespace psx::hle
