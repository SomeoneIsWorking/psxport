// Sony BIOS libc string/character/memory-compare leaves.
#pragma once

#include <cstdint>

class Core;

// Dispatch an A0 libc leaf owned here. Returns true only for a handled leaf and writes guest V0.
bool bios_libc_string_dispatch(Core *core, uint32_t fn);
