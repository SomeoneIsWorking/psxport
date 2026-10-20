// host_input.cpp — psx::input::HostInput: the SDL event drain, the delivered host key state, the
// gamepads, and the PSX active-low button mask they add up to.
#include "host_input.h"
#include "diagnostic_config.h" // PSXPORT_PAD_NOPAD
#include "rmlui_overlay.h"
#include <lucent/log.h>

#ifdef PSXPORT_SDL
#include <SDL3/SDL.h>
#include <stdlib.h> // atoi (PSXPORT_PAD_NOPAD parse)
#endif

void psx::input::HostInput::drainEvents() {
#ifdef PSXPORT_SDL
  SDL_PumpEvents();
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
      noteHostKey(static_cast<int>(event.key.scancode), event.type == SDL_EVENT_KEY_DOWN);
    }
    // Every event reaches the overlay, so it keeps exactly the input it had when the present-path
    // drain was its only source: two drains of one queue are safe for the same reason the drain is
    // one function — whichever gets an event passes it on here.
    if (mOverlay) {
      mOverlay->event(&event);
    }
    if (event.type == SDL_EVENT_QUIT) {
      if (mQuitScope != nullptr) {
        mQuitScope->request();
      } else {
        exit(0);
      }
    }
  }
#endif
}

#ifdef PSXPORT_SDL

// SDL3 applies key events to its state array only for the focused window, so delivered events are the truth.
void psx::input::HostInput::noteHostKey(int scancode, bool down) {
  if (scancode > 0 && scancode < kHostKeyStates) {
    mKeyDown[scancode] = down;
  }
}

bool psx::input::HostInput::keyDown(int scancode) const {
  if (scancode > 0 && scancode < kHostKeyStates && mKeyDown[scancode]) {
    return true;
  }
  const bool *keys = SDL_GetKeyboardState(nullptr);
  return keys != nullptr && keys[scancode] != 0;
}

namespace {

// OR one controller's buttons and analog sticks into the active-low PSX mask.
//
// Button mapping (Sony layout, natural for a PSX title): A -> Cross, B -> Circle, X -> Square,
// Y -> Triangle (SDL's A/B/X/Y are positional SNES-style); shoulders -> L1/R1; the analog triggers
// thresholded -> L2/R2; Start -> Start, Back -> Select, stick clicks -> L3/R3. The d-pad AND the left
// stick are directions.
//
// Only a deliberate push registers; a resting or stale stick must not hold a direction.
constexpr Sint16 kDirectionalDeflection = 22000; // ~67% of 32767

void applyGamepad(SDL_Gamepad *gamepad, std::uint16_t &mask) {
#define HOST_BUTTON(button, bit)                                                                                       \
  if (SDL_GetGamepadButton(gamepad, (button))) {                                                                       \
    mask &= static_cast<std::uint16_t>(~(bit));                                                                        \
  }
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_UP, psx::input::kButtonUp)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, psx::input::kButtonRight)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_DOWN, psx::input::kButtonDown)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_LEFT, psx::input::kButtonLeft)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_START, psx::input::kButtonStart)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_BACK, psx::input::kButtonSelect)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_SOUTH, psx::input::kButtonCross)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_EAST, psx::input::kButtonCircle)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_WEST, psx::input::kButtonSquare)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_NORTH, psx::input::kButtonTriangle)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, psx::input::kButtonL1)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, psx::input::kButtonR1)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_LEFT_STICK, psx::input::kButtonL3)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_RIGHT_STICK, psx::input::kButtonR3)
#undef HOST_BUTTON
  const Sint16 lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
  const Sint16 ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
  if (ly < -kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~psx::input::kButtonUp);
  }
  if (lx > kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~psx::input::kButtonRight);
  }
  if (ly > kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~psx::input::kButtonDown);
  }
  if (lx < -kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~psx::input::kButtonLeft);
  }
  if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~psx::input::kButtonL2);
  }
  if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~psx::input::kButtonR2);
  }
}

} // namespace

// Lazily ensure SDL's gamepad subsystem is up. SDL_Init(SDL_INIT_VIDEO) happens with the present
// device, which this owner does not own; the gamepad subsystem is independent, so it is added on
// first use. Idempotent.
void psx::input::HostInput::ensureGamepadSubsystem() {
  if (mGamepadSubsystemInit) {
    return;
  }
  if ((SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) == 0) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  }
  mGamepadSubsystemInit = true;
}

bool psx::input::HostInput::noPadRequested() {
  if (mNoPad < 0) {
    mNoPad = psx::config::no_host_pads() ? 1 : 0;
  }
  return mNoPad != 0;
}

// HOTSWAP: open any newly-connected controller and drop any that vanished, every poll. Deliberately a
// rescan rather than a device-added/removed handler, because the controller events are delivered to
// whoever drains the queue and this owner cannot depend on being that site. SDL_NumJoysticks is a
// count, and only indices not already held are opened.
void psx::input::HostInput::rescanControllers() {
  if (noPadRequested()) {
    for (int slot = 0; slot < kMaxGamepads; slot++) {
      if (mGamepads[slot]) {
        SDL_CloseGamepad(mGamepads[slot]);
        mGamepads[slot] = nullptr;
        mGamepadInstance[slot] = -1;
      }
    }
    return;
  }
  ensureGamepadSubsystem();
  for (int slot = 0; slot < kMaxGamepads; slot++) {
    if (mGamepads[slot] && !SDL_GamepadConnected(mGamepads[slot])) {
      SDL_CloseGamepad(mGamepads[slot]);
      mGamepads[slot] = nullptr;
      mGamepadInstance[slot] = -1;
    }
  }
  int count = 0;
  SDL_JoystickID *instances = SDL_GetGamepads(&count);
  for (int i = 0; i < count; i++) {
    const SDL_JoystickID instance = instances[i];
    bool held = false;
    for (int slot = 0; slot < kMaxGamepads; slot++) {
      held = held || mGamepadInstance[slot] == static_cast<int>(instance);
    }
    if (held) {
      continue;
    }
    for (int slot = 0; slot < kMaxGamepads; slot++) {
      if (!mGamepads[slot]) {
        if (SDL_Gamepad *opened = SDL_OpenGamepad(instance)) {
          mGamepads[slot] = opened;
          mGamepadInstance[slot] = static_cast<int>(SDL_GetGamepadID(opened));
        }
        break;
      }
    }
  }
  SDL_free(instances);
}

std::uint16_t psx::input::HostInput::poll(bool windowAvailable) {
  drainEvents();
  if (!windowAvailable) {
    mPauseRequested = false;
    mFrameStepRequested = false;
    mBugReportRequested = false;
    return kNoButtons;
  }

  // Debug pause / frame-step / bug-report keys, edge-detected so one keypress is one action, and read
  // from the same host key state as gameplay — which means they keep working while the overlay owns
  // the gameplay keys, and on a window that does not hold focus. Not while the player is TYPING into
  // an overlay text field, though: there a "p" or a "b" is a letter, not a command.
  const bool typing = mOverlay != nullptr && mOverlay->typingText();
  const bool pauseKey = keyDown(SDL_SCANCODE_P);
  const bool stepKey = keyDown(SDL_SCANCODE_PERIOD);
  const bool bugReportKey = keyDown(SDL_SCANCODE_B);
  mPauseRequested = !typing && pauseKey && !mPrevPauseKey;
  mFrameStepRequested = !typing && stepKey && !mPrevStepKey;
  mBugReportRequested = !typing && bugReportKey && !mPrevBugReportKey;
  mPrevPauseKey = pauseKey;
  mPrevStepKey = stepKey;
  mPrevBugReportKey = bugReportKey;

  std::uint16_t mask = kNoButtons;

  // While the overlay is actively TYPING into a text widget it owns the keyboard, so a character the
  // player typed into a text field does not also move the guest. The overlay reports that itself; this
  // is where the answer gates the gamepad-like keyboard read below.
  const bool gameOwnsKeyboard = mOverlay == nullptr || !mOverlay->wantsKeyboard();
  if (gameOwnsKeyboard) {
    if (keyDown(SDL_SCANCODE_UP) || keyDown(SDL_SCANCODE_W)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonUp);
    }
    if (keyDown(SDL_SCANCODE_RIGHT) || keyDown(SDL_SCANCODE_D)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonRight);
    }
    if (keyDown(SDL_SCANCODE_DOWN) || keyDown(SDL_SCANCODE_S)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonDown);
    }
    if (keyDown(SDL_SCANCODE_LEFT) || keyDown(SDL_SCANCODE_A)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonLeft);
    }
    if (keyDown(SDL_SCANCODE_RETURN)) {
      mask &= static_cast<std::uint16_t>(~kButtonStart);
    }
    if (keyDown(SDL_SCANCODE_RSHIFT) || keyDown(SDL_SCANCODE_TAB)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonSelect);
    }
    if (keyDown(SDL_SCANCODE_K)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonCross);
    }
    if (keyDown(SDL_SCANCODE_L)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonCircle);
    }
    if (keyDown(SDL_SCANCODE_I)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonTriangle);
    }
    if (keyDown(SDL_SCANCODE_J)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonSquare);
    }
    if (keyDown(SDL_SCANCODE_Q)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonL1);
    }
    if (keyDown(SDL_SCANCODE_E)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonR1);
    }
    if (keyDown(SDL_SCANCODE_1)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonL2);
    }
    if (keyDown(SDL_SCANCODE_3)) {
      mask &= static_cast<std::uint16_t>(~psx::input::kButtonR2);
    }
  }

  // SDL_UpdateGamepads() FIRST, because SDL_PumpEvents only refreshes controller state if the gamepad
  // subsystem happened to be up when it ran, and this owner initialises it lazily — so on the frames
  // right after a controller is opened the reads below could be stale, and a stale axis reads as a
  // held direction.
  rescanControllers();
  SDL_UpdateGamepads();
  const std::uint16_t keyboardMask = mask;
  for (int slot = 0; slot < kMaxGamepads; slot++) {
    if (mGamepads[slot]) {
      applyGamepad(mGamepads[slot], mask);
    }
  }

  // A controller supplying directions while the keyboard supplied none is the one cause of "WASD is
  // dead" that the player cannot see, so it is reported once: a connected or phantom pad (a drifting
  // analog stick in "analog mode") is the input, not the keyboard. Active-low, so a CHANGED direction
  // bit is a cleared one. PSXPORT_PAD_NOPAD=1 is the documented way out when it cannot be unplugged.
  if (!mPadDirsWarned) {
    if ((keyboardMask & psx::input::kButtonDirections) == psx::input::kButtonDirections && (mask & psx::input::kButtonDirections) != psx::input::kButtonDirections) {
      lucent::info("pad",
                   "a game controller is driving DIRECTIONS (host pad mask=0x{:04x}). If WASD seems dead, an analog "
                   "stick / phantom pad is the input. Set PSXPORT_PAD_NOPAD=1 to use the keyboard only.",
                   (unsigned)mask);
      mPadDirsWarned = true;
    }
  }
  return mask;
}

#else // !PSXPORT_SDL

// No host input at all in a build without SDL: there is no window to read and no controller to open,
// so the mask is "nothing pressed" and no debug-key edge can exist.
std::uint16_t psx::input::HostInput::poll(bool windowAvailable) {
  (void)windowAvailable;
  drainEvents();
  mPauseRequested = false;
  mFrameStepRequested = false;
  mBugReportRequested = false;
  return kNoButtons;
}

#endif // PSXPORT_SDL
