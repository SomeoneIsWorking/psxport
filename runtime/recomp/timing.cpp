// Native VBlank / VSync source (S3). No CD/GPU IRQ is emulated, so the libetc VSync count
// DAT_800abde0 — normally bumped by the BIOS VBlank IRQ — never advances and the game's
// VSync(0) wrapper FUN_80085900 spins in FUN_80085a78 -> "VSync: timeout". Class Timing
// (game.h) owns the frame clock: each VSync(0)/VSync(n) advances a native frame clock and
// returns it; VSync(-1) queries it. This is the standard static-recomp frame model: one logic
// frame per VSync(0). Each tick also DeliverEvents the VBlank event class for any TestEvent-
// based waiter. Reached via c->game->timing.method().
#include "core.h"
#include "game.h"
#include <stdlib.h>
#include <stdio.h>

enum { A0 = 4, V0 = 2 };
#define VBLANK_COUNT 0x800ABDE0u   // DAT_800abde0: libetc VSync counter (FUN_80085900 returns it)

void hle_deliver_event(Core* c, uint32_t ev_class, uint32_t spec);

// 0x80085BB0 FUN_80085bb0 VSyncCallback(func): no-op. The original routes the per-vblank
// callback through the libapi interrupt vector we don't model; we don't deliver preemptive
// VBlank IRQs at all — the game's vblank busy-waits are ported to PC behavior natively
// (see games_tomba2.c), so registering the callback is unnecessary and its unmodeled-vector
// deref is skipped. Was ov_vsync_callback (taxi-in via c->r[4]; the callback ptr arg is
// unused here, so no arg on the method).
void Timing::vsyncCallback() {
  game->core.r[V0] = 0;
}

// Deliver the VBlank event to whichever class the game opened it under (RCnt3 vblank, or the
// libapi vblank class); broad spec so any opened+enabled vblank EvCB matches.
static void deliver_vblank_events(Core* c) {
  hle_deliver_event(c, 0xF2000003u, 0xFFFFFFFFu);
  hle_deliver_event(c, 0xF0000001u, 0xFFFFFFFFu);
}

// 0x80085900 FUN_80085900 = libetc VSync(mode) reached via c->r[A0]:
//   mode < 0  -> return current vblank count (query, no wait)
//   mode == 1 -> return hblank delta (query, no wait) — dummy 0 here
//   mode == 0 -> wait one vblank; mode > 1 -> wait `mode` vblanks. Advance the frame clock.
// Currently unreachable — sync_overrides traps VSync (all pacing is PC-native). Kept for RE.
void Timing::vsync() {
  Core* c = &game->core;
  int32_t mode = (int32_t)c->r[A0];
  if (mode < 0) {
    c->r[V0] = vblank;
  } else if (mode == 1) {
    c->r[V0] = 0;
  } else {
    vblank += (mode == 0) ? 1u : (uint32_t)mode;
    c->r[V0] = vblank;
    deliver_vblank_events(c);
  }
  c->mem_w32(VBLANK_COUNT, vblank);
}

// Advance the canonical libetc VSync counter once per native frame. The PC-native frame loop owns
// timing (one logic frame == one vblank), and VSync(0) is trapped (sync_overrides) so Timing::vsync
// never runs — meaning DAT_800abde0 would otherwise stay 0 forever. Native code reimplements its
// own paced logic and ignores this counter, but RECOMP code (full-PSX core in SBS, and any still-
// recomp leaf) reads DAT_800abde0 to pace animations/idle timers; if it never ticks, those tasks
// freeze in place (SBS core-B title-menu freeze). Bump it every frame for ALL cores so the recomp
// timebase advances.
void Timing::frameTick() {
  vblank += 1u;
  game->core.mem_w32(VBLANK_COUNT, vblank);
}
