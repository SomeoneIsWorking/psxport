// host_input.cpp — psx::input::HostInput: the SDL event drain, the delivered host key state, the
// gamepads, and the PSX active-low button mask they add up to.
#include "host_input.h"
#include "cfg.h" // cfg_str — PSXPORT_PAD_NOPAD
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
      // A close is a REQUEST. A product whose loop owns its end-of-run holds a `QuitScope` and asks
      // `quitRequested()`; without one the process ends here, which is what every product did before
      // the request existed.
      if (mQuitOwners > 0) {
        mQuitRequested = true;
      } else {
        exit(0);
      }
    }
  }
#endif
}

#ifdef PSXPORT_SDL

// THE HOST KEY STATE COMES FROM THE EVENTS, NOT FROM SDL_GetKeyboardState().
//
// SDL3 keeps its keyboard array per KEYBOARD-FOCUS window. When the product's window is not the
// focused one SDL still DELIVERS the key events, but it does not apply them to that array, so every
// entry reads "up". Measured on Spyro's shipping window (2026-10-01, SDL 3.0.4): a real X11 KEY_DOWN
// for SDL_SCANCODE_RETURN reached SDL_PollEvent and the array entry was still 0 on the same frame,
// so the pad mask never moved and Start/Cross did nothing.
//
// A player hits that whenever the window is not focused — after alt-tabbing back, a fullscreen
// change, a notification, or a window manager that leaves focus at the pointer root. The symptom is
// uniform and unreadable (the game ignores every press while the same key "works" under a forced mask
// or a replay), which is why every gate in the project stayed green. So the events are the truth and
// the SDL array is still OR-ed in, because it is correct whenever SDL does have focus and costs
// nothing.
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
// The directional STICK threshold is a LARGE fraction of full deflection, never a small off-centre
// value: a lower one OR-ed directions in from every connected pad with no lower guard, so a stick
// resting slightly off centre — or reading stale/extreme before the first joystick event is pumped —
// held a phantom direction continuously, which reads as "WASD doesn't move the player / maybe analog
// mode". At ~67% only a deliberate push registers and resting drift never does.
constexpr Sint16 kDirectionalDeflection = 22000; // ~67% of 32767
constexpr std::uint16_t kStart = psx::input::kButtonStart;

void applyGamepad(SDL_Gamepad *gamepad, std::uint16_t &mask) {
#define HOST_BUTTON(button, bit)                                                                                       \
  if (SDL_GetGamepadButton(gamepad, (button))) {                                                                       \
    mask &= static_cast<std::uint16_t>(~(bit));                                                                        \
  }
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0010u)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0020u)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0040u)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0080u)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_START, kStart)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_BACK, 0x0001u)  // Select
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_SOUTH, 0x4000u) // Cross  (SDL3 SOUTH = bottom)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_EAST, 0x2000u)  // Circle (EAST = right)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_WEST, 0x8000u)  // Square (WEST = left)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_NORTH, 0x1000u) // Triangle (NORTH = top)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0400u)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0800u)
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0002u)  // L3
  HOST_BUTTON(SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0004u) // R3
#undef HOST_BUTTON
  const Sint16 lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
  const Sint16 ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
  if (ly < -kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~0x0010u); // Up
  }
  if (lx > kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~0x0020u); // Right
  }
  if (ly > kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~0x0040u); // Down
  }
  if (lx < -kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~0x0080u); // Left
  }
  if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~0x0100u); // L2
  }
  if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kDirectionalDeflection) {
    mask &= static_cast<std::uint16_t>(~0x0200u); // R2
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
    const char *value = cfg_str("PSXPORT_PAD_NOPAD");
    mNoPad = (value && atoi(value) != 0) ? 1 : 0;
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
    return kNoButtons;
  }

  // Debug pause / frame-step keys, edge-detected so one keypress is one action, and read from the
  // same host key state as gameplay — which means they keep working while the overlay owns the
  // gameplay keys, and on a window that does not hold focus.
  const bool pauseKey = keyDown(SDL_SCANCODE_P);
  const bool stepKey = keyDown(SDL_SCANCODE_PERIOD);
  mPauseRequested = pauseKey && !mPrevPauseKey;
  mFrameStepRequested = stepKey && !mPrevStepKey;
  mPrevPauseKey = pauseKey;
  mPrevStepKey = stepKey;

  std::uint16_t mask = kNoButtons;

  // While the overlay is actively TYPING into a text widget it owns the keyboard, so a character the
  // player typed into a text field does not also move the guest. The overlay reports that itself; this
  // is where the answer gates the gamepad-like keyboard read below.
  const bool gameOwnsKeyboard = mOverlay == nullptr || !mOverlay->wantsKeyboard();
  if (gameOwnsKeyboard) {
    if (keyDown(SDL_SCANCODE_UP) || keyDown(SDL_SCANCODE_W)) {
      mask &= static_cast<std::uint16_t>(~0x0010u);
    }
    if (keyDown(SDL_SCANCODE_RIGHT) || keyDown(SDL_SCANCODE_D)) {
      mask &= static_cast<std::uint16_t>(~0x0020u);
    }
    if (keyDown(SDL_SCANCODE_DOWN) || keyDown(SDL_SCANCODE_S)) {
      mask &= static_cast<std::uint16_t>(~0x0040u);
    }
    if (keyDown(SDL_SCANCODE_LEFT) || keyDown(SDL_SCANCODE_A)) {
      mask &= static_cast<std::uint16_t>(~0x0080u);
    }
    if (keyDown(SDL_SCANCODE_RETURN)) {
      mask &= static_cast<std::uint16_t>(~kButtonStart);
    }
    if (keyDown(SDL_SCANCODE_RSHIFT) || keyDown(SDL_SCANCODE_TAB)) {
      mask &= static_cast<std::uint16_t>(~0x0001u); // Select
    }
    if (keyDown(SDL_SCANCODE_K)) {
      mask &= static_cast<std::uint16_t>(~0x4000u); // Cross
    }
    if (keyDown(SDL_SCANCODE_L)) {
      mask &= static_cast<std::uint16_t>(~0x2000u); // Circle
    }
    if (keyDown(SDL_SCANCODE_I)) {
      mask &= static_cast<std::uint16_t>(~0x1000u); // Triangle
    }
    if (keyDown(SDL_SCANCODE_J)) {
      mask &= static_cast<std::uint16_t>(~0x8000u); // Square
    }
    if (keyDown(SDL_SCANCODE_Q)) {
      mask &= static_cast<std::uint16_t>(~0x0400u); // L1
    }
    if (keyDown(SDL_SCANCODE_E)) {
      mask &= static_cast<std::uint16_t>(~0x0800u); // R1
    }
    if (keyDown(SDL_SCANCODE_1)) {
      mask &= static_cast<std::uint16_t>(~0x0100u); // L2
    }
    if (keyDown(SDL_SCANCODE_3)) {
      mask &= static_cast<std::uint16_t>(~0x0200u); // R2
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
    constexpr std::uint16_t kDirections = 0x00F0u; // Up/Right/Down/Left
    if ((keyboardMask & kDirections) == kDirections && (mask & kDirections) != kDirections) {
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
  return kNoButtons;
}

#endif // PSXPORT_SDL
