// objlist_walk.cpp — SUBSTRATE MIRROR for the 4 still-substrate OBJECT-LIST WALKERS reached from the
// field draw dispatcher FUN_8003F9A8 (docs/findings/render.md "0x8003F9A8 474-prim attribution
// resolved"): FUN_8003BB50, FUN_8003BCF4 (+ its shared-tail split FUN_8003BED8), FUN_8003BF00,
// FUN_8003EEC0. Everything BELOW them (FUN_8003CCA4/C2D4/C464 = Render::perObjRenderDispatch/
// billboardCompose1/billboardCompose2) is already native — these 4 are the last unowned hop between
// the field dispatcher and that already-owned chain, so the 474 prims the otattr shadow stack was
// mis-crediting to FUN_8003F9A8 attribute correctly once these are owned.
//
// RE method: generated/*.c (the recompiler's translation) is ground truth, used DIRECTLY (not
// Ghidra's pseudo-C, which — for FUN_8003BCF4 specifically — mis-portrays a genuine cross-function
// tail-call-continuation split as an ordinary single-function do-while loop; see the FUN_8003BCF4/
// FUN_8003BED8 banner below for the full account). Cross-checked against scratch/decomp/otattr_subs.c
// (Ghidra headless dump) for the case-value semantics, which the recompiler's own switch tables (read
// as REAL indirect jump-table data at each function's fixed table address) independently confirm.
// All 5 addresses confirmed unowned via tools/codemap.py before porting.
//
// COMMON SHAPE (BB50/BF00/EEC0 — self-contained single-function loops):
//   Each walks a fixed list of guest object pointers (an array for BB50/BCF4/BF00, a genuine
//   node+0x24-linked chain for EEC0), skipping dead entries (mem8(ptr+1)==0) and out-of-range TYPE
//   bytes (mem8(ptr+0xb)), and for every live in-range entry reads a REAL indirect jump table (fixed
//   guest address, static ROM data) to get a target address, then switches on that target: known
//   local labels run this function's OWN per-object work (mostly calling into the already-native
//   perObjRenderDispatch/billboardCompose1/billboardCompose2, or a still-substrate leaf via a plain
//   func_XXXX(c) call, exactly as gen does); an unrecognized target hits the recompiler's own
//   defensive fallback (`rec_dispatch(c, target); return;` — a full return bypassing the frame
//   epilogue, never hit by live game data since the live table only ever holds the enumerated case
//   values). BB50/BCF4 additionally maintain a per-field-frame "already refreshed this frame" cursor
//   pair in scratchpad (flag @0x1F800136, shared by all list walkers) that snapshots the list's
//   current head/count into a scratchpad working pair on the FIRST walker call each field frame and
//   leaves it alone on subsequent calls within the same frame — reproduced verbatim below (mem_r16/
//   mem_w16/mem_r32/mem_w32 at the literal scratchpad offsets; no magic constants, every offset is the
//   literal `generated/*.c` operand).
//
// FUN_8003BCF4 / FUN_8003BED8 — genuine two-function split, NOT flattened into one native loop:
//   gen_func_8003BCF4 processes ONLY the walk's FIRST live-and-in-range object, then either (a) hands
//   off to gen_func_8003BED8 (a plain C call — `func_8003BED8(c); return;`) to continue the walk over
//   the REST of the list, or (b) rec_dispatch's the resolved table target directly and returns
//   immediately WITHOUT popping its own 40-byte guest frame. Many OTHER still-substrate leaves this
//   table can resolve to (e.g. gen_func_8003BEA4/8003BEB4, generated/shard_1.c/shard_5.c) themselves
//   end by calling `func_8003BED8(c)` — i.e. FUN_8003BED8 is an independently guest-reachable
//   "continue the walk" trampoline, not private plumbing FUN_8003BCF4 alone uses. So it MUST be owned
//   at its OWN address too: any still-substrate leaf that tail-calls into it needs to land on the SAME
//   native continuation, not a copy. FUN_8003BED8's own body only pops the shared 40-byte frame at the
//   point the walk's remaining count reaches 0 (or a recognized dispatch, which — like FUN_8003BCF4's
//   own recognized-case arm — returns immediately without popping, trusting the target to eventually
//   re-enter FUN_8003BED8 to keep going and pop when the list is finally exhausted). The two native
//   methods below reproduce this exactly: objListWalk2 does a MANUAL (non-RAII) frame push and never
//   pops it itself; objListWalk2Continue does the manual pop, and ONLY there. Both read/write the
//   walk's live loop state (list cursor r18, remaining count r17, table base r20) through c->r[] itself
//   — never a C++ local — so the register-faithfulness a still-substrate leaf's own prologue spill
//   depends on (same class of bug as perobj_dispatch.cpp's CmdListFrame banner / f62 residual) survives
//   the native<->substrate boundary in either direction.
#include "core.h"
#include "game_ctx.h"
#include "game.h"
#include "render.h"
#include <cstdint>

void rec_dispatch(Core*, uint32_t);          // overlay_router.cpp — shared choke point for owned/substrate leaves
void func_8002AE0C(Core*);   // still-substrate: BB50's "flash"/highlight sub-dispatch (a0=cmd,a1=arg,a2=0)
void func_8003C5F8(Core*);   // still-substrate: BB50/BF00 case leaf
void func_8003C788(Core*);   // still-substrate: BB50/BF00 case leaf
void func_8004CC88(Core*);   // still-substrate: BF00's default-mode leaf
void func_8003B704(Core*);   // still-substrate: EEC0's case-1/0x10 shared tail
void shard_set_override(uint32_t addr, OverrideFn fn);   // generated/shard_disp.c (C++ linkage)

// gen_func_* fallbacks for the oracle-gated thunk — SBS core B (the pure oracle) must keep running the
// real recompiled body; see render_walk_dispatch.cpp's identical banner for the full rationale.
extern void gen_func_8003BB50(Core*);
extern void gen_func_8003BCF4(Core*);
extern void gen_func_8003BED8(Core*);
extern void gen_func_8003BF00(Core*);
extern void gen_func_8003EEC0(Core*);

namespace {
// Shared "already refreshed this field frame" flag (BB50/BCF4 both gate their cursor-refresh on it;
// some OTHER still-substrate walker not yet owned by this cluster clears it once per field frame).
constexpr uint32_t FRAME_FRESH_FLAG = 0x1F800136u;

// ---- FUN_8003BB50 -------------------------------------------------------------------------------
constexpr uint32_t W1_PTR_A  = 0x1F80013Cu;   // persistent cursor pointer (reset to LIST_HEAD each frame)
constexpr uint32_t W1_CNT_A  = 0x1F800144u;   // persistent cursor count  (reset to 0 each frame)
constexpr uint32_t W1_PTR_B  = 0x1F800140u;   // this-call working pointer (= old W1_PTR_A on refresh)
constexpr uint32_t W1_CNT_B  = 0x1F800146u;   // this-call working count   (= old W1_CNT_A on refresh)
constexpr uint32_t W1_LIST_HEAD = 0x800F2410u;
constexpr uint32_t W1_TABLE     = 0x80014A70u;   // 144-entry (idx<144) target-address table

// ---- FUN_8003BCF4 / FUN_8003BED8 ----------------------------------------------------------------
constexpr uint32_t W2_PTR_A  = 0x1F800148u;
constexpr uint32_t W2_CNT_A  = 0x1F800150u;
constexpr uint32_t W2_PTR_B  = 0x1F80014Cu;
constexpr uint32_t W2_CNT_B  = 0x1F800152u;
constexpr uint32_t W2_LIST_HEAD = 0x800F26C8u;
constexpr uint32_t W2_TABLE     = 0x80014CB0u;   // 33-entry (idx<33) target-address table

// ---- FUN_8003BF00 --------------------------------------------------------------------------------
constexpr uint32_t W3_PTR_A  = 0x1F800154u;
constexpr uint32_t W3_CNT_A  = 0x1F80015Cu;
constexpr uint32_t W3_PTR_B  = 0x1F800158u;
constexpr uint32_t W3_CNT_B  = 0x1F80015Eu;
constexpr uint32_t W3_LIST_HEAD = 0x800F2738u;
constexpr uint32_t W3_TABLE     = 0x80014D38u;   // 32-entry (idx<32) target-address table
constexpr uint32_t W3_MODE_BYTE = 0x800BF870u;   // render-mode-select byte (shared with perModeDispatch)

// ---- FUN_8003EEC0 --------------------------------------------------------------------------------
constexpr uint32_t W4_LIST_HEAD_VAR = 0x800F2738u;   // *this = head-of-chain object pointer (SAME
                                                       // storage FUN_8003BF00 treats as an array base —
                                                       // here dereferenced ONCE for the chain head)
constexpr uint32_t W4_TABLE = 0x80015000u;            // 33-entry (idx<33) target-address table
} // namespace

// ===================================================================================================
// FUN_8003BB50 (Render::objListWalk1) — no args (guest ABI).
// ORACLE: gen_func_8003BB50
void Render::objListWalk1() {
  Core* c = mCore;
  // Real -40 guest frame (RE: gen_func_8003BB50 prologue) — spills r16/r17/r18/r19/ra.
  const uint32_t s16 = c->r[16], s17 = c->r[17], s18 = c->r[18], s19 = c->r[19], sra = c->r[31];
  c->r[29] -= 40;
  c->mem_w32(c->r[29] + 32, sra); c->mem_w32(c->r[29] + 28, s19); c->mem_w32(c->r[29] + 24, s18);
  c->mem_w32(c->r[29] + 20, s17); c->mem_w32(c->r[29] + 16, s16);

  if (c->mem_r8(FRAME_FRESH_FLAG) == 0) {
    const uint16_t oldCnt = c->mem_r16(W1_CNT_A);
    const uint32_t oldPtr = c->mem_r32(W1_PTR_A);
    c->mem_w16(W1_CNT_A, 0);
    c->mem_w32(W1_PTR_A, W1_LIST_HEAD);
    c->mem_w16(W1_CNT_B, oldCnt);
    c->mem_w32(W1_PTR_B, oldPtr);
  }
  // Live loop state lives in c->r[] itself (register-faithfulness — see CLAUDE.md "MIRROR THE GUEST
  // STACK"): r17=remaining count, r18=list cursor pointer, r16=current object ptr, r19=table base
  // (loop-invariant, set once). gen keeps all four LIVE in the real callee-saved registers across every
  // nested dispatch; the still-substrate leaves this loop reaches (func_8002AE0C, and the rec_dispatch
  // vtable targets) SPILL them to their own guest-stack frames as "caller state". Keeping them only in
  // C++ locals was a real, reproducible bug: gen's r19=0x80014A70 (the table base) was being spilled by
  // a downstream leaf as native's stale 0 — the exact SBS diff at 0x801FE8C4/E4 (A=0 B=80014A70),
  // f119..f156, healed once r19 is set here. (Found via bisected SBS-full; baseline forced-gen = 0-diff.)
  c->r[17] = (uint32_t)(int16_t)c->mem_r16(W1_CNT_B);
  c->r[18] = c->mem_r32(W1_PTR_B);
  if (c->r[17] == 0u) goto epilogue;
  c->r[19] = W1_TABLE;

  while (c->r[17] != 0u) {
    c->r[16] = c->mem_r32(c->r[18]); c->r[18] += 4; c->r[17]--;
    const uint32_t cmd = c->r[16];
    if (c->mem_r8(cmd + 1) == 0) continue;
    const uint32_t type = c->mem_r8(cmd + 0xB);
    if (type >= 144u) continue;
    const uint32_t target = c->mem_r32(c->r[19] + type * 4u);
    c->r[4] = cmd;
    switch (target) {
      case 0x8003BC00u: {
        c->r[31] = 0x8003BC08u; perObjRenderDispatch();
        const uint32_t attr = c->mem_r8(cmd + 0xB);
        if ((attr & 0x40u) == 0u) {
          if ((attr & 0x80u) == 0u) break;
          c->r[4] = cmd; c->r[5] = (uint32_t)c->mem_r16s(cmd + 0x80u);
        } else {
          c->r[4] = cmd; c->r[5] = 0x50u;
        }
        c->r[31] = 0x8003BC64u; c->r[6] = 0u; func_8002AE0C(c);
        break;
      }
      case 0x8003BC24u: {
        c->r[31] = 0x8003BC2Cu; rec_dispatch(c, 0x80122974u);
        const uint32_t attr = c->mem_r8(cmd + 0xB);
        if ((attr & 0x40u) == 0u) {
          if ((attr & 0x80u) == 0u) break;
          c->r[4] = cmd; c->r[5] = (uint32_t)c->mem_r16s(cmd + 0x80u);
        } else {
          c->r[4] = cmd; c->r[5] = 0x50u;
        }
        c->r[31] = 0x8003BC64u; c->r[6] = 0u; func_8002AE0C(c);
        break;
      }
      case 0x8003BC6Cu: c->r[31] = 0x8003BC74u; billboardCompose1(); break;
      case 0x8003BC7Cu: c->r[31] = 0x8003BC84u; billboardCompose2(); break;
      case 0x8003BC8Cu: c->r[31] = 0x8003BC94u; func_8003C5F8(c); break;
      case 0x8003BC9Cu: c->r[31] = 0x8003BCA4u; func_8003C788(c); break;
      case 0x8003BCACu:
        c->r[31] = 0x8003BCB4u; billboardCompose1();
        [[fallthrough]];   // gen: L_8003BCAC falls straight into L_8003BCB4 (no separate advance)
      case 0x8003BCB4u: {
        // L_8003BCB4 is ALSO a directly-reachable switch target (Ghidra's case 0x16, vtable+0x7C
        // WITHOUT the preceding billboardCompose1 call BCAC/case-0x15 makes) — a genuinely separate
        // table entry, not merely BCAC's fallthrough tail. Read cmd+124 either way.
        const uint32_t vt = c->mem_r32(cmd + 124u);
        c->r[31] = 0x8003BCD0u; c->r[4] = cmd; rec_dispatch(c, vt);
        break;
      }
      case 0x8003BCC0u: {
        const uint32_t vt = c->mem_r32(cmd + 24u);
        c->r[31] = 0x8003BCD0u; c->r[4] = cmd; rec_dispatch(c, vt);
        break;
      }
      case 0x8003BCD0u:
        break;   // no-op table entry: skip (matches gen's dedicated loop-continue case value)
      default:
        // Defensive mirror of the recompiler's own indirect-jump fallback (generated/shard_1.c:5835's
        // `default: rec_dispatch(c, c->r[2]); return;`) — a full RETURN bypassing the frame epilogue.
        // Never hit by live game data: the live 144-slot table only ever holds the case values above.
        rec_dispatch(c, target);
        return;
    }
  }
epilogue:
  c->r[31] = c->mem_r32(c->r[29] + 32); c->r[19] = c->mem_r32(c->r[29] + 28);
  c->r[18] = c->mem_r32(c->r[29] + 24); c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16); c->r[29] += 40;
}

// ===================================================================================================
// FUN_8003BCF4 (Render::objListWalk2) — processes only the FIRST live/in-range object; hands the rest
// of the walk off to objListWalk2Continue (see the file banner). MANUAL (non-RAII) frame push: the
// pop happens in objListWalk2Continue, possibly several rec_dispatch hops later.
// ORACLE: gen_func_8003BCF4
void Render::objListWalk2() {
  Core* c = mCore;
  // Real -40 guest frame (RE: gen_func_8003BCF4 prologue) — spills r16/r17/r18/r19/r20/ra. Pushed here,
  // popped ONLY by objListWalk2Continue (see banner) — never in this function.
  const uint32_t s16 = c->r[16], s17 = c->r[17], s18 = c->r[18], s19 = c->r[19], s20 = c->r[20], sra = c->r[31];
  c->r[29] -= 40;
  c->mem_w32(c->r[29] + 36, sra); c->mem_w32(c->r[29] + 32, s20); c->mem_w32(c->r[29] + 28, s19);
  c->mem_w32(c->r[29] + 24, s18); c->mem_w32(c->r[29] + 20, s17); c->mem_w32(c->r[29] + 16, s16);

  if (c->mem_r8(FRAME_FRESH_FLAG) == 0) {
    const uint16_t oldCnt = c->mem_r16(W2_CNT_A);
    const uint32_t oldPtr = c->mem_r32(W2_PTR_A);
    c->mem_w16(W2_CNT_A, 0);
    c->mem_w32(W2_PTR_A, W2_LIST_HEAD);
    c->mem_w16(W2_CNT_B, oldCnt);
    c->mem_w32(W2_PTR_B, oldPtr);
  }
  // Live loop state lives in c->r[] itself (register-faithfulness — objListWalk2Continue and every
  // still-substrate leaf this table can resolve to reads/spills these as real registers, not a C++
  // local): r16=current object pointer, r17=remaining count, r18=list cursor pointer, r20=table base
  // (constant, set once here). CRITICAL: unlike objListWalk1/3/4's case handlers (which explicitly set
  // c->r[4]=object before every call), gen_func_8003BCF4/BED8's rec_dispatch NEVER sets r4 — dispatched
  // targets (e.g. gen_func_8003BEA4, `c->r[4] = c->r[16] + c->r[0]; func_8003C464(c); ...`) read the
  // object pointer straight out of r16. Keeping it only in a C++ local here (not c->r[16]) was a real
  // bug: those still-substrate leaves picked up whatever STALE value happened to be in c->r[16] instead
  // — found via a bisected SBS-full run (BB50/BF00/EEC0 alone: 0-diff; BCF4/BED8 alone: ~27KB/frame
  // packet-pool divergence from f180 on).
  c->r[17] = (uint32_t)(int16_t)c->mem_r16(W2_CNT_B);
  c->r[18] = c->mem_r32(W2_PTR_B);
  c->r[19] = 0x800C0000u;   // RE'd: gen sets this loop-invariant constant too (unused by this function's
                             // own body but callee-save-live for downstream substrate leaves).
  c->r[20] = W2_TABLE;

  if (c->r[17] == 0u) { objListWalk2Continue(); return; }   // count already 0: BED8 pops immediately
  c->r[16] = c->mem_r32(c->r[18]); c->r[18] += 4; c->r[17]--;
  const uint32_t cmd = c->r[16];
  if (c->mem_r8(cmd + 1) == 0u) { objListWalk2Continue(); return; }
  const uint32_t type = c->mem_r8(cmd + 0xB);
  if (type >= 33u) { objListWalk2Continue(); return; }
  const uint32_t target = c->mem_r32(c->r[20] + type * 4u);
  rec_dispatch(c, target);   // NOTE: on return, the walk is fully consumed and the shared frame already
                              // popped (target's own tail eventually reaches objListWalk2Continue).
}

// FUN_8003BED8 (Render::objListWalk2Continue) — the walk's shared "process the rest of the list, pop
// the shared frame when done" tail. Independently guest-reachable (see file banner): several other
// still-substrate leaves the type table can resolve to end by calling func_8003BED8(c) directly.
// ORACLE: gen_func_8003BED8
void Render::objListWalk2Continue() {
  Core* c = mCore;
  for (;;) {
    if (c->r[17] == 0u) {
      c->r[31] = c->mem_r32(c->r[29] + 36); c->r[20] = c->mem_r32(c->r[29] + 32);
      c->r[19] = c->mem_r32(c->r[29] + 28); c->r[18] = c->mem_r32(c->r[29] + 24);
      c->r[17] = c->mem_r32(c->r[29] + 20); c->r[16] = c->mem_r32(c->r[29] + 16);
      c->r[29] += 40;
      return;
    }
    c->r[16] = c->mem_r32(c->r[18]); c->r[18] += 4; c->r[17]--;
    const uint32_t cmd = c->r[16];   // register-faithfulness: see objListWalk2's banner — dispatched
                                       // targets read the object pointer from r16, never r4.
    if (c->mem_r8(cmd + 1) == 0u) continue;   // skip: loop
    const uint32_t type = c->mem_r8(cmd + 0xB);
    if (type >= 33u) continue;                // out of range: loop
    const uint32_t target = c->mem_r32(c->r[20] + type * 4u);
    rec_dispatch(c, target);   // recognized: dispatch and return WITHOUT popping — target's own tail
    return;                    // will re-enter objListWalk2Continue (via func_8003BED8) to keep going.
  }
}

// ===================================================================================================
// FUN_8003BF00 (Render::objListWalk3) — no args (guest ABI).
// ORACLE: gen_func_8003BF00
void Render::objListWalk3() {
  Core* c = mCore;
  // Real -32 guest frame (RE: gen_func_8003BF00 prologue) — spills r16/r17/r18/ra.
  const uint32_t s16 = c->r[16], s17 = c->r[17], s18 = c->r[18], sra = c->r[31];
  c->r[29] -= 32;
  c->mem_w32(c->r[29] + 28, sra); c->mem_w32(c->r[29] + 24, s18);
  c->mem_w32(c->r[29] + 20, s17); c->mem_w32(c->r[29] + 16, s16);

  if (c->mem_r8(FRAME_FRESH_FLAG) == 0) {
    const uint16_t oldCnt = c->mem_r16(W3_CNT_A);
    const uint32_t oldPtr = c->mem_r32(W3_PTR_A);
    c->mem_w16(W3_CNT_A, 0);
    c->mem_w32(W3_PTR_A, W3_LIST_HEAD);
    c->mem_w16(W3_CNT_B, oldCnt);
    c->mem_w32(W3_PTR_B, oldPtr);
  }
  // Live loop state in c->r[] (register-faithfulness, same rationale as objListWalk1): gen keeps
  // r16=remaining count, r17=list cursor pointer, r18=table base (loop-invariant) LIVE across every
  // nested dispatch (generated/shard_6.c:5061/5063/5065) — downstream substrate leaves spill them.
  c->r[16] = (uint32_t)(int16_t)c->mem_r16(W3_CNT_B);
  c->r[17] = c->mem_r32(W3_PTR_B);
  if (c->r[16] == 0u) goto epilogue;
  c->r[18] = W3_TABLE;

  while (c->r[16] != 0u) {
    const uint32_t cmd = c->mem_r32(c->r[17]); c->r[17] += 4; c->r[16]--;
    if (c->mem_r8(cmd + 1) == 0) continue;
    const uint32_t type = c->mem_r8(cmd + 0xB);
    if (type >= 32u) continue;
    const uint32_t target = c->mem_r32(c->r[18] + type * 4u);
    switch (target) {
      case 0x8003BFACu: c->r[31] = 0x8003BFB4u; c->r[4] = cmd; perObjRenderDispatch(); break;
      case 0x8003BFBCu: c->r[31] = 0x8003BFC4u; c->r[4] = cmd; billboardCompose1(); break;
      case 0x8003BFCCu: c->r[31] = 0x8003BFD4u; c->r[4] = cmd; billboardCompose2(); break;
      case 0x8003BFDCu: c->r[31] = 0x8003BFE4u; c->r[4] = cmd; func_8003C5F8(c); break;
      case 0x8003BFECu: c->r[31] = 0x8003BFF4u; c->r[4] = cmd; func_8003C788(c); break;
      case 0x8003BFFCu: {
        if (c->mem_r8(W3_MODE_BYTE) == 0x14u) {
          c->r[31] = 0x8003C018u; c->r[4] = cmd; rec_dispatch(c, 0x8010FC70u);
        } else {
          c->r[31] = 0x8003C028u; c->r[4] = cmd; func_8004CC88(c);
        }
        break;
      }
      case 0x8003C028u:
        break;   // no-op table entry: skip (matches gen's dedicated loop-continue case value)
      default:
        // Defensive mirror of the recompiler's own indirect-jump fallback (generated/shard_6.c:5076's
        // `default: rec_dispatch(c, c->r[2]); return;`) — a full RETURN bypassing the frame epilogue.
        c->r[4] = cmd; rec_dispatch(c, target);
        return;
    }
  }
epilogue:
  c->r[31] = c->mem_r32(c->r[29] + 28); c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20); c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 32;
}

// ===================================================================================================
// FUN_8003EEC0 (Render::objListWalk4) — no args (guest ABI). Walks a genuine singly-linked chain
// (node+0x24 "next"), NOT a positional array — no scratchpad cursor, no per-frame refresh flag; the
// chain head is re-read fresh from W4_LIST_HEAD_VAR every call.
// ORACLE: gen_func_8003EEC0
void Render::objListWalk4() {
  Core* c = mCore;
  // Real -32 guest frame (RE: gen_func_8003EEC0 prologue) — spills r16/r17/r18/ra.
  const uint32_t s16 = c->r[16], s17 = c->r[17], s18 = c->r[18], sra = c->r[31];
  c->r[29] -= 32;
  c->mem_w32(c->r[29] + 28, sra); c->mem_w32(c->r[29] + 24, s18);
  c->mem_w32(c->r[29] + 20, s17); c->mem_w32(c->r[29] + 16, s16);

  // Live loop state in c->r[] (register-faithfulness, same rationale as objListWalk1): gen keeps
  // r16=current node, r17=next node, r18=table base LIVE across every nested dispatch
  // (generated/shard_3.c:10991/10996/10999) — downstream substrate leaves (func_8003B704, the
  // rec_dispatch vtable targets) spill them.
  c->r[16] = c->mem_r32(W4_LIST_HEAD_VAR);
  if (c->r[16] != 0u) {
    c->r[18] = W4_TABLE;
    while (c->r[16] != 0u) {
      const uint32_t node   = c->r[16];
      const uint32_t active = c->mem_r8(node + 1u);
      c->r[17] = c->mem_r32(node + 0x24u);   // next: ALWAYS loaded, regardless of active/type
      if (active == 0u) { c->r[16] = c->r[17]; continue; }
      const uint32_t type = c->mem_r8(node + 0xB);
      if (type >= 33u) { c->r[16] = c->r[17]; continue; }
      const uint32_t target = c->mem_r32(c->r[18] + type * 4u);
      switch (target) {
        case 0x8003EF20u:
          c->r[31] = 0x8003EF28u; c->r[4] = node; perObjRenderDispatch();
          c->r[16] = c->r[17];
          continue;
        case 0x8003EF30u:
          c->r[31] = 0x8003EF38u; c->r[4] = node; perObjRenderDispatch();
          c->r[31] = 0x8003EF60u; c->r[4] = node; func_8003B704(c);
          c->r[16] = c->r[17];
          continue;
        case 0x8003EF40u: {
          c->r[31] = 0x8003EF48u; c->r[4] = node; billboardCompose1();
          if (c->mem_r8(node + 2u) != 1u) { c->r[16] = c->r[17]; continue; }
          c->r[31] = 0x8003EF60u; c->r[4] = node; func_8003B704(c);
          c->r[16] = c->r[17];
          continue;
        }
        case 0x8003EF68u: {
          const uint32_t vt = c->mem_r32(node + 24u);
          c->r[31] = 0x8003EF78u; c->r[4] = node; rec_dispatch(c, vt);
          c->r[16] = c->r[17];
          continue;
        }
        case 0x8003EF78u:
          c->r[16] = c->r[17];
          continue;   // no-op table entry: skip (matches gen's dedicated loop-continue case value)
        default:
          // Defensive mirror of the recompiler's own indirect-jump fallback (generated/shard_3.c:11006's
          // `default: rec_dispatch(c, c->r[2]); return;`) — a full RETURN bypassing the frame epilogue.
          c->r[4] = node; rec_dispatch(c, target);
          return;
      }
    }
  }
  c->r[31] = c->mem_r32(c->r[29] + 28); c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20); c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 32;
}

// ===================================================================================================
namespace {
void ov_objListWalk1(Core* c)         { rend(c)->objListWalk1(); }
void ov_objListWalk2(Core* c)         { rend(c)->objListWalk2(); }
void ov_objListWalk2Continue(Core* c) { rend(c)->objListWalk2Continue(); }
void ov_objListWalk3(Core* c)         { rend(c)->objListWalk3(); }
void ov_objListWalk4(Core* c)         { rend(c)->objListWalk4(); }
}

// ORACLE-PURITY: installed via engine_set_override_main (never the raw shard_set_override), so SBS
// core B (the pure gen_func_* oracle) always runs the real recompiled body while core A / standalone
// runs these native methods — see perobj_dispatch.cpp's identical banner for the full rationale.
void objlist_walk_install() {
  static bool done = false;
  if (done) return;
  done = true;
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x8003BB50u, ov_objListWalk1, gen_func_8003BB50);
  engine_set_override_main(0x8003BCF4u, ov_objListWalk2, gen_func_8003BCF4);
  engine_set_override_main(0x8003BED8u, ov_objListWalk2Continue, gen_func_8003BED8);
  engine_set_override_main(0x8003BF00u, ov_objListWalk3, gen_func_8003BF00);
  engine_set_override_main(0x8003EEC0u, ov_objListWalk4, gen_func_8003EEC0);
}
