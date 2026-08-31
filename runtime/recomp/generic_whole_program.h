// Generic whole-program execution for a bare static-recompiler port.
#pragma once

#include "coro.h"
#include "game_runtime.h"

#include <cstdint>

class Core;
class Game;

// Framework-owned FrameDriver implementation. It is not a title driver: its only policy is to
// preserve generated control flow across VSync, commit one presented frame, and resume that same
// generated stack on the next host turn.
class GenericWholeProgramFrameDriver final : public FrameDriver {
public:
  GenericWholeProgramFrameDriver(Game &game, GenericWholeProgramProfile profile);
  ~GenericWholeProgramFrameDriver() override = default;

  void stepFrame(Core &core, uint32_t frame) override;
  void yieldAtVSync(Core &core);

private:
  Game *game_;
  uint32_t entryAddress_ = 0;
  Coro guest_;
  bool started_ = false;
  bool waitingAtVSync_ = false;
};

// Installed by PlatformHle only for GenericWholeProgramProfile consumers. It is deliberately a
// framework function: a bare port supplies no custom VSync handler.
void generic_whole_program_vsync(Core *core);
bool is_generic_whole_program_driver(const FrameDriver &driver);
