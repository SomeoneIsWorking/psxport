// pad_input.h — class Pad — native controller input subsystem, owned by Game (c->game->pad).
// Carries the current host button state + REPL drive control + all the pad_* behavior (host poll,
// per-VBlank fill buffer, REPL hold/tap/release), plus the SDL gamepad handles and the headless
// test hooks (force/hold/record/replay/shot/dump/trace schedules). Implemented in pad_input.cpp.
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>
class Game;
typedef struct SDL_Gamepad SDL_Gamepad;   // opaque; only held as pointers (SDL build only)

class Pad {
public:
  Game* game = nullptr;
  uint16_t buttons    = 0xFFFF;  // current host button state, active-low (0 bit = pressed) (was s_buttons)
  uint16_t repl_hold  = 0xFFFF;  // REPL: bits cleared = held down (was s_repl_hold)
  uint16_t repl_tap   = 0xFFFF;  // REPL: active-low mask pressed for repl_tap_n frames (was s_repl_tap)
  int      repl_tap_n = 0;       // REPL: tap countdown frames (was s_repl_tap_n)
  int      repl_on    = 0;       // REPL drive active (was s_repl_on)

  void init();                              // was pad_init(Core*)
  void setButtons(uint16_t mask);           // was pad_set_buttons(Core*, mask) — feed the active-low mask
  void fillBuffer(uint8_t* buf);            // was pad_fill_buffer(Core*, buf) — per-VBlank guest read pad
  void pollSdl();                           // was pad_poll_sdl(Core*) — host SDL controller poll
  void overridesInit();                     // was pad_overrides_init(Core*) — install per-VBlank pad-read override
  void driveHold(uint16_t activeLowMask);   // was pad_repl_hold(c, mask) — REPL: hold down these bits
  void driveTap(uint16_t activeLowMask, int nframes);  // was pad_repl_tap(c, mask, n) — press for n frames
  void driveRelease();                      // was pad_repl_release(c) — clear REPL drive
  void serviceFrame();                      // was pad_service_frame(c) — per-frame native pad service

  // ---- live capture (dbg-server `padrec`) ----
  // Every frame's finalized mask is also kept in memory, unconditionally, so a running session can be
  // cut into a replay WITHOUT a file sink, a restart, or racing the incremental writer. 2 bytes/frame:
  // an hour of play is 432 KB. `saveRecording` writes the same uint16-LE format PSXPORT_PAD_REPLAY reads.
  size_t recordedFrames() const { return mRecLog.size(); }
  // nframes = 0 saves everything; otherwise the FIRST nframes (the useful trim — drop the idle tail
  // after a repro). A suffix is never offered: replays are only valid from boot.
  bool saveRecording(const char* path, size_t nframes) const;

private:
  // ---- SDL gamepad handles (hotswap-aware; SDL build only) ----
  static const int PAD_MAX_GC = 4;
  SDL_Gamepad* mGc[PAD_MAX_GC] = {};
  int  mGcInst[PAD_MAX_GC] = { -1, -1, -1, -1 };  // SDL_JoystickID per slot (-1 = empty)
  int  mGcSubInit = 0;                            // lazily added the gamepad subsystem?
  int  mNoPad = -1;                               // PSXPORT_PAD_NOPAD cache (-1 = not read)
  int  mPrevP = 0, mPrevStep = 0;                 // P / '.' debug-key edge detectors
  int  mPadDirsWarned = 0;                        // "controller is driving directions" once-notice
  void ensureGcSubsystem();
  void rescanControllers();

  // ---- serviceFrame test hooks / config caches ----
  int      mForceInit = 0, mForceOn = 0;
  uint16_t mForceMask = 0xFFFF;
  uint32_t mFc = 0;                 // internal frame counter for the pulse (== native frame index)
  uint16_t mHoldMask = 0xFFFF;      // headless test hook: a HELD (not pulsed) mask...
  uint32_t mHoldAt = 0;             // ...applied from this native frame onward
  long     mStopAt = -2;            // PSXPORT_FORCE_STOP_AT (-2 = not read, -1 = off)

  // ---- input record / replay + schedules ----
  int       mRecInit = 0;
  FILE*     mRecFp = nullptr;       // record sink
  uint16_t* mRepBuf = nullptr;      // replay source (loaded once)
  size_t    mRepN = 0;
  uint32_t  mRecFc = 0;             // shared record/replay frame index
  std::vector<uint16_t> mRecLog;    // every finalized mask, always — the `padrec save` source
  int       mShotInit = 0, mShotN = 0;
  uint32_t  mShotAt[64] = {};
  int       mDumpInit = 0, mDumpN = 0;
  uint32_t  mDumpAt[32] = {};
  int       mTraceInit = 0;
  uint32_t  mTraceLo = 1, mTraceHi = 0;
};
