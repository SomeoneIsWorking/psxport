#include "frame_loop_shell.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"

#include <cstdlib>
#include <lucent/log.h>

FrameDriver &FrameLoopShell::requireDriver(Game &game) const {
  if (!game.runtime) {
    lucent::error("frame-loop", "Game has no installed GameRuntime; refusing to infer a product frame loop");
    std::abort();
  }
  if (!game.frameDriver) {
    lucent::error("frame-loop",
                  "GameRuntime created no FrameDriver; refusing before bootInit can dispatch a "
                  "non-returning guest frame loop");
    std::abort();
  }
  return *game.frameDriver;
}

FrameDriver &FrameLoopShell::prepareProduct(Game &game) const {
  FrameDriver &driver = requireDriver(game);
  game.platform_hle.initBuiltins();
  game.platform_hle.requireNativeFrameLoopContract();
  game.productFrameLoopPrepared_ = true;
  return driver;
}

void FrameLoopShell::step(Core &core, uint32_t frame) const {
  if (!core.game) {
    lucent::error("frame-loop", "frame step has no bound Game");
    std::abort();
  }
  Game &game = *core.game;
  if (!game.productFrameLoopPrepared_) {
    lucent::error("frame-loop",
                  "product frame step ran before FrameLoopShell::prepareProduct; title overrides "
                  "may have displaced the mandatory VSync trap");
    std::abort();
  }
  const uint64_t fenceBefore = game.presentation.fence();
  requireDriver(game).stepFrame(core, frame);
  const uint64_t fenceAfter = game.presentation.fence();
  if (fenceAfter != fenceBefore + 1u) {
    lucent::error("frame-loop",
                  "FrameDriver violated the native frame contract at frame {}: presentation fence "
                  "advanced {} time(s), expected exactly 1",
                  frame,
                  fenceAfter - fenceBefore);
    std::abort();
  }
}
