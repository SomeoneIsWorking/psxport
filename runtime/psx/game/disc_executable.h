// disc_executable.h — a second executable a title boots from its own disc (a boot stub's hand-off).
#pragma once

#include "psx_exe_image.h"

class Game;

namespace psx::host {
struct TitleIdentity;
} // namespace psx::host

namespace psx::game {

// `identity`'s executable read from `game`'s disc, authenticated against it and loaded as the
// top-level program; a refusal names its reason and leaves the Core unchanged.
psx::cpu::PsxExeLoadResult loadDiscExecutable(Game &game, const psx::host::TitleIdentity &identity);

} // namespace psx::game
