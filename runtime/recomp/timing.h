// timing.h — class Timing — native VBlank/VSync frame clock subsystem, owned by Game
// (`c->game->timing`, back-pointer wired in Game()). Owns the libetc VSync counter mirror +
// the vsync/frame-tick behavior (timing.cpp).
#pragma once
#include <cstdint>
class Game;

class Timing {
public:
  Game* game = nullptr;
  uint32_t vblank = 0;      // libetc VSync counter mirror (was g_vblank)
  uint32_t logicFrame = 0;  // logic-frame counter, advanced by native_step_frame each iteration.
                            // Read by Cd::audioTrace / [bgmreq]-style diags. Was global g_bgm_frame.

  // vsyncCallback(): 0x80085BB0 FUN_80085bb0 VSyncCallback(func) — no-op. Native frame loop
  //   owns pacing; the libapi per-vblank IRQ vector isn't modeled. Was ov_vsync_callback.
  void vsyncCallback();

  // vsync(): 0x80085900 FUN_80085900 = libetc VSync(mode). Currently unreachable — sync_overrides
  //   traps VSync (all pacing is PC-native). Kept for RE reference / future re-enable.
  void vsync();

  // frameTick(): advance the canonical libetc VSync counter once per native frame. Called from
  //   the PC-native frame loop (native_step_frame) so recomp code reading DAT_800abde0 for
  //   pacing/idle-timers keeps advancing.
  void frameTick();

  // vsyncCallbackDispatch(): 0x80086288 FUN_80086288 — the BIOS intr.c VSyncCallback CHAIN
  //   invoker (real retail BIOS VSyncCallback table has exactly 8 slots at 0x800AFDC0, plus an
  //   incrementing IRQ-tick counter at 0x800AFDE0). Body per gen_func_80086288
  //   (generated/shard_4.c:13351): bump the counter, then walk the 8-entry fn-ptr table and
  //   rec_dispatch() any non-null slot. WIDE-RE DRAFT, UNWIRED: nothing in the port currently
  //   installs a callback into that table (vsyncCallback() above is a no-op — see its comment —
  //   so the real per-vblank IRQ vector this dispatcher hangs off is never modeled), and no
  //   static caller was found in generated/ (only ever reached indirectly through the IRQ
  //   vector, like 0x800909C0/0x80090BD0 — see game/audio/sequencer.h). Kept for RE reference /
  //   future re-enable if a callback slot is ever installed faithfully.
  void vsyncCallbackDispatch();
};
