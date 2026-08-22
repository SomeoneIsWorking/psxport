// pad_input.h — class Pad — native controller input subsystem, owned by Game (c->game->pad).
// Carries the current host button state + REPL drive control + all the pad_* behavior (host poll,
// per-VBlank fill buffer, REPL hold/tap/release), plus the SDL gamepad handles and the headless
// test hooks (force/hold/record/replay/shot/dump/trace schedules). Implemented in pad_input.cpp.
#pragma once
#include "active_low_edges.h"
#include <cstdint>
#include <cstdio>
#include <vector>
class Game;
typedef struct SDL_Gamepad SDL_Gamepad; // opaque; only held as pointers (SDL build only)

class Pad {
public:
  Game *game = nullptr;
  uint16_t buttons = 0xFFFF;   // current host button state, active-low (0 bit = pressed) (was s_buttons)
  uint16_t repl_hold = 0xFFFF; // REPL: bits cleared = held down (was s_repl_hold)
  uint16_t repl_tap = 0xFFFF;  // REPL: active-low mask pressed for repl_tap_n frames (was s_repl_tap)
  int repl_tap_n = 0;          // REPL: tap countdown frames (was s_repl_tap_n)
  int repl_on = 0;             // REPL drive active (was s_repl_on)

  void init();                            // was pad_init(Core*)
  void setButtons(uint16_t mask);         // was pad_set_buttons(Core*, mask) — feed the active-low mask
  void fillBuffer(uint8_t *buf);          // was pad_fill_buffer(Core*, buf) — per-VBlank guest read pad
  void pollSdl();                         // was pad_poll_sdl(Core*) — host SDL controller poll
  void overridesInit();                   // was pad_overrides_init(Core*) — install per-VBlank pad-read override
  void driveHold(uint16_t activeLowMask); // was pad_repl_hold(c, mask) — REPL: hold down these bits
  void driveTap(uint16_t activeLowMask, int nframes); // was pad_repl_tap(c, mask, n) — press for n frames
  void driveRelease();                                // was pad_repl_release(c) — clear REPL drive
  void serviceFrame();
  void applyGuestPoke(Core *c); // was pad_service_frame(c) — per-frame native pad service

  // Pump host input WITHOUT advancing a pad frame. For the debug-server pause loop, which must keep
  // the window responsive (and the P / '.' keys alive) while the game is explicitly NOT advancing.
  // serviceFrame() must never be used there: it ticks the record/replay frame index, so a capture
  // taken across a pause records frames the game never ran, and replaying it consumes them while the
  // game IS running — the whole session desyncs from that point on.
  void pumpHostInput();

  // Edge state for the FINAL effective active-low mask (host/forced/REPL/replay already resolved).
  // Consumers may inspect it, but only their own state machine decides whether an edge transitions a
  // logo, loading overlay, movie, or scripted sequence. This never consumes or suppresses game input.
  void sampleButtonEdges() {
    mButtonEdges.sample(buttons);
  }
  void resetButtonEdges(uint16_t current = 0xFFFFu) {
    mButtonEdges.reset(current);
  }
  uint16_t pressedButtons() const {
    return mButtonEdges.pressed();
  }
  uint16_t releasedButtons() const {
    return mButtonEdges.released();
  }
  bool pressedButton(uint16_t mask) const {
    return mButtonEdges.pressed(mask);
  }
  bool releasedButton(uint16_t mask) const {
    return mButtonEdges.released(mask);
  }

  // ---- live capture (dbg-server `padrec`) ----
  // Every frame's finalized mask is also kept in memory, unconditionally, so a running session can be
  // cut into a replay WITHOUT a file sink, a restart, or racing the incremental writer. 2 bytes/frame:
  // an hour of play is 432 KB. `saveRecording` writes the same uint16-LE format PSXPORT_PAD_REPLAY reads.
  size_t recordedFrames() const {
    return mRecLog.size();
  }
  // nframes = 0 saves everything; otherwise the FIRST nframes (the useful trim — drop the idle tail
  // after a repro). A suffix is never offered: replays are only valid from boot.
  bool saveRecording(const char *path, size_t nframes) const;

  // ---- resume-from-recording (PSXPORT_PAD_RESUME) ----
  // TRUE while a RESUME replay is still feeding the guest, i.e. while the run is replaying its way
  // back to where the player left off and has not handed the controller over yet. It is what the
  // rest of the runtime asks in order to fast-forward: the pacer does not sleep (gpu_native.cpp),
  // FMVs play uncapped (native_fmv.cpp), and rendered audio is dropped instead of queued
  // (spu_audio.cpp) — a 30-minute session replayed at wall-clock speed is not a resume.
  //
  // It goes FALSE the moment the recording runs out, which is the same instant the replay stops
  // overriding the pad mask, so speed, sound and control are handed back together and there is no
  // window in which the player is driving a fast-forwarding game. A plain PSXPORT_PAD_REPLAY is
  // NOT fast-forwarded: watching a replay at real speed is the other legitimate use, and the two
  // are told apart by which knob was set, never inferred.
  bool fastForwarding() const {
    return mResumeFf && mRepBuf && mRecFc < mRepN;
  }

  // ---- REPLAY PROGRESS, so a run cannot silently truncate a recording ----------------------------
  // A headless run is frame-capped (native_boot.cpp's "headless smoke default"). A pad replay that is
  // longer than that cap used to be cut off SILENTLY: the run ended with a cheerful "frame loop done"
  // having consumed 120 frames of a 30,612-frame recording, and every measurement taken from it was a
  // measurement of the title screen. It cost most of a session chasing that as a code regression.
  // These three make the truncation impossible to miss — the loop uncaps itself while a replay is
  // pending, and the run reports its own denominator at exit.
  bool replayPending() const {
    return mRepBuf && mRecFc < mRepN;
  } // frames still owed to the guest
  size_t replayTotal() const {
    return mRepN;
  }
  uint32_t replayConsumed() const {
    return mRecFc;
  }

private:
  ActiveLowEdges mButtonEdges;
  // ---- SDL gamepad handles (hotswap-aware; SDL build only) ----
  static const int PAD_MAX_GC = 4;
  SDL_Gamepad *mGc[PAD_MAX_GC] = {};
  int mGcInst[PAD_MAX_GC] = {-1, -1, -1, -1}; // SDL_JoystickID per slot (-1 = empty)
  int mGcSubInit = 0;                         // lazily added the gamepad subsystem?
  int mNoPad = -1;                            // PSXPORT_PAD_NOPAD cache (-1 = not read)
  int mPrevP = 0, mPrevStep = 0;              // P / '.' debug-key edge detectors
  int mPadDirsWarned = 0;                     // "controller is driving directions" once-notice
  void ensureGcSubsystem();
  void rescanControllers();

  // ---- serviceFrame test hooks / config caches ----
  int mForceInit = 0, mForceOn = 0;
  uint16_t mForceMask = 0xFFFF;
  uint32_t mFc = 0;            // internal frame counter for the pulse (== native frame index)
  uint16_t mHoldMask = 0xFFFF; // headless test hook: a HELD (not pulsed) mask...
  uint32_t mHoldAt = 0;        // ...applied from this native frame onward
  long mStopAt = -2;           // PSXPORT_FORCE_STOP_AT (-2 = not read, -1 = off)

  // ---- input record / replay + schedules ----
  int mRecInit = 0;
  FILE *mRecFp = nullptr;      // record sink
  uint16_t *mRepBuf = nullptr; // replay source (loaded once)
  size_t mRepN = 0;
  uint32_t mRecFc = 0;           // shared record/replay frame index
  int mResumeFf = 0;             // PSXPORT_PAD_RESUME: fast-forward until the replay is spent
  int mResumeDone = 0;           // handover already announced (once-only log)
  std::vector<uint16_t> mRecLog; // every finalized mask, always — the `padrec save` source
  int mShotInit = 0, mShotN = 0;
  uint32_t mShotAt[64] = {};
  // PSXPORT_GUEST_POKE — guest locations rewritten every frame (see applyGuestPoke).
  static constexpr int kPokeMax = 16;
  struct GuestPoke {
    uint32_t addr, val, width;
  };
  int mPokeInit = 0, mPokeN = 0;
  GuestPoke mPoke[kPokeMax] = {};
  int mDumpInit = 0, mDumpN = 0;
  uint32_t mDumpAt[32] = {};
  int mTraceInit = 0;
  uint32_t mTraceLo = 1, mTraceHi = 0;
};
