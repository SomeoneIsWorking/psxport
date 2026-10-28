// host_input.cpp — psx::input::HostInput: the SDL event drain, the delivered host key state, the
// gamepads, and the PSX active-low button mask they add up to.
#include "host_input.h"
#include "diagnostic_config.h" // PSXPORT_PAD_NOPAD
#include "rmlui_overlay.h"
#include <lucent/log.h>

#include <array>
#include <string>

#ifdef PSXPORT_SDL
#include <SDL3/SDL.h>
#include <stdlib.h> // atoi (PSXPORT_PAD_NOPAD parse)
#endif

#ifdef PSXPORT_SDL
psx::input::HostInput::HostInput() {
  mGamepadSubsystem = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  if (!mGamepadSubsystem) {
    lucent::error("pad", "SDL_InitSubSystem(GAMEPAD) failed: {}; controllers disabled", SDL_GetError());
  }
}

psx::input::HostInput::~HostInput() {
  for (SDL_Gamepad *&gamepad : mGamepads) {
    if (gamepad != nullptr) {
      SDL_CloseGamepad(gamepad);
      gamepad = nullptr;
    }
  }
  if (mGamepadSubsystem) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
}
#else
psx::input::HostInput::HostInput() = default;
psx::input::HostInput::~HostInput() = default;
#endif

void psx::input::HostInput::attachOverlay(RmlOverlay *overlay) {
  mOverlays.push_back({.overlay = overlay, .live = true});
}

void psx::input::HostInput::detachOverlay(const RmlOverlay *overlay) {
  std::erase_if(mOverlays, [overlay](const AttachedOverlay &attached) {
    return attached.overlay == overlay;
  });
}

void psx::input::HostInput::setOverlayLive(const RmlOverlay *overlay, bool live) {
  for (AttachedOverlay &attached : mOverlays) {
    if (attached.overlay == overlay) {
      attached.live = live;
    }
  }
}

RmlOverlay *psx::input::HostInput::liveOverlay() const {
  for (const AttachedOverlay &attached : mOverlays) {
    if (attached.live) {
      return attached.overlay;
    }
  }
  return nullptr;
}

void psx::input::HostInput::drainEvents() {
#ifdef PSXPORT_SDL
  SDL_PumpEvents();
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
      noteHostKey(static_cast<int>(event.key.scancode), event.type == SDL_EVENT_KEY_DOWN);
    }
    if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED) {
      mPadsChanged = true;
    }
    if (RmlOverlay *overlay = liveOverlay()) {
      overlay->event(&event);
    }
    if (event.type == SDL_EVENT_QUIT) {
      if (mQuitScope != nullptr) {
        mQuitScope->request();
      } else {
        exit(0);
      }
    }
  }
  mInjection.afterDrain();
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

// Each PSX button on an SDL gamepad: a button, or for L2/R2 a trigger axis.
struct GamepadBinding {
  std::uint16_t bit;
  SDL_GamepadButton button;
  SDL_GamepadAxis trigger;
};
constexpr std::array kGamepadBindings{
    GamepadBinding{psx::input::kButtonUp, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonStart, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonSelect, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonCross, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonCircle, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonSquare, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonTriangle, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonL3, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonR3, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{psx::input::kButtonL2, SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
    GamepadBinding{psx::input::kButtonR2, SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
};

void applyGamepad(SDL_Gamepad *gamepad, std::uint16_t &mask) {
  for (const GamepadBinding &binding : kGamepadBindings) {
    const bool down = binding.button != SDL_GAMEPAD_BUTTON_INVALID
                          ? SDL_GetGamepadButton(gamepad, binding.button)
                          : SDL_GetGamepadAxis(gamepad, binding.trigger) > kDirectionalDeflection;
    if (down) {
      mask &= static_cast<std::uint16_t>(~binding.bit);
    }
  }
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
}

} // namespace

std::string psx::input::HostInput::padButton(std::uint16_t bit, InjectedPress press, int holdFrames) {
  for (const GamepadBinding &binding : kGamepadBindings) {
    if (binding.bit == bit) {
      return binding.button != SDL_GAMEPAD_BUTTON_INVALID
                 ? mInjection.gamepadButton(binding.button, press, holdFrames, TapClock::PadFrames)
                 : mInjection.gamepadTrigger(binding.trigger, press, holdFrames, TapClock::PadFrames);
    }
  }
  return lucent::format("pad: 0x{:04X} is not one PSX button", bit);
}

bool psx::input::HostInput::noPadRequested() {
  if (mNoPad < 0) {
    mNoPad = psx::config::no_host_pads() ? 1 : 0;
  }
  return mNoPad != 0;
}

// Hot-swap: open any newly-connected controller and drop any that vanished, after a gamepad added or removed
// event this owner drained, or when physical pads are admitted or refused. Only instances not already held are opened.
void psx::input::HostInput::rescanControllers(bool shown) {
  if (!mGamepadSubsystem) {
    return;
  }
  // A tool window and PSXPORT_PAD_NOPAD read only the virtual pad, the control channel's.
  const bool physicalPads = shown && !noPadRequested();
  if (!mPadsChanged && physicalPads == mPhysicalPadsScanned) {
    return;
  }
  mPadsChanged = false;
  mPhysicalPadsScanned = physicalPads;
  for (int slot = 0; slot < kMaxGamepads; slot++) {
    if (mGamepads[slot] && (!SDL_GamepadConnected(mGamepads[slot]) ||
                            (!physicalPads && !SDL_IsJoystickVirtual(SDL_GetGamepadID(mGamepads[slot]))))) {
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
    if (held || (!physicalPads && !SDL_IsJoystickVirtual(instance))) {
      continue;
    }
    for (int slot = 0; slot < kMaxGamepads; slot++) {
      if (!mGamepads[slot]) {
        if (SDL_Gamepad *opened = SDL_OpenGamepad(instance)) {
          mGamepads[slot] = opened;
          mGamepadInstance[slot] = static_cast<int>(SDL_GetGamepadID(opened));
          lucent::info("pad", "controller {} opened in slot {}: {}", instance, slot, SDL_GetGamepadName(opened));
          mInjection.afterGamepadOpened(instance);
        }
        break;
      }
    }
  }
  SDL_free(instances);
}

std::uint16_t psx::input::HostInput::poll(bool shown) {
  drainEvents();

  // Debug pause / frame-step / bug-report keys, edge-detected so one keypress is one action, and read
  // from the same host key state as gameplay — which means they keep working while the overlay owns
  // the gameplay keys, and on a window that does not hold focus. Not while the player is TYPING into
  // an overlay text field, though: there a "p" or a "b" is a letter, not a command.
  const RmlOverlay *overlay = liveOverlay();
  const bool typing = overlay != nullptr && overlay->typingText();
  const bool pauseKey = keyDown(SDL_SCANCODE_P);
  const bool stepKey = keyDown(SDL_SCANCODE_PERIOD);
  const bool bugReportKey = keyDown(SDL_SCANCODE_B);
  mPauseRequested = mPauseRequested || (!typing && pauseKey && !mPrevPauseKey);
  mFrameStepRequested = mFrameStepRequested || (!typing && stepKey && !mPrevStepKey);
  mBugReportRequested = mBugReportRequested || (!typing && bugReportKey && !mPrevBugReportKey);
  mPrevPauseKey = pauseKey;
  mPrevStepKey = stepKey;
  mPrevBugReportKey = bugReportKey;

  std::uint16_t mask = kNoButtons;

  // While the overlay is actively TYPING into a text widget it owns the keyboard, so a character the
  // player typed into a text field does not also move the guest. The overlay reports that itself; this
  // is where the answer gates the gamepad-like keyboard read below.
  const bool gameOwnsKeyboard = overlay == nullptr || !overlay->wantsKeyboard();
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

  // A pad opened this poll has stale state until SDL_UpdateGamepads, and a stale axis reads as a held direction.
  rescanControllers(shown);
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
    if ((keyboardMask & psx::input::kButtonDirections) == psx::input::kButtonDirections &&
        (mask & psx::input::kButtonDirections) != psx::input::kButtonDirections) {
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
std::uint16_t psx::input::HostInput::poll(bool shown) {
  (void)shown;
  drainEvents();
  mPauseRequested = false;
  mFrameStepRequested = false;
  mBugReportRequested = false;
  return kNoButtons;
}

std::string psx::input::HostInput::padButton(std::uint16_t bit, InjectedPress, int) {
  return lucent::format("pad: 0x{:04X}: this build has no SDL gamepad", bit);
}

#endif // PSXPORT_SDL

std::string psx::input::HostInput::padButtons(std::uint16_t bits, InjectedPress press, int holdFrames) {
  std::string reply;
  for (std::uint16_t bit = 1; bit != 0; bit = static_cast<std::uint16_t>(bit << 1)) {
    if ((bits & bit) != 0) {
      reply += reply.empty() ? "" : "; ";
      reply += padButton(bit, press, holdFrames);
    }
  }
  return reply.empty() ? std::string("pad: no button named") : reply;
}
