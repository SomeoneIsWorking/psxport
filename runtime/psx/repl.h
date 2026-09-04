// repl.h — the interactive REPL driver (PSXPORT_REPL=1). Implemented in repl.cpp.
//
// The REPL reads stdin commands between frames; some commands ("newgame", "skip N", "warp <id>")
// don't act immediately — they arm auto-drive requests that the title's FrameDriver consumes on
// subsequent frames. That state lives on a Repl instance owned by Game (per-Core; SBS runs two
// cores and each can be REPL'd independently in principle). Was the process-globals
// g_nav_newgame / g_skip_frames / g_warp_armed / g_warp_dest.
#pragma once
#include <cstdint>
struct Core;

class Repl {
public:
  // Auto-drive requests armed by REPL commands, consumed + cleared by the title's FrameDriver.
  int navNewgame = 0;    // `newgame`: pulse Cross to the GAME prologue
  long skipFrames = 0;   // `skip N`: pulse Start N frames into the field
  int warpArmed = 0;     // `warp <id> [sub]`: arm one complete game-owned cold warp
  uint32_t warpDest = 0; // destination area id, interpreted by GameHooks::devWarp
  uint32_t warpSub = 0;  // destination sub-state, interpreted by GameHooks::devWarp

  // A title driver can request that the generic REPL loop return to its prompt after the current
  // frame has completed. The request is one-shot so it cannot pause a later run budget by accident.
  void requestPrompt() {
    promptRequested_ = true;
  }
  bool consumePromptRequest() {
    const bool requested = promptRequested_;
    promptRequested_ = false;
    return requested;
  }

  // Read+execute REPL commands from stdin until a `run N` (returns N) or quit/EOF (returns -1).
  long read(Core *c, uint32_t f);

private:
  bool promptRequested_ = false;
  uint16_t mHeldMask = 0xFFFF; // active-low held pad mask (all released); `press`/`release` edit it
};
