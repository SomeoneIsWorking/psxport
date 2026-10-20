#include "frame_loop_shell.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"

#include <cstdlib>
#include <lucent/log.h>

namespace psx::frame {

psx::game::FrameDriver &FrameLoopShell::prepareProduct(Game &game) const {
  psx::game::FrameDriver &driver = game.requireFrameDriver();
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
  game.requireFrameDriver().stepFrame(core, frame);
  // Once per step, after every presenter of the step, so a movie frame is what it reads (issue 0040).
  game.display.captureFrame();
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
