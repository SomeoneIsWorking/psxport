// repl.h — the interactive REPL driver (PSXPORT_REPL=1) interface. Implemented in repl.cpp;
// the native scheduler frame loop (native_boot.cpp) calls native_repl_read and reads the auto-drive
// state the REPL arms.
#pragma once
#include <cstdint>
struct Core;
// REPL-armed auto-drive state, consumed by the native scheduler frame loop.
extern int      g_nav_newgame;   // `newgame`: pulse Cross to the GAME prologue
extern long     g_skip_frames;   // `skip N`: pulse Start N frames into the field
extern int      g_warp_armed;    // `warp <id>`: arm an area warp
extern uint32_t g_warp_dest;
// Read+execute REPL commands until a `run N` (returns N) or quit/EOF (returns -1).
long native_repl_read(Core* c, uint32_t f);
