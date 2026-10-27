// input_injection.cpp — psx::input::InputInjection: control-channel input as SDL events and an SDL virtual gamepad.
#include "input_injection.h"
#include "number_text.h"

#include <SDL3/SDL.h>
#include <lucent/log.h>

#include <string>

namespace psx::input {
namespace {

// Touch events need a device id; SDL reserves 0 and the mouse-emulation ids.
inline constexpr SDL_TouchID kInjectedTouch = 1;

std::string pressText(InjectedPress press, int holdFrames) {
  return press == InjectedPress::Down ? std::string("down")
         : press == InjectedPress::Up ? std::string("up")
                                      : lucent::format("tap for {} pad frame(s)", holdFrames);
}

std::string scancodeNames() {
  std::string names;
  for (int code = SDL_SCANCODE_A; code < SDL_SCANCODE_COUNT; ++code) {
    const char *name = SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
    if (name != nullptr && *name != '\0') {
      names += names.empty() ? "" : ", ";
      names += name;
    }
  }
  return names;
}

std::string gamepadButtonNames() {
  std::string names;
  for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
    if (const char *name = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(button))) {
      names += names.empty() ? "" : ", ";
      names += name;
    }
  }
  return names;
}

std::string gamepadAxisNames() {
  std::string names;
  for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
    if (const char *name = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(axis))) {
      names += names.empty() ? "" : ", ";
      names += name;
    }
  }
  return names;
}

int mouseButtonOf(std::string_view name) {
  return name == "left"     ? SDL_BUTTON_LEFT
         : name == "middle" ? SDL_BUTTON_MIDDLE
         : name == "right"  ? SDL_BUTTON_RIGHT
         : name == "x1"     ? SDL_BUTTON_X1
         : name == "x2"     ? SDL_BUTTON_X2
                            : 0;
}

SDL_WindowID sdlWindow(WindowId windowId) {
  return static_cast<SDL_WindowID>(windowId);
}

bool push(SDL_Event &event) {
  if (!SDL_PushEvent(&event)) {
    lucent::warn("input", "injected event 0x{:x} refused: {}", static_cast<unsigned>(event.type), SDL_GetError());
    return false;
  }
  return true;
}

} // namespace

InputInjection::~InputInjection() {
  if (virtualPad_ != nullptr) {
    SDL_CloseJoystick(virtualPad_);
    SDL_DetachVirtualJoystick(virtualPadId_);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
}

std::string InputInjection::key(WindowId windowId, std::string_view name, InjectedPress press, int holdFrames) {
  const std::optional<std::uint32_t> number = config::parseUnsigned(name, 10);
  const int scancode = number ? static_cast<int>(*number) : SDL_GetScancodeFromName(std::string(name).c_str());
  if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
    return lucent::format("key: \"{}\" is not a key; names: {}", name, scancodeNames());
  }
  pushKey(windowId, scancode, press != InjectedPress::Up);
  if (press == InjectedPress::Tap) {
    schedule(Device::Key, scancode, windowId, holdFrames);
  }
  return lucent::format(
      "key {} {}", SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)), pressText(press, holdFrames));
}

std::string InputInjection::text(WindowId windowId, std::string_view utf8) {
  if (utf8.empty()) {
    return "text: nothing to type";
  }
  texts_.emplace_back(utf8);
  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  event.text.windowID = sdlWindow(windowId);
  event.text.text = texts_.back().c_str();
  push(event);
  return lucent::format("text \"{}\"", utf8);
}

std::string InputInjection::mouseMove(WindowId windowId, WindowXY at) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.windowID = sdlWindow(windowId);
  event.motion.state = mouseButtons_;
  event.motion.x = at.x;
  event.motion.y = at.y;
  event.motion.xrel = at.x - mouse_.x;
  event.motion.yrel = at.y - mouse_.y;
  mouse_ = at;
  push(event);
  return lucent::format("mouse at {},{}", at.x, at.y);
}

std::string InputInjection::mouseButton(WindowId windowId, std::string_view name, InjectedPress press, int holdFrames) {
  const int button = mouseButtonOf(name);
  if (button == 0) {
    return lucent::format("mouse: \"{}\" is not a button; buttons: left, middle, right, x1, x2", name);
  }
  pushMouseButton(windowId, button, press != InjectedPress::Up);
  if (press == InjectedPress::Tap) {
    schedule(Device::MouseButton, button, windowId, holdFrames);
  }
  return lucent::format("mouse {} {} at {},{}", name, pressText(press, holdFrames), mouse_.x, mouse_.y);
}

std::string InputInjection::mouseWheel(WindowId windowId, WindowXY scroll) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.windowID = sdlWindow(windowId);
  event.wheel.x = scroll.x;
  event.wheel.y = scroll.y;
  event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  event.wheel.mouse_x = mouse_.x;
  event.wheel.mouse_y = mouse_.y;
  event.wheel.integer_x = static_cast<Sint32>(scroll.x);
  event.wheel.integer_y = static_cast<Sint32>(scroll.y);
  push(event);
  return lucent::format("mouse wheel {},{} at {},{}", scroll.x, scroll.y, mouse_.x, mouse_.y);
}

std::string InputInjection::gamepadButton(std::string_view name, InjectedPress press, int holdFrames) {
  const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(std::string(name).c_str());
  if (button == SDL_GAMEPAD_BUTTON_INVALID) {
    return lucent::format(
        "gamepad: \"{}\" is not a button; buttons: {}; or `gamepad axis <name> <value>`", name, gamepadButtonNames());
  }
  if (!setGamepadButton(button, press != InjectedPress::Up)) {
    return lucent::format("gamepad: no virtual pad: {}", SDL_GetError());
  }
  if (press == InjectedPress::Tap) {
    schedule(Device::GamepadButton, button, WindowId{}, holdFrames);
  }
  return lucent::format("gamepad {} {}", SDL_GetGamepadStringForButton(button), pressText(press, holdFrames));
}

std::string InputInjection::gamepadAxis(std::string_view name, int value) {
  const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(std::string(name).c_str());
  if (axis == SDL_GAMEPAD_AXIS_INVALID) {
    return lucent::format("gamepad axis: \"{}\" is not an axis; axes: {}", name, gamepadAxisNames());
  }
  if (value < SDL_JOYSTICK_AXIS_MIN || value > SDL_JOYSTICK_AXIS_MAX) {
    return lucent::format("gamepad axis: {} is outside {}..{}", value, SDL_JOYSTICK_AXIS_MIN, SDL_JOYSTICK_AXIS_MAX);
  }
  if (!ensureVirtualPad() || !SDL_SetJoystickVirtualAxis(virtualPad_, axis, static_cast<Sint16>(value))) {
    return lucent::format("gamepad: no virtual pad: {}", SDL_GetError());
  }
  return lucent::format("gamepad axis {} = {}", SDL_GetGamepadStringForAxis(axis), value);
}

std::string InputInjection::touch(WindowId windowId, int finger, InjectedPress press, WindowXY at, int holdFrames) {
  if (finger < 0 || finger >= kMaxFingers) {
    return lucent::format("touch: finger {} is outside 0..{}", finger, kMaxFingers - 1);
  }
  const std::optional<WindowXY> normalised = normalise(windowId, at);
  if (!normalised) {
    return lucent::format("touch: the window has no size: {}", SDL_GetError());
  }
  pushFinger(windowId, finger, press != InjectedPress::Up, *normalised);
  if (press == InjectedPress::Tap) {
    schedule(Device::Finger, finger, windowId, holdFrames);
  }
  return lucent::format("touch {} {} at {},{}", finger, pressText(press, holdFrames), at.x, at.y);
}

void InputInjection::afterPadFrame() {
  for (auto it = releases_.begin(); it != releases_.end();) {
    if (--it->framesLeft > 0) {
      ++it;
      continue;
    }
    switch (it->device) {
    case Device::Key:
      pushKey(it->windowId, it->code, false);
      break;
    case Device::MouseButton:
      pushMouseButton(it->windowId, it->code, false);
      break;
    case Device::GamepadButton:
      setGamepadButton(it->code, false);
      break;
    case Device::Finger:
      pushFinger(it->windowId, it->code, false, fingerAt_[it->code]);
      break;
    }
    it = releases_.erase(it);
  }
}

void InputInjection::afterDrain() {
  texts_.clear();
}

void InputInjection::pushKey(WindowId windowId, int scancode, bool down) {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.windowID = sdlWindow(windowId);
  event.key.scancode = static_cast<SDL_Scancode>(scancode);
  event.key.key = SDL_GetKeyFromScancode(event.key.scancode, SDL_KMOD_NONE, true);
  event.key.mod = SDL_KMOD_NONE;
  event.key.down = down;
  push(event);
}

void InputInjection::pushMouseButton(WindowId windowId, int button, bool down) {
  const std::uint32_t bit = SDL_BUTTON_MASK(button);
  mouseButtons_ = down ? (mouseButtons_ | bit) : (mouseButtons_ & ~bit);
  SDL_Event event{};
  event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  event.button.windowID = sdlWindow(windowId);
  event.button.button = static_cast<Uint8>(button);
  event.button.down = down;
  event.button.clicks = 1;
  event.button.x = mouse_.x;
  event.button.y = mouse_.y;
  push(event);
}

bool InputInjection::setGamepadButton(int button, bool down) {
  return ensureVirtualPad() && SDL_SetJoystickVirtualButton(virtualPad_, button, down);
}

void InputInjection::pushFinger(WindowId windowId, int finger, bool down, WindowXY normalised) {
  const bool moved = down && fingerDown_[finger];
  SDL_Event event{};
  event.type = !down ? SDL_EVENT_FINGER_UP : moved ? SDL_EVENT_FINGER_MOTION : SDL_EVENT_FINGER_DOWN;
  event.tfinger.touchID = kInjectedTouch;
  event.tfinger.fingerID = static_cast<SDL_FingerID>(finger) + 1;
  event.tfinger.x = normalised.x;
  event.tfinger.y = normalised.y;
  event.tfinger.dx = moved ? normalised.x - fingerAt_[finger].x : 0.0f;
  event.tfinger.dy = moved ? normalised.y - fingerAt_[finger].y : 0.0f;
  event.tfinger.pressure = down ? 1.0f : 0.0f;
  event.tfinger.windowID = sdlWindow(windowId);
  fingerDown_[finger] = down;
  fingerAt_[finger] = normalised;
  push(event);
}

std::optional<WindowXY> InputInjection::normalise(WindowId windowId, WindowXY at) {
  int width = 0;
  int height = 0;
  SDL_Window *window = SDL_GetWindowFromID(sdlWindow(windowId));
  if (window == nullptr || !SDL_GetWindowSize(window, &width, &height) || width <= 0 || height <= 0) {
    return std::nullopt;
  }
  return WindowXY{.x = at.x / static_cast<float>(width), .y = at.y / static_cast<float>(height)};
}

// A gamepad-typed virtual joystick, so SDL maps it as a gamepad and HostInput opens it like a plugged-in pad.
bool InputInjection::ensureVirtualPad() {
  if (virtualPad_ != nullptr) {
    return true;
  }
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    return false;
  }
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
  desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
  desc.name = "psxport injected gamepad";
  const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  SDL_Joystick *pad = id != 0 ? SDL_OpenJoystick(id) : nullptr;
  if (pad == nullptr) {
    if (id != 0) {
      SDL_DetachVirtualJoystick(id);
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return false;
  }
  virtualPad_ = pad;
  virtualPadId_ = id;
  lucent::info("input", "virtual gamepad {} attached for injected input", id);
  return true;
}

void InputInjection::schedule(Device device, int code, WindowId windowId, int holdFrames) {
  releases_.push_back(
      {.device = device, .code = code, .windowId = windowId, .framesLeft = holdFrames > 0 ? holdFrames : 1});
}

} // namespace psx::input
