// Sony BIOS libc string/character/memory-compare leaves.
#pragma once

#include <cstdint>

class Core;

namespace psx::hle {

// Dispatch an A0 libc leaf owned here. Returns true only for a handled leaf and writes guest V0.
bool dispatchLibcString(Core *core, uint32_t fn);

} // namespace psx::hle
