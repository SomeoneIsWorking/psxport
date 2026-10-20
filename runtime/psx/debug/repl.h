// repl.h — the stdin REPL (PSXPORT_REPL=1): run control here, every other verb from DebugCommands.
#pragma once
#include <cstdint>
#include <span>
struct Core;

class Repl {
public:
  // Requests armed by `newgame` / `skip N`, consumed and cleared by the title's FrameDriver.
  int navNewgame = 0;
  long skipFrames = 0;

  // One-shot: return to the prompt after the current frame.
  void requestPrompt() {
    promptRequested_ = true;
  }
  bool consumePromptRequest() {
    const bool requested = promptRequested_;
    promptRequested_ = false;
    return requested;
  }

  // Fills one bounded command; false on EOF.
  using LineReader = bool (*)(std::span<char> line);

  // Run commands until `run N` (returns N), quit/EOF (-1) or end (-2).
  long read(Core *c, uint32_t f, LineReader readLine = readStdinLine);

  // Before each product step: block on commands until a `run N` budget covers it. False when the run ends.
  bool beforeStep(Core *c, uint32_t step, LineReader readLine = readStdinLine);
  void afterStep() {
    if (consumePromptRequest()) {
      budget_ = 0;
    }
  }

private:
  static bool readStdinLine(std::span<char> line);

  bool promptRequested_ = false;
  long budget_ = 0; // steps left in the current `run N`
};
