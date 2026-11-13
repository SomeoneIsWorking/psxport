// host_input.cpp — psx::input::HostInput: the SDL event drain, the delivered host key state, the
// gamepads, and the PSX active-low button mask they add up to.
#include "host_input.h"
#include "diagnostic_config.h" // PSXPORT_PAD_NOPAD
#ifdef PSXPORT_SDL
#include "input_binding.h"
#endif
#include "rmlui_overlay.h"
#include <lucent/log.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <string>

#ifdef PSXPORT_SDL
#include <SDL3/SDL.h>
#include <stdlib.h> // atoi (PSXPORT_PAD_NOPAD parse)
#endif

std::uint16_t psx::input::buttonNamed(std::string_view name) {
  struct ButtonName {
    std::string_view name;
    std::uint16_t bit;
  };
  constexpr std::array kNames{
      ButtonName{"select", kButtonSelect}, ButtonName{"l3", kButtonL3},
      ButtonName{"r3", kButtonR3},         ButtonName{"start", kButtonStart},
      ButtonName{"up", kButtonUp},         ButtonName{"right", kButtonRight},
      ButtonName{"down", kButtonDown},     ButtonName{"left", kButtonLeft},
      ButtonName{"l2", kButtonL2},         ButtonName{"r2", kButtonR2},
      ButtonName{"l1", kButtonL1},         ButtonName{"r1", kButtonR1},
      ButtonName{"t", kButtonTriangle},    ButtonName{"triangle", kButtonTriangle},
      ButtonName{"o", kButtonCircle},      ButtonName{"circle", kButtonCircle},
      ButtonName{"x", kButtonCross},       ButtonName{"cross", kButtonCross},
      ButtonName{"sq", kButtonSquare},     ButtonName{"square", kButtonSquare},
  };
  for (const ButtonName &entry : kNames) {
    if (entry.name == name) {
      return entry.bit;
    }
  }
  return 0;
}

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

#ifdef PSXPORT_SDL
namespace {
// A stick or trigger pushed this far is the player using that pad, not drift.
constexpr int kDeliberateAxis = 22000;
} // namespace
#endif

void psx::input::HostInput::drainEvents() {
#ifdef PSXPORT_SDL
  SDL_PumpEvents();
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
      noteHostKey(static_cast<int>(event.key.scancode), event.type == SDL_EVENT_KEY_DOWN);
    }
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
      mGlyphs.noteKeyboard();
    }
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
      mGlyphs.noteGamepad(SDL_GetGamepadTypeForID(event.gbutton.which));
    }
    if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
        std::abs(static_cast<int>(event.gaxis.value)) > kDeliberateAxis) {
      mGlyphs.noteGamepad(SDL_GetGamepadTypeForID(event.gaxis.which));
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
// Buttons through kGamepadBindings, the analog triggers thresholded; the d-pad AND the left stick are directions.
//
// Only a deliberate push registers; a resting or stale stick must not hold a direction.
constexpr Sint16 kDirectionalDeflection = kDeliberateAxis;

// Two pads on one port: each axis takes the farther-pushed stick.
std::int16_t fartherAxis(std::int16_t held, std::int16_t read) {
  return std::abs(static_cast<int>(read)) > std::abs(static_cast<int>(held)) ? read : held;
}

void applyRightStick(SDL_Gamepad *gamepad, psx::input::StickAxes &stick) {
  stick.x = fartherAxis(stick.x, SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
  stick.y = fartherAxis(stick.y, SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
}

void applyGamepad(SDL_Gamepad *gamepad, std::uint16_t &mask) {
  for (const psx::input::GamepadBinding &binding : psx::input::kGamepadBindings) {
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

std::string
psx::input::HostInput::padButton(std::uint16_t bit, InjectedPress press, int holdFrames, std::uint32_t port) {
  if (const std::optional<GamepadBinding> binding = gamepadBinding(bit)) {
    return binding->button != SDL_GAMEPAD_BUTTON_INVALID
               ? mInjection.gamepadButton(binding->button, press, holdFrames, TapClock::PadFrames, port)
               : mInjection.gamepadTrigger(binding->trigger, press, holdFrames, TapClock::PadFrames, port);
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
          mGlyphs.noteGamepad(SDL_GetGamepadType(opened));
        }
        break;
      }
    }
  }
  SDL_free(instances);
}

psx::input::HostPads psx::input::HostInput::poll(bool shown, std::uint32_t ports) {
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

  // While the overlay holds input (its menu open, or a text field typing) the keyboard and the pads are its, so a
  // key or a button that moves the menu does not also move the guest.
  const bool gameOwnsInput = overlay == nullptr || !overlay->holdsInput();
  if (gameOwnsInput) {
    for (const KeyBinding &binding : kKeyboardBindings) {
      if (keyDown(binding.key) || (binding.alternate != SDL_SCANCODE_UNKNOWN && keyDown(binding.alternate))) {
        mask &= static_cast<std::uint16_t>(~binding.bit);
      }
    }
  }

  // A pad opened this poll has stale state until SDL_UpdateGamepads, and a stale axis reads as a held direction.
  rescanControllers(shown);
  SDL_UpdateGamepads();
  const std::uint16_t keyboardMask = mask;
  HostPads pads;
  pads.masks[0] = mask;
  std::uint32_t physical = 0;
  for (int slot = 0; slot < kMaxGamepads; slot++) {
    if (!mGamepads[slot]) {
      continue;
    }
    std::uint32_t port = 0;
    if (ports > 1) {
      const std::optional<std::uint32_t> virtualPort =
          mInjection.virtualPadPort(static_cast<std::uint32_t>(mGamepadInstance[slot]));
      port = virtualPort ? *virtualPort : std::min(physical++, ports - 1);
    }
    if (gameOwnsInput) {
      applyGamepad(mGamepads[slot], pads.masks[port]);
      applyRightStick(mGamepads[slot], pads.rightSticks[port]);
    }
    pads.port2Present = pads.port2Present || port == 1;
  }
  mask = pads.masks[0];

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
  return pads;
}

#else // !PSXPORT_SDL

// No host input at all in a build without SDL: there is no window to read and no controller to open,
// so the mask is "nothing pressed" and no debug-key edge can exist.
psx::input::HostPads psx::input::HostInput::poll(bool shown, std::uint32_t ports) {
  (void)shown;
  (void)ports;
  drainEvents();
  mPauseRequested = false;
  mFrameStepRequested = false;
  mBugReportRequested = false;
  return {};
}

std::string psx::input::HostInput::padButton(std::uint16_t bit, InjectedPress, int, std::uint32_t) {
  return lucent::format("pad: 0x{:04X}: this build has no SDL gamepad", bit);
}

#endif // PSXPORT_SDL

std::string
psx::input::HostInput::padButtons(std::uint16_t bits, InjectedPress press, int holdFrames, std::uint32_t port) {
  std::string reply;
  for (std::uint16_t bit = 1; bit != 0; bit = static_cast<std::uint16_t>(bit << 1)) {
    if ((bits & bit) != 0) {
      reply += reply.empty() ? "" : "; ";
      reply += padButton(bit, press, holdFrames, port);
    }
  }
  return reply.empty() ? std::string("pad: no button named") : reply;
}
