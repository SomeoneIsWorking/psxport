// game/render/wide_re_gpu_loadimage_streamer.cpp — WIDE-RE DRAFT of the libgpu LoadImage()-style
// chunked GP0-FIFO pixel streamer, guest address 0x80082734 (~196 free-roam rec_dispatch hits / 600
// frames). Dedicated deep-RE pass per docs/fleet-workflow.md §6/§9: this address was explicitly left
// MAPPED-NOT-DRAFTED by two prior wide-RE waves (game/render/wide_re_libgpu_leaves.cpp's header and
// game/render/wide_re_gpu_dma_queue.cpp's tail note both flag it as "a substantially larger function
// ... left for a dedicated follow-up pass"). RE'd this session line-by-line from
// generated/shard_5.c:13663 gen_func_80082734 (140 gen-C lines, 48-byte frame, 6 callee-save spills:
// ra/s0..s5 == r16..r21) — ground truth, cross-checked twice for every branch polarity and every
// delay-slot write (MIPS branch-delay-slot instructions execute UNCONDITIONALLY even though the
// recompiler emits them textually inside the branch's `{ ... }` block — same convention flagged in
// wide_re_libgpu_leaves.cpp's ClearOTagR comment).
//
// WIRED 2026-07-10: promoted from wide-RE draft to verified ownership per docs/fleet-workflow.md §9.
// Re-verified line-by-line against generated/shard_5.c gen_func_80082734 (the exhaustive re-diff found
// NO discrepancies — every branch polarity, delay-slot write, double-pointer-dereference, and the
// decrement-then-test PIO remainder loop's net iteration count were re-checked and match gen exactly).
// Reached as a plain intra-shard C call from the substrate (func_80082D04/GpuDmaQueueEnqueue's fast
// path calls it via `rec_dispatch(c, fn)` where fn resolves to this address), so it is owned via
// `engine_set_override_main` — the oracle-gated thunk that keeps SBS core B running the pure
// gen_func_80082734 body while core A dispatches to the native method below.
//
// ------------------------------------------------------------------------------------------------
// CORRECTION to both prior waves' field-map notes: the raw decimal offset used by the gen-C body for
// the GP0 FIFO port pointer is **23204**, which is **0x800A5AA4** — NOT 0x800A5A84 as stated in
// wide_re_gpu_dma_queue.cpp's header ("GPU_GP0_PORT_PTR: ... +23204 (0x5A84)"). 23204 decimal is
// 0x5AA4 in hex (verified: 5*4096 + 0xA*256 + 0xA*16 + 4 = 23204); 0x5A84 is 23172 decimal, a
// different (and in this function, unused) address. Likewise the rect-clip max-W/H shorts this
// function reads are at decimal offsets **22948 / 22950 = 0x800A59A4 / 0x800A59A6** (both prior
// waves' engine_re.md prose said "0x800A5964/0x800A5966", also a transcription slip — 0x5964 =
// 22884 decimal, not 22948). All addresses below are re-derived directly from the exact decimal
// immediates in generated/shard_5.c, the only trustworthy source.
//
// GUEST ABI: a0(r4) = rectPtr — a 4x-int16 struct {s16 x, y, w, h} (classic PSX RECT16 shape; only
// +4(w)/+6(h) are read/written by this leaf, +0(x|y) is forwarded verbatim as one packed 32-bit
// word). a1(r5) = srcPtr — pointer to the pixel-word source buffer (16bpp packed, 2px/word). No other
// args read. Return v0: -1 on a GPU-DMA-ready timeout OR if the clamped rect resolves to <=0 words to
// send; 0 on success (matches func_80082C68/queue-cluster's void-ish "0 on success, -1 on timeout"
// convention throughout this GPU_SYS band).
//
// STRUCT MAP used by this function (base = 0x800A0000, i.e. `GPU_SYS_BASE = (32778u << 16)`):
//   +22948 (0x59A4)  GPU_CLIP_MAXW: s16, this leaf's own W clip max (separate global pair from the
//                    queue cluster's status block — confirmed used ONLY here this session).
//   +22950 (0x59A6)  GPU_CLIP_MAXH: s16, H clip max, same family.
//   +23204 (0x5AA4)  GPU_GP0_PORT_PTR: pointer-to-pointer — dereferenced once to get the actual raw
//                    GP0 FIFO port address; every write through it is a repeated write to the SAME
//                    target address (classic memory-mapped hardware FIFO push semantics, matching the
//                    real PS1 GPU_DATA I/O port 0x1F801810).
//   +23208 (0x5AA8)  GPU_DMA_FLAGS_PTR (== queue cluster's GPU_DMA_READY_PTR, SAME address — this
//                    leaf both TESTS bit 0x04000000 (ready) via this pointer's target AND, on the
//                    async-continuation path, WRITES the fixed value 0x04000002 to it — byte-
//                    identical to func_80082C68's GPU-DMA status-block RESET write, confirming this
//                    is genuinely the shared status word, not a distinct field).
//   +23212 (0x5AAC)  GPU_DMA_ARG0_PTR (== queue cluster's field): async path stashes the
//                    post-PIO-remainder src pointer here (start of the chunked-DMA source region).
//   +23216 (0x5AB0)  GPU_DMA_ARG1_PTR (== queue cluster's field): async path stashes
//                    `(chunkCount << 16) | 16` — chunk count packed with the fixed 16-word burst size.
//   +23220 (0x5AB4)  GPU_DMA_STATE_PTR (== queue cluster's field, bit 0x01000000 = DMA busy): async
//                    path writes the fixed value 0x01000201 (busy bit set, low byte 0x01 + 0x200 —
//                    exact meaning of the low bits not resolved, transcribed literally).
// The four GPU_DMA_* fields above are the SAME globals game/render/wide_re_gpu_dma_queue.cpp already
// maps in detail; this leaf writes/reads them directly rather than going through Enqueue/Drain,
// consistent with that file's note that 0x80082734 "shares only the busy-wait idiom" with the queue
// cluster, not its ring. GPU_CLIP_MAXW/MAXH and GPU_GP0_PORT_PTR are NOT used by the queue cluster.
//
// CONTROL FLOW SUMMARY (see the function body below for the literal RE):
//   1. Arm the GPU-DMA timeout (func_800834A0, shared platform primitive — same one the queue
//      cluster and wide_re_libgpu_leaves.cpp's DrawSync/ClearOTagR use).
//   2. Clamp rect.w (rectPtr+4) and rect.h (rectPtr+6) against GPU_CLIP_MAXW/MAXH, writing the
//      clamped values back IN PLACE (negative w or h clamps to 0; the compare uses a SIGNED
//      less-than on the max-vs-value, i.e. `max < value ? max : value`).
//   3. pixelCount = clampedW * clampedH (16bpp pixel count); numWords = ceil(pixelCount / 2)
//      (2 pixels pack into one 32-bit FIFO word). If numWords <= 0, return -1 immediately — NO
//      further side effects (the timeout-arm from step 1 already happened, everything else is
//      skipped).
//   4. chunkCount = numWords >> 4 (count of 16-word FIFO bursts); remainder = numWords - chunkCount*16
//      (0..15 leftover words sent one-at-a-time via direct PIO).
//   5. Busy-wait (timeout-checked, aborts -1 on timeout) for the GP0/DMA ready bit (0x04000000) via
//      GPU_DMA_FLAGS_PTR's target.
//   6. Push, via GPU_GP0_PORT_PTR's target (repeated writes = FIFO push): ack-ready word 0x04000000,
//      GP0(0x01) ClearCache (0x01000000), GP0(0xA0) "Copy Rectangle CPU->VRAM" tag (0xA0000000, see
//      dead-code note below), the rect's X|Y word (rectPtr+0, forwarded verbatim), the CLAMPED W|H
//      word (rectPtr+4, re-read as one 32-bit word after both 16-bit clamped writes landed).
//   7. PIO-stream `remainder` words one at a time (test-at-bottom loop matching the gen body's -1
//      sentinel counter) from srcPtr to the GP0 port, advancing srcPtr.
//   8. If chunkCount == 0: return 0 (everything already sent via steps 6-7). Else: hand the remaining
//      chunkCount*16 words off to an ASYNC GPU-DMA continuation (outside this function — not found
//      this session) by stashing srcPtr/chunk-count/status into the shared GPU_DMA_ARG0/ARG1/STATE
//      globals (see struct map above) and returning 0 without actually transferring them here.
//
// DEAD-CODE NOTE (step 6's tag word): the gen body computes the CopyRectCpuToVram tag via a branch
// on register s5(r21) that this SAME function's prologue-adjacent code unconditionally zeroes exactly
// once, as the (always-executed) branch-delay-slot instruction of an EARLIER unrelated branch (the
// rect.w<0 check) — r21 is never written again anywhere else in the function body. The `r21==0` arm
// (tag = 0xA0000000, GP0 CopyRect CPU->VRAM) is therefore the ONLY reachable arm; the `else` arm
// (tag = 0xB0000000, not a documented real GP0 command — plausibly VRAM->VRAM copy dead code left by
// the compiler from a shared code template) is PROVABLY unreachable for every call and is omitted
// here rather than transcribed as dead code, per this file's "no bandaid" mandate to name what's
// actually true rather than leave an unreachable/misleading branch in a faithful port. Confidence:
// HIGH on this being genuinely dead (verified twice against the raw MIPS delay-slot semantics).
//
// CONFIDENCE: HIGH on control flow and every memory read/write (mechanically re-derived from the
// exact gen-C, no paraphrase). MEDIUM-LOW on the async-DMA-continuation handoff's semantic meaning
// (steps 8's low-bit encodings in ARG1/STATE) — the actual background consumer of those fields was
// NOT located this session (not in this function, not in the already-drafted queue cluster); the
// VALUES written are transcribed exactly, but their downstream interpretation is inferred from shape
// alone. A wiring pass should grep for other readers of GPU_DMA_ARG0/ARG1/STATE before trusting the
// async path is fully understood.
#include "core.h"
#include "game_ctx.h"
#include "render.h"
#include <stdint.h>

extern "C" void rec_dispatch(Core* c, uint32_t addr);

namespace {
constexpr uint32_t GPU_SYS_BASE = (32778u << 16);              // 0x800A0000
constexpr uint32_t GPU_CLIP_MAXW      = GPU_SYS_BASE + 22948;  // 0x800A59A4
constexpr uint32_t GPU_CLIP_MAXH      = GPU_SYS_BASE + 22950;  // 0x800A59A6
constexpr uint32_t GPU_GP0_PORT_PTR   = GPU_SYS_BASE + 23204;  // 0x800A5AA4
constexpr uint32_t GPU_DMA_FLAGS_PTR  = GPU_SYS_BASE + 23208;  // 0x800A5AA8
constexpr uint32_t GPU_DMA_ARG0_PTR   = GPU_SYS_BASE + 23212;  // 0x800A5AAC
constexpr uint32_t GPU_DMA_ARG1_PTR   = GPU_SYS_BASE + 23216;  // 0x800A5AB0
constexpr uint32_t GPU_DMA_STATE_PTR  = GPU_SYS_BASE + 23220;  // 0x800A5AB4

constexpr uint32_t GPU_READY_BIT       = (1024u << 16);  // 0x04000000
constexpr uint32_t GP0_CLEAR_CACHE     = (256u << 16);   // 0x01000000 — GP0(0x01)
constexpr uint32_t GP0_COPY_CPU_TO_VRAM= (40960u << 16);  // 0xA0000000 — GP0(0xA0) tag

constexpr uint32_t FN_GPU_TIMEOUT_ARM = 0x800834A0u;  // native-owned HLE, runtime/recomp/sync_overrides.cpp
constexpr uint32_t FN_GPU_TIMEOUT_CHK = 0x800834D4u;  // native-owned HLE, runtime/recomp/sync_overrides.cpp
}  // namespace

// func_80082734 (0x80082734) — libgpu LoadImage()-internal chunked GP0-FIFO pixel streamer.
// See file header for the full RE (struct map, control flow, dead-code note, confidence).
void Render::gpuLoadImageStream() {
  Core* c = mCore;
  c->r[29] -= 48;
  c->mem_w32(c->r[29] + 20, c->r[17]);
  const uint32_t rectPtr = c->r[4];   // s1
  c->mem_w32(c->r[29] + 24, c->r[18]);
  uint32_t srcPtr = c->r[5];          // s2 (advances as the PIO remainder loop consumes it)
  c->mem_w32(c->r[29] + 40, c->r[31]);
  c->mem_w32(c->r[29] + 36, c->r[21]);
  c->mem_w32(c->r[29] + 32, c->r[20]);
  c->mem_w32(c->r[29] + 28, c->r[19]);
  c->mem_w32(c->r[29] + 16, c->r[16]);

  auto epilogue = [&](uint32_t retVal) {
    c->r[2] = retVal;
    c->r[31] = c->mem_r32(c->r[29] + 40);
    c->r[21] = c->mem_r32(c->r[29] + 36);
    c->r[20] = c->mem_r32(c->r[29] + 32);
    c->r[19] = c->mem_r32(c->r[29] + 28);
    c->r[18] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 48;
  };

  rec_dispatch(c, FN_GPU_TIMEOUT_ARM);

  // --- clamp rect.w (rectPtr+4) against GPU_CLIP_MAXW, write back in place ---
  {
    int32_t wSigned = c->mem_r16s(rectPtr + 4);
    uint32_t wUnsigned = c->mem_r16(rectPtr + 4);
    uint32_t wClamped;
    if (wSigned < 0) {
      wClamped = 0;
    } else {
      int32_t maxWSigned = c->mem_r16s(GPU_CLIP_MAXW);
      uint32_t maxWUnsigned = c->mem_r16(GPU_CLIP_MAXW);
      wClamped = (maxWSigned < wSigned) ? maxWUnsigned : wUnsigned;
    }
    c->mem_w16(rectPtr + 4, (uint16_t)wClamped);
  }

  // --- clamp rect.h (rectPtr+6) against GPU_CLIP_MAXH, write back in place ---
  {
    int32_t hSigned = c->mem_r16s(rectPtr + 6);
    uint32_t hUnsigned = c->mem_r16(rectPtr + 6);
    uint32_t hClamped;
    if (hSigned < 0) {
      hClamped = 0;
    } else {
      int32_t maxHSigned = c->mem_r16s(GPU_CLIP_MAXH);
      uint32_t maxHUnsigned = c->mem_r16(GPU_CLIP_MAXH);
      hClamped = (maxHSigned < hSigned) ? maxHUnsigned : hUnsigned;
    }
    c->mem_w16(rectPtr + 6, (uint16_t)hClamped);
  }

  // pixelCount = clampedW * clampedH (both re-read post-clamp, sign-extended as s16, per gen body).
  int32_t wSignExt = c->mem_r16s(rectPtr + 4);
  int32_t hSignExt = c->mem_r16s(rectPtr + 6);
  int64_t product = (int64_t)wSignExt * (int64_t)hSignExt;
  uint32_t pixelCount = (uint32_t)(int32_t)product;  // mflo (lo 32 bits)
  // Mirror gen's `mult` side-effect on HI/LO (generated/shard_5.c:13707) — the streamer's own
  // algorithm only reads lo (pixelCount), but HI/LO are ABI registers that persist across calls;
  // MIRROR_VERIFY compares them, and downstream callees (gpuDmaQueueEnqueue's fn dispatch path)
  // inherit them. An earlier draft left them stale — the f389 SBS diverge root cause.
  c->lo = (uint32_t)product;
  c->hi = (uint32_t)((uint64_t)product >> 32);

  // numWords = ceil(pixelCount / 2) via the gen body's "(x+1) + sign-bit carry, then >>1" idiom.
  // NOTE the carry term is a LOGICAL >>31 in the gen (`c->r[3] >> 31` on uint32 — srl, adds 0 or 1),
  // NOT an arithmetic shift (an earlier transcription of this line had that wrong).
  uint32_t rounded = pixelCount + 1u;
  rounded = rounded + (rounded >> 31);
  int32_t numWords = (int32_t)rounded >> 1;
  int32_t chunkCountSigned = (int32_t)rounded >> 5;  // == numWords >> 4 (16-word chunks)

  if (!(numWords > 0)) { epilogue((uint32_t)-1); return; }

  const uint32_t chunkCount = (uint32_t)chunkCountSigned;
  uint32_t remainder = (uint32_t)numWords - chunkCount * 16u;

  // --- busy-wait (timeout-checked) for the GP0/DMA ready bit ---
  {
    uint32_t readyWord = c->mem_r32(c->mem_r32(GPU_DMA_FLAGS_PTR));
    if ((readyWord & GPU_READY_BIT) == 0) {
      for (;;) {
        rec_dispatch(c, FN_GPU_TIMEOUT_CHK);
        if (c->r[2] != 0) { epilogue((uint32_t)-1); return; }
        uint32_t rw = c->mem_r32(c->mem_r32(GPU_DMA_FLAGS_PTR));
        if ((rw & GPU_READY_BIT) != 0) break;
      }
    }
  }

  // --- push the fixed header words through the GP0 FIFO port ---
  c->mem_w32(c->mem_r32(GPU_DMA_FLAGS_PTR), GPU_READY_BIT);       // ack ready
  c->mem_w32(c->mem_r32(GPU_GP0_PORT_PTR), GP0_CLEAR_CACHE);      // GP0(0x01) ClearCache
  c->mem_w32(c->mem_r32(GPU_GP0_PORT_PTR), GP0_COPY_CPU_TO_VRAM); // GP0(0xA0) tag (see dead-code note)
  c->mem_w32(c->mem_r32(GPU_GP0_PORT_PTR), c->mem_r32(rectPtr + 0));  // dest X|Y, forwarded verbatim
  c->mem_w32(c->mem_r32(GPU_GP0_PORT_PTR), c->mem_r32(rectPtr + 4));  // clamped W|H

  // --- PIO-stream the `remainder` leftover words one at a time ---
  if (remainder != 0) {
    do {
      uint32_t word = c->mem_r32(srcPtr);
      srcPtr += 4;
      c->mem_w32(c->mem_r32(GPU_GP0_PORT_PTR), word);
      remainder--;
    } while (remainder != 0);
  }

  if (chunkCount == 0) { epilogue(0); return; }

  // --- hand the remaining chunkCount*16 words to the async GPU-DMA continuation (see file header) ---
  c->mem_w32(c->mem_r32(GPU_DMA_FLAGS_PTR), GPU_READY_BIT | 2u);        // 0x04000002
  c->mem_w32(c->mem_r32(GPU_DMA_ARG0_PTR), srcPtr);                     // start of chunk-DMA source
  c->mem_w32(c->mem_r32(GPU_DMA_ARG1_PTR), (chunkCount << 16) | 16u);   // chunkCount | burst size
  c->mem_w32(c->mem_r32(GPU_DMA_STATE_PTR), GP0_CLEAR_CACHE | 513u);    // 0x01000201

  epilogue(0);
}

// ==================================================================================================
// Wiring (2026-07-10): promoted from wide-RE draft to verified ownership per docs/fleet-workflow.md
// §9. Reached as a plain intra-shard C call (func_80082D04's fast-path `rec_dispatch(c, fn)` where fn
// resolves here — NOT via a jump table), so `engine_set_override_main` (runtime/recomp/
// override_registry.h, a thin forwarder over overrides::install) is the correct install path: its
// oracle-gated dispatch keeps SBS core B running the pure gen_func_80082734 body while core A
// dispatches to Render::gpuLoadImageStream().
namespace {
void ov_gpuLoadImageStream(Core* c) { rend(c)->gpuLoadImageStream(); }
}  // namespace

extern void gen_func_80082734(Core*);

void gpu_loadimage_streamer_install() {
  static bool done = false;
  if (done) return;
  done = true;
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x80082734u, ov_gpuLoadImageStream, gen_func_80082734);
}
