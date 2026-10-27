// input_injection.h — psx::input::InputInjection: host input from the control channel, pushed as SDL events
// into the one event queue, so a hidden run drives the player's own input path.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Joystick; // opaque; only held as a pointer

namespace psx::input {

// The SDL window an injected event names.
enum class WindowId : std::uint32_t {};

// Window coordinates in display units, or a wheel's scroll along the same axes.
struct WindowXY {
  float x = 0.0f;
  float y = 0.0f;
};

// What one key, button or finger does: press and hold, let go, or press for some input turns and let go.
enum class InjectedPress : std::uint8_t { Down, Up, Tap };

// Every call runs on the thread that pumps the queue and answers one reply line; a refusal names the words
// it accepts. Coordinates are window coordinates in display units, as a real mouse or finger reports them.
class InputInjection {
public:
  InputInjection() = default;
  InputInjection(const InputInjection &) = delete;
  InputInjection &operator=(const InputInjection &) = delete;
  ~InputInjection();

  // `name` is an SDL scancode name (Escape, B, Return, Up) or a scancode number; a modifier key held down
  // sets SDL's modifier state, as a real keyboard does, so a chord is the modifier down, the key, the modifier up.
  std::string key(WindowId windowId, std::string_view name, InjectedPress press, int holdTurns);
  std::string text(WindowId windowId, std::string_view utf8);
  std::string mouseMove(WindowId windowId, WindowXY at);
  // `name` is left, middle, right, x1 or x2.
  std::string mouseButton(WindowId windowId, std::string_view name, InjectedPress press, int holdTurns);
  std::string mouseWheel(WindowId windowId, WindowXY scroll);
  // `name` is an SDL gamepad button name (a, b, start, dpup); the virtual pad attaches on first use.
  std::string gamepadButton(std::string_view name, InjectedPress press, int holdTurns);
  // `name` is an SDL gamepad axis name (leftx, lefttrigger); `value` is the raw axis value.
  std::string gamepadAxis(std::string_view name, int value);
  // Down and tap need the finger up, up needs it down.
  std::string touch(WindowId windowId, int finger, InjectedPress press, WindowXY at, int holdTurns);
  std::string touchMove(WindowId windowId, int finger, WindowXY at);

  // Once per input turn of the Game holding player input, after its drain, paused turns included:
  // lets go of every tap whose hold ran out.
  void afterInputTurn();
  // After a drain: the queue no longer holds a text event that points into this owner.
  void afterDrain();

private:
  enum class Device : std::uint8_t { Key, MouseButton, GamepadButton, Finger };
  struct PendingRelease {
    Device device;
    int code;
    WindowId windowId;
    int turnsLeft;
  };

  void pushKey(WindowId windowId, int scancode, bool down);
  void pushMouseButton(WindowId windowId, int button, bool down);
  bool setGamepadButton(int button, bool down);
  void pushFinger(WindowId windowId, int finger, bool down, WindowXY normalised);
  // Window coordinates as the 0..1 a touch event carries.
  static std::optional<WindowXY> normalise(WindowId windowId, WindowXY at);
  bool ensureVirtualPad();
  void schedule(Device device, int code, WindowId windowId, int holdTurns);

  std::vector<PendingRelease> releases_;
  // An SDL text event points at its text; each string lives here until the next drain has taken it.
  std::deque<std::string> texts_;
  WindowXY mouse_;
  std::uint32_t mouseButtons_ = 0;
  static constexpr int kMaxFingers = 10;
  bool fingerDown_[kMaxFingers] = {};
  WindowXY fingerAt_[kMaxFingers] = {};
  SDL_Joystick *virtualPad_ = nullptr;
  std::uint32_t virtualPadId_ = 0;
};

} // namespace psx::input
