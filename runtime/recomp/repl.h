// repl.h — the interactive REPL driver (PSXPORT_REPL=1). Implemented in repl.cpp.
//
// The REPL reads stdin commands between frames; some commands ("newgame", "skip N", "warp <id>")
// don't act immediately — they arm auto-drive requests that the native scheduler frame loop
// consumes on subsequent frames. That state lives on a Repl instance owned by Game (per-Core; SBS
// runs two cores and each can be REPL'd independently in principle). Was the process-globals
// g_nav_newgame / g_skip_frames / g_warp_armed / g_warp_dest.
#pragma once
#include <cstdint>
struct Core;

class Repl {
public:
  // Auto-drive requests armed by REPL commands, consumed + cleared by the native scheduler frame loop.
  int      navNewgame = 0;   // `newgame`: pulse Cross to the GAME prologue
  long     skipFrames = 0;   // `skip N`: pulse Start N frames into the field
  int      warpArmed  = 0;   // `warp <id>`: arm an area warp
  uint32_t warpDest   = 0;

  // Read+execute REPL commands from stdin until a `run N` (returns N) or quit/EOF (returns -1).
  long read(Core* c, uint32_t f);
};
