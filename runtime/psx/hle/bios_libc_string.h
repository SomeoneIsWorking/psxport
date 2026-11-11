// Sony BIOS libc string/character/memory-compare leaves.
#pragma once

#include <cstdint>

class Core;

namespace psx::hle {

// Forward reproduces memcpy's byte order when the ranges overlap; OverlapSafe is memmove.
enum class CopyOrder : std::uint8_t { Forward, OverlapSafe };
void guestCopy(Core &core, uint32_t dst, uint32_t src, uint32_t n, CopyOrder order);

// An A0 libc leaf: reads and writes only guest memory and V0, given the call's a0..a2.
using LibcStringLeaf = void (*)(Core &core, uint32_t a0, uint32_t a1, uint32_t a2);
// The A0 leaf owned here for `fn`, or nullptr.
LibcStringLeaf libcStringLeaf(uint32_t fn);

} // namespace psx::hle
