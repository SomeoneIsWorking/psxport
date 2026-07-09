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

void rec_dispatch(Core*, uint32_t);

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
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x10u) { c->r[4] = s; c->r[5] = ch; rec_dispatch(c, 0x80090E40u); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x20u) { c->r[4] = s; c->r[5] = ch; rec_dispatch(c, 0x80090E40u); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x40u) { c->r[4] = s; c->r[5] = ch; rec_dispatch(c, 0x80092080u); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x80u) { c->r[4] = s; c->r[5] = ch; rec_dispatch(c, 0x80092080u); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x02u) { c->r[4] = s; c->r[5] = ch; channelReleaseClear(); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x08u) { c->r[4] = s; c->r[5] = ch; channelStopFlagSet(); }
          if (c->mem_r32(chBase() + CH_FLAGS) & 0x04u) {
            c->r[4] = s; c->r[5] = ch;
            rec_dispatch(c, 0x80091970u);
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
