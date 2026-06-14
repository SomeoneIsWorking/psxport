// Tomba!2-specific native overrides (per-game tier). Generic mechanisms live in timing.c /
// cd_override.c; this file holds glue tied to MAIN.EXE's own addresses.
//
// VBlank pacing: port the dwell to PC (don't dwell)
// ------------------------------------------------
// The StrPlayer main loop FUN_80050b08 paces each displayed frame with a busy-wait at
// 0x80050CE4:  DAT_800e809c = 0;  ... ;  do {} while (DAT_800e809c < DAT_1f800235);
// On hardware the VBlank IRQ bumps DAT_800e809c (0x800E809C, u16) until it reaches the
// per-frame quota DAT_1f800235 (scratchpad u8, =2 => the engine's 30 fps logic rate). This
// is pure frame-rate pacing. In a PC port frame pacing belongs to the host present loop, not
// a self-spinning counter, and we deliver no preemptive VBlank IRQ — so we make the loop NOT
// dwell: FUN_800788ac is the per-frame state update called exactly once per iteration (its
// only caller is the loop, right after the counter reset and before the dwell), so after its
// real body we set the display counter to the quota the dwell tests => the dwell falls
// through on its first check. This is exactly the state the real VBlank handler would have
// produced (the cb at 0x800506B4 only increments that counter), computed directly.
// (When a host present loop exists it will pace frames; this just removes the busy-wait.)
#include "r3000.h"

void gen_func_800788AC(R3000*);    // recomp body (super-call)
void gpu_present(void);            // native GPU: present the displayed VRAM region
void spu_audio_frame(void);        // SPU: advance the mixer one frame + feed the audio device

#define DISPLAY_COUNTER 0x800E809Cu   // DAT_800e809c (u16) — the dwell's vblank counter
#define VBLANK_QUOTA    0x1F800235u   // DAT_1f800235 (u8)  — vblanks per displayed frame

static void ov_frame_update(R3000* c) {
  gen_func_800788AC(c);                              // real per-frame state update
  mem_w16(DISPLAY_COUNTER, mem_r8(VBLANK_QUOTA));    // satisfy the pacing dwell immediately
  gpu_present();                                     // one rendered frame per loop iteration
  spu_audio_frame();                                 // advance + feed audio (sole spu_update driver)
}

void games_tomba2_init(void) {
  rec_set_override(0x800788ACu, ov_frame_update);
}
