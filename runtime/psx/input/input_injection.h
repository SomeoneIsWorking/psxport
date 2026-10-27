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

// What one key, button or finger does: press and hold, let go, or press for a while and let go.
enum class InjectedPress : std::uint8_t { Down, Up, Tap };

// What a tap's hold counts: input turns of the Game holding player input (paused turns included), or the pad
// frames the guest samples, so a guest-pad tap spans the same guest frames however the run is paced or stepped.
enum class TapClock : std::uint8_t { InputTurns, PadFrames };

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
  // The same by SDL_GamepadButton value, a tap held for `hold` ticks of `clock`.
  std::string gamepadButton(int button, InjectedPress press, int hold, TapClock clock);
  // An SDL_GamepadAxis trigger pulled fully or let go.
  std::string gamepadTrigger(int axis, InjectedPress press, int hold, TapClock clock);
  // `name` is an SDL gamepad axis name (leftx, lefttrigger); `value` is the raw axis value.
  std::string gamepadAxis(std::string_view name, int value);
  // Down and tap need the finger up, up needs it down.
  std::string touch(WindowId windowId, int finger, InjectedPress press, WindowXY at, int holdTurns);
  std::string touchMove(WindowId windowId, int finger, WindowXY at);

  // Once per input turn of the Game holding player input, after its drain, paused turns included:
  // lets go of every input-turn tap whose hold ran out.
  void afterInputTurn();
  // Once per pad frame, after the guest's mask is resolved: lets go of every pad-frame tap whose hold ran out.
  void afterPadFrame();
  // After a drain: the queue no longer holds a text event that points into this owner.
  void afterDrain();
  // A gamepad was opened: the virtual pad's axes set before then did not reach it, so they are set again.
  void afterGamepadOpened(std::uint32_t id);

private:
  enum class Device : std::uint8_t { Key, MouseButton, GamepadButton, GamepadTrigger, Finger };
  struct PendingRelease {
    Device device;
    int code;
    WindowId windowId;
    TapClock clock;
    int ticksLeft;
  };

  void pushKey(WindowId windowId, int scancode, bool down);
  void pushMouseButton(WindowId windowId, int button, bool down);
  bool setGamepadButton(int button, bool down);
  bool setGamepadTrigger(int axis, bool pulled);
  bool setGamepadAxis(int axis, std::int16_t value);
  void pushFinger(WindowId windowId, int finger, bool down, WindowXY normalised);
  // Window coordinates as the 0..1 a touch event carries.
  static std::optional<WindowXY> normalise(WindowId windowId, WindowXY at);
  bool ensureVirtualPad();
  void schedule(Device device, int code, WindowId windowId, int hold, TapClock clock);
  void releaseExpired(TapClock clock);

  std::vector<PendingRelease> releases_;
  // An SDL text event points at its text; each string lives here until the next drain has taken it.
  std::deque<std::string> texts_;
  WindowXY mouse_;
  std::uint32_t mouseButtons_ = 0;
  static constexpr int kMaxFingers = 10;
  bool fingerDown_[kMaxFingers] = {};
  WindowXY fingerAt_[kMaxFingers] = {};
  SDL_Joystick *virtualPad_ = nullptr;
  static constexpr int kAxes = 6;
  std::int16_t axes_[kAxes] = {};
  std::uint32_t virtualPadId_ = 0;
};

} // namespace psx::input
