// Sony BIOS libc string/character leaves.
#pragma once

#include <cstdint>

class Core;

// Dispatch an A0 string/character leaf. Returns true only for a leaf owned here and writes guest V0.
bool bios_libc_string_dispatch(Core *core, uint32_t fn);
