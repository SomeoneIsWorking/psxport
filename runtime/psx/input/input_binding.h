// input_binding.h — the one PSX button binding of the host's keyboard and SDL gamepads (SDL builds only).
//
// HostInput reads the devices through it and presses PSX buttons on the virtual pad through it; GlyphSet
// names a button's prompt from it.
#pragma once

#include "host_input.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <cstdint>
#include <optional>

namespace psx::input {

// A PSX button on the keyboard: the keys that press it, the first the one a prompt shows.
struct KeyBinding {
  std::uint16_t bit;
  SDL_Scancode key;
  SDL_Scancode alternate;
};
inline constexpr std::array kKeyboardBindings{
    KeyBinding{kButtonUp, SDL_SCANCODE_UP, SDL_SCANCODE_W},
    KeyBinding{kButtonRight, SDL_SCANCODE_RIGHT, SDL_SCANCODE_D},
    KeyBinding{kButtonDown, SDL_SCANCODE_DOWN, SDL_SCANCODE_S},
    KeyBinding{kButtonLeft, SDL_SCANCODE_LEFT, SDL_SCANCODE_A},
    KeyBinding{kButtonStart, SDL_SCANCODE_RETURN, SDL_SCANCODE_UNKNOWN},
    KeyBinding{kButtonSelect, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_TAB},
    KeyBinding{kButtonCross, SDL_SCANCODE_K, SDL_SCANCODE_UNKNOWN},
    KeyBinding{kButtonCircle, SDL_SCANCODE_L, SDL_SCANCODE_UNKNOWN},
    KeyBinding{kButtonTriangle, SDL_SCANCODE_I, SDL_SCANCODE_UNKNOWN},
    KeyBinding{kButtonSquare, SDL_SCANCODE_J, SDL_SCANCODE_UNKNOWN},
    KeyBinding{kButtonL1, SDL_SCANCODE_Q, SDL_SCANCODE_UNKNOWN},
    KeyBinding{kButtonR1, SDL_SCANCODE_E, SDL_SCANCODE_UNKNOWN},
    KeyBinding{kButtonL2, SDL_SCANCODE_1, SDL_SCANCODE_UNKNOWN},
    KeyBinding{kButtonR2, SDL_SCANCODE_3, SDL_SCANCODE_UNKNOWN},
};

// A PSX button on an SDL gamepad: a button, or for L2/R2 a trigger axis. SDL's face buttons are positional,
// so Cross is the south button whatever the pad prints on it.
struct GamepadBinding {
  std::uint16_t bit;
  SDL_GamepadButton button;
  SDL_GamepadAxis trigger;
};
inline constexpr std::array kGamepadBindings{
    GamepadBinding{kButtonUp, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonStart, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonSelect, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonCross, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonCircle, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonSquare, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonTriangle, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonL3, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonR3, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_AXIS_INVALID},
    GamepadBinding{kButtonL2, SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
    GamepadBinding{kButtonR2, SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
};

inline std::optional<GamepadBinding> gamepadBinding(std::uint16_t bit) {
  for (const GamepadBinding &binding : kGamepadBindings) {
    if (binding.bit == bit) {
      return binding;
    }
  }
  return std::nullopt;
}

// The PSX button an SDL gamepad button presses, or 0.
inline std::uint16_t psxButtonOf(SDL_GamepadButton button) {
  for (const GamepadBinding &binding : kGamepadBindings) {
    if (binding.button == button) {
      return binding.bit;
    }
  }
  return 0;
}

// The PSX button a key presses, or 0.
inline std::uint16_t psxButtonOfKey(SDL_Scancode key) {
  for (const KeyBinding &binding : kKeyboardBindings) {
    if (binding.key == key || binding.alternate == key) {
      return binding.bit;
    }
  }
  return 0;
}

} // namespace psx::input
