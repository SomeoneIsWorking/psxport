// render_walk_dispatch.cpp — SUBSTRATE MIRROR for the render-WALK loop, FUN_8003C048
// (Render::renderWalk).
//
// WHY THIS FUNCTION: the f118 SBS residual (docs/findings/render.md, "overlay_ground_gt3gt4 cluster")
// traced its last two divergent ranges (0x801FE8B8..0x801FE8BC, 0x801FE900..0x801FE902) to
// Render::cmdListDispatch's OWN r16/r17 prologue spill (CmdListFrame) receiving STALE register
// content from its caller chain (perObjRenderDispatch -> CCA4Frame -> ... -> FUN_8003C048) instead of
// gen's real values — because FUN_8003C048, the render-walk loop that is the ROOT caller of the whole
// perObjRenderDispatch/cmdListDispatch/perModeDispatch chain, was still fully unowned/shared substrate.
// gen_func_8003C048 keeps its loop's CURRENT NODE and NEXT-NODE pointers LIVE in the real MIPS s0/s1
// registers (r16/r17) across every nested dispatch call (never re-loaded per call site — plain
// register lifetime), plus two loop-invariant constants in r18/r19 (a scratch buffer base and the
// per-node jump-table base). The still-substrate/owned callees this loop reaches (perObjRenderDispatch
// via CCA4Frame, and transitively cmdListDispatch via CmdListFrame) SPILL those incoming r16..r19 to
// their own guest-stack frames as "caller state" — i.e. the caller's r16/r17/r18/r19 values are
// genuine guest-RAM-visible bytes, not dead scratch. Owning this loop and setting c->r[16..19] to
// gen's real values (not C++ locals) is what makes those downstream spills byte-match.
//
// RE method: instruction-exact transcription of generated/shard_7.c gen_func_8003C048 (the
// recompiler's translation is ground truth; Ghidra headless was used only to cross-check control flow
// shape, never for the COP2/register semantics this function doesn't even touch). Confirmed unowned
// via tools/codemap.py before porting. Same ownership band (0x8003xxxx) and mechanism
// (engine_set_override_main, oracle-gated) as the sibling files this loop calls into.
//
// gen_func_8003C048 (no args, guest ABI): the caller passes nothing in r4-r7; the function reads the
// global render-node-list head itself.
//   - prologue: sp -= 112; spill caller's r16 @sp+88, r31 @sp+104, r19 @sp+100, r18 @sp+96,
//     r17 @sp+92 (order per generated/shard_7.c:4425-4436).
//   - r16 = *0x800F2624 (RENDER_LIST_HEAD, the global render-node linked-list head pointer). If null,
//     go straight to the epilogue (empty list).
//   - r19 = 0x800104B8 (JUMP_TABLE, 33-entry per-node case-target table), r18 = 0x1F8000F8
//     (CASE188_SCR, a scratchpad scratch buffer ONLY the 0x8003C188 case uses — numerically the same
//     address as perobj_billboard.cpp's CAM2, but that is coincidental scratch reuse across unrelated
//     call paths, not a shared meaning).
//   - loop: r17 = *(node+36) (next-node pointer, loaded EVERY iteration regardless of whether this
//     node dispatches). If mem8(node+1)==0 (dead/inactive node), skip dispatch entirely. Else read the
//     case-index byte at node+11; if >=33, skip dispatch. Else target = *(JUMP_TABLE + idx*4) and
//     switch on target (16 known label values + a defensive rec_dispatch-then-return default,
//     matching the recompiler's own indirect-jump fallback convention used throughout this codebase's
//     sibling switch dispatchers). Every real case sets r31 to its own RE'd return-address constant,
//     r4 = node (+ r5/r6 for a couple of cases), calls its target, then falls through to the
//     shared "advance" step; case 0x8003C29C reads a per-node function pointer at node+24 (a fully
//     dynamic per-object dispatch, the RCASE_DEFAULT class documented in older render findings) and
//     rec_dispatch's it; case's own r31/table-value 0x8003C2AC is a pure no-op (skip) entry.
//   - advance: r16 = r17; loop while r16 != 0.
//   - epilogue: restore r31/r19/r18/r17/r16 from their spill slots, sp += 112, return.
#include "core.h"
#include "game_ctx.h"
#include "game.h"
#include "render.h"
#include "render_internal.h"   // withDepthTag (#39: depth-tag the RCASE_DEFAULT custom renderer)
#include "cfg.h"
#include <stdio.h>

void rec_dispatch(Core*, uint32_t);          // overlay_router.cpp — shared choke point for owned/substrate leaves
void func_8003F174(Core*);   // still-substrate: case 0x8003C0C4 (a0=node, a1=0)
void func_8003EF9C(Core*);   // still-substrate: case 0x8003C0D8
void func_80039F4C(Core*);   // still-substrate: case 0x8003C0E8
void func_800726D4(Core*);   // still-substrate: case 0x8003C138
void func_8003C5F8(Core*);   // still-substrate: case 0x8003C168
void func_8003C788(Core*);   // still-substrate: case 0x8003C178
void func_8003B054(Core*);   // still-substrate: case 0x8003C188's particle color/UV fill
void func_80084660(Core*);   // still-substrate: case 0x8003C188's pool-span bracket (open)
void func_80084690(Core*);   // still-substrate: case 0x8003C188's pool-span bracket (close)
void func_8003B320(Core*);   // still-substrate: case 0x8003C188's packet emit
void shard_set_override(uint32_t addr, OverrideFn fn);   // generated/shard_disp.c (C++ linkage)

// gen_func_8003C048 fallback for the oracle-gated thunk. g_override[] is process-global (shared by
// SBS core A and core B), so the oracle side MUST keep running the real recompiled body — see
// perobj_billboard.cpp's identical banner for the full rationale.
extern void gen_func_8003C048(Core*);

namespace {
constexpr uint32_t RENDER_LIST_HEAD = 0x800F2624u;   // *this = head of the global render-node list
constexpr uint32_t JUMP_TABLE       = 0x80014DB8u;   // 33-entry (idx<33) per-node case-target table
                                                      // (32769<<16 + 19896 = 0x80010000 + 0x4DB8 —
                                                      // adjacent to CCA4's own 9-slot table at
                                                      // 0x80014EC8, part of the same shared jump-table
                                                      // data region. An earlier draft mis-added the low
                                                      // 16 bits and used 0x800104B8, landing on an
                                                      // UNRELATED table belonging to a different
                                                      // function and crashing on a bogus dispatch —
                                                      // caught by cross-checking the live table content
                                                      // against generated/shard_7.c's neighboring
                                                      // gen_func_8003D0BC (same 32769<<16 base).
constexpr uint32_t CASE188_SCR      = 0x1F8000F8u;   // scratch buffer used only by case 0x8003C188
// perobj_dispatch.cpp's MODE_BYTE (0x800BF870) — case 0x8003C188 reads the SAME render-mode-select
// byte perModeDispatch does, confirming both are part of the one per-area render-mode mechanism.
constexpr uint32_t MODE_BYTE_188    = 0x800BF870u;

// Real -112 guest-stack frame (RE: gen_func_8003C048's prologue) — see CLAUDE.md "MIRROR THE GUEST
// STACK". Spills the CALLER's live r16/r17/r18/r19/r31 (whatever the render-frame orchestrator that
// reaches FUN_8003C048 had at that call site) and restores them on every exit, matching gen's real
// callee-save prologue/epilogue exactly (order per generated/shard_7.c:4425-4436,4564-4569).
struct WalkFrame {
  Core* c; uint32_t s16, s31, s19, s18, s17;
  explicit WalkFrame(Core* c_)
    : c(c_), s16(c_->r[16]), s31(c_->r[31]), s19(c_->r[19]), s18(c_->r[18]), s17(c_->r[17]) {
    c->r[29] -= 112;
    c->mem_w32(c->r[29] + 88,  s16);
    c->mem_w32(c->r[29] + 104, s31);
    c->mem_w32(c->r[29] + 100, s19);
    c->mem_w32(c->r[29] + 96,  s18);
    c->mem_w32(c->r[29] + 92,  s17);
  }
  ~WalkFrame() {
    c->r[31] = c->mem_r32(c->r[29] + 104);
    c->r[19] = c->mem_r32(c->r[29] + 100);
    c->r[18] = c->mem_r32(c->r[29] + 96);
    c->r[17] = c->mem_r32(c->r[29] + 92);
    c->r[16] = c->mem_r32(c->r[29] + 88);
    c->r[29] += 112;
  }
};

// L_8003C188/L_8003C1AC — the walk-cluster's "generic particle" dispatch entry. Two sub-paths per
// gen (generated/shard_7.c:4500-4554):
//  - render-mode byte (0x800BF870, the SAME byte perModeDispatch reads) == 4: rec_dispatch straight
//    to FUN_8011BE5C(node) (a dedicated mode-4 particle-emit leaf).
//  - otherwise: resolve the node's active particle SUB-LIST (node+56/+60, the identical
//    {count:s16@0, byteOff:s16@2}[]-indexed-by-node+56's-own-index layout billboardEmit's own
//    particle indexing uses — see perobj_billboard.cpp), fill a packet struct via the still-substrate
//    func_8003B054 into THIS frame's own guest-stack scratch (sp+16..), force its primitive-code byte
//    to 45, copy 11 halfwords of node+96..118 vertex/UV data to sp+56..84, bracket with
//    func_80084660/func_80084690 (r4=CASE188_SCR — pool-span open/close markers, the guest-side
//    counterpart of PktSpanSession), then emit via func_8003B320(sp+16, sp+56, count=16).
void renderWalkCase188(Core* c) {
  const uint32_t node = c->r[16];
  const uint32_t sp   = c->r[29];
  if (c->mem_r8(MODE_BYTE_188) == 4u) {
    c->r[4] = node; c->r[31] = 0x8003C1A4u; rec_dispatch(c, 0x8011BE5Cu);
    return;
  }
  const uint32_t tbl = c->mem_r32(node + 56u);
  if (tbl == 0u) return;
  const int idx           = (int16_t)c->mem_r16(tbl + 0u);
  const uint32_t listBase = c->mem_r32(node + 60u);
  const uint32_t entry    = listBase + (uint32_t)(idx << 2);
  const int16_t byteOff   = (int16_t)c->mem_r16(entry + 2u);
  c->r[4] = sp + 16u; c->r[5] = listBase + (uint32_t)(int32_t)byteOff; c->r[6] = 0u;
  c->r[31] = 0x8003C1E0u;
  func_8003B054(c);
  c->mem_w8(sp + 23u, 45u);
  for (uint32_t i = 0; i < 11u; i++)
    c->mem_w16(sp + 56u + i * 2u, c->mem_r16(node + 96u + i * 2u));
  c->r[4] = c->r[18]; c->r[31] = 0x8003C27Cu; func_80084660(c);
  c->r[4] = c->r[18]; c->r[31] = 0x8003C284u; func_80084690(c);
  c->r[4] = sp + 16u; c->r[5] = sp + 56u; c->r[6] = 16u; c->r[31] = 0x8003C294u;
  func_8003B320(c);
}
} // namespace

// FUN_8003C048
void Render::renderWalk() {
  Core* c = mCore;
  WalkFrame frame(c);
  c->r[16] = c->mem_r32(RENDER_LIST_HEAD);
  if (c->r[16] == 0u) return;   // empty list -> straight to epilogue (WalkFrame restores)
  c->r[19] = JUMP_TABLE;
  c->r[18] = CASE188_SCR;
  for (;;) {
    const uint32_t node = c->r[16];
    // Loaded EVERY iteration regardless of whether this node dispatches (gen: L_8003C07C's first two
    // statements) — this is the loop's real s1/r17, live into every nested call below.
    c->r[17] = c->mem_r32(node + 36u);
    if (c->mem_r8(node + 1u) != 0u) {
      const uint32_t idxByte = c->mem_r8(node + 11u);
      if (idxByte < 33u) {
        const uint32_t target = c->mem_r32(c->r[19] + idxByte * 4u);
        cfg_logf("walk", "node=%08x idx=%u table=%08x target=%08x", node, idxByte, c->r[19], target);
        switch (target) {
          case 0x8003C0B4u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C0BCu; perObjRenderDispatch();
            break;
          case 0x8003C0C4u:
            c->r[4] = c->r[16]; c->r[5] = 0u; c->r[31] = 0x8003C0D0u; func_8003F174(c);
            break;
          case 0x8003C0D8u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C0E0u; func_8003EF9C(c);
            break;
          case 0x8003C0E8u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C0F0u; func_80039F4C(c);
            break;
          case 0x8003C0F8u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C100u; rec_dispatch(c, 0x8012A43Cu);
            break;
          case 0x8003C108u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C110u; rec_dispatch(c, 0x801295B4u);
            break;
          case 0x8003C118u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C120u; rec_dispatch(c, 0x80129114u);
            break;
          case 0x8003C128u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C130u; rec_dispatch(c, 0x8013DD58u);
            break;
          case 0x8003C138u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C140u; func_800726D4(c);
            break;
          case 0x8003C148u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C150u; billboardCompose1();
            break;
          case 0x8003C158u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C160u; billboardCompose2();
            break;
          case 0x8003C168u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C170u; func_8003C5F8(c);
            break;
          case 0x8003C178u:
            c->r[4] = c->r[16]; c->r[31] = 0x8003C180u; func_8003C788(c);
            break;
          case 0x8003C188u:
            renderWalkCase188(c);
            break;
          case 0x8003C29Cu: {
            // Fully dynamic per-node dispatch through a function pointer at node+24 (the RCASE_DEFAULT
            // class documented in older render findings — a per-object-type custom renderer). #39: wrap
            // in withDepthTag (like perObjRenderDispatch) so the emitted prims get the object's world
            // depth and the field tee KEEPS them (weapon chain + impact effect were dropped untagged).
            withDepthTag(c, c->r[16], [](Core* c) {
              const uint32_t fn = c->mem_r32(c->r[16] + 24u);
              c->r[4] = c->r[16]; c->r[31] = 0x8003C2ACu; rec_dispatch(c, fn);
            });
            break;
          }
          case 0x8003C2ACu:
            break;   // no-op table entry: skip (matches gen's dedicated L_8003C2AC case value)
          default:
            // Defensive mirror of the recompiler's own indirect-jump fallback (generated/shard_7.c:
            // 4446's `default: rec_dispatch(c, c->r[2]); return;`) — a full RETURN from the whole
            // function, not a loop-continue, exactly like perObjRenderDispatch's and billboardEmit's
            // own default cases. Never hit by live game data: the live 33-slot table only ever holds
            // the case values enumerated above.
            c->r[4] = c->r[16]; rec_dispatch(c, target);
            return;
        }
      }
    }
    c->r[16] = c->r[17];
    if (c->r[16] == 0u) break;
  }
}

namespace {
void ov_renderWalk(Core* c) { rend(c)->renderWalk(); }
}

void render_walk_dispatch_install() {
  static bool done = false;
  if (done) return;
  done = true;
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x8003C048u, ov_renderWalk, gen_func_8003C048);
}
