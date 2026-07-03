// class ScreenFade — implementation. See screen_fade.h for the architecture note.
#include "screen_fade.h"
#include "core.h"
#include "cfg.h"
#include <cstdio>
#include <execinfo.h>
#include <cstdlib>

// `debug fadetrace` channel — logs every native-path fade call (`ScreenFade::set` +
// `applyLeafCall`) with the calling context. Pairs with `PSXPORT_DISPWATCH=0x8007E9C8`
// which surfaces every substrate-path fade call. Together they show BOTH sides of the
// fade caller graph — essential for #27 (cutscene fadeouts stuck black), where the
// symptom is that some fade caller doesn't reach ScreenFade at all so the HOLD latch
// at full-black never releases. When a cutscene fades correctly, this channel shows
// per-frame set() calls with r/g/b ramping. When it doesn't, this channel goes silent
// while the DISPWATCH shows the substrate leaf firing = a substrate SM handler owns
// the failing fade; RE + port it native.
// Print the native C++ call chain for a fade caller — resolves function names via backtrace_symbols.
// Rate-limited: only print the stack on FIRST occurrence of a given (op,mode,rgb) tuple within one
// run so a per-frame ramp doesn't spam N identical stacks. The one-line summary still fires every
// call so the frame cadence is visible.
static void fadetrace(const char* op, uint8_t mode, uint32_t rgb, const char* extra) {
  static int s_on = -1;
  if (s_on < 0) s_on = cfg_dbg("fadetrace") ? 1 : 0;
  if (!s_on) return;
  const char* modeName = mode == 0 ? "NONE" : mode == 1 ? "ADDITIVE" : mode == 2 ? "SUBTRACTIVE" : "?";
  fprintf(stderr, "[fadetrace] %s mode=%s rgb=0x%06X%s%s\n", op, modeName, rgb,
          extra && *extra ? " " : "", extra ? extra : "");
  // First-time-seen (op,mode,rgb) tuple gets a C++ backtrace to identify the caller. Store in a tiny
  // fixed ring — bugged (a truly unique caller stream would overflow), but our fade caller set is
  // small (<32 distinct tuples) so it's fine for this diagnostic.
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

void ScreenFade::frameStart() {
  // Reset only the frame-scoped state. The held fully-faded state (+4..+7) persists across frames.
  core->mem_w8(GUEST_ADDR + 0, (uint8_t)NONE);
  core->mem_w8(GUEST_ADDR + 1, 0);
  core->mem_w8(GUEST_ADDR + 2, 0);
  core->mem_w8(GUEST_ADDR + 3, 0);
}

void ScreenFade::set(Mode mode, uint8_t r, uint8_t g, uint8_t b) {
  fadetrace("set", (uint8_t)mode, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b, nullptr);
  // Frame-scoped state.
  core->mem_w8(GUEST_ADDR + 0, (uint8_t)mode);
  core->mem_w8(GUEST_ADDR + 1, r);
  core->mem_w8(GUEST_ADDR + 2, g);
  core->mem_w8(GUEST_ADDR + 3, b);
  // Held fully-faded latch. Latched only when the fade is at or above the threshold in every channel
  // (screen is essentially fully black for SUBTRACTIVE or fully white for ADDITIVE). Any set() below
  // the threshold in any channel — i.e. the game has started ramping back toward "scene visible" —
  // releases the hold.
  bool prevHeldMode = core->mem_r8(GUEST_ADDR + 4) != (uint8_t)NONE;
  if (mode != NONE && r >= FULLY_FADED_THRESHOLD && g >= FULLY_FADED_THRESHOLD && b >= FULLY_FADED_THRESHOLD) {
    core->mem_w8(GUEST_ADDR + 4, (uint8_t)mode);
    core->mem_w8(GUEST_ADDR + 5, r);
    core->mem_w8(GUEST_ADDR + 6, g);
    core->mem_w8(GUEST_ADDR + 7, b);
    if (!prevHeldMode) fadetrace("HOLD latched", (uint8_t)mode,
                                 ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b, nullptr);
  } else {
    core->mem_w8(GUEST_ADDR + 4, (uint8_t)NONE);
    core->mem_w8(GUEST_ADDR + 5, 0);
    core->mem_w8(GUEST_ADDR + 6, 0);
    core->mem_w8(GUEST_ADDR + 7, 0);
    if (prevHeldMode) fadetrace("HOLD released", 0, 0, nullptr);
  }
}

void ScreenFade::applyLeafCall(uint32_t color, uint32_t a1) {
  // Guest ABI: color is 0x00RRGGBB in a0. a1 selects blend: !=0 additive, ==0 subtractive.
  fadetrace("applyLeafCall", (uint8_t)(a1 ? ADDITIVE : SUBTRACTIVE), color & 0xFFFFFFu, nullptr);
  set((a1 != 0u) ? ADDITIVE : SUBTRACTIVE,
      (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color);
}

ScreenFade::State ScreenFade::get() const {
  Mode frame_mode = (Mode)core->mem_r8(GUEST_ADDR + 0);
  if (frame_mode != NONE) {
    return State{
      frame_mode,
      core->mem_r8(GUEST_ADDR + 1),
      core->mem_r8(GUEST_ADDR + 2),
      core->mem_r8(GUEST_ADDR + 3),
    };
  }
  // No caller set fade this frame — return the held fully-faded state if any.
  return State{
    (Mode)core->mem_r8(GUEST_ADDR + 4),
    core->mem_r8(GUEST_ADDR + 5),
    core->mem_r8(GUEST_ADDR + 6),
    core->mem_r8(GUEST_ADDR + 7),
  };
}
