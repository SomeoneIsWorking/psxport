// game/audio/sequencer.h — class Sequencer — libsnd per-vblank sequencer TICK wrapper +
// the SsSeqCalled channel-flag dispatcher and its high-confidence leaves.
//
// PROPER OOP: one instance per Core, embedded on Engine (`c->engine.sequencer`). Back-pointer
// `core` wired at Core construction time (same pattern as MusicCoord / AudioDispatch).
//
// SCOPE (WIDE-RE DRAFT — see docs/engine_re.md libsnd section, docs/journal.md 2026-06-15
// "later 54" entry): 0x800909C0 FUN_800909c0 is libsnd's per-VBlank tick wrapper, installed by
// SsSetTickMode as the IRQ-driven "user cb". Runtime globals (confirmed live via REPL dump):
//   tick mode        DAT_800ac424 = 5
//   SsSeqCalled ptr   DAT_800ac42c = 0x80090BD0   (the sequencer engine itself)
//   user callback     DAT_800ac430 = 0x80086288   (Timing::vsyncCallbackDispatch — see timing.h;
//                                                   also unwired/unowned by us)
// The wrapper itself (FUN_800909c0) is a 2-call trampoline: run the user cb if installed, then
// unconditionally run *SsSeqCalled(). NEITHER 0x800909c0 nor 0x80090bd0 is reached by a direct
// `jal` anywhere in MAIN.EXE (only ever fired through the IRQ callback pointer), so the static
// recompiler's indirect-call discovery never saw them — they only run via the hybrid interpreter
// today (game/game_tomba2.cpp SEQ_TICK_WRAPPER, called once per ov_frame_update).
//
// 2026-07-09 wide-RE wave — SsSeqCalled (0x80090BD0) cluster. RE'd via generated/shard_3.c:21497:
// a reentrancy-guarded (flag 0x80104C24) double loop over up to `*(s16*)0x801054B0` sequences x
// up to `*(s16*)0x801054B2` channels each, indexed through a pointer array at 0x80104C30
// (4-byte stride, one base pointer per sequence), testing a per-sequence active bitmask at
// 0x80104C28 and, per channel, testing 8 independent bits in a per-channel record's flags field
// at `channelBase+152` (channelBase = *seqArrayPtr + chan*176) to conditionally dispatch 7
// leaves. NOTE: an earlier pass's summary (see docs/engine_re.md) transcribed these globals as
// 0x8010CC24/0x8010CC28/0x80109E70/0x80109E72 — WRONG, corrected here from a fresh direct read
// of the current gen body (32784u<<16 = 0x80100000, +19492/19496/19504/21680/21682 decimal).
//
// Per-channel flags(+152) bit -> leaf (bit value, guest addr, native status):
//   bit0 (0x01) 0x800910F0  DRAFTED channelPitchSelectDispatch() — thin wrapper, tail-dispatches
//                            still-unowned 0x80091120 with sign-extended (seq,chan).
//   bit1 (0x02) 0x80091050  DRAFTED channelReleaseClear() — clears the bit + a status byte, calls
//                            still-unowned 0x80095B90(seq|chan<<8).
//   bit3 (0x08) 0x80091910  DRAFTED channelStopFlagSet() — sets a status byte, clears the bit.
//                            True leaf, no stack frame in the gen body.
//   bit4 (0x10) 0x80090E40  MAPPED, NOT drafted — pitch-slide/portamento per-tick interpolator:
//                            counter/limit ramp at +160/+156, cpu_div-based step ratio applied to
//                            +72/+74, clamps two derived values to [0,127] via a call to
//                            still-unowned 0x80095A9C, applies via still-unowned 0x80095530, then
//                            a register-level "retarget" combine (r16/r17/r19/r20/r21) recomputes
//                            the channel pointer used by the shared tail (clears bit4, writes
//                            +92/+94 to 127 via 0x80095A9C again). Control flow fully RE'd
//                            (generated/shard_4.c:15017); FIELD SEMANTICS beyond +72/+74/+152/
//                            +156/+160 are inferred, not confirmed. See docs/engine_re.md.
//   bit5 (0x20) 0x80090E40  same leaf, second call site (identical body).
//   bit6 (0x40) 0x80092080  MAPPED, NOT drafted — ADSR/envelope ramp: decrements +168 counter, or
//                            (when exhausted) computes a new +148 envelope value via cpu_div
//                            against +78 (rate) or a direct +/- step against +80, clamps to
//                            +172 (target), writes +84 (output level) via cpu_divu, clears
//                            bit6/bit7. Control flow fully RE'd (generated/shard_1.c:17775).
//   bit7 (0x80) 0x80092080  same leaf, second call site.
//   bit2 (0x04) 0x80091970  MAPPED, NOT drafted — large per-channel NOTE-INIT/retrigger block:
//                            clears bits {0,1,3,10}, sets bit2, calls still-unowned 0x80095B90 +
//                            0x800931A0 (using a LIVE/STALE a2 register the SsSeqCalled caller
//                            never explicitly sets — see docs/engine_re.md), then zeroes ~14
//                            per-channel status bytes and (re)inits +0/+8/+84/+92/+94/+144/+148
//                            and a 16-entry breakpoint-table pair at +39.. / +55.. plus a 16x u16
//                            array at +96..+126 (all set to 127). Control flow fully RE'd
//                            (generated/shard_4.c:15144). SsSeqCalled itself, after dispatching
//                            this bit, unconditionally zeroes the WHOLE flags field (+152) — that
//                            part IS native (seqChannelDispatch()) since it's SsSeqCalled's own
//                            code, not this leaf's.
//
// seqChannelDispatch() (0x80090BD0 SsSeqCalled) is DRAFTED: the double loop + reentrancy guard +
// prep-call + all 8 bit tests are native; the 3 leaves above that are drafted are called as real
// C++ methods, the 3 not-yet-owned ones (0x80090E40 x2, 0x80092080 x2, 0x80091970) stay
// rec_dispatch(c, addr) call-outs, same pattern frameTick() already uses for SsSeqCalled itself.
// Guest-stack frame MIRRORED (sp-56, spill ra/s0-s7 at their RE'd offsets, including on the
// reentrancy-guard early-exit path, since the gen prologue spills unconditionally before the
// guard test — CLAUDE.md "mirror the guest stack" applies even to the do-nothing path).
//
// WIDE-RE DRAFT, ALL METHODS UNWIRED: no EngineOverrides registration, no SBS run. Compiles only.
#pragma once
#include <cstdint>
class Core;

class Sequencer {
public:
  Core* core = nullptr;

  // frameTick(): 0x800909C0 FUN_800909c0 — per-VBlank libsnd tick wrapper. Faithful to
  //   gen_func_800909C0 (generated/shard_7.c:14127): if the user-cb slot (0x800AC430) is
  //   non-null, rec_dispatch() it; then unconditionally rec_dispatch() the SsSeqCalled pointer
  //   at 0x800AC42C (today: seqChannelDispatch() below, still routed through rec_dispatch since
  //   this draft doesn't rewrite frameTick's own dispatch target — see header note above).
  void frameTick();

  // 0x80090BD0 SsSeqCalled — the per-VBlank sequence/channel scheduler. See header comment.
  void seqChannelDispatch();

  // --- leaves (see header comment for RE detail / confidence) ---
  void channelPitchSelectDispatch();  // 0x800910F0 — HIGH confidence, thin wrapper
  void channelReleaseClear();         // 0x80091050 — HIGH confidence
  void channelStopFlagSet();          // 0x80091910 — HIGH confidence, true leaf
};
