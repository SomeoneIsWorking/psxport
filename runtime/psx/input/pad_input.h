// pad_input.h — class Pad — the PSX controller as the guest sees it, owned by Game (c->game->pad).
//
// Pad owns everything about the BUTTONS once they are host buttons: the current active-low mask, the
// per-VBlank guest packet fill, the REPL drive, the deterministic record/replay session, and the
// headless test hooks. The host itself — the SDL event drain, the delivered key state, the gamepads and
// the mask they add up to — belongs to psx::input::HostInput (host_input.h), which Pad consumes.
//
// Its constructor takes that owner: a pad with no host input source is not a pad that can be
// configured later, it is a pad that silently ignores the player.
#pragma once
#include "active_low_edges.h"
#include "host_input.h" // psx::input::HostInput — the host input owner this pad consumes
#include "pad_record_replay.h"
#include <cstdint>
#include <cstdio>
#include <vector>

class Core;
class Game;

class Pad {
public:
  explicit Pad(psx::input::HostInput &host) : mHost(host) {}

  Game *game = nullptr;
  uint16_t buttons = psx::input::kNoButtons;   // current effective mask, active-low (0 bit = pressed)
  uint16_t repl_hold = psx::input::kNoButtons; // REPL: bits cleared = held down
  uint16_t repl_tap = psx::input::kNoButtons;  // REPL: active-low mask pressed for repl_tap_n frames
  int repl_tap_n = 0;                          // REPL: tap countdown frames
  int repl_on = 0;                             // REPL drive active

  void init();                    // reset the mask to nothing pressed
  void setButtons(uint16_t mask); // feed the effective active-low mask (a forced or replayed mask)
  void fillBuffer(uint8_t *buf);  // per-VBlank guest read pad — write the standard digital packet

  // THE ONE HOST-INPUT PUMP. Every site that must keep the host responsive goes through this: the
  // per-frame service, the SCEA splash, a blocking movie, and the debug-server pause wait. It resolves
  // the host mask, and applies the P / '.' debug keys to the debug channel's pause and step state.
  void pollHostInput();

  void overridesInit();                               // install the per-VBlank pad-read override
  void driveHold(uint16_t activeLowMask);             // REPL: hold down these bits
  void driveTap(uint16_t activeLowMask, int nframes); // REPL: press for n frames
  void driveRelease();                                // REPL: clear the drive entirely

  // The once-per-frame pad service: poll the host, resolve forced / REPL / replay input into `buttons`,
  // record or replay the frame, apply the test hooks, and write the guest packet into the registered
  // slot buffers. Advances the pad-frame clock, so it is NOT what a pump site that must not advance a
  // frame calls.
  void serviceFrame();
  void applyGuestPoke(Core *c); // PSXPORT_GUEST_POKE: rewrite named guest locations every frame

  // Edge state for the FINAL effective active-low mask (host/forced/REPL/replay already resolved).
  // Consumers may inspect it, but only their own state machine decides whether an edge transitions a
  // logo, loading overlay, movie, or scripted sequence. This never consumes or suppresses game input.
  void sampleButtonEdges() {
    mButtonEdges.sample(buttons);
  }
  void resetButtonEdges(uint16_t current = psx::input::kNoButtons) {
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

  // ---- recording / replay (psx::input::PadRecordReplay, pad_record_replay.h) ----
  // Every frame's finalized mask is kept in memory with its title phase, unconditionally, so a
  // running session can be cut into a replay WITHOUT a file sink, a restart, or racing the writer.
  // `saveRecording` writes the same phase-keyed format PSXPORT_PAD_REPLAY reads.
  size_t recordedFrames() const {
    return mSession.recording().totalFrames();
  }
  // nframes = 0 saves everything; otherwise the FIRST nframes (the useful trim — drop the idle tail
  // after a repro). A suffix is never offered: a recording starts in the phase the game boots into.
  bool saveRecording(const char *path, size_t nframes) const {
    return mSession.saveRecording(path, nframes);
  }
  // The memory-card image the recording above started from (see PadRecordReplay::startCardImage).
  const std::vector<std::uint8_t> &recordingStartCard() const {
    return mSession.startCardImage();
  }

  // TRUE while a RESUME replay (PSXPORT_PAD_RESUME) is still feeding the guest: the pacer does not sleep,
  // FMVs play uncapped and rendered audio is dropped while it holds, and all three come back together
  // the moment the replay stops driving the pad. A plain PSXPORT_PAD_REPLAY is NOT fast-forwarded; the
  // two are told apart by which knob was set.
  bool fastForwarding() const {
    return mSession.fastForwarding();
  }
  // True while a replay still drives the pad. The frame loop is uncapped for a replay run
  // (native_boot.cpp), and the run-end line below reports how far the replay got.
  bool replayPending() const {
    return mSession.replayPending();
  }
  // The replay's own denominators at run end: segments and frames delivered, frames waited per phase,
  // button frames dropped, and whether it COMPLETED, STALLED, or was TRUNCATED by the run ending.
  // Silent when no replay was loaded.
  void reportReplayRunEnd() const {
    if (const psx::input::PhaseReplay *replay = mSession.replay()) {
      replay->reportRunEnd();
    }
  }

  // A host-only screen (a title picker) wants the live keyboard/controller and the control channel's
  // presses, and none of a run's recording: no default `scratch/bin/pad_session.pad` sink and no
  // PSXPORT_PAD_REPLAY, which belong to the title started after it. Call before the first
  // serviceFrame(). The in-memory recording keeps running, so `padrec save` still answers.
  void useLiveInputOnly() {
    mLiveInputOnly = true;
  }

  // The HOST owns the player's input for now, so this session's guest must not see it. A process
  // running several sessions reads the pad itself to pick a panel; without this claim every session
  // delivers the selecting press to its own guest. The pad keeps polling, recording and learning host
  // keys, but the mask handed to the guest — and to the recording and the replay, so all three agree —
  // is "nothing pressed". Distinct from useLiveInputOnly (the sink) and the REPL drive (a debug
  // channel with its own replay interactions).
  void setPlayerInputSuppressed(bool suppressed) {
    mPlayerInputSuppressed = suppressed;
  }
  bool playerInputSuppressed() const {
    return mPlayerInputSuppressed;
  }

  // Slot-1 controller presence is game policy. The default remains absent so existing single-pad
  // ports retain their current guest-visible packet; a title whose guest reads both slots opts in.
  void setSlot1Connected(bool connected) {
    mSlot1Connected = connected;
  }

private:
  psx::input::HostInput &mHost; // the host input owner (host_input.h)
  ActiveLowEdges mButtonEdges;
  bool mSlot1Connected = false;

  // ---- serviceFrame test hooks / config caches ----
  int mForceInit = 0, mForceOn = 0;
  uint16_t mForceMask = psx::input::kNoButtons;
  uint32_t mFc = 0;                            // internal frame counter for the pulse (== native frame index)
  uint16_t mHoldMask = psx::input::kNoButtons; // headless test hook: a HELD (not pulsed) mask...
  uint32_t mHoldAt = 0;                        // ...applied from this native frame onward
  long mStopAt = -2;                           // PSXPORT_FORCE_STOP_AT (-2 = not read, -1 = off)

  // ---- input record / replay + schedules ----
  psx::input::PadRecordReplay mSession;
  bool mLiveInputOnly = false;             // a host-only screen: no sink, no replay, live input only
  bool mPlayerInputSuppressed = false;     // the host owns input; the guest's mask is nothing pressed
  uint64_t currentPhase(Core &core) const; // the title's input phase this frame (GameRuntime::inputPhase)
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
