// game/render/wide_re_gpu_dma_queue.cpp — WIDE-RE DRAFT of the libgpu "GPU sys" GPU-DMA
// completion-callback QUEUE cluster: 0x80082D04, 0x80082FB4, 0x80083364, 0x80082424 (0x80082734 is
// MAPPED but NOT drafted — see note at the bottom of this header, it turned out on close RE to be a
// different, larger LoadImage-style routine, not part of this cluster's mutual recursion).
//
// Dedicated deep-RE pass per docs/fleet-workflow.md §6/§9 (a prior wide-RE wave over
// game/render/wide_re_libgpu_leaves.cpp explicitly deferred this cluster, citing the "9 bugs in one
// function" failure mode given its ~380 gen-C lines and mutual recursion — see that file's header
// and docs/engine_re.md's "Wide-RE wave: libgpu GPU sys jump-table cluster" section). This pass RE'd
// the whole call graph from generated/shard_*.c gen_func_<addr> bodies (ground truth) line-by-line,
// with every branch polarity checked twice against the gen-C before being transcribed.
//
// Wide-RE tier (docs/fleet-workflow.md §6): UNWIRED / UNVERIFIED. Nothing here is called from
// anywhere (no EngineOverrides registration, no shard_set_override) — dead code that only needs to
// COMPILE. A wiring pass MUST re-diff every line against the generated C (and run SBS) before
// registering + gating.
//
// ------------------------------------------------------------------------------------------------
// CORRECTION to the prior wave's field map (game/render/wide_re_libgpu_leaves.cpp's header claimed
// head/tail counters live at 0x800A5A88/0x800A5A8C — that was a guess, WRONG). Re-derived directly
// from generated/shard_5.c:13804 gen_func_80082D04 this session: the head/tail counters are at
// **0x800A5AC8 / 0x800A5ACC** (offsets +23240/+23244 from base 0x800A0000 = 32778u<<16). The ring
// buffer base (0x80100C30, stride 0x60) WAS correct.
//
// FULL STRUCT MAP (base = 0x800A0000, i.e. `GPU_SYS_BASE = (32778u << 16)`), confirmed this session:
//   +22944 (0x59A0)  GPU_QSTAT: "queue activation" sub-struct base.
//     +0x00 (0x59A0): unread/unwritten by this cluster (not touched by any of the 4 drafted fns).
//     +0x01 (0x59A1): STARTED byte — 0 until the first Enqueue call ever transitions it via +0x08.
//     +0x08 (0x59A8): ACTIVE flag (word) — set to 1 by Enqueue's fast (idle) path; cleared to 0 by
//                     Drain's tail block right before firing the one-shot completion handler.
//     +0x0C (0x59AC): COMPLETION HANDLER fn-ptr (word) — a one-shot "queue fully drained" callback,
//                     installed by some caller OUTSIDE this cluster (not found this session — every
//                     read site in the 4 drafted fns treats it as caller-owned state), fired once by
//                     Drain when the queue empties with ACTIVE!=0, then the field is left untouched
//                     (only ACTIVE is cleared — re-arming for a next completion is the installer's
//                     job, matching a one-shot/"oneshot flag" idiom).
//   +23204 (0x5A84)  GPU_GP0_PORT_PTR: pointer to the raw GP0 FIFO port (only used by 0x80082734,
//                     the MAPPED-not-drafted LoadImage-style routine — not part of the queue proper).
//   +23208 (0x5AA8)  GPU_DMA_READY_PTR: pointer to a HW status word; bit 0x04000000 = "FIFO/DMA
//                     ready for next word" (busy-waited before every dispatch and before the DMA
//                     kick's post-kick poll).
//   +23212 (0x5AAC)  GPU_DMA_ARG0_PTR: pointer, target for func_80082C68's (already-drafted, see
//                     wide_re_libgpu_leaves.cpp) reset-time a0 stash — not read by the 4 fns here.
//   +23216 (0x5AB0)  GPU_DMA_ARG1_PTR: pointer, zeroed by func_80082C68's reset — not read here.
//   +23220 (0x5AB4)  GPU_DMA_STATE_PTR: pointer to a HW status word; bit 0x01000000 = "DMA channel
//                     busy" (tested by Enqueue's fast-path gate, Drain's early-out + tail gate, and
//                     Sync's both branches).
//   +23224 (0x5AB8)  GPU_DMA_LASTADDR_PTR: pointer, target for the DMA-kick's "last element address"
//                     (arrayPtr + count*4 - 4) — written by 0x80082424 only.
//   +23228 (0x5ABC)  GPU_DMA_COUNT_PTR: pointer, target for the DMA-kick's element count — written by
//                     0x80082424 only.
//   +23232 (0x5AC0)  GPU_DMA_CHCR_PTR: pointer to the DMA channel's control register (CHCR-shaped:
//                     kicked with 0x11000002, busy bit 0x01000000) — written/polled by 0x80082424.
//   +23236 (0x5AC4)  GPU_DMA_ENABLE_PTR: pointer to a DMA-enable/mask register, OR'd with 0x08000000
//                     by 0x80082424 before the kick (one-time-idempotent enable-bit set).
//   +23240 (0x5AC8)  RING_HEAD: next free ring slot index, mod 64 (CORRECTED address, see above).
//   +23244 (0x5ACC)  RING_TAIL: next slot to drain, mod 64 (CORRECTED address, see above).
//   +23248 (0x5AD0)  ENQ_SAVED_MASK: scratch — Enqueue's saved interrupt mask across its critical
//                     section (func_80085C9C interrupt-mask-set/get, not itself drafted this
//                     session — still substrate, reached via rec_dispatch).
//   +23252 (0x5AD4)  DRAIN_SAVED_MASK: same role, Drain's own critical section.
//   +23260 (0x5ADC), +23264 (0x5AE0): the GPU-DMA-completion TIMEOUT pair already native-owned as
//                     `gpu_timeout_arm`/`gpu_timeout_chk` in runtime/recomp/sync_overrides.cpp
//                     (registered in PlatformHle, so `rec_dispatch(c, 0x800834A0/0x800834D4)` reaches
//                     the native no-op HLE, not the recompiled busy-wait body).
//
// RING ENTRY (base = 0x80100000, i.e. `RING_BASE = (32784u << 16)`; entry(i) = RING_BASE + i*0x60 +
// 0xC30, i.e. absolute ring base 0x80100C30, stride 0x60 = 96 B, 64 slots):
//   +0x00: fn-ptr to dispatch (rec_dispatch target).
//   +0x04: EITHER the raw scalar arg1 value (if enqueued with size==0) OR a pointer to the inline
//          payload at this same entry's +0x0C (if enqueued with size!=0) — Enqueue picks the shape
//          based on its 3rd argument (byte count); Drain always just forwards whatever is here
//          verbatim as its dispatch's a0, agnostic to which shape it is.
//   +0x08: raw scalar arg3 value (always passed through verbatim, no pointer form).
//   +0x0C..: inline payload storage (present only when Enqueue's size!=0), holds ceil(size/4) words
//          copied from the caller's arg1 pointer at enqueue time — up to 0x60-0x0C = 0x54 (84) bytes
//          per slot.
//
// CALL GRAPH:
//   0x80082D04 GpuDmaQueueEnqueue(fn, argValOrPtr, sizeBytes, arg3) — calls timeout-arm/chk,
//     interrupt-mask set/get (func_80085C9C), ISR-register (func_80085B80), Drain (0x80082FB4,
//     mutual recursion: full queue -> drain-then-retry), and rec_dispatch(fn) directly on the fast
//     (idle) path.
//   0x80082FB4 GpuDmaQueueDrain() — the GPU-DMA-completion INTERRUPT HANDLER BODY (installed as such
//     by Enqueue's deferred path via func_80085B80(mode=2, &0x80082FB4), and unregistered the same
//     way with fn=0 when the ring empties past its last entry) — drains ring entries via
//     rec_dispatch, fires the one-shot GPU_QSTAT completion handler when the queue empties.
//   0x80083364 GpuDmaQueueSync(mode) — mode==0: BLOCKING wait until the queue is fully drained AND
//     the DMA channel is idle+ready (arms timeout, loops Drain+timeout-check); mode!=0: single-shot
//     poll (drain once if non-empty, report queue depth or ready-state) — same mode-0-blocks /
//     mode-nonzero-polls shape as the real SDK `DrawSync(mode)`, but this is a DIFFERENT function
//     (DrawSync itself is func_80080F6C, already drafted in wide_re_libgpu_leaves.cpp) — an internal
//     sync primitive scoped to this queue. Calls Drain + the timeout pair, no ring access itself
//     beyond HEAD/TAIL depth math.
//   0x80082424 GpuDmaSend(arrayPtr, count) — the actual OT-linked-list DMA KICK: sets the DMA-enable
//     bit, programs last-address + count fields, writes CHCR=0x11000002 (start), arms the timeout,
//     then busy-waits (timeout-checked) for the CHCR busy bit to clear. Does NOT touch the ring —
//     shares only the status-block globals with the queue cluster (GPU_DMA_STATE_PTR etc.), so it's
//     the ring's "how a queued Enqueue eventually turns into a real GPU-DMA-visible transfer"
//     downstream primitive, but is not itself mutually recursive with the other 3.
//
// MAPPED, NOT DRAFTED this session: **0x80082734**. On RE this turned out to be a substantially
// larger (48-byte frame, 6 callee-save spills: ra/s0-s5) function that streams pixel words directly
// to the raw GP0 FIFO port (GPU_GP0_PORT_PTR, +23204/0x5A84) with a rectangle argument (fields +4/+6
// = width/height) CLIPPED against two global max-W/H shorts at 0x800A5964/0x800A5966 (32778u<<16 +
// 22948/22950 — a DIFFERENT pair from anything in this cluster), chunked into (count>>5)-sized
// 16-word FIFO bursts (the classic "GPU FIFO can't take an unbounded burst" governor). This is the
// classic libgpu `LoadImage()`-internal chunked-transfer routine, NOT part of the callback-queue
// mutual recursion (it shares only the GPU_DMA_READY_PTR / GPU_DMA_STATE_PTR busy-wait idiom with
// the other 4). Confidence: HIGH on role (streamed pixel transfer with W/H clip + FIFO chunking),
// MEDIUM-LOW on exact field semantics of the rect-clip and the chunking arithmetic (not re-verified
// against a live VRAM-transfer dump). Left for a dedicated follow-up pass — do not fold it into this
// cluster's wiring without RE'ing it on its own terms first.
#include "core.h"
#include <stdint.h>

extern "C" void rec_dispatch(Core* c, uint32_t addr);

namespace {
constexpr uint32_t GPU_SYS_BASE = (32778u << 16);        // 0x800A0000
constexpr uint32_t GPU_QSTAT_BASE      = GPU_SYS_BASE + 22944; // 0x800A59A0
constexpr uint32_t GPU_QSTAT_STARTED   = GPU_QSTAT_BASE + 1;   // 0x800A59A1
constexpr uint32_t GPU_QSTAT_ACTIVE    = GPU_QSTAT_BASE + 8;   // 0x800A59A8
constexpr uint32_t GPU_QSTAT_HANDLER   = GPU_QSTAT_BASE + 12;  // 0x800A59AC
constexpr uint32_t GPU_DMA_READY_PTR   = GPU_SYS_BASE + 23208; // 0x800A5AA8
constexpr uint32_t GPU_DMA_LASTADDR_PTR= GPU_SYS_BASE + 23224; // 0x800A5AB8
constexpr uint32_t GPU_DMA_COUNT_PTR   = GPU_SYS_BASE + 23228; // 0x800A5ABC
constexpr uint32_t GPU_DMA_STATE_PTR   = GPU_SYS_BASE + 23220; // 0x800A5AB4
constexpr uint32_t GPU_DMA_CHCR_PTR    = GPU_SYS_BASE + 23232; // 0x800A5AC0
constexpr uint32_t GPU_DMA_ENABLE_PTR  = GPU_SYS_BASE + 23236; // 0x800A5AC4
constexpr uint32_t RING_HEAD           = GPU_SYS_BASE + 23240; // 0x800A5AC8
constexpr uint32_t RING_TAIL           = GPU_SYS_BASE + 23244; // 0x800A5ACC
constexpr uint32_t ENQ_SAVED_MASK      = GPU_SYS_BASE + 23248; // 0x800A5AD0
constexpr uint32_t DRAIN_SAVED_MASK    = GPU_SYS_BASE + 23252; // 0x800A5AD4

constexpr uint32_t RING_BASE = (32784u << 16);  // 0x80100000
constexpr uint32_t RING_ENTRY_OFF = 3120;       // 0xC30 — entry(i) = RING_BASE + i*0x60 + RING_ENTRY_OFF

constexpr uint32_t GPU_DMA_READY_BIT = (1024u << 16); // 0x04000000
constexpr uint32_t GPU_DMA_BUSY_BIT  = (256u << 16);  // 0x01000000

inline uint32_t ringEntry(uint32_t idx) { return RING_BASE + idx * 0x60u + RING_ENTRY_OFF; }

// The 4 leaf platform primitives this cluster shares with the rest of libgpu — none drafted this
// session (still substrate), reached uniformly via rec_dispatch so a future native port of any of
// them is picked up automatically without touching this file.
constexpr uint32_t FN_GPU_TIMEOUT_ARM = 0x800834A0u;
constexpr uint32_t FN_GPU_TIMEOUT_CHK = 0x800834D4u;
constexpr uint32_t FN_INT_MASK_SET    = 0x80085C9Cu;  // enter/restore interrupt mask, returns old in v0
constexpr uint32_t FN_ISR_REGISTER    = 0x80085B80u;  // (mode, fnPtr) install/uninstall an interrupt handler
constexpr uint32_t FN_DRAIN           = 0x80082FB4u;  // this cluster's own Drain, see below
}  // namespace

// func_80082D04 (0x80082D04) — GpuDmaQueueEnqueue(fn, argValOrPtr, sizeBytes, arg3). DRAFT.
// RE'd from generated/shard_5.c:13804 gen_func_80082D04 (~165 gen-C ln). The single highest-value
// target in the band (824 free-roam rec_dispatch hits / 600 frames). Guest ABI: a0=fn-ptr to later
// dispatch, a1=scalar value OR pointer-to-data (shape picked by a2), a2=size in BYTES of data to
// copy inline (0 = a1 is used as a raw scalar), a3=a second scalar passed through verbatim. Returns
// v0: -1 on timeout, 0 on the fast/immediate-dispatch path, else the post-enqueue queue depth.
//
// Frame -40, callee-save spills ra/s0-s3 (sp+16=s0/r16, sp+20=s1/r17, sp+24=s2/r18, sp+28=s3/r19,
// sp+32=ra) — s3=fn, s0=argValOrPtr, s1=sizeBytes, s2=arg3 (the guest ABI args, moved into
// callee-saves because the body calls out repeatedly).
static void func_80082D04(Core* c) {
  c->r[29] -= 40;
  c->mem_w32(c->r[29] + 28, c->r[19]);
  const uint32_t fn = c->r[4];             // s3
  c->mem_w32(c->r[29] + 16, c->r[16]);
  const uint32_t argValOrPtr = c->r[5];    // s0
  c->mem_w32(c->r[29] + 20, c->r[17]);
  const uint32_t sizeBytes = c->r[6];      // s1
  c->mem_w32(c->r[29] + 24, c->r[18]);
  c->mem_w32(c->r[29] + 32, c->r[31]);
  const uint32_t arg3 = c->r[7];           // s2

  auto epilogue = [&](uint32_t retVal) {
    c->r[2] = retVal;
    c->r[31] = c->mem_r32(c->r[29] + 32);
    c->r[19] = c->mem_r32(c->r[29] + 28);
    c->r[18] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 40;
  };

  rec_dispatch(c, FN_GPU_TIMEOUT_ARM);

  // L_80082D50 loop: wait for a free ring slot (retry via Drain + timeout-checked wait if full).
  for (;;) {
    uint32_t head = c->mem_r32(RING_HEAD);
    uint32_t tail = c->mem_r32(RING_TAIL);
    uint32_t next = (head + 1) & 63u;
    if (next != tail) break;  // room available
    // L_80082D38: full — timeout-check, then drain and retry
    rec_dispatch(c, FN_GPU_TIMEOUT_CHK);
    if (c->r[2] != 0) { epilogue((uint32_t)-1); return; }
    rec_dispatch(c, FN_DRAIN);
  }

  // Enter critical section (disable interrupts), save the old mask.
  c->r[4] = 0;
  rec_dispatch(c, FN_INT_MASK_SET);
  c->mem_w32(ENQ_SAVED_MASK, c->r[2]);

  // Decide fast (immediate dispatch) vs deferred (ring-enqueue) path.
  uint8_t started = c->mem_r8(GPU_QSTAT_STARTED);
  bool fastPath;
  if (started == 0) {
    c->mem_w32(GPU_QSTAT_ACTIVE, 1);
    fastPath = true;
  } else {
    uint32_t head = c->mem_r32(RING_HEAD);
    uint32_t tail = c->mem_r32(RING_TAIL);
    if (head != tail) {
      fastPath = false;
    } else {
      uint32_t stateWord = c->mem_r32(c->mem_r32(GPU_DMA_STATE_PTR));
      if ((stateWord & GPU_DMA_BUSY_BIT) != 0) {
        fastPath = false;
      } else {
        fastPath = (c->mem_r32(GPU_QSTAT_HANDLER) == 0);
      }
    }
  }

  if (fastPath) {
    // L_80082DE4: busy-wait for DMA ready, dispatch fn synchronously, restore mask, return 0.
    while ((c->mem_r32(c->mem_r32(GPU_DMA_READY_PTR)) & GPU_DMA_READY_BIT) == 0) {}
    c->r[4] = argValOrPtr;
    c->r[5] = arg3;
    rec_dispatch(c, fn);
    c->r[4] = c->mem_r32(ENQ_SAVED_MASK);
    rec_dispatch(c, FN_INT_MASK_SET);
    epilogue(0);
    return;
  }

  // L_80082E28: deferred path — (re)install Drain as the completion-callback ISR, copy the payload
  // (if sizeBytes!=0) into the ring slot's inline storage, write the entry's fixed fields, advance
  // HEAD, restore the mask, kick a Drain pass, and return the new queue depth.
  c->r[4] = 2;
  c->r[5] = FN_DRAIN;  // install THIS cluster's Drain as the ISR
  rec_dispatch(c, FN_ISR_REGISTER);

  if (sizeBytes != 0) {
    int32_t sizeSigned = (int32_t)sizeBytes;
    int32_t rounded = sizeSigned >= 0 ? sizeSigned : sizeSigned + 3;
    int32_t wordCount = rounded >> 2;
    uint32_t src = argValOrPtr;
    for (int32_t i = 0; i < wordCount; i++) {
      uint32_t word = c->mem_r32(src);
      src += 4;
      uint32_t head = c->mem_r32(RING_HEAD);
      c->mem_w32(ringEntry(head) + 0x0C + (uint32_t)i * 4, word);
    }
    uint32_t head = c->mem_r32(RING_HEAD);
    c->mem_w32(ringEntry(head) + 4, ringEntry(head) + 0x0C);  // +4 = pointer to the copied payload
  } else {
    uint32_t head = c->mem_r32(RING_HEAD);
    c->mem_w32(ringEntry(head) + 4, argValOrPtr);  // +4 = raw scalar
  }

  {
    uint32_t head = c->mem_r32(RING_HEAD);
    c->mem_w32(ringEntry(head) + 8, arg3);
    c->mem_w32(ringEntry(head) + 0, fn);
  }

  {
    uint32_t head = (c->mem_r32(RING_HEAD) + 1) & 63u;
    c->mem_w32(RING_HEAD, head);
  }
  c->r[4] = c->mem_r32(ENQ_SAVED_MASK);
  rec_dispatch(c, FN_INT_MASK_SET);
  rec_dispatch(c, FN_DRAIN);

  {
    uint32_t head = c->mem_r32(RING_HEAD);
    uint32_t tail = c->mem_r32(RING_TAIL);
    epilogue((head - tail) & 63u);
  }
}

// func_80082FB4 (0x80082FB4) — GpuDmaQueueDrain(). DRAFT. RE'd from generated/shard_6.c:14620
// gen_func_80082FB4 (~130 gen-C ln). This is the completion-callback ring's DRAIN body — both
// called synchronously (by Enqueue and Sync) AND installed as the GPU-DMA-completion INTERRUPT
// HANDLER itself (Enqueue's deferred path registers `&0x80082FB4` via func_80085B80(mode=2, ...)).
// Guest ABI: no args. Returns v0: 1 if the DMA channel was already busy on entry (nothing drained),
// -1 on timeout, else the remaining queue depth after draining.
//
// Frame -32, spills ra (sp+24), s1/r17 (sp+20), s0/r16 (sp+16) — both s-regs are pure scratch here
// (the gen body reuses them as bitmask temporaries after spilling; this port uses locals instead,
// since only the STACK BYTES need to match, not host-register contents).
static void func_80082FB4(Core* c) {
  uint32_t stateWordEarly = c->mem_r32(c->mem_r32(GPU_DMA_STATE_PTR));
  c->r[29] -= 32;
  c->mem_w32(c->r[29] + 24, c->r[31]);
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->mem_w32(c->r[29] + 16, c->r[16]);

  auto epilogue = [&](uint32_t retVal) {
    c->r[2] = retVal;
    c->r[31] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 32;
  };

  if ((stateWordEarly & GPU_DMA_BUSY_BIT) != 0) { epilogue(1); return; }

  c->r[4] = 0;
  rec_dispatch(c, FN_INT_MASK_SET);
  c->mem_w32(DRAIN_SAVED_MASK, c->r[2]);  // unconditional (matches gen delay-slot write)

  bool doDrain = true;
  {
    uint32_t head = c->mem_r32(RING_HEAD);
    uint32_t tail = c->mem_r32(RING_TAIL);
    if (head == tail) doDrain = false;
    else {
      uint32_t stateWord = c->mem_r32(c->mem_r32(GPU_DMA_STATE_PTR));
      if ((stateWord & GPU_DMA_BUSY_BIT) != 0) doDrain = false;
    }
  }

  if (doDrain) {
    // L_8008302C drain loop.
    for (;;) {
      uint32_t tail = c->mem_r32(RING_TAIL);
      uint32_t head = c->mem_r32(RING_HEAD);
      uint32_t nextTail = (tail + 1) & 63u;
      if (nextTail == head) {
        // about to drain the LAST queued entry: if no one-shot completion handler is armed,
        // uninstall the Drain ISR now (queue is emptying and won't need async draining anymore).
        if (c->mem_r32(GPU_QSTAT_HANDLER) == 0) {
          c->r[4] = 2;
          c->r[5] = 0;
          rec_dispatch(c, FN_ISR_REGISTER);
        }
      }

      while ((c->mem_r32(c->mem_r32(GPU_DMA_READY_PTR)) & GPU_DMA_READY_BIT) == 0) {}

      uint32_t curTail = c->mem_r32(RING_TAIL);
      uint32_t entry = ringEntry(curTail);
      uint32_t argVal = c->mem_r32(entry + 4);
      uint32_t arg3 = c->mem_r32(entry + 8);
      uint32_t fn = c->mem_r32(entry + 0);
      c->r[4] = argVal;
      c->r[5] = arg3;
      rec_dispatch(c, fn);

      uint32_t newTail = (c->mem_r32(RING_TAIL) + 1) & 63u;
      c->mem_w32(RING_TAIL, newTail);

      uint32_t headNow = c->mem_r32(RING_HEAD);
      if (headNow == newTail) break;  // queue now empty -> shared tail
      uint32_t stateWord = c->mem_r32(c->mem_r32(GPU_DMA_STATE_PTR));
      if ((stateWord & GPU_DMA_BUSY_BIT) == 0) continue;  // not busy -> keep draining
      break;  // busy -> stop, fall to shared tail
    }
  }

  // L_80083164: shared tail — restore mask, fire the one-shot completion handler if the queue is
  // idle+empty+active, return the final depth.
  c->r[4] = c->mem_r32(DRAIN_SAVED_MASK);
  rec_dispatch(c, FN_INT_MASK_SET);

  {
    uint32_t head = c->mem_r32(RING_HEAD);
    uint32_t tail = c->mem_r32(RING_TAIL);
    if (head == tail) {
      uint32_t stateWord = c->mem_r32(c->mem_r32(GPU_DMA_STATE_PTR));
      if ((stateWord & GPU_DMA_BUSY_BIT) == 0) {
        uint32_t active = c->mem_r32(GPU_QSTAT_ACTIVE);
        if (active != 0) {
          uint32_t handler = c->mem_r32(GPU_QSTAT_HANDLER);
          if (handler != 0) {
            c->mem_w32(GPU_QSTAT_ACTIVE, 0);
            rec_dispatch(c, handler);
          }
        }
      }
    }
  }

  {
    uint32_t head = c->mem_r32(RING_HEAD);
    uint32_t tail = c->mem_r32(RING_TAIL);
    epilogue((head - tail) & 63u);
  }
}

// func_80083364 (0x80083364) — GpuDmaQueueSync(mode). DRAFT. RE'd from generated/shard_0.c:12956
// gen_func_80083364 (~70 gen-C ln). Same mode-0-BLOCKS / mode-nonzero-POLLS shape as the real SDK
// `DrawSync(mode)` (a DIFFERENT function, already drafted as func_80080F6C in
// wide_re_libgpu_leaves.cpp) — this is an internal sync primitive scoped to THIS queue. Guest ABI:
// a0=mode. Returns v0: -1 on timeout; mode==0 success returns the (nonzero) ready-bit mask value
// 0x04000000 (transcribed literally — the gen body really does return the raw AND-masked bit, not a
// normalized bool); mode!=0 returns the queue depth (or 1 if depth was 0 and the channel wasn't
// idle+ready).
//
// Frame -24, spills ra (sp+20), s0/r16 (sp+16, holds the pre-drain depth across the mode!=0 branch).
static void func_80083364(Core* c) {
  const uint32_t mode = c->r[4];
  c->r[29] -= 24;
  c->mem_w32(c->r[29] + 20, c->r[31]);
  c->mem_w32(c->r[29] + 16, c->r[16]);

  auto epilogue = [&](uint32_t retVal) {
    c->r[2] = retVal;
    c->r[31] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 24;
  };

  if (mode == 0) {
    rec_dispatch(c, FN_GPU_TIMEOUT_ARM);
    for (;;) {
      uint32_t head = c->mem_r32(RING_HEAD);
      uint32_t tail = c->mem_r32(RING_TAIL);
      if (head != tail) {
        // L_80083384: not empty -> drain once, timeout-check, retry
        rec_dispatch(c, FN_DRAIN);
        rec_dispatch(c, FN_GPU_TIMEOUT_CHK);
        if (c->r[2] != 0) { epilogue((uint32_t)-1); return; }
        continue;
      }
      // L_800833D0: empty -> wait for channel idle (busy bit clear) AND ready
      for (;;) {
        uint32_t stateWord = c->mem_r32(c->mem_r32(GPU_DMA_STATE_PTR));
        if ((stateWord & GPU_DMA_BUSY_BIT) != 0) {
          rec_dispatch(c, FN_GPU_TIMEOUT_CHK);
          if (c->r[2] != 0) { epilogue((uint32_t)-1); return; }
          continue;
        }
        uint32_t readyWord = c->mem_r32(c->mem_r32(GPU_DMA_READY_PTR));
        uint32_t readyBit = readyWord & GPU_DMA_READY_BIT;
        if (readyBit == 0) {
          rec_dispatch(c, FN_GPU_TIMEOUT_CHK);
          if (c->r[2] != 0) { epilogue((uint32_t)-1); return; }
          continue;
        }
        epilogue(readyBit);
        return;
      }
    }
  }

  // mode != 0: single-shot poll.
  uint32_t head0 = c->mem_r32(RING_HEAD);
  uint32_t tail0 = c->mem_r32(RING_TAIL);
  uint32_t depth = (head0 - tail0) & 63u;
  if (depth != 0) rec_dispatch(c, FN_DRAIN);

  uint32_t stateWord = c->mem_r32(c->mem_r32(GPU_DMA_STATE_PTR));
  if ((stateWord & GPU_DMA_BUSY_BIT) == 0) {
    uint32_t readyWord = c->mem_r32(c->mem_r32(GPU_DMA_READY_PTR));
    if ((readyWord & GPU_DMA_READY_BIT) != 0) { epilogue(depth); return; }
  }
  epilogue(depth != 0 ? depth : 1u);
}

// func_80082424 (0x80082424) — GpuDmaSend(arrayPtr, count). DRAFT. RE'd from generated/shard_3.c:19562
// gen_func_80082424 (~50 gen-C ln, self-contained). The actual OT-linked-list DMA KICK: programs the
// last-element address + count into the status block, writes the DMA channel control register
// (CHCR-shaped) with the start/chop value 0x11000002, arms the timeout, then busy-waits
// (timeout-checked) for the CHCR busy bit (0x01000000) to clear. Does not touch the ring buffer —
// shares only the status-block globals with the queue cluster. Guest ABI: a0=arrayPtr (linked-list
// head, last word = -4-relative offset from (arrayPtr + count*4)), a1=count (words). Returns v0:
// count on the immediate-not-busy fast path; -1 on timeout; 0 if it had to wait and the busy bit
// cleared normally (transcribed literally — the gen body's post-wait return value really is the
// AND-masked-to-zero busy bit, not `count`).
//
// Frame -32, spills ra (sp+24), s1/r17 (sp+20, holds the busy-bit mask constant across the wait
// loop), s0/r16 (sp+16, holds `count`).
static void func_80082424(Core* c) {
  const uint32_t arrayPtr = c->r[4];
  c->r[29] -= 32;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  const uint32_t count = c->r[5];  // s0
  const uint32_t enableRegPtr = c->mem_r32(GPU_DMA_ENABLE_PTR);
  c->mem_w32(c->r[29] + 24, c->r[31]);
  c->mem_w32(c->r[29] + 20, c->r[17]);

  auto epilogue = [&](uint32_t retVal) {
    c->r[2] = retVal;
    c->r[31] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 32;
  };

  c->mem_w32(enableRegPtr, c->mem_r32(enableRegPtr) | 0x08000000u);

  const uint32_t chcrPtr = c->mem_r32(GPU_DMA_CHCR_PTR);
  c->mem_w32(chcrPtr, 0);

  const uint32_t lastAddrFieldPtr = c->mem_r32(GPU_DMA_LASTADDR_PTR);
  c->mem_w32(lastAddrFieldPtr, arrayPtr + (count << 2) - 4);

  const uint32_t countFieldPtr = c->mem_r32(GPU_DMA_COUNT_PTR);
  c->mem_w32(countFieldPtr, count);

  c->mem_w32(chcrPtr, 0x11000002u);  // KICK
  rec_dispatch(c, FN_GPU_TIMEOUT_ARM);

  uint32_t chcrWord = c->mem_r32(chcrPtr);
  if ((chcrWord & GPU_DMA_BUSY_BIT) == 0) { epilogue(count); return; }

  for (;;) {
    rec_dispatch(c, FN_GPU_TIMEOUT_CHK);
    if (c->r[2] != 0) { epilogue((uint32_t)-1); return; }
    uint32_t busy = c->mem_r32(chcrPtr) & GPU_DMA_BUSY_BIT;
    if (busy != 0) continue;
    epilogue(busy);  // busy == 0 here
    return;
  }
}
