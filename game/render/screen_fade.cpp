// class ScreenFade — implementation. See screen_fade.h for the architecture note.
#include "screen_fade.h"
#include "game_ctx.h"
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
void ScreenFade::fadetrace(const char* op, uint8_t mode, uint32_t rgb, const char* extra) {
  if (mTraceOn < 0) mTraceOn = cfg_dbg("fadetrace") ? 1 : 0;
  if (!mTraceOn) return;
  const char* modeName = mode == 0 ? "NONE" : mode == 1 ? "ADDITIVE" : mode == 2 ? "SUBTRACTIVE" : "?";
  fprintf(stderr, "[fadetrace] %s mode=%s rgb=0x%06X%s%s\n", op, modeName, rgb,
          extra && *extra ? " " : "", extra ? extra : "");
  uint32_t key = ((uint32_t)(uintptr_t)op * 0x9E37u) ^ ((uint32_t)mode << 24) ^ (rgb & 0xFFFFFFu);
  for (int i = 0; i < mSeenN; i++) if (mSeen[i] == key) return;
  if (mSeenN < 64) mSeen[mSeenN++] = key;
  void* stack[16];
  int n = backtrace(stack, 16);
  char** syms = backtrace_symbols(stack, n);
  if (syms) {
    fprintf(stderr, "  first-time stack for (%s, %s, 0x%06X):\n", op, modeName, rgb);
    for (int i = 0; i < n; i++) fprintf(stderr, "    %s\n", syms[i]);
    free(syms);
  }
}

void ScreenFade::frameStart() {
  // Reset the frame-scoped state. Matches PSX: OT slot 4 is rebuilt fresh every frame, so a frame
  // with no caller has nothing in it. No cross-frame hold — see the class header note.
  mFrameMode = NONE;
  mFrameR = 0;
  mFrameG = 0;
  mFrameB = 0;
}

void ScreenFade::set(Mode mode, uint8_t r, uint8_t g, uint8_t b, uint32_t otSlot) {
  fadetrace("set", (uint8_t)mode, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b, nullptr);

  // Update host-owned frame-scoped state (what the native renderer reads via get()). Last call
  // wins for this frame; nothing persists past the next frameStart().
  mFrameMode = mode;
  mFrameR = r;
  mFrameG = g;
  mFrameB = b;

  (void)otSlot;   // native renderer reads via get(); no guest-side packet build here
}

void ScreenFade::applyLeafCall(uint32_t color, uint32_t a1, uint32_t otSlot) {
  fadetrace("applyLeafCall", (uint8_t)(a1 ? ADDITIVE : SUBTRACTIVE), color & 0xFFFFFFu, nullptr);
  set((a1 != 0u) ? ADDITIVE : SUBTRACTIVE,
      (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color,
      otSlot);
}

ScreenFade::State ScreenFade::get() const {
  return State{ mFrameMode, mFrameR, mFrameG, mFrameB };
}

// ---- Fade-leaf tap: own FUN_8007e9c8 GLOBALLY (engine-overrides directive) -------------------
// Every still-substrate fade caller reaches gen_func_8007E9C8 via a STATIC gen-to-gen jal — a call
// path that never passes rec_dispatch, so neither PSXPORT_DISPWATCH nor this class ever saw it.
// Concretely (#63): the fisherman-cutscene fade-in ramps the guest OT rect every frame while the
// native present read mFrameMode==NONE and showed the scene at full brightness. The fix is the
// sanctioned leaf-engine global ownership: install on the shard g_override[] table via the
// oracle-gated thunk (engine_set_override_main), run the ORIGINAL gen body so guest packet-pool/
// OT/scratchpad state stays byte-exact for SBS, then mirror the guest-ABI args {a0=color,
// a1=blend, a2=slot} into the host frame state. SBS core B keeps running pure gen (thunk gates on
// psx_fallback), and pc_render still writes zero guest bytes — the tap is logic-side, not render.
extern void gen_func_8007E9C8(Core*);
extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
namespace {
void fadeLeafTap(Core* c) {
  const uint32_t color = c->r[4], blend = c->r[5], slot = c->r[6];
  gen_func_8007E9C8(c);
  fade(c).applyLeafCall(color, blend, slot);
}
}
void ScreenFade::installLeafTap() {
  static bool done = false;
  if (done) return;
  done = true;
  engine_set_override_main(0x8007E9C8u, fadeLeafTap, gen_func_8007E9C8);
}

// ScreenFade::sequence — the GAME-overlay a0l per-node fade sequencer (guest FUN_8010957C).
// Multi-step ramp SM driven by node+2 (outer state) / node+3 (running-substep) / node+106
// (fade LEVEL 0..31) / node+104 (step-2 delay counter). Two still-substrate leaves: the
// per-frame helper 0x8010CC68 (returns a "ready-to-advance" boolean in v0) and the init poke
// 0x8010D030 (per-node overlay init). See engine.h for the caller (fieldRun sm[0x4e]==0xb).
#include "core/engine.h"                     // Engine::zoneTransitionSetup (native)
void ScreenFade::sequence(uint32_t node) {
  Core* c = core;
  const uint8_t outer = c->mem_r8(node + 2);

  if (outer == 0) {
    // Init step: three prep calls + arm the state to run its first ramp on the next tick.
    eng(c).modeStateArm.arm();                  // native — was rec_dispatch 0x8005082C(0,0,0)
    eng(c).audioDispatch.zoneTransitionSetup(11);         // FUN_8001D71C(11) — native
    c->mem_w8(0x800BFA55u, 0);
    c->mem_w8(node + 3, 0);
    c->mem_w8(node + 2, (uint8_t)(outer + 1)); // -> outer state 1
    c->r[4] = node;
    rec_dispatch(c, 0x8010D030u);              // ov_a0l_func_8010D030(node) — not yet decoded
    c->mem_w16(node + 106, 31);
    applyLeafCall(0x00FFFFFFu, 0);             // full black (subtractive white)
    return;
  }

  if (outer != 1) return;                      // any other outer value: permanent no-op

  const uint8_t step = c->mem_r8(node + 3);
  if (step >= 6) return;                       // bounds check — once step reaches 6 this is inert

  auto rampLevel = [&](int32_t sign) -> uint32_t {
    // v = (level << 3) [negated if sign<0] & 0xFF, replicated into R/G/B.
    const int16_t level = c->mem_r16s(node + 106);
    const uint32_t v = (uint32_t)((sign < 0) ? -(level << 3) : (level << 3)) & 0xFFu;
    return (v << 16) | (v << 8) | v;
  };
  auto decrementLevelClamped = [&]() {
    const int16_t level = c->mem_r16s(node + 106);
    if (level != 0) c->mem_w16(node + 106, (uint16_t)(level - 1));
  };
  auto advanceStep = [&]() {
    c->mem_w8(node + 3, (uint8_t)(c->mem_r8(node + 3) + 1));
  };
  auto helperCC68 = [&](uint32_t arg) {
    c->r[4] = arg;
    rec_dispatch(c, 0x8010CC68u);              // ov_a0l_func_8010CC68 — returns bool in v0
  };

  switch (step) {
    case 0: {                                  // ramp UP, gated by helper return value
      applyLeafCall(rampLevel(+1), 1);
      decrementLevelClamped();
      helperCC68(0);
      if (c->r[2] == 0) return;                // not done yet
      c->mem_w16(node + 106, 31);
      advanceStep();
      return;
    }
    case 1: {                                  // ramp DOWN, gated by fade LEVEL reaching 0
      applyLeafCall(rampLevel(-1), 1);
      decrementLevelClamped();
      helperCC68(0);                           // result unused this branch
      if (c->mem_r16s(node + 106) != 0) return;
      advanceStep();
      c->mem_w16(node + 104, 20);              // arm the step-2 delay counter
      return;
    }
    case 2: {                                  // plain ~20-tick delay, then reset level + advance
      const uint16_t d = (uint16_t)(c->mem_r16(node + 104) - 1);
      c->mem_w16(node + 104, d);
      if (d != 0) return;
      c->mem_w16(node + 106, 31);
      advanceStep();
      return;
    }
    case 3: {                                  // same as case 0 but helper called with a0=1
      applyLeafCall(rampLevel(+1), 1);
      decrementLevelClamped();
      helperCC68(1);
      if (c->r[2] == 0) return;
      c->mem_w16(node + 106, 31);
      advanceStep();
      return;
    }
    case 4: {                                  // same as case 1 but helper called with a0=1;
                                               // on completion does NOT reset the level to 31
      applyLeafCall(rampLevel(-1), 1);
      decrementLevelClamped();
      helperCC68(1);
      if (c->mem_r16s(node + 106) != 0) return;
      advanceStep();
      return;
    }
    case 5: {                                  // completion tail: poke the shared sm struct + the
                                               // 0x800BF839/0x800BF83A control globals, advance
      const uint32_t sm = c->mem_r32(0x1F800138u);
      c->mem_w16(sm + 74, 1);
      c->mem_w16(sm + 76, 2);
      c->mem_w16(sm + 78, 6);
      c->mem_w8 (0x800BF839u, 3);
      c->mem_w16(0x800BF83Au, 0x1501);
      advanceStep();
      return;
    }
  }
}
