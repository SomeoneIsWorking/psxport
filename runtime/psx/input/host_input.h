// host_input.h — class psx::input::HostInput: the ONE owner of this process's host input.
//
// WHAT IT OWNS, and nothing else:
//   * the SDL event queue drain — every pump site goes through the same function;
//   * the delivered host key state, one bit per SDL scancode;
//   * the SDL gamepads: open, hotswap, close, and their contribution to the button mask;
//   * whether the GAME or the overlay currently owns the keyboard;
//   * the P / '.' / B debug-key edges, latched here and acted on by whoever owns the action.
//
// It knows nothing about the PSX guest, the pad packet, the frame clock, a recording or a replay:
// those belong to psx::input::Pad, which consumes the mask this owner produces. One host-input
// owner means one place where "is the player holding a key" can be answered, and one place where a
// host event can be dropped by accident.
#pragma once
#include <cstdint>

class RmlOverlay;
struct SDL_Gamepad; // opaque; only held as pointers (SDL build only)

namespace psx::input {

// PSX digital button bits are ACTIVE-LOW: a cleared bit is a pressed button, and nothing pressed is
// every bit set. This owner and the pad it feeds both speak that layout, so nothing converts between
// them, and these are the only spellings of it.
inline constexpr std::uint16_t kNoButtons = 0xFFFFu;
inline constexpr std::uint16_t kButtonStart = 0x0008u;

class HostInput {
public:
  HostInput() = default;
  HostInput(const HostInput &) = delete;
  HostInput &operator=(const HostInput &) = delete;

  // The overlay consumes the same events this owner drains, so it is an explicit dependency rather
  // than a per-event lookup through the Game. Optional: with no overlay attached, events are still
  // drained and recorded.
  void attachOverlay(RmlOverlay *overlay) {
    mOverlay = overlay;
  }

  // Drain the host event queue: record every delivered key into the state below, hand every event to
  // the overlay, and RECORD a window close for `quitRequested`.
  //
  // EVERY site that pumps host input calls this, directly or through poll(). Two drains of one queue
  // is how an event gets consumed by the wrong owner — a press that arrived while a guest draw was
  // presenting is lost if the present-path drain is the only one that learns about it, and a window
  // close is ignored if the other drain swallowed it. One function, any number of call sites, each
  // event delivered exactly once to each consumer. Safe to call several times per frame.
  //
  // A window close is a REQUEST, and ending the run from inside an event drain is how a product could
  // not choose how it stopped. A caller that owns its run holds a `QuitScope` and asks
  // `quitRequested()`; with no scope alive the drain ends the process, which is what every product
  // did before the request existed.
  void drainEvents();

  // Whether the host window has been closed since this was last taken. Consumed by the product loop
  // that owns the run.
  bool quitRequested() const {
    return mQuitRequested;
  }
  bool takeQuitRequest() {
    const bool requested = mQuitRequested;
    mQuitRequested = false;
    return requested;
  }

  // The active-low PSX button mask for this frame, from the host keyboard and every connected
  // controller, additive so both work at once. Drains first, so a caller only ever has to pump here.
  //
  // `windowAvailable` is the caller's answer to "is a live on-screen window up". A leg with no window
  // has no host input at all, so it answers kNoButtons and drops any pending debug-key edge rather
  // than resolving one from a keyboard no window is reading.
  std::uint16_t poll(bool windowAvailable);

  // Take the P (pause / resume) edge the last poll detected, and the '.' (freeze, advance one frame)
  // edge. Edge-detected once per poll and latched until taken, so one press is one action no matter
  // which turn consumes it. Taking is separate from asking because the ACTION is not this owner's:
  // the debug channel owns pause and step state (DbgServer::togglePause / addStep).
  bool takePauseRequest() {
    const bool requested = mPauseRequested;
    mPauseRequested = false;
    return requested;
  }
  bool takeFrameStepRequest() {
    const bool requested = mFrameStepRequested;
    mFrameStepRequested = false;
    return requested;
  }
  // The B (file a bug report) edge; the action is psx::debug::BugReportSession's.
  bool takeBugReportRequest() {
    const bool requested = mBugReportRequested;
    mBugReportRequested = false;
    return requested;
  }

private:
#ifdef PSXPORT_SDL
  bool keyDown(int scancode) const;
  void noteHostKey(int scancode, bool down);
  void ensureGamepadSubsystem();
  void rescanControllers();
  // PSXPORT_PAD_NOPAD: ignore every controller and use the keyboard only. For a machine where a
  // connected or phantom pad reports a drifting analog stick and cannot be unplugged.
  bool noPadRequested();
#endif

  RmlOverlay *mOverlay = nullptr;

#ifdef PSXPORT_SDL
  // The delivered host key state, fed from the events themselves rather than from SDL's keyboard array
  // (see poll()). 512 is SDL_NUM_SCANCODES, spelled as a number so this header stays free of SDL
  // types; every scancode that arrives is bounds-checked against it.
  static constexpr int kHostKeyStates = 512;
  bool mKeyDown[kHostKeyStates] = {};

  // Up to kMaxGamepads simultaneously open controllers, hotswap-aware.
  static constexpr int kMaxGamepads = 4;
  SDL_Gamepad *mGamepads[kMaxGamepads] = {};
  int mGamepadInstance[kMaxGamepads] = {-1, -1, -1, -1}; // SDL_JoystickID per slot (-1 = empty)
  bool mGamepadSubsystemInit = false;                    // lazily added the gamepad subsystem?
  int mNoPad = -1;                                       // PSXPORT_PAD_NOPAD cache (-1 = not read)
  bool mPrevPauseKey = false;                            // P / '.' debug-key edge detectors
  bool mPrevStepKey = false;
  bool mPrevBugReportKey = false;
  bool mPadDirsWarned = false; // "controller is driving directions" once-notice
#endif

  bool mPauseRequested = false;
  bool mFrameStepRequested = false;
  bool mBugReportRequested = false;
  bool mQuitRequested = false;
  // How many `QuitScope`s are alive. While at least one is, a window close is recorded rather than
  // ending the process — see drainEvents(). Private, with `QuitScope` the only way to count one: the
  // count is a property of a composition, not a setting.
  int mQuitOwners = 0;

  friend class QuitScope;
};

// While this is alive, a host window close is RECORDED instead of ending the process, so the loop
// that owns the run decides what ending it means. The scope is what makes that a property of a
// composition rather than a global switch: a product that has not adopted an owned end-of-run keeps
// the process exit it always had, because no scope is alive to record instead.
class QuitScope {
public:
  explicit QuitScope(HostInput &input) : input_(&input) {
    ++input_->mQuitOwners;
  }
  ~QuitScope() {
    --input_->mQuitOwners;
  }
  QuitScope(const QuitScope &) = delete;
  QuitScope &operator=(const QuitScope &) = delete;

private:
  HostInput *input_;
};

} // namespace psx::input
