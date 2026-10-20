#pragma once

class Core;

namespace psx::cpu {

// A native body: an override, a host-turn service or a stock HLE function.
using NativeFunction = void (*)(Core *);

} // namespace psx::cpu
