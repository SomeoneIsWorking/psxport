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

// The digital pad's button bits; masks are active-low, so nothing pressed is every bit set.
inline constexpr std::uint16_t kNoButtons = 0xFFFFu;
inline constexpr std::uint16_t kButtonSelect = 0x0001u;
inline constexpr std::uint16_t kButtonL3 = 0x0002u;
inline constexpr std::uint16_t kButtonR3 = 0x0004u;
inline constexpr std::uint16_t kButtonStart = 0x0008u;
inline constexpr std::uint16_t kButtonUp = 0x0010u;
inline constexpr std::uint16_t kButtonRight = 0x0020u;
inline constexpr std::uint16_t kButtonDown = 0x0040u;
inline constexpr std::uint16_t kButtonLeft = 0x0080u;
inline constexpr std::uint16_t kButtonL2 = 0x0100u;
inline constexpr std::uint16_t kButtonR2 = 0x0200u;
inline constexpr std::uint16_t kButtonL1 = 0x0400u;
inline constexpr std::uint16_t kButtonR1 = 0x0800u;
inline constexpr std::uint16_t kButtonTriangle = 0x1000u;
inline constexpr std::uint16_t kButtonCircle = 0x2000u;
inline constexpr std::uint16_t kButtonCross = 0x4000u;
inline constexpr std::uint16_t kButtonSquare = 0x8000u;
inline constexpr std::uint16_t kButtonDirections = kButtonUp | kButtonRight | kButtonDown | kButtonLeft;

class QuitScope;

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
  // the overlay, and hand a window close to the attached QuitScope.
  //
  // EVERY site that pumps host input calls this, directly or through poll(). Two drains of one queue
  // is how an event gets consumed by the wrong owner — a press that arrived while a guest draw was
  // presenting is lost if the present-path drain is the only one that learns about it, and a window
  // close is ignored if the other drain swallowed it. One function, any number of call sites, each
  // event delivered exactly once to each consumer. Safe to call several times per frame.
  //
  // With no QuitScope attached a window close ends the process.
  void drainEvents();

  // The run that owns its end. The SDL queue is the process's, so every Game whose drain can see the
  // close attaches the same scope; null detaches.
  void attachQuitScope(QuitScope *scope) {
    mQuitScope = scope;
  }

  // The active-low PSX button mask for this frame, from the host keyboard and every connected
  // controller, additive so both work at once. Drains first, so a caller only ever has to pump here.
  //
  // `shown` says the window is mapped for a player. An unmapped tool window takes no host input, so it
  // answers kNoButtons and drops any pending debug-key edge; a connected controller never steers a tool run.
  std::uint16_t poll(bool shown);

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
  QuitScope *mQuitScope = nullptr;
};

// A window close recorded for the loop that owns the run, which decides what ending it means.
class QuitScope {
public:
  QuitScope() = default;
  QuitScope(const QuitScope &) = delete;
  QuitScope &operator=(const QuitScope &) = delete;

  void request() {
    requested_ = true;
  }
  bool requested() const {
    return requested_;
  }
  // Taken, so a later run in this process does not end on its first field.
  bool take() {
    const bool requested = requested_;
    requested_ = false;
    return requested;
  }

private:
  bool requested_ = false;
};

} // namespace psx::input
