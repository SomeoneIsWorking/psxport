// overlay_type_dispatch.cpp — SUBSTRATE MIRROR for the per-AREA-TYPE overlay render dispatcher,
// FUN_8003D0BC (Render::overlayTypeDispatch).
//
// WHY THIS FUNCTION: docs/findings/render.md's "overlay_ground_gt3gt4 cluster" entry names this as
// the NEW frontier once FUN_8003C048 (renderWalk) closed f118/f62: SBS-full's first divergence moved
// to f158, in packet bytes emitted by OverlayGroundGt3Gt4::gt3/gt4 (already owned) — but those leaves
// are reached ONLY via this still-fully-unowned dispatcher, so per CLAUDE.md's "never debug through
// unowned code" rule it must be owned FIRST, before any gt3/gt4 data diff-hunt.
//
// RE method: instruction-exact transcription of generated/shard_7.c gen_func_8003D0BC (ground truth
// — this function is a pure integer dispatch, no GTE/COP2, so Ghidra's pseudo-C would have been fine
// too, but the recompiled C was used directly for consistency with the sibling files in this band).
// Confirmed unowned via tools/codemap.py before porting.
//
// gen_func_8003D0BC(a0=list, guest ABI): the function's OWN body never reads or writes r4 — whatever
// the caller passed in r4 (gen_func_8003F9A8 passes SCENE_ENT_TABLE @0x800F2418, matching
// OverlayGroundGt3Gt4::entityLoop's own "list=a0" ABI) flows through UNCHANGED into every dispatch
// target's own r4 read. So the native port must NOT set c->r[4] itself — it is plain MIPS register
// lifetime, not an explicit pass-through the port needs to reproduce.
//   - prologue: sp -= 24; ALWAYS spill caller's r31 @sp+16 (even on the immediate early-out).
//   - AREA_TYPE = mem8(0x800BF870) (the same render-mode-select byte perModeDispatch/renderWalk's
//     case-0x8003C188 both read — one shared per-area render-mode mechanism, see
//     perobj_dispatch.cpp's MODE_BYTE / render_walk_dispatch.cpp's MODE_BYTE_188). If AREA_TYPE >= 22,
//     skip straight to the epilogue (no area-specific overlay for this type).
//   - else: target = *(JUMP_TABLE + AREA_TYPE*4), JUMP_TABLE = 0x80014EF0 (32769<<16 + 20208 —
//     adjacent to renderWalk's own JUMP_TABLE @0x80014DB8 and perObjRenderDispatch's CCA4 table
//     @0x80014EC8: all three live in the same shared jump-table data region). Switch on target (20
//     known case labels + the recompiler's own generic-fallback default, mirrored verbatim —
//     `rec_dispatch(c, target); return;` WITHOUT restoring the frame, exactly like gen; this default
//     is never hit by live game data since the real 22-entry table only ever holds the 20 enumerated
//     case values plus the early-out path).
//   - every real case sets r31 to its own RE'd return-address constant, then rec_dispatch()s straight
//     to a per-area-type overlay leaf (case AREA_TYPE==0 -> 0x801401B8, the ALREADY-OWNED
//     OverlayGroundGt3Gt4::entityLoop; the other 19 -> still-substrate leaves), then falls into the
//     shared epilogue.
//   - epilogue (L_8003D22C): restore r31 from sp+16, sp += 24, return.
#include "core.h"
#include "game_ctx.h"
#include "game.h"
#include "render.h"
#include <cstdint>

void rec_dispatch(Core*, uint32_t);          // overlay_router.cpp — shared choke point for owned/substrate leaves
void shard_set_override(uint32_t addr, OverrideFn fn);   // generated/shard_disp.c (C++ linkage)

// gen_func_8003D0BC fallback for the oracle-gated thunk — see render_walk_dispatch.cpp's identical
// banner for the full rationale (g_override[] is process-global; SBS core B must keep running gen).
extern void gen_func_8003D0BC(Core*);

namespace {
constexpr uint32_t AREA_TYPE_BYTE = 0x800BF870u;   // render-mode-select byte (shared with perModeDispatch)
constexpr uint32_t JUMP_TABLE     = 0x80014EF0u;   // 32769<<16 + 20208 — 22-entry per-area-type table

// Real -24 guest-stack frame (RE: gen_func_8003D0BC's prologue, generated/shard_7.c:4576/4578) — see
// CLAUDE.md "MIRROR THE GUEST STACK". Spills the caller's live r31 (whatever gen_func_8003F9A8 had at
// its own 0x8003F9F4 call site) and restores it on every exit.
struct DispatchFrame {
  Core* c; uint32_t s31;
  explicit DispatchFrame(Core* c_) : c(c_), s31(c_->r[31]) {
    c->r[29] -= 24;
    c->mem_w32(c->r[29] + 16, s31);
  }
  ~DispatchFrame() {
    c->r[31] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 24;
  }
};
} // namespace

// FUN_8003D0BC
void Render::overlayTypeDispatch() {
  Core* c = mCore;
  DispatchFrame frame(c);
  const uint32_t areaType = c->mem_r8(AREA_TYPE_BYTE);
  if (areaType >= 22u) return;   // no area-specific overlay for this type -> epilogue only

  const uint32_t target = c->mem_r32(JUMP_TABLE + areaType * 4u);
  switch (target) {
    case 0x8003D0F4u: c->r[31] = 0x8003D0FCu; rec_dispatch(c, 0x801401B8u); break;  // entityLoop (owned)
    case 0x8003D104u: c->r[31] = 0x8003D10Cu; rec_dispatch(c, 0x80132358u); break;
    case 0x8003D114u: c->r[31] = 0x8003D11Cu; rec_dispatch(c, 0x80124CB8u); break;
    case 0x8003D124u: c->r[31] = 0x8003D12Cu; rec_dispatch(c, 0x801185F0u); break;
    case 0x8003D134u: c->r[31] = 0x8003D13Cu; rec_dispatch(c, 0x8013606Cu); break;
    case 0x8003D144u: c->r[31] = 0x8003D14Cu; rec_dispatch(c, 0x8013CD34u); break;
    case 0x8003D154u: c->r[31] = 0x8003D15Cu; rec_dispatch(c, 0x8012DA14u); break;
    case 0x8003D164u: c->r[31] = 0x8003D16Cu; rec_dispatch(c, 0x8012A7CCu); break;
    case 0x8003D174u: c->r[31] = 0x8003D17Cu; rec_dispatch(c, 0x8011024Cu); break;
    case 0x8003D184u: c->r[31] = 0x8003D18Cu; rec_dispatch(c, 0x80113050u); break;
    case 0x8003D194u: c->r[31] = 0x8003D19Cu; rec_dispatch(c, 0x80113DB4u); break;
    case 0x8003D1A4u: c->r[31] = 0x8003D1ACu; rec_dispatch(c, 0x80113628u); break;
    case 0x8003D1B4u: c->r[31] = 0x8003D1BCu; rec_dispatch(c, 0x80114320u); break;
    case 0x8003D1C4u: c->r[31] = 0x8003D1CCu; rec_dispatch(c, 0x80115364u); break;
    case 0x8003D1D4u: c->r[31] = 0x8003D1DCu; rec_dispatch(c, 0x8010C2A4u); break;
    case 0x8003D1E4u: c->r[31] = 0x8003D1ECu; rec_dispatch(c, 0x8010B5BCu); break;
    case 0x8003D1F4u: c->r[31] = 0x8003D1FCu; rec_dispatch(c, 0x8010BA40u); break;
    case 0x8003D204u: c->r[31] = 0x8003D20Cu; rec_dispatch(c, 0x8010AA20u); break;
    case 0x8003D214u: c->r[31] = 0x8003D21Cu; rec_dispatch(c, 0x80115F1Cu); break;
    case 0x8003D224u: c->r[31] = 0x8003D22Cu; rec_dispatch(c, 0x8010B0B8u); break;
    case 0x8003D22Cu: break;   // early-out target value itself: no-op (falls straight to epilogue)
    default:
      // Defensive mirror of the recompiler's own indirect-jump fallback (generated/shard_7.c:4584's
      // `default: rec_dispatch(c, c->r[2]); return;`) — a full RETURN bypassing the frame epilogue,
      // exactly like renderWalk's/perObjRenderDispatch's own default cases. Never hit by live game
      // data: the live 22-slot table only ever holds the 20 case values above (or routes through the
      // areaType>=22 early-out).
      rec_dispatch(c, target);
      return;
  }
}

namespace {
void ov_overlayTypeDispatch(Core* c) { rend(c)->overlayTypeDispatch(); }
}

void overlay_type_dispatch_install() {
  static bool done = false;
  if (done) return;
  done = true;
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x8003D0BCu, ov_overlayTypeDispatch, gen_func_8003D0BC);
}
