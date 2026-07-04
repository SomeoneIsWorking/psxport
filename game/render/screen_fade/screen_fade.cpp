// class ScreenFade — implementation. See screen_fade.h for the architecture note.
#include "screen_fade.h"
#include "core.h"
#include "game.h"
#include "cfg.h"
#include <cstdio>
#include <execinfo.h>
#include <cstdlib>

// `debug fadetrace` channel — logs every native-path fade call with the calling context. Pairs
// with `PSXPORT_DISPWATCH=0x8007E9C8` which surfaces every substrate-path fade call. Together they
// show both sides of the fade caller graph — essential when a substrate-side SM owns a fade and
// the class never sees it (the HOLD then never releases). Rate-limited: only prints the C++
// backtrace on FIRST occurrence of a given (op,mode,rgb) tuple; the one-line summary fires every
// call so the frame cadence stays visible.
static void fadetrace(const char* op, uint8_t mode, uint32_t rgb, const char* extra) {
  static int s_on = -1;
  if (s_on < 0) s_on = cfg_dbg("fadetrace") ? 1 : 0;
  if (!s_on) return;
  const char* modeName = mode == 0 ? "NONE" : mode == 1 ? "ADDITIVE" : mode == 2 ? "SUBTRACTIVE" : "?";
  fprintf(stderr, "[fadetrace] %s mode=%s rgb=0x%06X%s%s\n", op, modeName, rgb,
          extra && *extra ? " " : "", extra ? extra : "");
  static uint32_t seen[64] = {0};
  static int seen_n = 0;
  uint32_t key = ((uint32_t)(uintptr_t)op * 0x9E37u) ^ ((uint32_t)mode << 24) ^ (rgb & 0xFFFFFFu);
  for (int i = 0; i < seen_n; i++) if (seen[i] == key) return;
  if (seen_n < 64) seen[seen_n++] = key;
  void* stack[16];
  int n = backtrace(stack, 16);
  char** syms = backtrace_symbols(stack, n);
  if (syms) {
    fprintf(stderr, "  first-time stack for (%s, %s, 0x%06X):\n", op, modeName, rgb);
    for (int i = 0; i < n; i++) fprintf(stderr, "    %s\n", syms[i]);
    free(syms);
  }
}

// Dispatch the substrate FUN_8007e9c8 (screen-fade packet builder) with the ABI it expects, so its
// scratchpad + packet-pool + OT-link writes fire. Guest-call setup mirrors the d3(c, fn, a0, a1, a2)
// helper in engine_stage.cpp: load a0..a2, invoke rec_dispatch. r[29] (sp) is preserved so the
// callee's stack cleanup doesn't leak into the native caller's stack frame. Called ONLY under
// pc_faithful (pc_skip=false) — the native render doesn't read what the substrate writes here.
static constexpr uint32_t kGuestFadePacketBuilder = 0x8007E9C8u;
static void dispatch_faithful_fade(Core* c, uint32_t color, uint32_t a1, uint32_t otSlot) {
  uint32_t saved_sp = c->r[29];
  c->r[4] = color;
  c->r[5] = a1;
  c->r[6] = otSlot;
  rec_dispatch(c, kGuestFadePacketBuilder);
  c->r[29] = saved_sp;
}

void ScreenFade::frameStart() {
  // Reset only the frame-scoped state. The held fully-faded state persists across frames.
  mFrameMode = NONE;
  mFrameR = 0;
  mFrameG = 0;
  mFrameB = 0;
}

void ScreenFade::set(Mode mode, uint8_t r, uint8_t g, uint8_t b, uint32_t otSlot) {
  fadetrace("set", (uint8_t)mode, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b, nullptr);

  // Update host-owned frame-scoped state (what the native renderer reads via get()).
  mFrameMode = mode;
  mFrameR = r;
  mFrameG = g;
  mFrameB = b;

  // Held-fully-faded latch. Latched only when the fade is at or above the threshold in every
  // channel (screen essentially fully black for SUBTRACTIVE / fully white for ADDITIVE). A set()
  // below the threshold in any channel — i.e. game is ramping back toward "scene visible" —
  // releases the hold.
  bool prevHeld = mHeldMode != NONE;
  if (mode != NONE && r >= FULLY_FADED_THRESHOLD && g >= FULLY_FADED_THRESHOLD && b >= FULLY_FADED_THRESHOLD) {
    mHeldMode = mode; mHeldR = r; mHeldG = g; mHeldB = b;
    if (!prevHeld) fadetrace("HOLD latched", (uint8_t)mode,
                             ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b, nullptr);
  } else {
    mHeldMode = NONE; mHeldR = 0; mHeldG = 0; mHeldB = 0;
    if (prevHeld) fadetrace("HOLD released", 0, 0, nullptr);
  }

  // pc_faithful side-effect: reproduce the substrate FUN_8007e9c8 guest writes so SBS sees
  // identical scratchpad + packet-pool bytes on core-A native and core-B substrate. Skipped in
  // pc_skip (the native renderer doesn't need those bytes).
  if (core->game && !core->game->pc_skip) {
    uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    uint32_t a1    = (mode == ADDITIVE) ? 1u : 0u;
    dispatch_faithful_fade(core, color, a1, otSlot);
  }
}

void ScreenFade::applyLeafCall(uint32_t color, uint32_t a1, uint32_t otSlot) {
  fadetrace("applyLeafCall", (uint8_t)(a1 ? ADDITIVE : SUBTRACTIVE), color & 0xFFFFFFu, nullptr);
  set((a1 != 0u) ? ADDITIVE : SUBTRACTIVE,
      (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color,
      otSlot);
}

ScreenFade::State ScreenFade::get() const {
  if (mFrameMode != NONE) return State{ mFrameMode, mFrameR, mFrameG, mFrameB };
  return State{ mHeldMode, mHeldR, mHeldG, mHeldB };
}
