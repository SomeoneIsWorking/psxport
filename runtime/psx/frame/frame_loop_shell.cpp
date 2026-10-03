#include "frame_loop_shell.h"

#include "hw_bind.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "gpu_native_internal.h"

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
  // NAME THE SESSION THAT IS ABOUT TO RUN — but only when it CHANGED. The Beetle SPU and the XA
  // streamer reach their state through a process-global binding (their vendored entry points cannot
  // carry an instance), and that binding must name the Core that is RUNNING, not the one that booted
  // last: a host that runs several sessions at once — a title picker whose panels are live sessions —
  // otherwise delivers one guest's SPU interrupt line and CD-audio pull into another guest, and into
  // freed memory once a session is destroyed. It was done on EVERY step, which is the same answer
  // asked a thousand times a second: `bind` re-points global state a title's own per-frame GTE work
  // reads (the terrain producer projects through it), and re-pointing it under a running frame cost
  // Spyro 2's boot prefix two orders of magnitude in time. A single-session run binds once, at boot,
  // exactly as it always did; a picker binds on the switch, which is the only moment the answer can
  // differ.
  static Core *bound = nullptr;
  if (bound != &core) {
    gte_bind(&core);
    core.rsub.projprim.bind(&core);
    spu_bind(&core);
    mdec_bind(&core);
    xa_bind(&core);
    bound = &core;
  }
  requireDriver(game).stepFrame(core, frame);
  // THE FRAME'S CAPTURES, HERE, because this is where the frame's presentation ends. A frame can have
  // more than one presenter — the main one, and gpu_vk_present_image when a native movie is playing —
  // and in the movie case the second runs last and is what the window shows. Capturing at the tail of
  // whichever presenter happened to be main therefore read the movie's frame as the empty guest-VRAM
  // composite: black, on every frame of a movie (issue 0040). Both presenters now only present; the
  // capture is taken once, here, and reads whatever the turn ended showing.
  gpu_present_frame_capture(&core);
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
