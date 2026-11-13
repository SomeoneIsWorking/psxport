#pragma once

#include <cstdint>

class Core;

namespace psx::cpu {

// A native body: an override, a host-turn service or a stock HLE function.
using NativeFunction = void (*)(Core *);

// What the replaced guest function hands back: Void leaves v0/v1 as scratch, so the differential does not judge them.
enum class NativeReturn : std::uint8_t { Value, Void };

} // namespace psx::cpu
