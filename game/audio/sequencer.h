// game/audio/sequencer.h — class Sequencer — libsnd per-vblank sequencer TICK wrapper +
// the SsSeqCalled channel-flag dispatcher and its high-confidence leaves.
//
// PROPER OOP: one instance per Core, embedded on Engine (`eng(c).sequencer`). Back-pointer
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
// 2026-07-10 wide-RE wave — the 3 remaining bit4/5/6/7/2 leaves DRAFTED (follow-up closing the
// prior wave's "MAPPED, NOT drafted" trio). Corrects nothing global; only adds methods + their
// own small callees. Full field-level detail lives at each method's definition in sequencer.cpp
// (kept there per this file's own "class IS the RE artifact" convention) — summary:
//
//   bit4/bit5 (0x800910E40) -> channelPitchSlideTick(): portamento ramp. Faithful transcription
//     kept register-literal with goto/labels named after the guest addresses (the gen body has
//     several re-converging branches -- L_80090FF8 is reached from two different call sites, and
//     restructuring into if/else risks silently changing fallthrough order). Calls the newly
//     drafted channelVolumeSnapshot() (0x80095A9C) for both its "read +88/+90" points; the SPU
//     voice-register write (0x80095530, generated/shard_0.c:15026, ~320 lines, a KON/pan-table
//     loop over the hardware voice-bit array) stays MAPPED/rec_dispatch — too large/deep for this
//     pass, see docs/engine_re.md.
//   bit6/bit7 (0x80092080) -> channelEnvelopeRampTick(): ADSR ramp. TRUE LEAF in the gen body (no
//     `addiu sp,-N` at all) -- no stack frame to mirror. Same goto/label-literal style as the
//     pitch-slide leaf (multiple branches re-converge on the shared "clear bit6+bit7" tail at
//     L_80092284). No unowned callees (the gen body's trailing `func_800922A0(c)` after the real
//     `return` is the same shard-grouping dead-tail artifact documented elsewhere in this file;
//     not ported).
//   bit2 (0x80091970) -> channelNoteInit(): per-channel note retrigger. Mostly linear (bit-clear
//     housekeeping + a 16-entry breakpoint-table init loop), ported as structured C++ rather than
//     goto/labels since there's exactly one real branch (the 16-iteration loop). Calls two newly
//     drafted leaves: channelKeyEventScan() (0x80095B90) and channelKeyRegisterMerge()
//     (0x80094B50, called via channelKeyEventScan, not directly).
//
//   New small callees drafted this wave (all true leaves, no stack frame):
//     channelVolumeSnapshot() (0x80095A9C) — HIGH confidence: reads a channel's +88/+90 u16 pair
//       into two caller-given guest addresses; also has a genuine (if likely inert) side-effect
//       write of the combined seq|chan<<8 arg to a scratch global (0x80105D0C) that the gen body
//       itself never reads back (the reload 3 lines later is dead — an unreachable duplicate tail,
//       same shard artifact pattern, not ported).
//     channelKeyEventScan() (0x80095B90) — LOW-MEDIUM confidence: role NOT independently confirmed
//       (inherited uncertainty from the prior wave's note). Scans the 0x800AC3F4 hardware-voice
//       bitmask (32779u<<16-15372) against a per-voice pitch table at 0x801054D8 (stride 8) for a
//       match, and on hit stamps a scratch u16 at 0x80105D10 then calls channelKeyRegisterMerge().
//       Control-flow transcription is exact; the SEMANTIC read ("what does a matching voice mean")
//       is inferred from the neighboring SPU-register cluster, not confirmed against a live dump.
//     channelKeyRegisterMerge() (0x80094B50) — MEDIUM confidence: builds a KON/KOFF-shaped 16-bit
//       register pair (OR-then-AND-mask idiom typical of SPU key-on/key-off command words) from the
//       scratch value channelKeyEventScan() just stamped; self-contained, no calls, no branches.
//
// 2026-07-10 (2nd wide-RE wave, disjoint band) — closes the "0x80095530 stays MAPPED/rec_dispatch,
// too large/deep" gap left by the pass above. DRAFTED (LOW-MEDIUM confidence — pure register-
// literal transcription, semantics NOT independently reasoned about beyond what's needed to name
// the method and identify the shared voice-record layout):
//   channelVoiceRegisterWrite() (0x80095530, generated/shard_0.c:15026, 320 lines) — the "SPU
//     voice-register write leaf" channelPitchSlideTick() calls via rec_dispatch. ABI a0(r4)=
//     seq|chan<<8 packed byte pair (same packing channelVolumeSnapshot/channelPitchSlideTick use),
//     a1(r5)/a2(r6)=clamped [0,127] snapshot pair (write-through into the channel's +88/+90 u16
//     fields, matching channelPitchSlideTick's post-clamp r5/r6), a3(r7)=a small flag (values 0/1
//     seen at call sites — channelPitchSlideTick always passes 1). Body: writes the clamped pair to
//     the channel record's +88/+90 fields (clamping each to 127 again if either input was already
//     >=127, a redundant-looking belt-and-suspenders re-clamp); if the KEYSCAN voice-count byte
//     (SEQ_KEYSCAN_COUNT, +23788) is >0, loops voice index 0..count-1 over the SAME stride-56
//     record array channelKeyEventScan()/channelKeyRegisterMerge() use (base offset +21712, i.e.
//     SEQ_KEYSCAN_TABLE-8): skips any voice whose bit is set in the SAME hw voice-active bitmask
//     (SEQ_KEYSCAN_VOICE_MASK) channelKeyEventScan() scans, then gates on three per-voice-record
//     field compares (+21720 vs the packed-arg-derived channel index, +21728 vs channelBase+38,
//     — the three-way gate that decides "this voice belongs to this channel/tone") before calling
//     channelVoiceSelectPrep() (0x800962B0, new leaf this wave) and running a dense fixed-point
//     pipeline (several 32x32->64 multiplies feeding a pan-table lookup at +23780/+23772/+23784
//     stride-16-then-32 record clusters and constants 0x516*65536|2065, 0x33288<<16|8323,
//     0x32770<<16|9 = ADSR/pan curve constants — NOT independently confirmed, just carried
//     literally) that produces a left/right amplitude pair written to two parallel u16 tables at
//     +23080 (r30) and +23082 (r8|r30-relative) indexed by voice*16, then ORs 0x3 into a per-voice
//     status byte at +23048. This is the piece CLAUDE.md calls "GP0/OT reasoning is a red flag" —
//     PC render must NEVER consume this table; it exists purely so pc_faithful's guest RAM matches
//     recomp_path's SPU-adjacent scratch state byte-for-byte.
//   channelVoiceSelectPrep() (0x800962B0, generated/shard_5.c:16263, 49 lines) — TRUE LEAF, no
//     stack frame. ABI-ish: consumes whatever's live in r4/r5/r6/r7 at the call site (channel index
//     value, pitch/velocity value, and two caller locals r6/r7 that are NOT freshly set by the
//     caller — they're the outer function's OWN a2/a3-slot locals, still resident in the shared
//     Core register file at call time, exactly the MIPS ABI's "whatever's in a2/a3" convention).
//     Looks up an instrument/tone-table entry (byte @ +23832 indexed by channel) — if it's not
//     exactly 1, writes -1 (0xFFFFFFFF) into a shared "voice select result" local and returns; else
//     compares the pitch value against a lower bound (+23770) and, if in range, looks up 3 pointer-
//     sized fields from a table indexed by tone (+23632/+23568/+23704 stride 4) and writes them
//     into the SAME scratch fields channelVoiceRegisterWrite()'s pipeline reads right after this
//     call returns (+23780/+23772/+23784, plus 3 status bytes at +23801/+23806/+23807). This is
//     effectively "resolve tone->sample-bank pointers into scratch before the pan/volume compute."
//   Both kept register-literal (c->r[] direct, goto/labels named after guest addresses) — same
//   rationale as the bit4/6 leaves above: this is dense fixed-point arithmetic with re-converging
//   branches (L_80095854/L_800958EC/L_80095970 are each reached from two paths); restructuring by
//   hand risks a silent operand-order or shift-amount error that only an SBS run would catch, and
//   this wave is explicitly UNWIRED/pre-gate. A future wiring pass MUST do the line-by-line verify
//   docs/fleet-workflow.md §9 requires before registering these as overrides.
//
// STATUS (2026-07-10 wiring pass — supersedes the draft-status notes above): 8 addresses WIRED via
// engine_set_override_main + VERIFIED (SBS-full 0-diff through f9030; MIRROR_VERIFY 23k armed tick
// subtrees byte-compared, 0 mismatches): frameTick, seqChannelDispatch, channelPitchSelectDispatch,
// channelNoteInit, channelKeyEventScan, channelKeyRegisterMerge, channelVoiceRegisterWrite,
// channelVoiceSelectPrep. 5 leaves DELIBERATELY UNWIRED (never fired in any run — honest-gate rule;
// seqChannelDispatch routes their bits to the substrate via rec_dispatch): channelReleaseClear,
// channelStopFlagSet, channelPitchSlideTick, channelEnvelopeRampTick, channelVolumeSnapshot.
// 5 real bugs found+fixed at the §9 re-verify — full list in docs/findings/audio.md ("Sequencer
// cluster wiring", 2026-07-10). NOTE: SBS diff_mode skips the per-vblank audio block on BOTH cores,
// so the tick path is UNREACHABLE under any SBS config — PSXPORT_MIRROR_VERIFY=0x800909C0 in a
// normal run is the byte-verifier for this cluster's tick subtree.
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

  // --- 2026-07-10 wave: the remaining bit4/5/6/7/2 leaves + their own small callees ---
  void channelPitchSlideTick();       // 0x80090E40 — MEDIUM confidence, portamento ramp (bit4/5)
  void channelEnvelopeRampTick();     // 0x80092080 — MEDIUM confidence, ADSR ramp (bit6/7), true leaf
  void channelNoteInit();             // 0x80091970 — MEDIUM confidence, note retrigger (bit2)
  void channelVolumeSnapshot();       // 0x80095A9C — HIGH confidence, true leaf
  void channelKeyEventScan();         // 0x80095B90 — LOW-MEDIUM confidence, true-leaf-ish (1 call)
  void channelKeyRegisterMerge();     // 0x80094B50 — MEDIUM confidence, true leaf

  // --- 2026-07-10 (2nd wave): the SPU voice-register write leaf channelPitchSlideTick() still
  //     rec_dispatch()es to, plus its own callee. See header comment for RE detail / confidence.
  void channelVoiceRegisterWrite();   // 0x80095530 — LOW-MEDIUM confidence, ~320-line KON/pan loop
  void channelVoiceSelectPrep();      // 0x800962B0 — LOW-MEDIUM confidence, true leaf

  // --- 2026-07-17 wave: six further per-channel leaves (see sequencer.cpp for RE detail) ---
  void channelStreamAccumulate();     // 0x80090160 — true leaf, VLQ stream read into +136
  void channelVoiceKeyOn();           // 0x80091810 — true leaf, key-on flag/status init
  void channelToneRecordCopy();       // 0x80092310 — frame (sp-32), 6B+u16 voice-record copy
  void channelToneRecordCopyWide();   // 0x80092420 — frame (sp-32), 14B+4xu16 voice-record copy
  void voiceAllocateOrSteal();        // 0x80094150 — true leaf, LRU voice-steal alloc
  void channelNotePeriodCompute();    // 0x80094474 — true leaf, note->SPU-period compute

  // 2026-07-10 wiring pass (frontier): registers every method above into the process-global
  // g_override[] table via engine_set_override_main (runtime/recomp/override_registry.cpp) —
  // oracle-gated (core B / psx_fallback always runs the gen_func_* body; core A runs native). This
  // is the SAME mechanism rec_dispatch's main_dispatch() ultimately reaches for MAIN-shard
  // addresses, and ALSO the mechanism direct in-body calls like `func_800910F0(c)` inside
  // gen_func_80090BD0 reach — one registration covers both call paths. See sequencer.cpp for the
  // per-address wiring list / bottom-up order (leaves first, then seqChannelDispatch, then
  // frameTick) and docs/fleet-workflow.md §9 for the re-verify-before-wire discipline this pass
  // followed.
  void registerOverrides();
};
