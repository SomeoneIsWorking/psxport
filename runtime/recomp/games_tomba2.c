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
#include <stdio.h>

void gen_func_800788AC(R3000*);    // recomp body (super-call)
void gen_func_8007712C(R3000*);    // recomp body of the per-object cull/LOD dispatcher
void gen_func_80044D8C(R3000*);    // DEBUG: LZ decompressor (CLUT/texture build)
void wide60_frame_commit(void);    // wide60: per-logic-frame fence (rate detect / interp)
void wide60_init(void);            // wide60: read PSXPORT_WIDE60
extern uint32_t g_current_object;  // wide60: object* whose RTP ops are being tagged
extern int g_wide60_on;            // wide60: capture enabled (PSXPORT_WIDE60)
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
  // Per-VBLANK audio work. On hardware the libsnd sequencer ticks once per VBlank IRQ (60 Hz NTSC)
  // and the SPU plays in realtime. One ov_frame_update is one *logic frame*, which on hardware spans
  // DAT_1f800235 (=quota) VBlanks (=2 => Tomba2's 30 fps). So the per-vblank work — the sequencer
  // tick AND the SPU's 1/60 s field advance (spu_audio_frame) — must run `quota` times per logic
  // frame to stay at the hardware 60 Hz rate in real time. later-54 ran BOTH once (matching each
  // other but at half real-time); windowed that plays audio at HALF tempo — the user heard the
  // menu-cursor tick too slow (the headless WAV hid it: its timeline is field-count, not wall-clock,
  // so 1 tick/1 field there is still 60:60 = correct-sounding). Running both quota× fixes real-time
  // playback and keeps the WAV's tick:field ratio unchanged (just a longer, more correct duration).
  // Sequencer guard: pointer initialized + sane code address (never call through null pre-SsStart).
  // Opt out (A/B): PSXPORT_T2_NOSEQTICK. Adaptive: a true-60fps scene (quota=1) ticks once.
  int quota = mem_r8(VBLANK_QUOTA); if (quota < 1) quota = 1;
  uint32_t seqfn = mem_r32(SEQ_FUNC_PTR);
  int seq_ok = !getenv("PSXPORT_T2_NOSEQTICK")
               && (seqfn & 0x1FFFFFFFu) >= 0x10000u && (seqfn & 0x1FFFFFFFu) < 0x200000u;
  for (int v = 0; v < quota; v++) {                  // once per VBlank this logic frame spans
    if (seq_ok) rec_dispatch(c, SEQ_TICK_WRAPPER);   // libsnd per-vblank tick (user cb + SsSeqCalled)
    spu_audio_frame();                               // advance SPU one 1/60 s field + feed device
  }
  mem_w16(DISPLAY_COUNTER, mem_r8(VBLANK_QUOTA));    // satisfy the pacing dwell immediately
  wide60_frame_commit();                             // wide60: this frame's geometry is projected
  gpu_present();                                     // one rendered frame per loop iteration
  gpu_pace_frame();                                  // throttle to game pace when windowed (1 call/frame)
}

// wide60 object tag: the universal per-object cull/LOD dispatcher (a0 = object*, once per logic
// frame for every live drawable). Every RTP op fired in its call tree is tagged with this object's
// stable pool-pointer id (the join key). Super-call the recomp body unchanged; clear on exit.
static void ov_object_cull(R3000* c) {
  uint32_t prev = g_current_object;
  g_current_object = c->r[4];                      // a0 = object* (MIPS arg register $a0)
  gen_func_8007712C(c);
  g_current_object = prev;
}

// PC-owned LZ image decompressor — replaces recompiled FUN_80044D8C (0x80044D8C). This routine
// rebuilds the per-frame CLUTs (0x801FCDC0) and sprite/texture data from compressed area assets.
// It was the source of the gameplay 2D-sprite corruption: the SAME function gave correct output
// when recompiled but ZEROS when flat-interpreted by the coroutine interpreter (rec_coro_run) at
// runtime — a recompiler-vs-interpreter divergence. A pure decompressor belongs to the PC side,
// so we own it natively here (one implementation, reached identically from both engines).
//
// ABI (matches the MIPS at 0x80044D8C, verified by disassembly):
//   a0=descriptor, a1=dest, a2=src, a3=srclen. Returns v0 = bytes written.
//   Setup: build 8 back-ref offsets from the static table at 0x800153C8, scaled by the per-call
//   stride at desc+4:  offset[i] = base[i] + 2*(factor[i]*stride)  (2D image predictors: mode 1 =
//   previous byte, modes 2-7 = previous-row neighbours; row pitch = stride).
//   Stream of control bytes: len=ctrl>>3, mode=ctrl&7.  mode==0 -> literal copy `len` bytes from
//   src (ctrl byte 0 / len 0 terminates).  mode!=0 -> back-ref copy `len` bytes from dest+offset
//   [mode], BYTE-granular so overlapping copies replicate (RLE), exactly as the original loop.
#define LZ_OFFTAB_BASE 0x800153C8u
static void ov_lz_decompress(R3000* c) {
  const uint32_t desc = c->r[4], dst = c->r[5], src0 = c->r[6], srclen = c->r[7];
  const uint32_t src_end = src0 + srclen;
  const int32_t stride = (int16_t)mem_r16(desc + 4);
  int32_t offtab[8];
  for (int i = 0; i < 8; i++) {
    const int32_t base   = (int32_t)mem_r32(LZ_OFFTAB_BASE + i * 8 + 0);
    const int32_t factor = (int32_t)mem_r32(LZ_OFFTAB_BASE + i * 8 + 4);
    offtab[i] = base + 2 * (factor * stride);
  }
  uint32_t src = src0, out = dst;
  while (src < src_end) {
    const uint8_t ctrl = mem_r8(src++);
    const uint32_t len = ctrl >> 3, mode = ctrl & 7u;
    if (mode != 0) {                                  // back-reference into the output so far
      uint32_t bsrc = out + (uint32_t)offtab[mode];
      for (uint32_t k = 0; k < len; k++) mem_w8(out++, mem_r8(bsrc++));
    } else {                                          // literal run from the source
      if (len == 0) break;                            // terminator
      for (uint32_t k = 0; k < len; k++) mem_w8(out++, mem_r8(src++));
    }
  }
  c->r[2] = out - dst;                                // v0 = total bytes written
}

void games_tomba2_init(void) {
  rec_set_override(0x800788ACu, ov_frame_update);
  // PC-owned decompressor (A/B: PSXPORT_LZ_RECOMP=1 keeps the recomp body for comparison).
  if (!getenv("PSXPORT_LZ_RECOMP")) rec_set_override(0x80044D8Cu, ov_lz_decompress);
  wide60_init();
  if (g_wide60_on)                                 // object-tag dispatcher only when capturing
    rec_set_override(0x8007712Cu, ov_object_cull);
}
