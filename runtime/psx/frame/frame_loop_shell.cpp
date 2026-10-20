#include "frame_loop_shell.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"

#include <cstdlib>
#include <lucent/log.h>

namespace psx::frame {

psx::game::FrameDriver &FrameLoopShell::requireDriver(Game &game) const {
  if (!game.frameDriver) {
    lucent::error("frame-loop",
                  "GameRuntime created no FrameDriver; refusing before startProgram can dispatch a "
                  "non-returning guest frame loop");
    std::abort();
  }
  return *game.frameDriver;
}

psx::game::FrameDriver &FrameLoopShell::prepareProduct(Game &game) const {
  psx::game::FrameDriver &driver = requireDriver(game);
  game.platform_hle.requireNativeFrameLoopContract();
  game.productFrameLoopPrepared_ = true;
  return driver;
}

void FrameLoopShell::bindRunningCore(Core &core) {
  core.gte().bind();
  core.game->spu.bind(&core);
  core.game->mdec.bind();
  core.game->xa.bindSamplePull();
  bound_ = &core;
}

void FrameLoopShell::forgetCore(const Core &core) {
  if (bound_ == &core) {
    bound_ = nullptr;
  }
}

void FrameLoopShell::step(Core &core, uint32_t frame) {
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
  // Rebinding re-points state the title's per-frame GTE work reads; do it only on a session switch.
  if (bound_ != &core) {
    bindRunningCore(core);
  }
  requireDriver(game).stepFrame(core, frame);
  // THE FRAME'S CAPTURES, HERE, because this is where the frame's presentation ends. A frame can have
  // more than one presenter — the main one, and DisplayPresenter::buildHostPicture when a native movie is playing —
  // and in the movie case the second runs last and is what the window shows. Capturing at the tail of
  // whichever presenter happened to be main therefore read the movie's frame as the empty guest-VRAM
  // composite: black, on every frame of a movie (issue 0040). Both presenters now only present; the
  // capture is taken once, here, and reads whatever the turn ended showing.
  core.game->display.captureFrame();
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

} // namespace psx::frame
