// Native VBlank / VSync source (S3). No CD/GPU IRQ is emulated, so the libetc VSync count
// DAT_800abde0 — normally bumped by the BIOS VBlank IRQ — never advances and the game's
// VSync(0) wrapper FUN_80085900 spins in FUN_80085a78 -> "VSync: timeout". We override the
// VSync core itself so each VSync(0)/VSync(n) advances a native frame clock and returns it;
// VSync(-1) queries it. This is the standard static-recomp frame model: one logic frame per
// VSync(0). Each tick also DeliverEvents the VBlank event class for any TestEvent-based waiter.
#include "core.h"
#include "game.h"
#include <stdlib.h>
#include <stdio.h>

enum { A0 = 4, V0 = 2 };
#define VBLANK_COUNT 0x800ABDE0u   // DAT_800abde0: libetc VSync counter (FUN_80085900 returns it)

void hle_deliver_event(Core* c, uint32_t ev_class, uint32_t spec);

// vblank counter now lives on the instance: c->game->timing.vblank (was the file-scope g_vblank)

// 0x80085BB0 FUN_80085bb0 VSyncCallback(func): no-op. The original routes the per-vblank
// callback through the libapi interrupt vector we don't model; we don't deliver preemptive
// VBlank IRQs at all — the game's vblank busy-waits are ported to PC behavior natively
// (see games_tomba2.c), so registering the callback is unnecessary and its unmodeled-vector
// deref is skipped.
void ov_vsync_callback(Core* c) { c->r[V0] = 0; }

static void frame_tick(Core* c) {
  // Deliver the VBlank event to whichever class the game opened it under (RCnt3 vblank, or
  // the libapi vblank class); broad spec so any opened+enabled vblank EvCB matches.
  hle_deliver_event(c, 0xF2000003u, 0xFFFFFFFFu);
  hle_deliver_event(c, 0xF0000001u, 0xFFFFFFFFu);
}

// 0x80085900 FUN_80085900 = libetc VSync(mode):
//   mode < 0  -> return current vblank count (query, no wait)
//   mode == 1 -> return hblank delta (query, no wait) — dummy 0 here
//   mode == 0 -> wait one vblank; mode > 1 -> wait `mode` vblanks. Advance the frame clock.
void ov_vsync(Core* c) {
  uint32_t& vblank = c->game->timing.vblank;
  int32_t mode = (int32_t)c->r[A0];
  if (mode < 0) {
    c->r[V0] = vblank;
  } else if (mode == 1) {
    c->r[V0] = 0;
  } else {
    vblank += (mode == 0) ? 1u : (uint32_t)mode;
    c->r[V0] = vblank;
    frame_tick(c);
  }
  c->mem_w32(VBLANK_COUNT, vblank);
}

uint32_t timing_vblank(Core* c) { return c->game->timing.vblank; }

void timing_init(void) {
}
