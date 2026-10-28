// host_input.h — class psx::input::HostInput: the ONE owner of this process's host input.
//
// WHAT IT OWNS, and nothing else:
//   * the SDL event queue drain — every pump site goes through the same function;
//   * the delivered host key state, one bit per SDL scancode;
//   * the SDL gamepads: open, hotswap, close, and their contribution to the button mask;
//   * which overlay takes the events: the one of the Game holding the player's input;
//   * whether the GAME or the overlay currently owns the keyboard;
//   * the P / '.' / B debug-key edges, latched here and acted on by whoever owns the action;
//   * the control channel's injected input, pushed into the same queue (InputInjection), and the one
//     PSX-to-SDL gamepad binding that both reads controllers and presses PSX buttons on the virtual pad.
//
// It knows nothing about the PSX guest, the pad packet, the frame clock, a recording or a replay:
// those belong to psx::input::Pad, which consumes the mask this owner produces. One per process, like
// the window whose queue it drains: every Game of the process (the picker and its panels, a title)
// reads this one, so a key's press and release always land in the same state.
#pragma once
#include "input_injection.h"

#include <cstdint>
#include <string>
#include <vector>

class RmlOverlay;
class Pad;
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
  // Brings SDL's gamepad subsystem up for the process: its device scan takes hundreds of ms, so never mid-run.
  HostInput();
  ~HostInput();
  HostInput(const HostInput &) = delete;
  HostInput &operator=(const HostInput &) = delete;

  // Each Game's overlay, from its construction to its destruction. Events go to the live one, the
  // overlay of the Game holding the player's input; with none live, events are still drained and recorded.
  void attachOverlay(RmlOverlay *overlay);
  void detachOverlay(const RmlOverlay *overlay);
  // Pad::setPlayerInputSuppressed: whether this overlay's Game holds the player's input.
  void setOverlayLive(const RmlOverlay *overlay, bool live);

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

  // The run that owns its end; null detaches.
  void attachQuitScope(QuitScope *scope) {
    mQuitScope = scope;
  }

  // The active-low PSX button mask for this frame, from the host keyboard and every connected
  // controller, additive so both work at once. Drains first, so a caller only ever has to pump here.
  //
  // `shown` says the window is mapped for a player. An unmapped tool window reads the same queue, which only
  // injected events reach, and opens only virtual gamepads, so a connected controller never steers a tool run.
  std::uint16_t poll(bool shown);

  // The control channel's keys, mouse, touch and virtual gamepad, delivered through drainEvents and poll.
  InputInjection &injection() {
    return mInjection;
  }
  // Take the P (pause / resume) edge a poll detected, and the '.' (freeze, advance one frame) edge.
  // Latched until taken, so one press is one action whichever Game's poll saw it. Taking is separate from asking
  // because the ACTION is not this owner's: the debug channel owns pause and step state (DbgServer::togglePause /
  // addStep).
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
  friend class ::Pad;
  // PSX buttons (kButton bits, active high) on the virtual gamepad, through the binding the controllers are read with;
  // a tap holds for `holdFrames` pad frames the guest samples. Reached only through Pad::injectButtons.
  std::string padButtons(std::uint16_t bits, InjectedPress press, int holdFrames);
  std::string padButton(std::uint16_t bit, InjectedPress press, int holdFrames);
#ifdef PSXPORT_SDL
  bool keyDown(int scancode) const;
  void noteHostKey(int scancode, bool down);
  void rescanControllers(bool shown);
  // PSXPORT_PAD_NOPAD: ignore every physical controller and use the keyboard only, for a machine where a
  // connected or phantom pad reports a drifting analog stick and cannot be unplugged. The virtual pad stays open.
  bool noPadRequested();
#endif

  struct AttachedOverlay {
    RmlOverlay *overlay;
    bool live;
  };
  // The overlay of the Game holding the player's input, or null.
  RmlOverlay *liveOverlay() const;

  std::vector<AttachedOverlay> mOverlays;
  InputInjection mInjection;

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
  bool mGamepadSubsystem = false;                        // this owner's reference on SDL_INIT_GAMEPAD
  bool mPadsChanged = true;                              // a gamepad was added or removed since the last scan
  bool mPhysicalPadsScanned = false;                     // whether the last scan admitted physical pads
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
