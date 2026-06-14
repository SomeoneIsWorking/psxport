// Native VBlank / VSync source (S3). No CD/GPU IRQ is emulated, so the libetc VSync count
// DAT_800abde0 — normally bumped by the BIOS VBlank IRQ — never advances and the game's
// VSync(0) wrapper FUN_80085900 spins in FUN_80085a78 -> "VSync: timeout". We override the
// VSync core itself so each VSync(0)/VSync(n) advances a native frame clock and returns it;
// VSync(-1) queries it. This is the standard static-recomp frame model: one logic frame per
// VSync(0). Each tick also DeliverEvents the VBlank event class for any TestEvent-based waiter.
#include "r3000.h"
#include <stdlib.h>

enum { A0 = 4, V0 = 2 };
#define VBLANK_COUNT 0x800ABDE0u   // DAT_800abde0: libetc VSync counter (FUN_80085900 returns it)

void hle_deliver_event(uint32_t ev_class, uint32_t spec);

static uint32_t g_vblank = 0;

static void frame_tick(void) {
  // Deliver the VBlank event to whichever class the game opened it under (RCnt3 vblank, or
  // the libapi vblank class); broad spec so any opened+enabled vblank EvCB matches.
  hle_deliver_event(0xF2000003u, 0xFFFFFFFFu);
  hle_deliver_event(0xF0000001u, 0xFFFFFFFFu);
}

// 0x80085900 FUN_80085900 = libetc VSync(mode):
//   mode < 0  -> return current vblank count (query, no wait)
//   mode == 1 -> return hblank delta (query, no wait) — dummy 0 here
//   mode == 0 -> wait one vblank; mode > 1 -> wait `mode` vblanks. Advance the frame clock.
static void ov_vsync(R3000* c) {
  int32_t mode = (int32_t)c->r[A0];
  if (mode < 0) {
    c->r[V0] = g_vblank;
  } else if (mode == 1) {
    c->r[V0] = 0;
  } else {
    g_vblank += (mode == 0) ? 1u : (uint32_t)mode;
    c->r[V0] = g_vblank;
    frame_tick();
  }
  mem_w32(VBLANK_COUNT, g_vblank);
}

uint32_t timing_vblank(void) { return g_vblank; }

void timing_init(void) {
  rec_set_override(0x80085900u, ov_vsync);
}
