// game/audio/sequencer.cpp — Sequencer method bodies. See sequencer.h for the RE contract.

#include "audio/sequencer.h"
#include "core.h"

#define SEQ_USER_CB    0x800AC430u   // DAT_800ac430 — optional user callback fn-ptr
#define SEQ_TICK_FN    0x800AC42Cu   // DAT_800ac42c — *SsSeqCalled fn-ptr (0x80090BD0 today)

// SsSeqCalled cluster globals (base 32784u<<16 = 0x80100000; offsets are the exact decimal
// immediates in generated/shard_3.c's gen_func_80090BD0 — see sequencer.h header note on the
// earlier pass's incorrect 0x8010CCxx/0x80109Exx transcription).
#define SEQ_REENTRY_FLAG   0x80104C24u   // guard: 1 while SsSeqCalled is running (re-entry no-op)
#define SEQ_ACTIVE_MASK    0x80104C28u   // bit i set => sequence i is active
#define SEQ_PTR_ARRAY      0x80104C30u   // 4-byte-stride array of per-sequence channel-table bases
#define SEQ_COUNT          0x801054B0u   // s16 — number of sequence slots (loop bound, <=7 seen live)
#define SEQ_CHAN_COUNT     0x801054B2u   // s16 — number of channels per sequence (<=15 seen live)
#define SEQ_PREP_FN        0x800931C0u   // one-shot prep call before the seq loop (input_dispatch_931c0)

#define CH_FLAGS   152u   // per-channel record: bitfield tested/cleared by SsSeqCalled + leaves
#define CH_STRIDE  176u

// 2026-07-10 wave — globals touched by the bit4/5/6/7/2 leaves (all same 0x80100000 base as the
// SEQ_* cluster above; offsets are the exact decimal immediates in each gen body, see sequencer.h
// header for the per-leaf confidence notes).
#define SEQ_KEYSCAN_VOICE_MASK  0x800AC3F4u   // 32779u<<16-15372 — hw voice-active bitmask (SPU)
#define SEQ_KEYSCAN_COUNT       0x80105CECu   // s8  @ +23788 — channelKeyEventScan() loop bound
#define SEQ_KEYSCAN_TABLE       0x801054D8u   // s16 @ +21720, stride 56 — per-voice pitch table
#define SEQ_KEYSCAN_MATCH       0x80105D10u   // u16 @ +23824 — scratch: matched pitch value
#define SEQ_KEYMERGE_STATUS     21733u        // u8  offset into the stride-56 voice table (cleared)
#define SEQ_KON_LO              0x80105BF0u   // u16 @ +23536 — KON-style armed-mask low word
#define SEQ_KON_HI              0x80105BF2u   // u16 @ +23538 — KON-style armed-mask high word
#define SEQ_ACTIVE_VOICE_LO     0x801054B8u   // u16 @ +21688 — active-voice mask low word
#define SEQ_ACTIVE_VOICE_HI     0x801054BAu   // u16 @ +21690 — active-voice mask high word
#define SEQ_VOLSNAP_SCRATCH     0x80105D0Cu   // u16 @ +23820 — channelVolumeSnapshot() dead-write scratch

void rec_dispatch(Core*, uint32_t);
// cpu_div / rec_break already declared in core.h (included above).

// 0x800909C0 FUN_800909c0 — libsnd per-VBlank tick wrapper. WIDE-RE DRAFT, UNWIRED (see header).
void Sequencer::frameTick() {
  Core* c = core;
  uint32_t cb = c->mem_r32(SEQ_USER_CB);
  if (cb != 0u) rec_dispatch(c, cb);
  uint32_t seq = c->mem_r32(SEQ_TICK_FN);
  rec_dispatch(c, seq);
}

// 0x800910F0 — thin arg-repacking wrapper: sign-extend (seq,chan) to 32-bit and tail-dispatch the
// still-unowned pitch-table leaf 0x80091120. Faithful to gen_func_800910F0
// (generated/shard_6.c:15995). Guest-stack frame mirrored (sp-24, ra spilled at +16).
void Sequencer::channelPitchSelectDispatch() {
  Core* c = core;
  uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 24u;
  c->mem_w32(c->r[29] + 16u, c->r[31]);
  c->r[4] = (uint32_t)(int32_t)(int16_t)(uint16_t)c->r[4];
  c->r[5] = (uint32_t)(int32_t)(int16_t)(uint16_t)c->r[5];
  rec_dispatch(c, 0x80091120u);
  c->r[31] = c->mem_r32(c->r[29] + 16u);
  c->r[29] = sp0;
}

// 0x80091050 — "release"/note-off housekeeping: zeroes the per-channel status byte at +20, clears
// flags bit1 (value 2), and calls the still-unowned leaf 0x80095B90 with a0 = seq | (chan<<8)
// (sign-extended 16) — role of 0x80095B90 not confirmed (SPU key-off command builder, inferred
// from context). Faithful to gen_func_80091050 (generated/shard_5.c:15597). Guest-stack frame
// mirrored (sp-32, spill ra/s16/s17/s18 at their RE'd offsets).
void Sequencer::channelReleaseClear() {
  Core* c = core;
  uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 32u;
  c->mem_w32(c->r[29] + 24u, c->r[18]);
  uint32_t seqRaw = c->r[4];
  uint32_t chanRaw = c->r[5];
  int32_t  chan = (int32_t)(int16_t)(uint16_t)chanRaw;
  uint32_t chanStride = (uint32_t)(chan * 11) << 4;   // == chan*176, gen's shift-mul idiom
  c->mem_w32(c->r[29] + 16u, c->r[16]);
  // gen: a0 = seqRaw | (chanRaw<<8), sign-extended 16 — the combine uses the RAW incoming a0/a1
  // registers (not the sign-extended chan local), matching gen_func_80091050 exactly.
  uint32_t combined = (uint32_t)(int32_t)(int16_t)(uint16_t)(seqRaw | (chanRaw << 8));
  c->mem_w32(c->r[29] + 28u, c->r[31]);
  c->mem_w32(c->r[29] + 20u, c->r[17]);
  uint32_t seqPtrSlot = SEQ_PTR_ARRAY + (uint32_t)(((int32_t)(seqRaw << 16)) >> 14);  // seq*4
  uint32_t channelBase = c->mem_r32(seqPtrSlot) + chanStride;
  c->r[4] = combined;
  rec_dispatch(c, 0x80095B90u);
  c->mem_w8(channelBase + 20u, 0u);
  // gen re-derefs seqArrayPtr fresh here (redundant unless the callee above mutated it) — mirror
  // that re-read for strict register/memory faithfulness rather than reusing the cached pointer.
  channelBase = c->mem_r32(seqPtrSlot) + chanStride;
  uint32_t flags = c->mem_r32(channelBase + CH_FLAGS) & ~2u;
  c->mem_w32(channelBase + CH_FLAGS, flags);
  c->r[31] = c->mem_r32(c->r[29] + 28u);
  c->r[18] = c->mem_r32(c->r[29] + 24u);
  c->r[17] = c->mem_r32(c->r[29] + 20u);
  c->r[16] = c->mem_r32(c->r[29] + 16u);
  c->r[29] = sp0;
}

// 0x80091910 — sets the per-channel status byte at +20 to 1, clears flags bit3 (value 8). A true
// leaf: no stack frame, no sub-calls in the gen body (generated/shard_3.c:21635). Prior doc pass's
// "toggles a stop bit + a busy flag" summary matches this shape (busy=+20, stop=bit3).
void Sequencer::channelStopFlagSet() {
  Core* c = core;
  uint32_t seqRaw = c->r[4];
  uint32_t chanRaw = c->r[5];
  int32_t  chan = (int32_t)(int16_t)(uint16_t)chanRaw;
  uint32_t chanStride = (uint32_t)(chan * 11) << 4;
  uint32_t seqPtrSlot = SEQ_PTR_ARRAY + (uint32_t)(((int32_t)(seqRaw << 16)) >> 14);
  uint32_t seqBasePtr = c->mem_r32(seqPtrSlot);
  c->mem_w8(seqBasePtr + chanStride + 20u, 1u);
  uint32_t channelBase = c->mem_r32(seqPtrSlot) + chanStride;  // re-derefed, mirrors gen
  uint32_t flags = c->mem_r32(channelBase + CH_FLAGS) & ~8u;
  c->mem_w32(channelBase + CH_FLAGS, flags);
}

// 0x80090BD0 SsSeqCalled — the per-VBlank sequence/channel scheduler. Faithful to
// gen_func_80090BD0 (generated/shard_3.c:21497). See sequencer.h header for the full bit-to-leaf
// map and confidence notes. Guest-stack frame MIRRORED (sp-56, spill ra/s0-s7 at their RE'd
// offsets) — including on the reentrancy-guard early-exit path, since the gen prologue's spills
// are unconditional (they execute before the guard test in the gen body).
void Sequencer::seqChannelDispatch() {
  Core* c = core;
  uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 56u;
  c->mem_w32(c->r[29] + 52u, c->r[31]);
  c->mem_w32(c->r[29] + 48u, c->r[30]);
  c->mem_w32(c->r[29] + 44u, c->r[23]);
  c->mem_w32(c->r[29] + 40u, c->r[22]);
  c->mem_w32(c->r[29] + 36u, c->r[21]);
  c->mem_w32(c->r[29] + 32u, c->r[20]);
  c->mem_w32(c->r[29] + 28u, c->r[19]);
  c->mem_w32(c->r[29] + 24u, c->r[18]);
  c->mem_w32(c->r[29] + 20u, c->r[17]);
  c->mem_w32(c->r[29] + 16u, c->r[16]);

  if (c->mem_r32(SEQ_REENTRY_FLAG) != 1u) {
    c->mem_w32(SEQ_REENTRY_FLAG, 1u);
    rec_dispatch(c, SEQ_PREP_FN);   // 0x800931C0 input_dispatch_931c0 — still-unwired by us

    int32_t seqCount = c->mem_r16s(SEQ_COUNT);
    if (seqCount > 0) {
      uint32_t seqArrayPtr = SEQ_PTR_ARRAY;
      for (int32_t seq = 0; seq < seqCount; seq++, seqArrayPtr += 4u, seqCount = c->mem_r16s(SEQ_COUNT)) {
        if ((c->mem_r32(SEQ_ACTIVE_MASK) & (1u << (uint32_t)(seq & 31))) == 0u) continue;

        int32_t chanCount = c->mem_r16s(SEQ_CHAN_COUNT);
        for (int32_t chan = 0; chan < chanCount; chan++, chanCount = c->mem_r16s(SEQ_CHAN_COUNT)) {
          // gen re-derefs seqArrayPtr fresh before EVERY bit test (not cached across leaf calls,
          // in case a leaf mutates the seq base-pointer table) — mirror that literally.
          auto chBase = [&]() -> uint32_t { return c->mem_r32(seqArrayPtr) + (uint32_t)chan * CH_STRIDE; };
          uint32_t s = (uint32_t)seq, ch = (uint32_t)chan;

          if (c->mem_r32(chBase() + CH_FLAGS) & 0x01u) { c->r[4] = s; c->r[5] = ch; channelPitchSelectDispatch(); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x10u) { c->r[4] = s; c->r[5] = ch; channelPitchSlideTick(); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x20u) { c->r[4] = s; c->r[5] = ch; channelPitchSlideTick(); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x40u) { c->r[4] = s; c->r[5] = ch; channelEnvelopeRampTick(); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x80u) { c->r[4] = s; c->r[5] = ch; channelEnvelopeRampTick(); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x02u) { c->r[4] = s; c->r[5] = ch; channelReleaseClear(); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x08u) { c->r[4] = s; c->r[5] = ch; channelStopFlagSet(); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x04u) {
            c->r[4] = s; c->r[5] = ch;
            channelNoteInit();
            c->mem_w32(chBase() + CH_FLAGS, 0u);   // SsSeqCalled's OWN post-call full clear (native)
          }
        }
      }
    }
    c->mem_w32(SEQ_REENTRY_FLAG, 0u);
  }

  c->r[31] = c->mem_r32(c->r[29] + 52u);
  c->r[30] = c->mem_r32(c->r[29] + 48u);
  c->r[23] = c->mem_r32(c->r[29] + 44u);
  c->r[22] = c->mem_r32(c->r[29] + 40u);
  c->r[21] = c->mem_r32(c->r[29] + 36u);
  c->r[20] = c->mem_r32(c->r[29] + 32u);
  c->r[19] = c->mem_r32(c->r[29] + 28u);
  c->r[18] = c->mem_r32(c->r[29] + 24u);
  c->r[17] = c->mem_r32(c->r[29] + 20u);
  c->r[16] = c->mem_r32(c->r[29] + 16u);
  c->r[29] = sp0;
}

// ============================================================================
// 2026-07-10 wide-RE wave — the remaining SsSeqCalled leaves (bit4/5, bit6/7, bit2) + their own
// small callees. See sequencer.h header for the summary / confidence table. All UNWIRED.
// ============================================================================

// 0x80095A9C channelVolumeSnapshot — true leaf (no stack frame). Faithful to gen_func_80095A9C
// (generated/shard_1.c:18517; the code past its first `return` is an unreachable duplicate block,
// a shard-grouping artifact like others documented in this file — not ported). ABI: a0(r4)=combined
// (seq | chan<<8, low 16 bits meaningful), a1(r5)=&outL, a2(r6)=&outR. Reads channelBase+88/+90
// (u16 each) into *outL/*outR. Also has a genuine but functionally dead side-effect: it stamps the
// raw combined arg to a scratch global (SEQ_VOLSNAP_SCRATCH) that the gen body itself never reads
// back with effect (the reload 2 lines later is discarded, part of the same dead tail).
void Sequencer::channelVolumeSnapshot() {
  Core* c = core;
  uint32_t combined = c->r[4];
  uint32_t outLPtr = c->r[5];
  uint32_t outRPtr = c->r[6];

  uint32_t seqLow = combined & 0xFFu;
  uint32_t seqPtrSlot = SEQ_PTR_ARRAY + (seqLow << 2);
  uint32_t seqBasePtr = c->mem_r32(seqPtrSlot);

  c->mem_w16(SEQ_VOLSNAP_SCRATCH, (uint16_t)combined);   // dead write, mirrored for fidelity

  int32_t chan = (int32_t)((int32_t)(combined & 0xFF00u) >> 8);
  uint32_t chanStride = (uint32_t)(chan * 11) << 4;       // chan*176
  uint32_t channelBase = seqBasePtr + chanStride;

  c->mem_w16(outLPtr, c->mem_r16(channelBase + 88u));
  c->mem_w16(outRPtr, c->mem_r16(channelBase + 90u));
}

// 0x80094B50 channelKeyRegisterMerge — true leaf (no stack frame). Faithful to gen_func_80094B50
// (generated/shard_3.c:22039). No ABI args — reads its input from SEQ_KEYSCAN_MATCH (the scratch
// value channelKeyEventScan() just stamped there). Builds a KON-style 1-bit-set lo/hi word pair
// from the match value (0-15 -> lo bit, 16-31 -> hi bit), clears a per-voice status byte in the
// stride-56 voice table, ORs the new bit into the KON lo/hi words, and clears the SAME bit from the
// active-voice lo/hi mask (classic "arm this voice for key-on, drop it from the active set" idiom).
void Sequencer::channelKeyRegisterMerge() {
  Core* c = core;
  uint32_t value = c->mem_r16(SEQ_KEYSCAN_MATCH);

  uint32_t bitLo = 0, bitHi = 0;
  if (value < 16u) {
    bitLo = 1u << (value & 31u);
  } else {
    bitHi = 1u << ((value - 16u) & 31u);
  }

  uint32_t tableOff = (uint32_t)(value * 7u) << 3;   // value*56 (stride-56 idiom, same as key-scan table)
  uint32_t tableBase = 0x80100000u + tableOff;
  c->mem_w8(tableBase + SEQ_KEYMERGE_STATUS, 0u);
  c->mem_w16(tableBase + 21708u, 0u);
  c->mem_w16(tableBase + 21704u, 0u);

  uint32_t konLo = c->mem_r16(SEQ_KON_LO) | bitLo;
  c->mem_w16(SEQ_KON_LO, (uint16_t)konLo);
  uint32_t activeLo = c->mem_r16(SEQ_ACTIVE_VOICE_LO) & ~konLo;
  c->mem_w16(SEQ_ACTIVE_VOICE_LO, (uint16_t)activeLo);

  uint32_t konHi = c->mem_r16(SEQ_KON_HI) | bitHi;
  c->mem_w16(SEQ_KON_HI, (uint16_t)konHi);
  uint32_t activeHi = c->mem_r16(SEQ_ACTIVE_VOICE_HI) & ~konHi;
  c->mem_w16(SEQ_ACTIVE_VOICE_HI, (uint16_t)activeHi);
}

// 0x80095B90 channelKeyEventScan — stack frame present (sp-32, spill ra/s16/s17/s18). Faithful to
// gen_func_80095B90 (generated/shard_2.c:13170). ABI: a0(r4)=combined seq (low 16 bits are the
// "target pitch" value to match — see caller channelNoteInit()). LOW-MEDIUM confidence: semantic
// role of the hw-voice-bitmask scan is inherited-uncertain from the prior wave's note on this
// address (never independently confirmed against a live SPU dump); the CONTROL FLOW transcription
// below is exact. Scans voices 0..SEQ_KEYSCAN_COUNT-1: skip any voice whose bit is set in the hw
// voice-active bitmask (SEQ_KEYSCAN_VOICE_MASK); for the rest, compare the per-voice pitch table
// entry (SEQ_KEYSCAN_TABLE, stride 56) against the target; on a match, stamp the value to
// SEQ_KEYSCAN_MATCH and call channelKeyRegisterMerge().
void Sequencer::channelKeyEventScan() {
  Core* c = core;
  uint32_t sp0 = c->r[29];
  int8_t count = (int8_t)c->mem_r8(SEQ_KEYSCAN_COUNT);
  c->r[29] = sp0 - 32u;
  c->mem_w32(c->r[29] + 28u, c->r[31]);
  c->mem_w32(c->r[29] + 24u, c->r[18]);
  c->mem_w32(c->r[29] + 20u, c->r[17]);
  if (count > 0) {
    int32_t target = (int32_t)(int16_t)(uint16_t)c->r[4];
    for (int32_t i = 0; i < (int32_t)(int8_t)c->mem_r8(SEQ_KEYSCAN_COUNT); i++) {
      uint32_t voiceBit = 1u << (uint32_t)(i & 31);
      if ((c->mem_r32(SEQ_KEYSCAN_VOICE_MASK) & voiceBit) != 0u) continue;   // voice busy, skip
      uint32_t tableOff = (uint32_t)(i * 7) << 3;                            // i*56
      int32_t tableVal = (int32_t)(int16_t)c->mem_r16(SEQ_KEYSCAN_TABLE + tableOff);
      if (tableVal != target) continue;
      c->mem_w16(SEQ_KEYSCAN_MATCH, (uint16_t)tableVal);
      channelKeyRegisterMerge();
    }
  }
  c->r[31] = c->mem_r32(c->r[29] + 28u);
  c->r[18] = c->mem_r32(c->r[29] + 24u);
  c->r[17] = c->mem_r32(c->r[29] + 20u);
  c->r[29] = sp0;
}

// 0x80090E40 channelPitchSlideTick — pitch-slide/portamento per-tick interpolator (SsSeqCalled
// flags bit4 AND bit5 route here). Faithful to gen_func_80090E40 (generated/shard_4.c:15017).
// Guest-stack frame mirrored (sp-56, spill ra/s0-s5 at their RE'd offsets -- MIPS s0..s5 here are
// r16..r21). ABI: a0(r4)=seq, a1(r5)=chan.
//
// Kept register-literal with goto/labels named after the guest addresses rather than restructured
// into if/else: the gen body has several branches that re-converge on shared tails (L_80090FF8 is
// reached from two different call sites; L_8009100C/L_80091010 are reached from three), and
// re-deriving that fallthrough order by hand risks a silent logic change. This is the same
// transcription style CLAUDE.md's "mirror the guest stack" section asks for -- the algorithm
// executing against the same machine state, not a hand-simplified paraphrase.
//
// Shape: increments a ramp counter (+160); once it exceeds the limit (+156), clears flags bit4
// (0x10, "portamento active") and finishes (snapshot channelVolumeSnapshot() into +92/+94). While
// still ramping, computes delta = (rate(+72) * counter) / limit(+156) - base(+74) via cpu_div (incl.
// the div-by-zero / INT_MIN/-1 overflow rec_break traps, faithfully reproduced even though rec_break
// itself is a no-op logger), applies it to the +88/+90 snapshot pair (channelVolumeSnapshot()),
// clamps each to [0,127], writes the clamped pair out via the still-unowned SPU leaf 0x80095530
// (MAPPED, not drafted -- ~320 lines, a KON/pan-table write loop, see docs/engine_re.md), then
// clears bit4 early too if BOTH clamped values saturated at the SAME extreme (0 or 127).
void Sequencer::channelPitchSlideTick() {
  Core* c = core;
  uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 56u;
  c->r[2] = c->r[4] << 16;
  c->r[3] = SEQ_PTR_ARRAY;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 14);
  c->r[8] = c->r[2] + c->r[3];
  c->r[3] = c->r[5] << 16;
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
  c->r[2] = c->r[3] << 1;
  c->r[2] = c->r[2] + c->r[3];
  c->r[2] = c->r[2] << 2;
  c->r[2] = c->r[2] - c->r[3];
  c->r[7] = c->r[2] << 4;
  c->mem_w32(c->r[29] + 44u, c->r[21]);
  c->r[21] = c->r[4];
  c->mem_w32(c->r[29] + 48u, c->r[31]);
  c->mem_w32(c->r[29] + 40u, c->r[20]);
  c->mem_w32(c->r[29] + 36u, c->r[19]);
  c->mem_w32(c->r[29] + 32u, c->r[18]);
  c->mem_w32(c->r[29] + 28u, c->r[17]);
  c->mem_w32(c->r[29] + 24u, c->r[16]);
  c->r[3] = c->mem_r32(c->r[8] + 0u);
  c->r[20] = c->r[5];
  c->r[18] = c->r[3] + c->r[7];
  c->r[2] = c->mem_r32(c->r[18] + 160u);
  c->r[3] = c->mem_r32(c->r[18] + 156u);
  c->r[6] = c->r[2] + 1u;
  {
    int _t = ((int32_t)c->r[3] < (int32_t)c->r[6]);
    c->mem_w32(c->r[18] + 160u, c->r[6]);
    if (!_t) goto L_80090EC4;
  }
  c->r[2] = c->mem_r32(c->r[8] + 0u);
  c->r[2] = c->r[7] + c->r[2];
  goto L_80090FF8;

L_80090EC4:
  c->r[2] = (uint32_t)c->mem_r16s(c->r[18] + 72u);
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[6];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[2] = c->lo;
  cpu_div(c, c->r[2], c->r[3]);
  if (c->r[3] == 0u) rec_break(c, 7168u);
  if (c->r[3] == 0xFFFFFFFFu && c->r[2] == 0x80000000u) rec_break(c, 6144u);
  c->r[16] = c->lo;
  c->r[2] = (uint32_t)c->mem_r16s(c->r[18] + 74u);
  c->r[3] = (uint32_t)c->mem_r16(c->r[18] + 74u);
  c->r[16] = c->r[16] - c->r[2];
  {
    int _t = (c->r[16] == 0u);
    if (_t) goto L_80091008;
  }
  c->r[2] = c->r[4] | (c->r[5] << 8);
  c->r[2] = c->r[2] << 16;
  c->r[19] = (uint32_t)((int32_t)c->r[2] >> 16);
  c->r[4] = c->r[19];
  c->r[5] = c->r[29] + 16u;
  c->r[6] = c->r[29] + 18u;
  c->r[2] = c->r[3] + c->r[16];
  c->mem_w16(c->r[18] + 74u, (uint16_t)c->r[2]);
  channelVolumeSnapshot();
  c->r[2] = (uint32_t)c->mem_r16(c->r[29] + 16u);
  c->r[17] = c->r[2] + c->r[16];
  { int _t = ((int32_t)c->r[17] < 128); if (_t) goto L_80090F5C; }
  c->r[17] = 127u;
L_80090F5C:
  { int _t = ((int32_t)c->r[17] >= 0); if (_t) goto L_80090F68; }
  c->r[17] = 0u;
L_80090F68:
  c->r[2] = (uint32_t)c->mem_r16(c->r[29] + 18u);
  c->r[16] = c->r[2] + c->r[16];
  { int _t = ((int32_t)c->r[16] < 128); if (_t) goto L_80090F84; }
  c->r[16] = 127u;
L_80090F84:
  { int _t = ((int32_t)c->r[16] >= 0); if (_t) goto L_80090F90; }
  c->r[16] = 0u;
L_80090F90:
  c->r[5] = c->r[17] & 65535u;
  c->r[6] = c->r[16] & 65535u;
  c->r[4] = c->r[19];
  c->r[7] = 1u;
  rec_dispatch(c, 0x80095530u);   // MAPPED, not drafted (see comment above)
  {
    int _t = (c->r[17] != 127u);
    if (_t) goto L_80090FB4;
  }
  { int _t = (c->r[16] == c->r[17]); c->r[4] = c->r[21] << 16; if (_t) goto L_80090FC8; }
L_80090FB4:
  { int _t = (c->r[17] != 0u); c->r[4] = c->r[20] << 8; if (_t) goto L_8009100C; }
  { int _t = (c->r[16] != 0u); c->r[4] = c->r[21] | c->r[4]; if (_t) goto L_80091010; }
  c->r[4] = c->r[21] << 16;
L_80090FC8:
  c->r[4] = (uint32_t)((int32_t)c->r[4] >> 14);
  c->r[3] = c->r[20] << 16;
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
  c->r[2] = c->r[3] << 1;
  c->r[2] = c->r[2] + c->r[3];
  c->r[2] = c->r[2] << 2;
  c->r[2] = c->r[2] - c->r[3];
  c->r[3] = 0x80100000u + c->r[4];
  c->r[3] = c->mem_r32(c->r[3] + 19504u);
  c->r[2] = c->r[2] << 4;
  c->r[2] = c->r[2] + c->r[3];
L_80090FF8:
  c->r[3] = c->mem_r32(c->r[2] + 152u);
  c->r[3] = c->r[3] & ~0x10u;
  c->mem_w32(c->r[2] + 152u, c->r[3]);
L_80091008:
  c->r[4] = c->r[20] << 8;
L_8009100C:
  c->r[4] = c->r[21] | c->r[4];
L_80091010:
  c->r[4] = (uint32_t)((int32_t)(c->r[4] << 16) >> 16);
  c->r[5] = c->r[18] + 92u;
  c->r[6] = c->r[18] + 94u;
  channelVolumeSnapshot();
  c->r[31] = c->mem_r32(c->r[29] + 48u);
  c->r[21] = c->mem_r32(c->r[29] + 44u);
  c->r[20] = c->mem_r32(c->r[29] + 40u);
  c->r[19] = c->mem_r32(c->r[29] + 36u);
  c->r[18] = c->mem_r32(c->r[29] + 32u);
  c->r[17] = c->mem_r32(c->r[29] + 28u);
  c->r[16] = c->mem_r32(c->r[29] + 24u);
  c->r[29] = sp0;
}

// 0x80092080 channelEnvelopeRampTick — ADSR/envelope ramp (SsSeqCalled flags bit6 AND bit7 route
// here). Faithful to gen_func_80092080 (generated/shard_1.c:17775). TRUE LEAF in the gen body (no
// `addiu sp,-N` at all) -- no stack frame to mirror. ABI: a0(r4)=seq, a1(r5)=chan.
//
// Kept register-literal with goto/labels for the same reason as channelPitchSlideTick (several
// branches re-converge on shared tails: L_800921B0 from 4 different sites, L_80092284 from 2). The
// gen body's trailing `func_800922A0(c)` after the real `return` is the usual shard-grouping
// dead-tail artifact (unreachable, not ported).
//
// Shape: decrements a counter (+168); if it just went negative (was already 0), clear bit6 and jump
// straight to the shared bit7-clear tail (envelope already fully ramped). Otherwise, if rate(+78) is
// positive, cpu_div (counter-1)*1 / rate — WRONG, see body: actually divides the just-decremented
// counter by rate, and only on an EXACT division (remainder==0) nudges the envelope value (+148) by
// +/-1 toward the target (+172); a NONZERO remainder skips everything below (including the output-
// level write AND the bit-clear tail) and returns immediately. If rate<=0, a fixed +/-|rate| step
// against +80... no: uses rate itself (<=0) added/subtracted, clamped so it doesn't overshoot the
// target. Then (unless the early nonzero-remainder return already fired) computes a new output level
// (+84) via cpu_divu of (envelope*10) by (a cluster-adjacent scale field *15), clamping to 1 if the
// result is 0 or has bit15 set. Finally clears bit6+bit7 if the counter is now exactly 0 OR the
// envelope value equals the target (whether reached via the div-remainder-zero path above, or was
// already equal); otherwise leaves both set for next tick.
void Sequencer::channelEnvelopeRampTick() {
  Core* c = core;
  c->r[2] = c->r[4] << 16;
  c->r[3] = SEQ_PTR_ARRAY;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 14);
  c->r[9] = c->r[2] + c->r[3];
  c->r[3] = c->r[5] << 16;
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
  c->r[2] = c->r[3] << 1;
  c->r[2] = c->r[2] + c->r[3];
  c->r[2] = c->r[2] << 2;
  c->r[2] = c->r[2] - c->r[3];
  c->r[8] = c->r[2] << 4;                 // chanStride
  c->r[3] = c->mem_r32(c->r[9] + 0u);
  c->r[6] = c->r[4];
  c->r[7] = c->r[3] + c->r[8];            // channelBase
  c->r[2] = c->mem_r32(c->r[7] + 168u);
  c->r[2] = c->r[2] - 1u;
  {
    int _t = ((int32_t)c->r[2] >= 0);
    c->mem_w32(c->r[7] + 168u, c->r[2]);
    if (_t) goto L_800920F8;
  }
  // counter just went negative -- envelope already finished; clear bit6 only, then finalize.
  c->r[3] = c->mem_r32(c->r[9] + 0u);
  c->r[3] = c->r[8] + c->r[3];
  c->r[2] = c->mem_r32(c->r[3] + 152u);
  c->r[4] = ~0x40u;
  c->r[2] = c->r[2] & c->r[4];
  c->mem_w32(c->r[3] + 152u, c->r[2]);
  c->r[3] = c->mem_r32(c->r[9] + 0u);
  c->r[3] = c->r[8] + c->r[3];
  goto L_80092284;

L_800920F8:
  c->r[4] = (uint32_t)c->mem_r16s(c->r[7] + 78u);
  { int _t = ((int32_t)c->r[4] <= 0); if (_t) goto L_80092168; }
  cpu_div(c, c->r[2], c->r[4]);
  if (c->r[4] == 0u) rec_break(c, 7168u);
  if (c->r[4] == 0xFFFFFFFFu && c->r[2] == 0x80000000u) rec_break(c, 6144u);
  c->r[2] = c->hi;
  { int _t = (c->r[2] != 0u); if (_t) return; }   // nonzero remainder -- skip EVERYTHING below, return
  c->r[3] = c->mem_r32(c->r[7] + 148u);            // cur
  c->r[4] = c->mem_r32(c->r[7] + 172u);            // target
  c->r[2] = (uint32_t)(c->r[4] < c->r[3]);          // target < cur ?
  {
    int _t = (c->r[2] != 0u);
    c->r[2] = c->r[3] - 1u;
    if (_t) goto L_80092160;
  }
  c->r[2] = (uint32_t)(c->r[3] < c->r[4]);          // cur < target ?
  {
    int _t = (c->r[2] == 0u);
    c->r[2] = c->r[3] + 1u;
    if (_t) goto L_800921B0;                        // cur == target already: no write
  }
L_80092160:
  c->mem_w32(c->r[7] + 148u, c->r[2]);
  goto L_800921B0;

L_80092168:
  c->r[3] = c->mem_r32(c->r[7] + 148u);             // cur
  c->r[8] = c->mem_r32(c->r[7] + 172u);             // target
  c->r[2] = (uint32_t)(c->r[8] < c->r[3]);           // target < cur ?
  {
    int _t = (c->r[2] == 0u);
    c->r[2] = c->r[3] + c->r[4];                     // cur + rate  (rate<=0 here -> decreases)
    if (_t) goto L_8009218C;
  }
  c->mem_w32(c->r[7] + 148u, c->r[2]);
  c->r[2] = (uint32_t)(c->r[2] < c->r[8]);            // undershot target?
  goto L_800921A4;
L_8009218C:
  c->r[2] = (uint32_t)(c->r[3] < c->r[8]);            // cur < target ?
  {
    int _t = (c->r[2] == 0u);
    c->r[2] = c->r[3] - c->r[4];                      // cur - rate  (rate<=0 here -> increases)
    if (_t) goto L_800921B0;                          // cur == target already: no write
  }
  c->r[8] = c->mem_r32(c->r[7] + 172u);
  c->mem_w32(c->r[7] + 148u, c->r[2]);
  c->r[2] = (uint32_t)(c->r[8] < c->r[2]);             // overshot target?
L_800921A4:
  { int _t = (c->r[2] == 0u); if (_t) goto L_800921B0; }
  c->mem_w32(c->r[7] + 148u, c->r[8]);                 // clamp to target exactly

L_800921B0:
  c->r[2] = (uint32_t)c->mem_r16s(c->r[7] + 80u);
  c->r[3] = c->mem_r32(c->r[7] + 148u);
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[3];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[4] = c->mem_r32(0x80100000u + 19500u);   // cluster-adjacent scale field
  c->r[2] = c->lo;
  c->r[3] = c->r[2] << 2;
  c->r[3] = c->r[3] + c->r[2];
  c->r[3] = c->r[3] << 1;                        // r3 = lo*10
  c->r[2] = c->r[4] << 4;
  c->r[2] = c->r[2] - c->r[4];
  c->r[2] = c->r[2] << 2;                        // r2 = scale*15
  cpu_divu(c, c->r[3], c->r[2]);
  if (c->r[2] == 0u) rec_break(c, 7168u);
  c->r[3] = c->lo;
  c->mem_w16(c->r[7] + 84u, (uint16_t)c->r[3]);
  c->r[3] = c->r[3] << 16;
  { int _t = ((int32_t)c->r[3] > 0); c->r[2] = 1u; if (_t) goto L_8009220C; }
  c->mem_w16(c->r[7] + 84u, (uint16_t)c->r[2]);
L_8009220C:
  c->r[2] = c->mem_r32(c->r[7] + 168u);
  { int _t = (c->r[2] == 0u); if (_t) goto L_80092230; }
  c->r[3] = c->mem_r32(c->r[7] + 148u);
  c->r[2] = c->mem_r32(c->r[7] + 172u);
  { int _t = (c->r[3] != c->r[2]); if (_t) return; }   // still ramping, not at target -- leave bits set
L_80092230:
  c->r[6] = c->r[6] << 16;
  c->r[2] = SEQ_PTR_ARRAY;
  c->r[6] = (uint32_t)((int32_t)c->r[6] >> 14);
  c->r[6] = c->r[6] + c->r[2];
  c->r[2] = c->r[5] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  c->r[3] = c->r[2] << 1;
  c->r[3] = c->r[3] + c->r[2];
  c->r[3] = c->r[3] << 2;
  c->r[3] = c->r[3] - c->r[2];
  c->r[5] = c->mem_r32(c->r[6] + 0u);
  c->r[3] = c->r[3] << 4;
  c->r[5] = c->r[3] + c->r[5];
  c->r[2] = c->mem_r32(c->r[5] + 152u);
  c->r[4] = ~0x40u;
  c->r[2] = c->r[2] & c->r[4];
  c->mem_w32(c->r[5] + 152u, c->r[2]);
  c->r[2] = c->mem_r32(c->r[6] + 0u);
  c->r[3] = c->r[3] + c->r[2];
L_80092284:
  c->r[2] = c->mem_r32(c->r[3] + 152u);
  c->r[4] = ~0x80u;
  c->r[2] = c->r[2] & c->r[4];
  c->mem_w32(c->r[3] + 152u, c->r[2]);
  // L_80092294: return
}

// 0x80091970 channelNoteInit — per-channel note retrigger (SsSeqCalled flags bit2 routes here).
// Faithful to gen_func_80091970 (generated/shard_4.c:15144). Guest-stack frame mirrored (sp-24,
// spill ra/s0 at their RE'd offsets). ABI: a0(r4)=seq, a1(r5)=chan.
//
// Mostly linear (unlike its siblings above): clears flags bits {0,1,3,10} (values 1/2/8/1024) via
// 4 separate read-modify-write ops the gen body re-derives channelBase for independently (it never
// caches the pointer across them) -- since nothing between these ops can mutate SEQ_PTR_ARRAY[seq]
// or `chan`, every re-derivation lands on the SAME address as the initial one, so this port reuses
// the single `channelBase` local rather than re-deriving 4 more times (byte-identical result, per
// this file's convention of only mirroring a fresh re-deref when a leaf call could have mutated the
// pointer in between -- see seqChannelDispatch's own comment on that point). Then sets bit2 (0x04),
// calls channelKeyEventScan() (0x80095B90) then the trivial global-clear leaf at 0x800931A0 (still
// MAPPED via rec_dispatch -- input_dispatch_931c0's neighbor, not this wave's target), zeroes ~14
// per-channel status bytes, reinits several numeric fields, and fills a 16-entry breakpoint-table
// pair (+39../+55..) plus a 16x u16 array (+96..+126, all set to 127).
void Sequencer::channelNoteInit() {
  Core* c = core;
  uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 24u;
  uint32_t seqPtrSlot = SEQ_PTR_ARRAY + (uint32_t)(((int32_t)(c->r[4] << 16)) >> 14);
  int32_t chan = (int32_t)(int16_t)(uint16_t)c->r[5];
  uint32_t chanStride = (uint32_t)(chan * 11) << 4;   // chan*176
  c->mem_w32(c->r[29] + 20u, c->r[31]);
  c->mem_w32(c->r[29] + 16u, c->r[16]);

  uint32_t seqBasePtr = c->mem_r32(seqPtrSlot);
  uint32_t channelBase = seqBasePtr + chanStride;   // r16 -- the target channel record

  c->mem_w32(channelBase + CH_FLAGS, c->mem_r32(channelBase + CH_FLAGS) & ~1u);      // clear bit0
  c->mem_w32(channelBase + CH_FLAGS, c->mem_r32(channelBase + CH_FLAGS) & ~2u);      // clear bit1
  c->mem_w32(channelBase + CH_FLAGS, c->mem_r32(channelBase + CH_FLAGS) & ~8u);      // clear bit3
  c->mem_w32(channelBase + CH_FLAGS, c->mem_r32(channelBase + CH_FLAGS) & ~1025u);   // clear bits{0,10}

  uint32_t combined = (uint32_t)(int32_t)(int16_t)(uint16_t)(c->r[4] | (c->r[5] << 8));
  c->mem_w32(channelBase + CH_FLAGS, c->mem_r32(channelBase + CH_FLAGS) | 4u);       // set bit2

  c->r[4] = combined;
  channelKeyEventScan();
  rec_dispatch(c, 0x800931A0u);   // input_dispatch_931c0's neighbor, not this wave's target (MAPPED)

  uint32_t f132 = c->mem_r32(channelBase + 132u);
  uint32_t f140 = c->mem_r32(channelBase + 140u);
  uint32_t f86  = c->mem_r16(channelBase + 86u);
  uint32_t f4   = c->mem_r32(channelBase + 4u);

  c->mem_w8(channelBase + 20u, 0u);
  c->mem_w32(channelBase + 136u, 0u);
  c->mem_w8(channelBase + 28u, 0u);
  c->mem_w8(channelBase + 24u, 0u);
  c->mem_w8(channelBase + 25u, 0u);
  c->mem_w8(channelBase + 30u, 0u);
  c->mem_w8(channelBase + 26u, 0u);
  c->mem_w8(channelBase + 27u, 0u);
  c->mem_w8(channelBase + 31u, 0u);
  c->mem_w8(channelBase + 23u, 0u);
  c->mem_w8(channelBase + 33u, 0u);
  c->mem_w8(channelBase + 28u, 0u);
  c->mem_w8(channelBase + 29u, 0u);
  c->mem_w8(channelBase + 21u, 0u);
  c->mem_w8(channelBase + 22u, 0u);
  c->mem_w32(channelBase + 144u, f132);
  c->mem_w32(channelBase + 148u, f140);
  c->mem_w16(channelBase + 84u, (uint16_t)f86);
  c->mem_w32(channelBase + 0u, f4);
  c->mem_w32(channelBase + 8u, f4);

  for (int32_t i = 0; i < 16; i++) {
    c->mem_w8(channelBase + 55u + (uint32_t)i, (uint8_t)i);
    c->mem_w8(channelBase + 39u + (uint32_t)i, 64u);
    c->mem_w16(channelBase + 96u + (uint32_t)i * 2u, 127u);
  }
  c->mem_w16(channelBase + 92u, 127u);
  c->mem_w16(channelBase + 94u, 127u);

  c->r[31] = c->mem_r32(c->r[29] + 20u);
  c->r[16] = c->mem_r32(c->r[29] + 16u);
  c->r[29] = sp0;
}

// 0x800962B0 channelVoiceSelectPrep — called mid-loop by channelVoiceRegisterWrite() (see below).
// Faithful to gen_func_800962B0 (generated/shard_5.c:16263). TRUE LEAF, no stack frame. Consumes
// whatever's live in r4/r5/r6/r7 at the call site (see sequencer.h header comment) rather than a
// named ABI — this is a direct register-literal transcription operating on the same shared Core
// register file as its caller, so caller-set r6/r7 (the outer function's own locals, never
// re-set here) carry through exactly as the guest MIPS ABI would deliver them.
//
// LOW-MEDIUM confidence: control flow is exact; field semantics (an instrument/tone-table lookup
// gating 3 pointer-table reads into channelVoiceRegisterWrite()'s scratch fields) are inferred
// from the surrounding SPU-register cluster, not confirmed against a live dump.
void Sequencer::channelVoiceSelectPrep() {
  Core* c = core;
  c->r[7] = c->r[4] + c->r[0];
  c->r[2] = c->r[7] & 65535u;
  c->r[2] = (uint32_t)(c->r[2] < 16u);
  {
    int _t = (c->r[2] == c->r[0]);
    c->r[8] = c->r[5] + c->r[0];
    if (_t) goto L_80096300;
  }
  c->r[2] = c->r[4] << 16;
  c->r[4] = (uint32_t)((int32_t)c->r[2] >> 16);
  c->r[3] = 0x80100000u + c->r[4];
  c->r[3] = (uint32_t)c->mem_r8(c->r[3] + 23832u);
  c->r[2] = c->r[0] + 1u;
  {
    int _t = (c->r[3] != c->r[2]);
    c->r[2] = (uint32_t)-1;
    if (_t) goto L_80096368;
  }
  c->r[3] = c->r[5] << 16;
  c->r[2] = 0x80100000u;
  c->r[2] = (uint32_t)(int16_t)c->mem_r16(c->r[2] + 23770u);
  c->r[6] = (uint32_t)((int32_t)c->r[3] >> 16);
  c->r[2] = (uint32_t)((int32_t)c->r[6] < (int32_t)c->r[2]);
  {
    int _t = (c->r[2] != c->r[0]);
    c->r[2] = c->r[4] << 2;
    if (_t) goto L_80096308;
  }
  c->r[2] = (uint32_t)-1;
  goto L_80096368;

L_80096308:
  c->r[3] = 0x80100000u + c->r[2];
  c->r[3] = c->mem_r32(c->r[3] + 23632u);
  c->r[5] = 0x80100000u + c->r[2];
  c->r[5] = c->mem_r32(c->r[5] + 23568u);
  c->r[1] = 0x80100000u + c->r[2];
  c->r[2] = c->mem_r32(c->r[1] + 23704u);
  c->r[4] = 0x80100000u + 23801u;
  c->mem_w8(c->r[4] + 0u, (uint8_t)c->r[7]);
  c->mem_w8(c->r[4] + 5u, (uint8_t)c->r[8]);
  c->mem_w32(0x80100000u + 23784u, c->r[2]);
  c->r[2] = c->r[6] << 4;
  c->r[2] = c->r[2] + c->r[5];
  c->mem_w32(0x80100000u + 23780u, c->r[3]);
  c->mem_w32(0x80100000u + 23772u, c->r[5]);
  c->r[3] = (uint32_t)c->mem_r8(c->r[2] + 8u);
  c->r[2] = c->r[0] + c->r[0];
  c->mem_w8(c->r[4] + 6u, (uint8_t)c->r[3]);
L_80096300:
  ;
L_80096368:
  ;
}

// 0x80095530 channelVoiceRegisterWrite — the "SPU voice-register write leaf" channelPitchSlideTick()
// (0x80090E40, this file) still rec_dispatch()es to. Faithful to gen_func_80095530
// (generated/shard_0.c:15026). Guest-stack frame mirrored (sp-64, spill ra/s0-s7/fp(s8) at their
// RE'd offsets: r16..r23 = s0..s7, r30 = s8/fp, r31 = ra).
//
// LOW-MEDIUM confidence — see sequencer.h header comment for the full field-layout writeup. Kept
// strictly register-literal with goto/labels named after the guest addresses: several branches
// re-converge on shared tails (L_80095854/L_800958EC/L_80095970 each reached from two paths) and
// this is dense fixed-point arithmetic where a hand-restructure risks a silent operand-order or
// shift-amount error. UNWIRED/pre-gate; a wiring pass must do the line-by-line verify
// docs/fleet-workflow.md §9 requires before registering this as an override.
void Sequencer::channelVoiceRegisterWrite() {
  Core* c = core;
  uint32_t sp0 = c->r[29];
  c->r[29] = sp0 - 64u;
  c->r[7] = c->r[4] & 255u;
  c->r[7] = c->r[7] << 2;
  c->r[3] = c->r[4] & 65280u;
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 8);
  c->r[2] = c->r[3] << 1;
  c->r[2] = c->r[2] + c->r[3];
  c->r[2] = c->r[2] << 2;
  c->r[2] = c->r[2] - c->r[3];
  c->mem_w32(c->r[29] + 60u, c->r[31]);
  c->mem_w32(c->r[29] + 56u, c->r[30]);
  c->mem_w32(c->r[29] + 52u, c->r[23]);
  c->mem_w32(c->r[29] + 48u, c->r[22]);
  c->mem_w32(c->r[29] + 44u, c->r[21]);
  c->mem_w32(c->r[29] + 40u, c->r[20]);
  c->mem_w32(c->r[29] + 36u, c->r[19]);
  c->mem_w32(c->r[29] + 32u, c->r[18]);
  c->mem_w32(c->r[29] + 28u, c->r[17]);
  c->mem_w32(c->r[29] + 24u, c->r[16]);
  c->r[3] = 0x80100000u + c->r[7];
  c->r[3] = c->mem_r32(c->r[3] + 19504u);   // SEQ_PTR_ARRAY-relative, matches chBase() convention
  c->r[2] = c->r[2] << 4;
  c->r[17] = c->r[3] + c->r[2];             // channelBase
  c->mem_w16(c->r[17] + 88u, (uint16_t)c->r[5]);
  c->r[2] = (uint32_t)c->mem_r16(c->r[17] + 88u);
  c->r[22] = c->r[4] + c->r[0];
  c->r[2] = (uint32_t)(c->r[2] < 127u);
  {
    int _t = (c->r[2] != c->r[0]);
    c->mem_w16(c->r[17] + 90u, (uint16_t)c->r[6]);
    if (_t) goto L_800955B0;
  }
  c->r[2] = c->r[0] + 127u;
  c->mem_w16(c->r[17] + 88u, (uint16_t)c->r[2]);
L_800955B0:
  c->r[2] = (uint32_t)c->mem_r16(c->r[17] + 90u);
  c->r[2] = (uint32_t)(c->r[2] < 127u);
  {
    int _t = (c->r[2] != c->r[0]);
    c->r[2] = c->r[0] + 127u;
    if (_t) goto L_800955C8;
  }
  c->mem_w16(c->r[17] + 90u, (uint16_t)c->r[2]);
L_800955C8:
  c->r[2] = (uint32_t)(int8_t)c->mem_r8(0x80100000u + 23788u);   // SEQ_KEYSCAN_COUNT
  {
    int _t = ((int32_t)c->r[2] <= 0);
    c->r[18] = c->r[0] + c->r[0];
    if (_t) goto L_80095A64;
  }
  c->r[21] = ((uint32_t)516u << 16) | 2065u;
  c->r[23] = c->r[0] + 127u;
  c->r[19] = ((uint32_t)33288u << 16) | 8323u;
  c->r[20] = ((uint32_t)32770u << 16) | 9u;
  c->r[30] = 0x80100000u + 23080u;
  c->r[2] = c->r[18] << 16;
L_80095604:
  c->r[4] = (uint32_t)((int32_t)c->r[2] >> 16);        // voice index i
  c->r[2] = c->r[0] + 1u;
  c->r[3] = c->mem_r32(SEQ_KEYSCAN_VOICE_MASK);
  c->r[2] = c->r[2] << (c->r[4] & 31u);
  c->r[3] = c->r[3] & c->r[2];
  {
    int _t = (c->r[3] != c->r[0]);
    c->r[2] = c->r[18] + 1u;
    if (_t) goto L_80095A44;                            // voice busy -- skip
  }
  c->r[2] = c->r[4] << 3;
  c->r[2] = c->r[2] - c->r[4];
  c->r[16] = c->r[2] << 3;                               // voice*56 (record base, SEQ_KEYSCAN_TABLE-8)
  c->r[3] = 0x80100000u + c->r[16];
  c->r[3] = (uint32_t)(int16_t)c->mem_r16(c->r[3] + 21720u);
  c->r[2] = c->r[22] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  {
    int _t = (c->r[3] != c->r[2]);
    c->r[2] = c->r[18] + 1u;
    if (_t) goto L_80095A44;
  }
  c->r[4] = 0x80100000u + c->r[16];
  c->r[4] = (uint32_t)(int16_t)c->mem_r16(c->r[4] + 21728u);
  c->r[2] = (uint32_t)(int8_t)c->mem_r8(c->r[17] + 38u);
  {
    int _t = (c->r[4] != c->r[2]);
    c->r[2] = c->r[18] + 1u;
    if (_t) goto L_80095A44;
  }
  c->r[5] = 0x80100000u + c->r[16];
  c->r[5] = (uint32_t)(int16_t)c->mem_r16(c->r[5] + 21722u);
  channelVoiceSelectPrep();
  c->r[2] = 0x80100000u + c->r[16];
  c->r[2] = (uint32_t)(int16_t)c->mem_r16(c->r[2] + 21716u);
  c->r[3] = 0x80100000u + c->r[16];
  c->r[3] = (uint32_t)(int16_t)c->mem_r16(c->r[3] + 21712u);
  c->r[2] = c->r[2] << 1;
  c->r[2] = c->r[2] + c->r[17];
  c->r[2] = (uint32_t)(int16_t)c->mem_r16(c->r[2] + 96u);
  {
    int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[3] = c->lo;
  c->r[2] = ((uint32_t)33026u << 16) | 1033u;
  {
    int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[7] = c->hi;
  c->r[4] = c->r[7] + c->r[3];
  c->r[4] = (uint32_t)((int32_t)c->r[4] >> 6);
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 31);
  c->r[4] = c->r[4] - c->r[3];
  c->r[3] = c->mem_r32(0x80100000u + 23780u);
  c->r[2] = c->r[4] << 14;
  c->r[3] = (uint32_t)c->mem_r8(c->r[3] + 24u);
  c->r[2] = c->r[2] - c->r[4];
  {
    int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[3] = c->lo;
  c->r[2] = ((uint32_t)33286u << 16) | 4137u;
  {
    int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[2] = 0x80100000u + c->r[16];
  c->r[2] = (uint32_t)(int16_t)c->mem_r16(c->r[2] + 21724u);
  c->r[4] = c->mem_r32(0x80100000u + 23772u);
  c->r[2] = c->r[2] << 4;
  c->r[2] = c->r[2] + c->r[4];
  c->r[2] = (uint32_t)c->mem_r8(c->r[2] + 1u);
  c->r[7] = c->hi;
  c->r[5] = c->r[7] + c->r[3];
  c->r[5] = (uint32_t)((int32_t)c->r[5] >> 13);
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 31);
  c->r[4] = c->r[5] - c->r[3];
  {
    int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[6] = 0x80100000u + c->r[16];
  c->r[6] = (uint32_t)(int16_t)c->mem_r16(c->r[6] + 21722u);
  c->r[2] = 0x80100000u + c->r[16];
  c->r[2] = (uint32_t)(int16_t)c->mem_r16(c->r[2] + 21726u);
  c->r[6] = c->r[6] << 4;
  c->r[6] = c->r[6] + c->r[2];
  c->r[2] = c->mem_r32(0x80100000u + 23784u);
  c->r[6] = c->r[6] << 5;
  c->r[6] = c->r[6] + c->r[2];
  c->r[3] = c->lo;
  c->r[2] = (uint32_t)c->mem_r8(c->r[6] + 2u);
  {
    int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[2];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[2] = c->lo;
  c->r[3] = ((uint32_t)1036u << 16) | 8273u;
  {
    uint64_t _p = (uint64_t)c->r[2] * (uint64_t)c->r[3];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)(_p >> 32);
  }
  c->r[3] = c->hi;
  c->r[2] = c->r[2] - c->r[3];
  c->r[2] = c->r[2] >> 1;
  c->r[3] = c->r[3] + c->r[2];
  c->r[2] = (uint32_t)c->mem_r16(c->r[17] + 88u);
  c->r[5] = c->r[3] >> 13;
  {
    int64_t _p = (int64_t)(int32_t)c->r[5] * (int64_t)(int32_t)c->r[2];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[2] = c->lo;
  c->r[3] = (uint32_t)c->mem_r16(c->r[17] + 90u);
  {
    int64_t _p = (int64_t)(int32_t)c->r[5] * (int64_t)(int32_t)c->r[3];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[3] = c->lo;
  {
    uint64_t _p = (uint64_t)c->r[2] * (uint64_t)c->r[21];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)(_p >> 32);
  }
  c->r[4] = c->hi;
  {
    uint64_t _p = (uint64_t)c->r[3] * (uint64_t)c->r[21];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)(_p >> 32);
  }
  c->r[2] = c->r[2] - c->r[4];
  c->r[2] = c->r[2] >> 1;
  c->r[4] = c->r[4] + c->r[2];
  c->r[4] = c->r[4] >> 6;
  c->r[5] = c->hi;
  c->r[3] = c->r[3] - c->r[5];
  c->r[3] = c->r[3] >> 1;
  c->r[5] = c->r[5] + c->r[3];
  c->r[3] = (uint32_t)c->mem_r8(c->r[6] + 3u);
  c->r[2] = (uint32_t)(c->r[3] < 64u);
  {
    int _t = (c->r[2] == c->r[0]);
    c->r[5] = c->r[5] >> 6;
    if (_t) goto L_80095828;
  }
  {
    int64_t _p = (int64_t)(int32_t)c->r[5] * (int64_t)(int32_t)c->r[3];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[2] = c->lo;
  c->r[3] = ((uint32_t)1040u << 16) | 16645u;
  {
    uint64_t _p = (uint64_t)c->r[2] * (uint64_t)c->r[3];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)(_p >> 32);
  }
  c->r[3] = c->hi;
  c->r[2] = c->r[2] - c->r[3];
  c->r[2] = c->r[2] >> 1;
  c->r[3] = c->r[3] + c->r[2];
  c->r[5] = c->r[3] >> 5;
  goto L_80095854;
L_80095828:
  c->r[2] = c->r[23] - c->r[3];
  {
    int64_t _p = (int64_t)(int32_t)c->r[4] * (int64_t)(int32_t)c->r[2];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[2] = c->lo;
  c->r[3] = ((uint32_t)1040u << 16) | 16645u;
  {
    uint64_t _p = (uint64_t)c->r[2] * (uint64_t)c->r[3];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)(_p >> 32);
  }
  c->r[3] = c->hi;
  c->r[2] = c->r[2] - c->r[3];
  c->r[2] = c->r[2] >> 1;
  c->r[3] = c->r[3] + c->r[2];
  c->r[4] = c->r[3] >> 5;
L_80095854:
  c->r[3] = c->r[18] << 16;
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 16);
  c->r[2] = c->r[3] << 3;
  c->r[2] = c->r[2] - c->r[3];
  c->r[2] = c->r[2] << 3;
  c->r[1] = 0x80100000u + c->r[2];
  c->r[2] = (uint32_t)(int16_t)c->mem_r16(c->r[1] + 21724u);
  c->r[3] = c->mem_r32(0x80100000u + 23772u);
  c->r[2] = c->r[2] << 4;
  c->r[2] = c->r[2] + c->r[3];
  c->r[3] = (uint32_t)c->mem_r8(c->r[2] + 4u);
  c->r[2] = (uint32_t)(c->r[3] < 64u);
  {
    int _t = (c->r[2] == c->r[0]);
    c->r[2] = c->r[5] & 65535u;
    if (_t) goto L_800958BC;
  }
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[3];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[2] = c->lo;
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[19];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[8] = c->hi;
  c->r[2] = c->r[8] + c->r[2];
  c->r[5] = c->r[2] >> 5;
  goto L_800958EC;
L_800958BC:
  c->r[2] = c->r[4] & 65535u;
  c->r[3] = c->r[23] - c->r[3];
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[3];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[2] = c->lo;
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[19];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[8] = c->hi;
  c->r[3] = c->r[8] + c->r[2];
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 5);
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 31);
  c->r[4] = c->r[3] - c->r[2];
L_800958EC:
  c->r[2] = c->r[18] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  c->r[3] = c->r[2] << 3;
  c->r[3] = c->r[3] - c->r[2];
  c->r[3] = c->r[3] << 3;
  c->r[1] = 0x80100000u + c->r[3];
  c->r[3] = (uint32_t)c->mem_r8(c->r[1] + 21714u);
  c->r[2] = (uint32_t)(c->r[3] < 64u);
  {
    int _t = (c->r[2] == c->r[0]);
    c->r[2] = c->r[5] & 65535u;
    if (_t) goto L_80095940;
  }
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[3];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[2] = c->lo;
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[19];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[8] = c->hi;
  c->r[2] = c->r[8] + c->r[2];
  c->r[5] = c->r[2] >> 5;
  goto L_80095970;
L_80095940:
  c->r[2] = c->r[4] & 65535u;
  c->r[3] = c->r[23] - c->r[3];
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[3];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[2] = c->lo;
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[19];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[8] = c->hi;
  c->r[3] = c->r[8] + c->r[2];
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 5);
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 31);
  c->r[4] = c->r[3] - c->r[2];
L_80095970:
  c->r[3] = (uint32_t)(int16_t)c->mem_r16(0x80100000u + 23768u);
  c->r[2] = c->r[0] + 1u;
  {
    int _t = (c->r[3] != c->r[2]);
    c->r[2] = c->r[4] & 65535u;
    if (_t) goto L_800959A4;
  }
  c->r[3] = c->r[5] & 65535u;
  c->r[2] = (uint32_t)(c->r[2] < c->r[3]);
  {
    int _t = (c->r[2] == c->r[0]);
    if (_t) goto L_8009599C;
  }
  c->r[4] = c->r[5] + c->r[0];
  goto L_800959A0;
L_8009599C:
  c->r[5] = c->r[4] + c->r[0];
L_800959A0:
  c->r[2] = c->r[4] & 65535u;
L_800959A4:
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[2];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[2] = c->lo;
  c->r[3] = c->r[5] & 65535u;
  {
    int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[3];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[3] = c->lo;
  {
    int64_t _p = (int64_t)(int32_t)c->r[2] * (int64_t)(int32_t)c->r[20];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[9] = c->hi;
  {
    int64_t _p = (int64_t)(int32_t)c->r[3] * (int64_t)(int32_t)c->r[20];
    c->lo = (uint32_t)_p; c->hi = (uint32_t)((uint64_t)_p >> 32);
  }
  c->r[6] = c->r[18] << 16;
  c->r[6] = (uint32_t)((int32_t)c->r[6] >> 16);
  c->r[5] = c->r[6] << 4;
  c->r[8] = 0x80100000u + 23082u;
  c->r[4] = c->r[9] + c->r[2];
  c->r[4] = (uint32_t)((int32_t)c->r[4] >> 13);
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 31);
  c->r[4] = c->r[4] - c->r[2];
  c->r[2] = c->r[5] + c->r[30];
  c->r[5] = c->r[5] + c->r[8];
  c->mem_w16(c->r[2] + 0u, (uint16_t)c->r[4]);
  c->r[7] = c->hi;
  c->r[2] = c->r[7] + c->r[3];
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 13);
  c->r[3] = (uint32_t)((int32_t)c->r[3] >> 31);
  c->r[2] = c->r[2] - c->r[3];
  c->mem_w16(c->r[5] + 0u, (uint16_t)c->r[2]);
  c->r[2] = 0x80100000u + c->r[6];
  c->r[2] = (uint32_t)c->mem_r8(c->r[2] + 23048u);
  c->r[2] = c->r[2] | 3u;
  c->r[1] = 0x80100000u + c->r[6];
  c->mem_w8(c->r[1] + 23048u, (uint8_t)c->r[2]);
  c->r[2] = c->r[18] + 1u;
L_80095A44:
  c->r[18] = c->r[2] + c->r[0];
  c->r[2] = c->r[2] << 16;
  c->r[3] = (uint32_t)(int8_t)c->mem_r8(0x80100000u + 23788u);   // SEQ_KEYSCAN_COUNT
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
  {
    int _t = (c->r[2] != c->r[0]);
    c->r[2] = c->r[18] << 16;
    if (_t) goto L_80095604;
  }
L_80095A64:
  c->r[2] = c->r[22] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  c->r[31] = c->mem_r32(c->r[29] + 60u);
  c->r[30] = c->mem_r32(c->r[29] + 56u);
  c->r[23] = c->mem_r32(c->r[29] + 52u);
  c->r[22] = c->mem_r32(c->r[29] + 48u);
  c->r[21] = c->mem_r32(c->r[29] + 44u);
  c->r[20] = c->mem_r32(c->r[29] + 40u);
  c->r[19] = c->mem_r32(c->r[29] + 36u);
  c->r[18] = c->mem_r32(c->r[29] + 32u);
  c->r[17] = c->mem_r32(c->r[29] + 28u);
  c->r[16] = c->mem_r32(c->r[29] + 24u);
  c->r[29] = sp0;
}
