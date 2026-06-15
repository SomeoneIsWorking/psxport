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
#include <stdlib.h>

void gen_func_800788AC(R3000*);    // recomp body (super-call)
void wide60_frame_commit(void);    // wide60: per-logic-frame fence (rate detect / interp)
void wide60_init(void);            // wide60: read PSXPORT_WIDE60
void gpu_present(void);            // native GPU: present the displayed VRAM region
void gpu_pace_frame(void);         // native GPU: throttle to game pace when windowed (no-op headless)
void spu_audio_frame(void);        // SPU: advance the mixer one frame + feed the audio device
void rec_dispatch(R3000*, uint32_t);  // hybrid call: recomp body if emitted, else interpret

#define DISPLAY_COUNTER 0x800E809Cu   // DAT_800e809c (u16) — the dwell's vblank counter
#define VBLANK_QUOTA    0x1F800235u   // DAT_1f800235 (u8)  — vblanks per displayed frame

// libsnd music-sequencer tick (RE: docs/journal.md later-53; SsSetTickMode = FUN_80090750).
// Tomba2 sequences its in-game/menu BGM with the libsnd sequencer, ticked from the VBlank IRQ
// (tick mode 5, RCnt3/vblank). The IRQ runs the tick wrapper FUN_800909c0, which chains the
// optional per-vblank user callback (DAT_800ac430) then the sequencer SsSeqCalled (DAT_800ac42c).
// The port delivers NO preemptive IRQ and collapses the pace-dwell the IRQ would fire in, so on
// hardware-faithful boot the sequencer never ticks -> zero per-note KON -> silent SPU (verified
// vs the oracle, later-53: the oracle writes KON from this very ISR while parked in the dwell).
// FIX (port the HW interrupt work to PC, per the busy-wait-porting rule — NOT simulate the IRQ):
// run the same tick wrapper natively once per vblank. The wrapper/sequencer are NOT emitted by
// the static recompiler (only reached via the IRQ callback pointer, never a direct jal), so we
// invoke them through rec_dispatch -> the hybrid interpreter (bit-identical to recomp); it runs
// FUN_800909c0 to its `jr ra` and returns. Caller-saved regs it clobbers are dead across the
// FUN_800788ac call site by MIPS convention, so this is safe to run right after the super-call.
#define SEQ_TICK_WRAPPER 0x800909C0u  // FUN_800909c0: per-vblank libsnd tick (user cb + SsSeqCalled)
#define SEQ_FUNC_PTR     0x800AC42Cu  // DAT_800ac42c: SsSeqCalled pointer (0 until SsStart inits)

static void ov_frame_update(R3000* c) {
  gen_func_800788AC(c);                              // real per-frame state update
  // Tick the BGM sequencer at the SPU's clock rate. On hardware it ticks once per VBlank IRQ
  // (60 Hz NTSC); the SPU plays in realtime, so the faithful ratio is ONE tick per SPU field.
  // spu_audio_frame() below advances the SPU exactly one 1/60 s field per ov_frame_update, so we
  // tick ONCE here (NOT VBLANK_QUOTA=2 times — that is the game's 30 fps *display* pacing, which
  // is independent of audio tempo relative to SPU time; ticking twice plays the music 2x fast).
  // Guard on the sequencer pointer being initialized + a sane code address so we never call
  // through a null/garbage pointer before SsStart sets up libsnd. Opt out (A/B): PSXPORT_T2_NOSEQTICK.
  if (!getenv("PSXPORT_T2_NOSEQTICK")) {
    uint32_t seqfn = mem_r32(SEQ_FUNC_PTR);
    if ((seqfn & 0x1FFFFFFFu) >= 0x10000u && (seqfn & 0x1FFFFFFFu) < 0x200000u)
      rec_dispatch(c, SEQ_TICK_WRAPPER);
  }
  mem_w16(DISPLAY_COUNTER, mem_r8(VBLANK_QUOTA));    // satisfy the pacing dwell immediately
  wide60_frame_commit();                             // wide60: this frame's geometry is projected
  gpu_present();                                     // one rendered frame per loop iteration
  spu_audio_frame();                                 // advance + feed audio (sole spu_update driver)
  gpu_pace_frame();                                  // throttle to ~30fps when windowed (1 call/frame)
}

void games_tomba2_init(void) {
  rec_set_override(0x800788ACu, ov_frame_update);
  wide60_init();
}
