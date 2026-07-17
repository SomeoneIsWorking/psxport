// game/render/wide_re_libgpu_leaves.cpp — WIDE-RE DRAFT leaves of the libgpu "GPU sys" jump table
// documented in docs/engine_re.md ("Graphics pipeline — the REAL draw path (libgpu)"). The table
// lives at guest 0x800A5998 (32778<<16 + 22936); the per-frame loop FUN_80050b08 already names two
// of this file's addresses from that doc. Wide-RE tier (docs/fleet-workflow.md §6): UNWIRED /
// UNVERIFIED, hand-transliterated 1:1 from generated/shard_*.c gen_func_<addr> (ground truth — NOT
// mechanically diffed yet). Nothing here is called from anywhere (no overrides::install registration,
// no shard_set_override) — dead code that only needs to COMPILE. A wiring pass MUST re-diff every
// line against the generated C before registering + SBS-gating (per §9).
//
// Struct map confirmed this session (base = 0x800A0000, i.e. `(32778u<<16)`):
//   +22936 (0x5998)  GPU_SYS: fn-ptr jump table (per docs/engine_re.md: +0x08 DMA-send, +0x14
//                     DrawOTagEnv, +0x18 DrawOTag, +0x2C ClearOTagR, +0x3C DrawSync)
//   +22940 (0x599C)  GPU_SYS_INIT: a SECOND fn-ptr, called by DrawSync/ClearOTagR/PutDrawEnv only
//                     while the flag byte @+22946 is >= 2 (POLARITY CORRECTED 2026-07-10 — the
//                     original "< 2 / one-time init" reading was inverted; it's a steady-state
//                     "GPU sys is up" hook), not itself in the jump table.
//   +22944 (0x59A0)..+22950 (0x59A6): small ints/shorts read by func_80081FB0 (PutDrawEnv helper,
//                     NOT drafted this session — see MAP note below); +22946 is the same boot/reset
//                     flag byte DrawSync/ClearOTagR both gate on.
//   +23208 (0x5AA8)  GPU_DMA_FLAGS:  status/flags word pointer (indirect: the struct itself holds a
//                     POINTER, dereferenced before use) — bit 0x00000002 (1024u<<16 | 2, see
//                     gpuDmaQueueReset below) and bit 0x10000000 are tested by the queue cluster
//                     (0x80082D04/0x80082FB4/0x80083364/0x80082424 — NOT drafted, see MAP note).
//   +23212 (0x5AAC)  GPU_DMA_ARG0:   pointer, stores the caller's r4 arg (gpuDmaQueueReset).
//   +23216 (0x5AB0)  GPU_DMA_ARG1:   pointer, zeroed at reset.
//   +23220 (0x5AB4)  GPU_DMA_STATE:  pointer, flags word — reset writes (256u<<16 | 1025) = 0x01000401.
//   +23260 (0x5ADC), +23264 (0x5AE0): the SAME two fields runtime/recomp/sync_overrides.cpp's
//                     `gpu_timeout_arm`/`gpu_timeout_chk` already own as the libgpu GPU-DMA-completion
//                     TIMEOUT (arm/check) — CONFIRMS this whole struct is the libgpu OT-DMA-send
//                     status block, and the 0x80082D04 queue cluster is its interrupt/completion-
//                     callback ring buffer (64 slots, stride 0x60, base 0x80100000+0xC30). UPDATE
//                     (2026-07-10, dedicated deep-RE pass): DRAFTED — see
//                     game/render/wide_re_gpu_dma_queue.cpp and docs/engine_re.md.
//
// MAP-only this session (identified, NOT drafted — too large / too deep a callee chain):
//   0x800815D0 = PutDrawEnv (CONFIRMED identity, already named in docs/engine_re.md). Calls
//     func_80081FB0 (40-line struct-pack helper) which itself calls 5 more unowned leaves
//     (0x80082240, 0x800822D8, 0x80082370, 0x80082220, 0x8008238C) — a proper port needs those RE'd
//     first. Left for a dedicated frontier pass; this file only covers the two CONFIRMED single-leaf
//     table entries (DrawSync, ClearOTagR) plus the queue-reset helper.
//   0x80082D04, 0x80082FB4, 0x80083364, 0x80082424 — the GPU-DMA completion callback queue. UPDATE
//     (2026-07-10, dedicated deep-RE pass): DRAFTED in game/render/wide_re_gpu_dma_queue.cpp — see
//     that file's header for the corrected field map (this comment's head/tail addresses,
//     0x800A5A88/0x800A5A8C, were a wrong guess; ground truth is 0x800A5AC8/0x800A5ACC) and call
//     graph. 0x80082734 turned out NOT to be part of this cluster (a separate, larger LoadImage-style
//     FIFO streamer) — still MAPPED only, see the new file's header.
#include "core.h"
#include "game_ctx.h"
#include "render.h"
#include <stdint.h>

extern "C" void rec_dispatch(Core* c, uint32_t addr);

namespace {
constexpr uint32_t GPU_SYS_BASE = (32778u << 16);       // 0x800A0000
constexpr uint32_t GPU_SYS_TABLE = GPU_SYS_BASE + 22936; // 0x800A5998 — libgpu fn-ptr jump table
constexpr uint32_t GPU_SYS_INIT_FN = GPU_SYS_BASE + 22940; // 0x800A599C — one-time init hook fn-ptr
constexpr uint32_t GPU_BOOT_FLAG = GPU_SYS_BASE + 22946; // 0x800A59A2 — boot/reset flag byte
constexpr uint32_t GPU_DMA_FLAGS_PTR  = GPU_SYS_BASE + 23208; // 0x800A5AA8
constexpr uint32_t GPU_DMA_ARG0_PTR   = GPU_SYS_BASE + 23212; // 0x800A5AAC
constexpr uint32_t GPU_DMA_ARG1_PTR   = GPU_SYS_BASE + 23216; // 0x800A5AB0
constexpr uint32_t GPU_DMA_STATE_PTR  = GPU_SYS_BASE + 23220; // 0x800A5AB4
}  // namespace

// func_80080F6C (0x80080F6C) — DrawSync(mode). VERIFIED & WIRED 2026-07-10 (was DRAFT). RE'd from generated/shard_2.c gen_func_80080F6C (25 gen-C
// ln). CONFIRMED identity via docs/engine_re.md's per-frame-loop RE ("FUN_80080f6c(0) = DrawSync(0)
// // WAIT for previous frame's draw to finish"). Guest ABI: a0=mode (arg not read by this leaf body
// itself — passed straight through to the callee as the 2nd dispatch's a1).
//
// When the boot/reset flag byte @GPU_BOOT_FLAG is >= 2: call the hook GPU_SYS_INIT_FN with (a0 = a
// fixed BIOS-window constant 0x8001BEDC, a1 = mode). [POLARITY CORRECTED 2026-07-10 by the dedicated
// PutDrawEnv/streamer pass: the original draft had `< 2` — inverted. The raw gen-C
// (generated/shard_2.c gen_func_80080F6C) is `_t = (bootFlag < 2); if (_t) goto L_80080FA8;`, i.e.
// bootFlag<2 SKIPS the hook call; the call happens on fallthrough, when bootFlag>=2. ClearOTagR
// below and PutDrawEnv (wide_re_gpu_putdrawenv.cpp) have the same shape and polarity — all three
// now agree. The hook is therefore NOT a "first frames after reset" init but a "GPU sys is up"
// steady-state hook; naming updated accordingly.] Unconditionally after that: call
// GPU_SYS_TABLE[+60] (table+0x3C = DrawSync's OWN table slot per the doc) with (a0 = mode).
//
// RE-VERIFY CORRECTION (2026-07-10, wiring pass): the prior draft's "no stack frame, leaf, sp
// untouched" claim was WRONG — gen_func_80080F6C DOES push a -24 guest frame and spill s0/r16 +
// ra (generated/shard_2.c:10854-10858: `sp-=24; mem_w32(sp+16,r16); r16=a0; ...
// mem_w32(sp+20,ra)`), restored at both exits. Both call targets here (GPU_SYS_INIT_FN and the
// table+0x3C entry) are ARBITRARY dispatch targets that may push their own frames — if this leaf
// doesn't mirror gen's sp/ra, every downstream guest-stack write from those callees lands 24 bytes
// off from gen's, which is an immediate SBS-fatal divergence. Mirrored below (frame + both r31
// return-address literals, per CLAUDE.md "mirror the guest stack, never omit it").
void Render::drawSync() {
  Core* c = mCore;
  const uint32_t mode = c->r[4];
  c->r[29] -= 24;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = mode;
  c->mem_w32(c->r[29] + 20, c->r[31]);

  auto epilogue = [&]() {
    c->r[31] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 24;
  };

  uint8_t bootFlag = c->mem_r8(GPU_BOOT_FLAG);
  if (bootFlag >= 2) {
    c->r[4] = (32770u << 16) + (uint32_t)(int32_t)(-16676);  // 0x8001BEDC — fixed BIOS-window arg
    uint32_t initFn = c->mem_r32(GPU_SYS_INIT_FN);
    c->r[5] = c->r[16];
    c->r[31] = 0x80080FA8u;
    rec_dispatch(c, initFn);
  }
  // BUG FIX (2026-07-10, wiring re-verify): GPU_SYS_TABLE (0x800A5998) is a POINTER FIELD holding
  // the address of the real jump table — the gen body dereferences it TWICE
  // (generated/shard_2.c:10854 lines 16-18: `r2=mem_r32(base+22936); r2=mem_r32(r2+60)`), not once.
  // The prior draft read `mem_r32(GPU_SYS_TABLE + 60)` directly (single deref), which — before the
  // table is ever relocated/reallocated to match the raw base+22936 address — reads garbage
  // (observed: 0xFFFFFFFF / stale scratch like 0x0101000A) and dispatches into nowhere, corrupting
  // the whole downstream OT chain. GPU_SYS_INIT_FN below is NOT a pointer-to-pointer (single deref
  // is correct there — confirmed against gen, no second indirection on that field).
  uint32_t tableBase = c->mem_r32(GPU_SYS_TABLE);
  uint32_t tableSlot60 = c->mem_r32(tableBase + 60);  // table+0x3C, the DrawSync entry itself
  c->r[4] = c->r[16];
  c->r[31] = 0x80080FC4u;
  rec_dispatch(c, tableSlot60);
  epilogue();
}

// func_80081458 (0x80081458) — ClearOTagR(OT, entries). VERIFIED & WIRED 2026-07-10 (was DRAFT). RE'd from generated/shard_7.c gen_func_80081458
// (64 gen-C ln). CONFIRMED identity via docs/engine_re.md ("FUN_80081458=ClearOTagR (table+0x2c)";
// per-frame loop calls it as `FUN_80081458(ctx, 0x800)` = 2048 OT entries).
//
// NOTE: the guest C emission for this address contains a SECOND, unreachable-from-here prologue/
// epilogue pair after this function's `return` (a recompiler artifact — the shard groups adjacent
// guest code without a clean symbol boundary). That trailing block is a DIFFERENT, un-RE'd MIPS
// function reachable only via its own call sites (not via a call to 0x80081458) — NOT ported here.
//
// Guest ABI: a0=OT pointer, a1=entry count. Same boot-flag-gated hook pattern as DrawSync
// (hook gets a0=fixed const 0x8001BF68, a1=OT, a2=entryCount) — and the SAME polarity correction
// applies (2026-07-10, dedicated PutDrawEnv/streamer pass): the gen-C
// (generated/shard_7.c gen_func_80081458) is `_t = (bootFlag < 2); if (_t) goto L_800814A0;`, i.e.
// bootFlag<2 SKIPS the hook; the call happens when bootFlag>=2 (the original draft had this
// inverted). Then calls GPU_SYS_TABLE[+44] (table+0x2C, ClearOTagR's own slot) with
// (a0=OT, a1=entryCount) — presumably the real hardware-facing OT-clear loop, opaque to this leaf.
// AFTER that call, ClearOTagR additionally links *OT to a small shared "dummy tail packet"
// (classic libgpu ClearOTagR internal: every table build shares one small terminator/padding
// structure) — CONSTANTS CORRECTED 2026-07-10: the gen-C decimals are +23136/+23116 from base
// 0x800A0000, i.e. tail packet at **0x800A5A60** and tag content **0x800A5A4C** (the original
// draft's 0x800A5B20/0x800A5B0C was a decimal→hex conversion slip, off by 0xC0). Writes a tag word
// (0x04000000 | (0x800A5A4C & 0x00FFFFFF)) to 0x800A5A60, then sets *OT = (0x800A5A60 &
// 0x00FFFFFF). Transcribed as literal constant-folded values (the gen body computes on raw
// addresses-as-integers, not memory reads through pointers, for this whole tail — no dereference).
// Frame -32, spills ra/s17/s16 at +24/+20/+16 (s16=OT ptr kept live across the hook call,
// s17=entryCount).
void Render::clearOTagR() {
  Core* c = mCore;
  c->r[29] -= 32;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = c->r[4];                       // s16 = OT
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->r[17] = c->r[5];                       // s17 = entryCount
  uint8_t bootFlag = c->mem_r8(GPU_BOOT_FLAG);
  c->mem_w32(c->r[29] + 24, c->r[31]);  // ra spill happens unconditionally (branch-delay-slot write)
  if (bootFlag >= 2) {
    c->r[4] = (32770u << 16) + (uint32_t)(int32_t)(-16536);  // 0x8001BF68 — fixed BIOS-window arg
    c->r[5] = c->r[16];
    uint32_t initFn = c->mem_r32(GPU_SYS_INIT_FN);
    c->r[6] = c->r[17];
    c->r[31] = 0x800814A0u;
    rec_dispatch(c, initFn);
  }
  // Same missing-indirection bug as DrawSync above (fixed 2026-07-10): GPU_SYS_TABLE must be
  // dereferenced once to get the table's real base, THEN +44 dereferenced again (generated/
  // shard_7.c:12284 lines 19-22: `r2=mem_r32(base+22936); r2=mem_r32(r2+44)`).
  uint32_t tableBase = c->mem_r32(GPU_SYS_TABLE);
  uint32_t tableSlot44 = c->mem_r32(tableBase + 44);  // table+0x2C, ClearOTagR's own entry
  c->r[4] = c->r[16];
  c->r[5] = c->r[17];
  c->r[31] = 0x800814BCu;
  rec_dispatch(c, tableSlot44);

  const uint32_t mask24 = (255u << 16) | 65535u;  // 0x00FFFFFF
  constexpr uint32_t kDummyTagAddr = 0x800A0000u + 23136u;     // 0x800A5A60 (gen-C decimal, CORRECTED — see header)
  constexpr uint32_t kDummyTagContent = 0x800A0000u + 23116u;  // 0x800A5A4C (gen-C decimal, CORRECTED)
  uint32_t tag = (kDummyTagContent & mask24) | (1024u << 16);  // 0x04000000 | low24
  c->mem_w32(kDummyTagAddr, tag);
  c->mem_w32(c->r[16], kDummyTagAddr & mask24);  // *OT = low24(0x800A5A60)
  // gen publishes v0 (r2) = s16 = the OT pointer (arg r4) — its final r2 after `r2 = r16+0` right
  // before the *OT store. The prior draft left r2 stale (MIRROR_VERIFY: native=0x800
  // substrate=<OT ptr e.g. 0x800E80A8>) because it wrote *OT via c->r[16] without also publishing r2.
  // gen also publishes v1 (r3) = the dummy-tag word itself (r3 = low24(content)|0x04000000) — its
  // final r3 after `r3 = r3|r4` (MIRROR_VERIFY: substrate=0x040A5A4C). Native left it stale.
  c->r[2] = c->r[16];
  c->r[3] = tag;

  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 32;
}

// ==================================================================================================
// Wiring (2026-07-10): promoted DrawSync/ClearOTagR from wide-RE draft to verified ownership per
// docs/fleet-workflow.md §9. Re-verify found TWO real bugs, both fixed above:
//   (1) DrawSync had a MISSING STACK FRAME — the draft claimed "no stack frame, leaf, sp untouched"
//       but gen_func_80080F6C actually pushes a -24 frame and spills s0/r16+ra; every downstream
//       guest-stack write from its 2 dispatch targets would have landed 24 bytes off from gen.
//   (2) BOTH DrawSync and ClearOTagR read `GPU_SYS_TABLE + offset` with only ONE dereference —
//       GPU_SYS_TABLE (0x800A5998) is actually a POINTER FIELD to the real jump table, and gen
//       dereferences it TWICE (`r2=mem_r32(base+22936); r2=mem_r32(r2+60)`). The single-deref
//       version read garbage (0xFFFFFFFF / stale scratch) and dispatched into nowhere, corrupting
//       the entire downstream OT chain (frame-0 SBS: 6109+ RAM bytes diverged, then a
//       render-queue-overflow crash within a few frames). Root-caused via PSXPORT_THUNK_FORCE_GEN
//       bisection + a temporary debug print, NOT by staring at the RE.
// Both are substrate-called leaves (plain intra-shard C calls, not rec_dispatch) — wired via the
// oracle-gated engine_set_override_main thunk so SBS core B keeps running the pure gen_func_* body.
// OVHIT (PSXPORT_DEBUG=ovhit, 5-frame REPL run): both FIRE with MATCHING native/oracle counts —
// `0x80080F6C native=1045 oracle=1045`, `0x80081458 native=1020 oracle=1020` — real gate coverage,
// not a "0-diff because never called" false positive.
// The other 3 functions in this file (func_80082C68/80083DE0/800847B0) are OUT OF SCOPE for this
// wiring pass — left as unwired wide-RE drafts.
namespace {
void ov_drawSync(Core* c)    { rend(c)->drawSync(); }
void ov_clearOTagR(Core* c)  { rend(c)->clearOTagR(); }
}  // namespace

extern void gen_func_80080F6C(Core*);
extern void gen_func_80081458(Core*);

void gpu_libgpu_leaves_install() {
  static bool done = false;
  if (done) return;
  done = true;
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x80080F6Cu, ov_drawSync,   gen_func_80080F6C);
  engine_set_override_main(0x80081458u, ov_clearOTagR, gen_func_80081458);
}

// func_80082C68 (0x80082C68) — GPU-DMA status-block RESET. DRAFT. RE'd from generated/shard_2.c gen_func_80082C68
// (19 gen-C ln, no branches, no calls — fully self-contained). Not itself a GPU_SYS_TABLE entry
// (no table dereference); writes the same status-block fields the 0x80082D04 completion-queue
// cluster (MAPPED, not drafted — see file header) tests every call. Guest ABI: a0 = an opaque
// pointer the caller owns (stashed verbatim into GPU_DMA_ARG0_PTR's target — this leaf never reads
// through it, just stores it for the queue cluster to consume later).
//
// Writes: *GPU_DMA_FLAGS_PTR = (1024u<<16) | 2  (0x04000002); *GPU_DMA_ARG0_PTR = a0;
// *GPU_DMA_ARG1_PTR = 0; *GPU_DMA_STATE_PTR = (256u<<16) | 1025  (0x01000401).
static void func_80082C68(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t flagsPtr = c->mem_r32(GPU_DMA_FLAGS_PTR);
  c->mem_w32(flagsPtr, (1024u << 16) | 2u);
  uint32_t arg0Ptr = c->mem_r32(GPU_DMA_ARG0_PTR);
  c->mem_w32(arg0Ptr, a0);
  uint32_t arg1Ptr = c->mem_r32(GPU_DMA_ARG1_PTR);
  c->mem_w32(arg1Ptr, 0);
  uint32_t statePtr = c->mem_r32(GPU_DMA_STATE_PTR);
  c->mem_w32(statePtr, (256u << 16) | 1025u);
}

// func_80083DE0 (0x80083DE0) — libgpu draw-mode / texture-window PACKET-HEADER builder. DRAFT. RE'd from
// generated/shard_0.c gen_func_80083DE0 (39 gen-C ln, fully self-contained — no calls, no
// sub-dispatch, NO stack frame — leaf, sp untouched). LOW-MEDIUM confidence on the exact SCEI name
// (not cross-referenced against a debug string this session), but the shape is unambiguous: it
// builds TWO raw GP0 command words matching the top-byte tags of libgpu's DR_TPAGE (0xE1) and
// DR_TWIN (0xE2) commands — i.e. this is the "SetDrawTPage"/"SetTexWindow"-style low-level
// packet-header packer libgpu's poly/sprite primitive setters call internally (same family as the
// PutDrawEnv/DrawOTag cluster this file otherwise covers).
//
// Guest ABI: a0(r4)=dst (12 B out: +3 tag byte, +4 draw-mode word, +8 texwin word), a1(r5)=rgbBitsSrc
// (bits 0x9FF of this land in the mode word's low bits, +0x400 added when a1!=0 — NOT itself a
// bool, the VALUE's low bits are used AND its zero-ness gates the extra bit), a2(r6)=modeFlag (bool:
// non-zero ORs 0x200 into the mode word), a3(r7)=UNUSED by this leaf (register alias only, verified:
// the gen body never reads r7). 5th arg = the STACK slot at incoming sp+16 (o32 ABI outgoing-arg
// convention; this leaf never adjusts sp, so `c->r[29]+16` is the correct read) = texWinSrc pointer,
// 0 = "no texture window" (writes +8 = 0 instead of building the DR_TWIN word).
//
// dst+3 is UNCONDITIONALLY set to the constant 2 (both branch arms write it before the branch
// decision — a delay-slot artifact, not conditional). mode = 0xE1000000 [| 0x200 if a2!=0] |
// (a1 & 0x9FF) [| 0x400 if a1!=0], always written to dst+4. texWinSrc==0 short-circuits straight to
// writing 0 at dst+8; otherwise builds the DR_TWIN word from texWinSrc's 4 fields (2 mask bytes @+0/
// +2, 2 signed 16-bit offsets @+4/+6, PSX texture-window encoding: (maskX>>3)<<10 | (maskY>>3)<<15 |
// ((-offY)<<2 & 0x3E0) | ((uint8_t)(-offX) >> 3)).
static void func_80083DE0(Core* c) {
  uint32_t dst = c->r[4];
  uint32_t a1 = c->r[5];
  uint32_t a2 = c->r[6];
  uint32_t texWinSrc = c->mem_r32(c->r[29] + 16);  // 5th arg, o32 outgoing-arg stack slot

  c->mem_w8(dst + 3, 2u);  // unconditional (both branch arms write this literal before branching)

  uint32_t mode = (57600u << 16);           // 0xE1000000 — DR_TPAGE tag
  if (a2 != 0) mode |= 512u;
  uint32_t rgbBits = a1 & 2559u;             // 0x9FF
  if (a1 != 0) rgbBits |= 1024u;             // 0x400
  mode |= rgbBits;
  c->mem_w32(dst + 4, mode);

  if (texWinSrc == 0) {
    c->mem_w32(dst + 8, 0);
    return;
  }
  uint32_t twin = (57856u << 16);           // 0xE2000000 — DR_TWIN tag
  uint32_t maskY = c->mem_r8(texWinSrc + 2);
  uint32_t maskX = c->mem_r8(texWinSrc + 0);
  twin |= (maskY >> 3) << 15;
  twin |= (maskX >> 3) << 10;
  int32_t offY = c->mem_r16s(texWinSrc + 6);
  int32_t offX = c->mem_r16s(texWinSrc + 4);
  uint32_t negOffY = (uint32_t)(0 - offY);
  negOffY = (negOffY << 2) & 992u;           // 0x3E0
  twin |= negOffY;
  uint32_t negOffX = (uint32_t)(0 - offX);
  negOffX &= 255u;
  negOffX = (uint32_t)((int32_t)negOffX >> 3);
  twin |= negOffX;
  c->mem_w32(dst + 8, twin);
}

// func_800847B0 (0x800847B0) — 20-byte SoA->AoS vertex-header REPACK. DRAFT. RE'd from generated/shard_4.c
// gen_func_800847B0 (18 gen-C ln, fully self-contained — no calls, no branches). LOW confidence on
// semantic name: the shape is a fixed 5-word struct copy from a0 to a1 with fields 0/1 swapped and
// three of the five words additionally overwritten in their LOW 16 bits by a value taken from a
// DIFFERENT source word — i.e. it repacks a {u32,u32,u32,u32,s16} source into a differently-ordered
// destination where three fields are (high16 old, low16 new). This is the same "pack two logical
// halfwords into one word" idiom used throughout the GT3/GT4 packet builders (game/render/
// overlay_gt3gt4.cpp, overlay_ground_gt3gt4.cpp) — plausibly a shared vertex/UV-pair repacker for
// that family, but NOT confirmed against a caller this session (no direct caller found in
// generated/shard_*.c; reached only via rec_dispatch, consistent with the free-roam dispatch count).
// Guest ABI: a0=src (20 B), a1=dst (20 B); no return value read by any caller pattern seen.
static void func_800847B0(Core* c) {
  uint32_t src = c->r[4];
  uint32_t dst = c->r[5];

  uint32_t w0 = c->mem_r32(src + 0);
  uint32_t w1 = c->mem_r32(src + 4);
  c->mem_w32(dst + 4, w0);
  c->mem_w32(dst + 0, w1);
  c->mem_w16(dst + 0, (uint16_t)w0);

  uint32_t w2 = c->mem_r32(src + 8);
  uint32_t w3 = c->mem_r32(src + 12);
  c->mem_w32(dst + 12, w2);
  c->mem_w32(dst + 8, w3);
  c->mem_w16(dst + 12, (uint16_t)w1);
  c->mem_w16(dst + 8, (uint16_t)w2);

  uint32_t h4 = (uint32_t)(int32_t)c->mem_r16s(src + 16);
  c->mem_w16(dst + 4, (uint16_t)w3);
  c->mem_w16(dst + 16, (uint16_t)h4);
}
