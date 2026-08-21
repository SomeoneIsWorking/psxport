// Sony BIOS libc string leaves over guest addresses.
#pragma once

#include <cstdint>

class Core;

// Dispatch an A0 string leaf. Returns true only for a leaf owned by this module and writes guest V0.
bool bios_libc_string_dispatch(Core *core, uint32_t fn);
