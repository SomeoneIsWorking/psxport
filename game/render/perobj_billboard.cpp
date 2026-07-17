// perobj_billboard.cpp — SUBSTRATE MIRROR for the per-object render-TYPE dispatch (FUN_8003CCA4) and
// 3 of its "special effect" billboard/particle-quad leaf renderers (FUN_8003C2D4, FUN_8003C464,
// FUN_8003C8F4). Same band (0x8003xxxx) as perobj_dispatch.cpp's cmdListDispatch/perModeDispatch;
// same ownership mechanism (shard_set_override — these are reached as PLAIN intra-shard C calls from
// the still-substrate walk cluster gen_func_8003BF00/etc, never through rec_dispatch).
//
// PRIOR STATE: all 4 addresses carried a TRANSPARENT RenderObserver wrapper (render_observer.cpp) that
// ran the literal gen_func_* body then host-side depth-tagged the packet span it wrote
// (obj_world_ord/gpu_obj_depth_add — the billboard-occlusion fix, issue #4 class). Owning
// these natively must preserve that: each of the 4 methods below opens its own PktSpanSession and tags
// on exit, exactly mirroring render_observer.cpp's obs_body. RenderObserver itself no longer installs
// wrappers for these 4 addresses (render_observer.cpp trimmed accordingly); it still wraps the 2
// remaining unowned siblings (0x8003C5F8, 0x8003C788) and 0x80039F4C.
//
// RE method: Ghidra headless decompile of a live free-roam RAM dump cross-checked against the ACTUAL
// recompiled body in generated/shard_0.c (C2D4), shard_1.c (C464), shard_4.c (C8F4), shard_5.c (CCA4),
// generated/shard_disp.c (g_override slot wiring) — the recompiler's gte_write_ctrl/gte_write_data/
// gte_op/gte_read_data calls are ground truth, not Ghidra's COP2 pseudo-C. All 4 addresses confirmed
// unowned via tools/codemap.py before porting.
//
// ==================================================================================================
// FUN_8003CCA4 (perObjRenderDispatch, a0=node r4): stores node into the "current render node" scratch
// (0x1F80028C — the same fallback obj_world_ord already reads), then selects one of 6 cases by
// `mem8(node+13) & 0xB` (bound-checked < 9) via a 9-slot table at 0x80014EC8. Every valid case runs
// cmdListDispatch() (already owned, FUN_8003CDD8) then, for 4 of the 6 cases, calls one of 5
// still-substrate "special effect" leaves (FUN_8003D584/F344/F3F4/F4C4/F594) with
// (node, poolPtrBeforeCmdListDispatch, poolPtrAfter) — i.e. the packet-pool span cmdListDispatch just
// emitted. None of these 5 leaves fire at seaside (perobj_dispatch.cpp's prior finding); they stay
// substrate, reached as plain guest-ABI calls so they still see the correct pool-pointer bracket.
//
// FUN_8003C2D4 / FUN_8003C464 (billboardCompose1/2, a0=node r4): each builds a "local" transform for a
// billboard-type node and composes it with a persistent camera MATRIX before handing off to
// billboardEmit. Both use a shared per-instance SCRATCHPAD region at 0x1F800000
// (BUF below) that holds ordinary libgte MATRIX structs (m[3][3] int16 row-major + 2 pad bytes + t[3]
// int32 — exactly what Mtx::identity's 8-word write pattern and Math::rotZ/matMul's byte
// reads agree on):
//   BUF+0x00 MAT_A     — C2D4: identity (Mtx::identity). C464: seeded by the still-substrate
//                         FUN_800517BC(node+122/124/126 as s16 x,y,z) instead of identity.
//   BUF+0x20 MAT_ROTZ  — identity, then Z-rotated in place by mem16(node+90) via Math::rotZ.
//   BUF+0x40 MAT_OUT   — Math::matMul(MAT_ROTZ, MAT_A, MAT_OUT) (= MAT_ROTZ for C2D4, since MAT_A is
//                         identity there); .t (=MAT_OUT+0x14) becomes the composed WORLD translation.
//   BUF+0xC0 WORLD_POS — object's world position triple (s16 x3, from node+46/50/54).
//   BUF+0xF8 CAM2      — the SCENE CAMERA view MATRIX at scratchpad 0x1F8000F8 — the EXACT bytes
//                         Fps60::sceneCam reads (m rows @+0xF8..0x109, t @+0x10C..0x117). Read-only
//                         here. (#67 correction: an earlier note called this "main-RAM 0x800C0000";
//                         that was the same mis-base the f117 fix corrected for BUF itself.)
// Both then: load CAM2.m into CR0-4, MVMVA-transform WORLD_POS by it (same opcode as cmdListDispatch's
// world-translate), add CAM2.t into MAT_OUT.t, reload CR0-7 from MAT_OUT (rotation + composed
// translation), and call billboardEmit(node, mem8(node+71)&1).
//
// FUN_8003C8F4 (billboardEmit, a0=node r4, a1=flag r5): resolves the node's active particle SUB-LIST
// (node+56 -> {count:s16@0, byteOff:s16@2}[] indexed by *(s16*)(*(node+56)); node+60 = sub-list base),
// then for each particle (16-byte stride):
//   1. still-substrate FUN_8003B220(a0=guest scratch, a1=0, a2=particle) fills 4 quad-corner vectors
//      (V0..V2 packed for RTPT, V3 for a second RTPS) into REAL GUEST STACK memory at r29+16..+45 —
//      genuine stack addresses (not a host buffer) because a substrate callee writes them via
//      Core::mem_w16/32, and because SBS compares guest RAM including live stack frames (see
//      docs/findings — Animation::attach's guest-stack residual). A real GuestFrame(96) allocation
//      backs this (found the hard way: omitting it — and the callers' own frame allocations —
//      shifted this frame relative to the recomp path and produced a real, reproducible SBS diff).
//   2. RTPT (0x4A280030) projects V0-2 -> SXY0-2; on success stash them at BUF+8/16/24, AVSZ3
//      (0x4B400006) gives a first depth estimate; RTPS (0x4A180001) projects V3, and on success AVSZ4
//      (0x4B68002E) gives the final OTZ-style depth (else the particle is invalid, depth=-1).
//   3. off-screen cull: skip if all 4 corners' X>=320 (unsigned) or all 4 corners' Y>=240.
//   4. quantize the depth into an OT bucket (node+8 signed per-node depth-bias byte, >>10/<<9 rebucket,
//      clamped to the valid <2044 range else reset to -1 and skip).
//   5. still-substrate FUN_8003B054(BUF, particle, flag) fills the packet's color/UV fields; an
//      optional node+92 half-word override and a node+13-selected small case table (6 labels) patch a
//      couple of BUF bytes (sprite index / extra color word) before emission.
//   6. emit a 10-word packet (1 tag word [size=9 | old-OT-head] + 9 data words copied from
//      BUF+4..+36) at the packet-pool tail (0x800BF544), prepended into the OT bucket
//      (*0x800ED8C8 + depth*4) — the identical packet-chain mechanism perobj_dispatch.cpp's
//      cmdListDispatch/perModeDispatch already documents.
#include "core.h"
#include "game_ctx.h"
#include "game.h"
#include "render.h"
#include "pkt_span.h"
#include "render_internal.h"   // obj_world_ord / gpu_obj_depth_add / withDepthTag
#include "proj_params.h"       // ProjParams::pzToOrd — billboardsRender depth normalize
#include <unordered_map>

void rec_dispatch(Core*, uint32_t);
void shard_set_override(uint32_t addr, OverrideFn fn);   // generated/shard_disp.c (C++ linkage)

// gen_func_* fallbacks for the psx_fallback gate. g_override[] is a single PROCESS-GLOBAL table
// shared by EVERY Core (SBS core A AND core B), so the trampolines below MUST defer to the real
// recompiled body on core B (the pure-substrate oracle) — otherwise the oracle runs this native
// mirror and SBS compares native-vs-native (a false 0-div) instead of native-vs-substrate. Same
// discipline as every other shard_set_override cluster (gte_math/node_xform/cull/...). The oracle
// may carry ONLY async→sync conversions (sync_overrides.cpp) + HLE BIOS — nothing engine/game.
extern void gen_func_8003CCA4(Core*);
extern void gen_func_8003C2D4(Core*);
extern void gen_func_8003C464(Core*);
extern void gen_func_8003C8F4(Core*);

// Still-substrate leaves called by these 4 (declared, called via plain guest-ABI intra-shard calls —
// exactly as the generated code reaches them; g_override still gates each, so if one is ever owned
// later these calls transparently pick that up).
void func_800517BC(Core*);   // C464's MAT_A seed (node+122/124/126 s16 xyz)
void func_8003B220(Core*);   // billboardEmit's quad-corner builder (writes real guest stack memory)
void func_8003B054(Core*);   // billboardEmit's color/UV fill
void func_8003D584(Core*);   // CCA4 special-effect leaf (case CD10)
void func_8003F344(Core*);   // CCA4 special-effect leaf (case CD38)
void func_8003F3F4(Core*);   // CCA4 special-effect leaf (case CD60, node+27==0)
void func_8003F4C4(Core*);   // CCA4 special-effect leaf (case CD60, node+27!=0)
void func_8003F594(Core*);   // CCA4 special-effect leaf (case CDA0)

namespace {
constexpr uint32_t CUR_NODE_SCR = 0x1F80028Cu;   // "current render node" scratch (obj_world_ord fallback)
constexpr uint32_t PKT_POOL_PTR = 0x800BF544u;   // packet-pool bump-allocator write pointer
constexpr uint32_t OTBASE_PTR   = 0x800ED8C8u;   // *this = the active ordering-table base
constexpr uint32_t BUF          = 0x1F800000u;   // SCRATCHPAD MATRIX-compose buffer (C2D4/C464/C8F4) —
                                                  // gen_func_8003C2D4/8003C8F4 base r16/r17 = 8064<<16
                                                  // = 0x1F800000, NOT main RAM. (Was wrongly 0x800C0000;
                                                  // the mis-base made every emitted packet's data differ
                                                  // from the substrate — the f117 divergence, masked by the
                                                  // false 0-div until the oracle-gate fix surfaced it.)
constexpr uint32_t MAT_A        = BUF + 0x00u;
constexpr uint32_t MAT_ROTZ     = BUF + 0x20u;
constexpr uint32_t MAT_OUT      = BUF + 0x40u;
constexpr uint32_t WORLD_POS    = BUF + 0xC0u;
constexpr uint32_t CAM2         = BUF + 0xF8u;   // persistent camera MATRIX mirror (read-only here)
constexpr uint32_t MVMVA_TRANS  = 0x4A486012u;   // same opcode cmdListDispatch uses for the world-translate

// RAII guest-stack frame: real recomp bodies allocate their own stack frame (r29 -= size) before
// running, and callees compute their OWN frame relative to the CALLER'S post-allocation r29 — so a
// callee reached from here (billboardEmit reads its scratch as c->r[29]-96+off) needs r29 to reflect
// the SAME depth the recomp path would have at that call, even though nothing in THIS function's own
// body reads/writes through r29 itself. Symmetric allocate/restore, net-zero like the recomp's own
// push/pop (found empirically: 8003C2D4/8003C464 omitting this shifted 8003C8F4's frame by their own
// size and produced a real, reproducible SBS diff at f118 in the task-0 stack region).
struct GuestFrame {
  Core* c; uint32_t size;
  GuestFrame(Core* c_, uint32_t size_) : c(c_), size(size_) { c->r[29] -= size; }
  ~GuestFrame() { c->r[29] += size; }
};

// FUN_8003CCA4's REAL prologue (register-faithfulness, f118 root cause, 2026-07-09): unlike the
// call sites above where GuestFrame's bare sp-adjust is enough (their bodies spill live-injected
// values inline themselves), gen_func_8003CCA4 (generated/shard_5.c) actually SPILLS its caller's
// live r16/r17/r18/r31 to guest memory at entry (mem_w32 sp+16/20/24/28) and restores them at every
// exit (L_8003CDC0) — a plain MIPS callee-save prologue/epilogue, not a value injection. The bare
// GuestFrame(c,32) this call site used only adjusted c->r[29] and never wrote those 4 words, leaving
// WHATEVER STALE bytes were already sitting in that guest-stack region (leftover from an unrelated
// earlier writer) instead of the caller's real r16/r17/r18/r31 — the exact SBS diff at
// 0x801FE8B8../0x801FE8E8.. (task-0 stack, several frames into this call chain) that unmasked once
// the f62 register-faithfulness gap (cmdListDispatch's r16=loop-index/r17=SCR, see
// perobj_dispatch.cpp) was fixed and the SBS gate advanced past it. r18 is reassigned to `node`
// immediately after the spill (matching gen's `r18 = r4` right after `mem_w32(sp+24,r18)`); r16/r17
// are pure save/restore (this function's own body never sets them — case 0x8003CD00, the only case
// seaside objects hit, doesn't either, per gen).
struct CCA4Frame {
  Core* c; uint32_t s16, s17, s18, sra;
  explicit CCA4Frame(Core* c_)
    : c(c_), s16(c_->r[16]), s17(c_->r[17]), s18(c_->r[18]), sra(c_->r[31]) {
    c->r[29] -= 32;
    c->mem_w32(c->r[29] + 24, s18);
    c->mem_w32(c->r[29] + 28, sra);
    c->mem_w32(c->r[29] + 20, s17);
    c->mem_w32(c->r[29] + 16, s16);
  }
  ~CCA4Frame() {
    c->r[31] = c->mem_r32(c->r[29] + 28);
    c->r[18] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 32;
  }
};

// withDepthTag moved to render_internal.h (#39) so renderWalk's 0x8003C29C RCASE_DEFAULT dispatch can
// share the exact same depth-tag discipline — see render_internal.h.

// Per-particle world anchor + view-ord (render.md#56 — flame-over-wall). withDepthTag tags the WHOLE
// billboardEmit packet span with ONE node-level ord (obj_world_ord(node): the manager node's single
// world point), so every particle in a manager's sub-list (all flame/torch licks, all AP-gem sparkles)
// shares one depth — when that's nearer than a real occluder's per-pixel depth, the WHOLE batch draws
// in front of it. Each particle actually sits at its OWN world position: the node's world pos (node+
// 46/50/54) offset by the node's own rotation applied to the particle's own planar offset
// (particle+14/+15, s8, scaled x5 — the IDENTICAL 5x(p14,p15,0) scale func_8003B220's quad-corner
// builder uses to place the same particle's on-screen quad, and the same anchor math the retired
// fps60 recordBillboardParticle registry used, docs/findings/render.md "fps60 billboard anchor must be
// PER-PARTICLE"). MAT_OUT (BUF+0x40, the composed rotation billboardComposeTail already loaded into
// GTE CR0-7 for this node) is the node's rotation; it does not change across this loop's particles.
// Projects through the SAME stable scene camera obj_world_ord uses, so per-particle occlusion never
// re-derives PSX OT order — it's the engine's own world-position depth, just resolved at the correct
// granularity.
static float billboardParticleOrd(Core* c, uint32_t node, uint32_t particle) {
  const float wx = (float)c->mem_r16s(node + 46);
  const float wy = (float)c->mem_r16s(node + 50);
  const float wz = (float)c->mem_r16s(node + 54);
  const float ox = (float)(5 * c->mem_r8s(particle + 14));
  const float oy = (float)(5 * c->mem_r8s(particle + 15));
  // MAT_OUT rotation: libgte MATRIX, m[row][col] int16 4.12 fixed, row-major (row stride 6 bytes).
  auto rot = [c](int row, int col) { return (float)(int16_t)c->mem_r16(MAT_OUT + row * 6 + col * 2); };
  constexpr float FX = 1.0f / 4096.0f;
  const float rx = (rot(0, 0) * ox + rot(0, 1) * oy) * FX;
  const float ry = (rot(1, 0) * ox + rot(1, 1) * oy) * FX;
  const float rz = (rot(2, 0) * ox + rot(2, 1) * oy) * FX;
  if (camview_valid()) return proj_camview_world_ord(wx + rx, wy + ry, wz + rz);
  return proj_obj_center_ord();   // same pre-scene-camera fallback obj_world_ord uses
}
} // namespace

// ==================================================================================================
// FUN_8003CCA4
void Render::perObjRenderDispatch() {
  Core* c = mCore;
  const uint32_t node = c->r[4];
  withDepthTag(c, node, [](Core* c) {
    CCA4Frame frame(c);
    const uint32_t node = c->r[4];
    // Register-faithfulness (2026-07-10, the f118 residual root cause — one level deeper than the
    // FUN_8003C048 ownership fix): gen_func_8003CCA4's REAL prologue (generated/shard_5.c:5060-5071)
    // reassigns r18 = r4 (node) IMMEDIATELY after its own spill, and computes r5 = ((mem8(node+13) ^
    // 15) < 1) ONCE, before the case switch — both values stay LIVE (plain MIPS register lifetime,
    // never re-set per case) all the way to whichever case's `func_8003CDD8(c)` call. This function's
    // own C++ body only ever needed the local `node`, so a prior draft never wrote c->r[18]/c->r[5] —
    // meaning cmdListDispatch's CmdListFrame (which spills "caller r18" as part of its own real
    // prologue) span stale bytes instead of gen's real node/flag, and cmdListDispatch's `flag` param
    // (c->r[5]) silently held garbage instead of gen's real per-node flag. Confirmed via
    // PSXPORT_SBS_PREWATCH=0x801FE8B8: core B's write came from gen_func_8003CDD8+0x18 (its own r18
    // spill) with the caller (gen_func_8003CCA4, reached via FUN_8003C048) holding r18=node, while
    // core A held whatever renderWalk's own r18 (CASE188_SCR, an unrelated constant) still was.
    c->r[18] = node;
    // gen: r3=mem8(node+11); r3^=15; r5=(r3<1) — the FLAG field is node+11 (NOT node+13, which is the
    // separate `sel` case-table index below). A prior draft of this fix used node+13 for both,
    // routing cmdListDispatch's flag&1 test the wrong way and making perModeDispatch pick the
    // per-mode table (native) instead of gen's real generic-fallback path (func_800803DC) for nodes
    // whose real flag has bit0 set — confirmed via PSXPORT_SBS_PREWATCH: core B's chain ended in
    // gen_func_800803DC while core A's ended in the per-mode target ov_a00_gen_80146478.
    const uint32_t flag = ((c->mem_r8(node + 11) ^ 15u) < 1u) ? 1u : 0u;
    c->mem_w32(CUR_NODE_SCR, node);
    const uint32_t sel = c->mem_r8(node + 13) & 11u;
    if (sel >= 9u) return;
    constexpr uint32_t TABLE = 0x80014EC8u;
    const uint32_t target = c->mem_r32(TABLE + sel * 4u);
    // RE'd return-address constants gen sets in r31 immediately before each nested call (see
    // generated/shard_5.c gen_func_8003CCA4). Register-faithfulness (2026-07-09, the f118 residual
    // root cause): a prior draft called cmdListDispatch()/the special-effect leaves without ever
    // setting c->r[31], leaving whatever stale value the OUTER caller (FUN_8003C048) left there
    // instead — a real, reproducible SBS diff at FUN_80146478's own ra spill slot (0x801FE8D0..),
    // several frames deep in this call chain. Mirrored per CLAUDE.md ("MIRROR THE GUEST STACK...
    // register-faithfulness"), same discipline as billboardCompose1/2's own fix (commit bef7769).
    switch (target) {
      case 0x8003CD00u: {
        c->r[4] = node; c->r[5] = flag; c->r[31] = 0x8003CD08u; rend(c)->cmdListDispatch();
        break;
      }
      case 0x8003CD10u: {
        const uint32_t pre = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = flag; c->r[31] = 0x8003CD20u; rend(c)->cmdListDispatch();
        const uint32_t post = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = pre; c->r[6] = post; c->r[31] = 0x8003CD30u; func_8003D584(c);
        break;
      }
      case 0x8003CD38u: {
        const uint32_t pre = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = flag; c->r[31] = 0x8003CD48u; rend(c)->cmdListDispatch();
        const uint32_t post = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = pre; c->r[6] = post; c->r[31] = 0x8003CD58u; func_8003F344(c);
        break;
      }
      case 0x8003CD60u: {
        const uint32_t pre = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = flag; c->r[31] = 0x8003CD70u; rend(c)->cmdListDispatch();
        const uint32_t post = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = pre; c->r[6] = post;
        // Branch polarity (2026-07-09, found during the same audit): gen_func_8003CCA4 L_8003CD60
        // tests node+27==0 -> func_8003F4C4 (the L_8003CD90 target), node+27!=0 -> func_8003F3F4 —
        // a prior draft had this INVERTED. Neither leaf fires at seaside (this file's own banner),
        // so the flip was never caught by the autonav gate; fixed here to match gen exactly.
        if (c->mem_r8(node + 27) == 0) { c->r[31] = 0x8003CD98u; func_8003F4C4(c); }
        else                            { c->r[31] = 0x8003CD88u; func_8003F3F4(c); }
        break;
      }
      case 0x8003CDA0u: {
        const uint32_t pre = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = flag; c->r[31] = 0x8003CDB0u; rend(c)->cmdListDispatch();
        const uint32_t post = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = pre; c->r[6] = post; c->r[31] = 0x8003CDC0u; func_8003F594(c);
        break;
      }
      case 0x8003CDC0u:
        break;   // no-op case: the recomp body falls straight to the epilogue
      default:
        // Defensive mirror of the recomp's raw `jr` fallback for an unrecognized table entry — never
        // hit by live game data (only the 6 cases above ever appear in the live table).
        rec_dispatch(c, target);
        return;
    }
  });
}

// ==================================================================================================
// Shared tail: compose CAM2 (camera rotation+translation) onto WORLD_POS, add it into MAT_OUT.t,
// reload CR0-7 from MAT_OUT, then hand off to billboardEmit(node, flag).
static void billboardComposeTail(Core* c, uint32_t node, uint32_t flag) {
  c->mem_w16(WORLD_POS + 0, c->mem_r16(node + 46));
  c->mem_w16(WORLD_POS + 2, c->mem_r16(node + 50));
  c->mem_w16(WORLD_POS + 4, c->mem_r16(node + 54));
  gte_write_ctrl(0, c->mem_r32(CAM2 + 0));
  gte_write_ctrl(1, c->mem_r32(CAM2 + 4));
  gte_write_ctrl(2, c->mem_r32(CAM2 + 8));
  gte_write_ctrl(3, c->mem_r32(CAM2 + 12));
  gte_write_ctrl(4, c->mem_r32(CAM2 + 16));
  gte_write_data(0, c->mem_r32(WORLD_POS + 0));
  gte_write_data(1, c->mem_r32(WORLD_POS + 4));
  gte_op(c, MVMVA_TRANS);
  c->mem_w32(MAT_OUT + 0x14, gte_read_data(25));
  c->mem_w32(MAT_OUT + 0x18, gte_read_data(26));
  c->mem_w32(MAT_OUT + 0x1C, gte_read_data(27));
  c->mem_w32(MAT_OUT + 0x14, c->mem_r32(MAT_OUT + 0x14) + c->mem_r32(CAM2 + 0x14));
  c->mem_w32(MAT_OUT + 0x18, c->mem_r32(MAT_OUT + 0x18) + c->mem_r32(CAM2 + 0x18));
  c->mem_w32(MAT_OUT + 0x1C, c->mem_r32(MAT_OUT + 0x1C) + c->mem_r32(CAM2 + 0x1C));
  gte_write_ctrl(0, c->mem_r32(MAT_OUT + 0));
  gte_write_ctrl(1, c->mem_r32(MAT_OUT + 4));
  gte_write_ctrl(2, c->mem_r32(MAT_OUT + 8));
  gte_write_ctrl(3, c->mem_r32(MAT_OUT + 12));
  gte_write_ctrl(4, c->mem_r32(MAT_OUT + 16));
  gte_write_ctrl(5, c->mem_r32(MAT_OUT + 0x14));
  gte_write_ctrl(6, c->mem_r32(MAT_OUT + 0x18));
  gte_write_ctrl(7, c->mem_r32(MAT_OUT + 0x1C));
  c->r[4] = node; c->r[5] = flag;
  rend(c)->billboardEmit();
}

// FUN_8003C2D4
void Render::billboardCompose1() {
  Core* c = mCore;
  const uint32_t node = c->r[4];
  if (c->mem_r32(node + 56) == 0) return;
  withDepthTag(c, node, [](Core* c) {
    GuestFrame frame(c, 40);
    // Register-faithfulness (gen_func_8003C2D4 prologue, L4509-4514): spill the caller's
    // r16..r19/ra at sp+16..+32. The GuestFrame only allocates the frame; the spill BYTES are
    // what SBS compares (gen writes them; the bare RAII left stale bytes there).
    const uint32_t sp = c->r[29];
    c->mem_w32(sp + 16, c->r[16]); c->mem_w32(sp + 20, c->r[17]);
    c->mem_w32(sp + 24, c->r[18]); c->mem_w32(sp + 28, c->r[19]);
    c->mem_w32(sp + 32, c->r[31]);
    const uint32_t node = c->r[4];
    mtxOf(c).identity(MAT_A);
    mtxOf(c).identity(MAT_ROTZ);
    mathOf(c).rotZ((int16_t)c->mem_r16(node + 90), MAT_ROTZ);
    const uint32_t flag = c->mem_r8(node + 71) & 1u;
    mathOf(c).matMul(MAT_ROTZ, MAT_A, MAT_OUT);
    // gen's live callee-saved state at the func_8003C8F4 call site (L4593-4595): billboardEmit
    // spills these as its "caller" registers, so they must hold gen's values here.
    c->r[16] = MAT_OUT; c->r[17] = MAT_A; c->r[18] = flag; c->r[19] = node;
    c->r[31] = 0x8003C448u;
    billboardComposeTail(c, node, flag);
    // Epilogue restore (gen_func_8003C2D4 L4597-4601): read the caller's values back from the spill
    // slots. MUST restore — the reassignments above (esp. r31=0x8003C448) would otherwise leak to the
    // substrate render-walk caller and corrupt its control flow (registers aren't SBS-compared, but
    // the substrate reads them).
    c->r[16] = c->mem_r32(sp + 16); c->r[17] = c->mem_r32(sp + 20);
    c->r[18] = c->mem_r32(sp + 24); c->r[19] = c->mem_r32(sp + 28);
    c->r[31] = c->mem_r32(sp + 32);
  });
}

// FUN_8003C464
void Render::billboardCompose2() {
  Core* c = mCore;
  const uint32_t node = c->r[4];
  if (c->mem_r32(node + 56) == 0) return;
  withDepthTag(c, node, [](Core* c) {
    GuestFrame frame(c, 32);
    // Register-faithfulness (gen_func_8003C464 prologue, L5907-5911): spill caller's
    // r16/r17/r18/ra at sp+16/+20/+24/+28. (C464's prologue does NOT spill r19 — it passes through.)
    const uint32_t sp = c->r[29];
    c->mem_w32(sp + 16, c->r[16]); c->mem_w32(sp + 20, c->r[17]);
    c->mem_w32(sp + 24, c->r[18]); c->mem_w32(sp + 28, c->r[31]);
    const uint32_t node = c->r[4];
    c->r[4] = MAT_A;
    c->r[5] = (uint32_t)c->mem_r16s(node + 122);
    c->r[6] = (uint32_t)c->mem_r16s(node + 124);
    c->r[7] = (uint32_t)c->mem_r16s(node + 126);
    func_800517BC(c);
    mtxOf(c).identity(MAT_ROTZ);
    mathOf(c).rotZ((int16_t)c->mem_r16(node + 90), MAT_ROTZ);
    const uint32_t flag = c->mem_r8(node + 71) & 1u;
    mathOf(c).matMul(MAT_ROTZ, MAT_A, MAT_OUT);
    // gen's live callee-saved state at the func_8003C8F4 call site (L5993-5995) — NOTE C464 differs
    // from C2D4: r17=flag (not MAT_A) and r18=node (gen reassigns r17 to flag at L5931 and keeps
    // r18=node from the prologue). billboardEmit spills these, so match gen exactly.
    c->r[16] = MAT_OUT; c->r[17] = flag; c->r[18] = node;
    c->r[31] = 0x8003C5E0u;
    billboardComposeTail(c, node, flag);
    // Epilogue restore (gen_func_8003C464): read the caller's values back from the spill slots
    // (r16/r17/r18/ra — C464 does not save r19). Same anti-leak discipline as billboardCompose1.
    c->r[16] = c->mem_r32(sp + 16); c->r[17] = c->mem_r32(sp + 20);
    c->r[18] = c->mem_r32(sp + 24); c->r[31] = c->mem_r32(sp + 28);
  });
}

// ==================================================================================================
// FUN_8003C8F4
void Render::billboardEmit() {
  Core* c = mCore;
  const uint32_t node = c->r[4];
  const uint32_t flag = c->r[5];
  if (c->mem_r32(node + 56) == 0) return;
  withDepthTag(c, node, [](Core* c) {
    GuestFrame frame(c, 96);   // real guest stack frame: func_8003B220 writes through this as a real
                                // guest address, and callers' own frames must already be allocated
                                // (see GuestFrame's comment) for this base to land on the same bytes
                                // the recomp path uses.
    // Register-faithfulness (gen_func_8003C8F4 prologue, L4367-4376): spill the caller's
    // r16..r22/ra at sp+64..+92. The spilled values are the caller's (billboardCompose1/2) live
    // callee-saved registers — which this port now sets correctly before the call (see above).
    const uint32_t sp = c->r[29];
    c->mem_w32(sp + 64, c->r[16]); c->mem_w32(sp + 68, c->r[17]);
    c->mem_w32(sp + 72, c->r[18]); c->mem_w32(sp + 76, c->r[19]);
    c->mem_w32(sp + 80, c->r[20]); c->mem_w32(sp + 84, c->r[21]);
    c->mem_w32(sp + 88, c->r[22]); c->mem_w32(sp + 92, c->r[31]);
    const uint32_t node = c->r[4];
    const uint32_t flag = c->r[5];
    auto FR = [c](uint32_t off) { return c->r[29] + off; };
    constexpr int32_t DEFAULT_DEPTH = -1;

    // Resolve the active particle sub-list.
    const uint32_t tbl = c->mem_r32(node + 56);
    const int idx = (int16_t)c->mem_r16(tbl + 0);
    const uint32_t listBase = c->mem_r32(node + 60);
    const uint32_t entry = listBase + (uint32_t)(idx << 2);
    const int16_t byteOff = (int16_t)c->mem_r16(entry + 2);
    int count = (int16_t)c->mem_r16(entry + 0);
    uint32_t particle = listBase + (uint32_t)(int32_t)byteOff;
    int bbIt = 0;   // particle index within THIS billboardEmit call (bbord diag: same-call grouping)
    for (; count != 0; count--, particle += 16u, bbIt++) {
      // 1) Build the quad's 4 corner vectors (still-substrate; writes real guest stack memory).
      c->r[4] = FR(16); c->r[5] = 0; c->r[6] = particle;
      func_8003B220(c);

      gte_write_data(0, c->mem_r32(FR(16) + 0));
      gte_write_data(1, c->mem_r32(FR(16) + 4));
      gte_write_data(2, c->mem_r32(FR(16) + 8));
      gte_write_data(3, c->mem_r32(FR(16) + 12));
      gte_write_data(4, c->mem_r32(FR(16) + 16));
      gte_write_data(5, c->mem_r32(FR(16) + 20));
      gte_op(c, 0x4A280030u);                    // RTPT: project V0-2 -> SXY0-2
      int32_t ctrl31 = (int32_t)gte_read_ctrl(31);
      c->mem_w32(FR(48), (uint32_t)ctrl31);
      int32_t depth;
      if (ctrl31 < 0) {
        depth = DEFAULT_DEPTH;
      } else {
        c->mem_w32(BUF + 8,  gte_read_data(12));
        c->mem_w32(BUF + 16, gte_read_data(13));
        c->mem_w32(BUF + 24, gte_read_data(14));
        gte_op(c, 0x4B400006u);                  // AVSZ3
        c->mem_w32(FR(48), gte_read_data(24));
        gte_write_data(0, c->mem_r32(FR(40) + 0));
        gte_write_data(1, c->mem_r32(FR(40) + 4));
        gte_op(c, 0x4A180001u);                  // RTPS: project V3
        ctrl31 = (int32_t)gte_read_ctrl(31);
        c->mem_w32(FR(48), (uint32_t)ctrl31);
        if (ctrl31 >= 0) {
          c->mem_w32(BUF + 32, gte_read_data(14));
          gte_op(c, 0x4B68002Eu);                // AVSZ4 -> OTZ
          c->mem_w32(FR(52), gte_read_data(7));
          depth = (int32_t)c->mem_r32(FR(52));
        } else {
          depth = DEFAULT_DEPTH;
        }
      }
      c->mem_w32(FR(56), (uint32_t)depth);

      // 2) Off-screen cull: skip if all 4 corners' X>=xmax or all 4 corners' Y>=240 (unsigned compares —
      // matches the recomp's zero-extended 16-bit reads). xmax follows submit.cpp's submit_xmax
      // precedent (later-119 / USER 2026-07-16 "extend widescreen render culling area"): under the
      // genuine engine-wide FOV (OFX=nw/2, already a sanctioned wide-mode guest deviation; SBS legs
      // run 4:3 so byte-exactness is untouched) the screen extends to the wide width — the stock 320
      // gate was culling this class out of the right wide band.
      int gpu_gpu_wide_engine(Core*), gpu_gpu_wide_engine_w(Core*);
      const uint32_t xmax = gpu_gpu_wide_engine(c) ? (uint32_t)gpu_gpu_wide_engine_w(c) : 320u;
      bool onX = (uint32_t)c->mem_r16(BUF + 8)  < xmax ||
                 (uint32_t)c->mem_r16(BUF + 16) < xmax ||
                 (uint32_t)c->mem_r16(BUF + 24) < xmax ||
                 (uint32_t)c->mem_r16(BUF + 32) < xmax;
      if (!onX) continue;
      bool onY = (uint32_t)c->mem_r16(BUF + 10) < 240u ||
                 (uint32_t)c->mem_r16(BUF + 18) < 240u ||
                 (uint32_t)c->mem_r16(BUF + 26) < 240u ||
                 (uint32_t)c->mem_r16(BUF + 34) < 240u;
      if (!onY) continue;

      // 3) Quantize into an OT bucket: node's signed per-node depth-bias byte, >>10/<<9 rebucket into
      // the valid range, else reset to invalid (-1).
      {
        int32_t a = (int32_t)c->mem_r32(FR(56)) + (int8_t)c->mem_r8(node + 8);
        int32_t shiftAmt = a >> 10;
        int32_t bucketed = a >> (shiftAmt & 31);
        int32_t d = bucketed + (shiftAmt << 9);
        c->mem_w32(FR(56), (uint32_t)d);
        if ((uint32_t)(d - 4) >= 2044u) c->mem_w32(FR(56), (uint32_t)DEFAULT_DEPTH);
      }
      if ((int32_t)c->mem_r32(FR(56)) < 0) continue;

      // 4) Fill color/UV (still-substrate), then optional overrides.
      c->r[4] = BUF; c->r[5] = particle; c->r[6] = flag;
      func_8003B054(c);
      if (c->mem_r16(node + 92) != 0) c->mem_w16(BUF + 14, c->mem_r16(node + 92));

      const uint32_t caseSel = c->mem_r8(node + 13);
      if (caseSel < 33u) {
        constexpr uint32_t CASE_TABLE = 0x80014E40u;
        const uint32_t caseTarget = c->mem_r32(CASE_TABLE + caseSel * 4u);
        switch (caseTarget) {
          case 0x8003CB60u: c->mem_w8(BUF + 7, 45); break;
          case 0x8003CB6Cu: c->mem_w8(BUF + 7, 47); break;
          case 0x8003CB78u:
            c->mem_w32(BUF + 4, c->mem_r32(node + 24));
            c->mem_w8(BUF + 7, 44);
            break;
          case 0x8003CB90u:
            c->mem_w32(BUF + 4, c->mem_r32(node + 24));
            c->mem_w8(BUF + 7, 46);
            break;
          case 0x8003CBA8u:
            c->mem_w8(BUF + 7, 45);
            c->mem_w16(BUF + 14, c->mem_r8(node + 24) != 0 ? 16507u : 16443u);
            break;
          case 0x8003CBC8u: break;   // explicit no-op case: falls through to packet emission
          default:
            // Defensive mirror of the recomp's raw `jr` fallback for an unrecognized table entry: the
            // recomp body does `rec_dispatch(c, caseTarget); return` here — a FULL early return, NOT a
            // fallthrough to packet emission. Never hit by live game data (this 33-entry table's slots
            // all resolve to one of the 5 cases above or the CBC8 no-op).
            rec_dispatch(c, caseTarget);
            return;
        }
      }

      // 5) Emit the 10-word packet (tag + 9 data words) at the pool tail, prepended into the OT bucket.
      const uint32_t packetLo = c->mem_r32(PKT_POOL_PTR);
      uint32_t tail = packetLo;
      uint32_t otbase = c->mem_r32(OTBASE_PTR);
      uint32_t otslot = otbase + (c->mem_r32(FR(56)) << 2);
      uint32_t oldHead = c->mem_r32(otslot);
      c->mem_w32(tail, oldHead | 0x09000000u);
      c->mem_w32(otslot, tail);
      tail += 4;
      for (uint32_t off = 4; off <= 36; off += 4) {
        c->mem_w32(tail, c->mem_r32(BUF + off));
        tail += 4;
      }
      c->mem_w32(PKT_POOL_PTR, tail);

      // Per-particle depth registration (render.md#56): tag THIS particle's own packet span with its
      // own world-view ord, instead of relying solely on withDepthTag's single whole-span registration
      // (which only fires on function exit, from `sess.close()` AFTER this loop finishes). obj_depth_
      // lookup scans registered spans in REGISTRATION ORDER and returns the FIRST containing match, so
      // — since every per-particle span here is registered strictly BEFORE withDepthTag's own coarse
      // whole-node span — a lookup for one of these packet's bytes always resolves to this particle's
      // OWN ord, never the coarse fallback. No suppression of the outer span needed; it stays the
      // fallback for anything this loop doesn't cover (skipped/off-screen particles emit no packet).
      // Host-only: GpuState::obj_depth_add stores into per-Core host arrays, no guest write. Skipped on
      // the SBS oracle core exactly like withDepthTag skips its own registration there (core B/
      // psx_fallback must stay the untouched reference).
      if (!c->game->oracle) {
        const float pord = billboardParticleOrd(c, node, particle);
        gpu_obj_depth_add(c, packetLo, tail, pord);
        // #67 (replaces the #65 DUAL-EMIT, deleted break-first): RECORD this particle for the
        // display-pass producer Render::billboardsRender instead of pushing the GTE-computed SXYs
        // verbatim. The record is pure game state resolved this tick — local corners (the ×5 ints
        // func_8003B220 just built into the guest-stack scratch), the node's composed MAT_OUT
        // rotation + world anchor, and the RESOLVED material words (post node+92 override +
        // node+13 case patches). billboardsRender projects it through the SAME float camera path
        // the world uses, so the fps60 interp re-run derives the quad under lerped inputs — real
        // and interpolated frames are made identically (USER principle). Host memory only.
        {
          Render::BbRec rb;
          rb.node = node; rb.particle = particle;
          const uint32_t vbase = FR(16);
          rb.cx[0] = c->mem_r16s(vbase + 0);  rb.cy[0] = c->mem_r16s(vbase + 2);
          rb.cx[1] = c->mem_r16s(vbase + 8);  rb.cy[1] = c->mem_r16s(vbase + 10);
          rb.cx[2] = c->mem_r16s(vbase + 16); rb.cy[2] = c->mem_r16s(vbase + 18);
          rb.cx[3] = c->mem_r16s(vbase + 24); rb.cy[3] = c->mem_r16s(vbase + 26);
          // Rotation = the LIVE GTE CR0-4 — whatever matrix this node's compose variant loaded
          // (compose1/2 → MAT_OUT @BUF+0x40, but FUN_8003C5F8 composes into BUF+0x40 via a
          // different chain and FUN_8003C788 into BUF+0x20 — reading a fixed scratch address
          // captured the WRONG rotation for the C788 class; the CRs are always the truth).
          // billboardEmit's own RTPT/RTPS ops never modify the control regs.
          { uint32_t g0 = gte_read_ctrl(0), g1 = gte_read_ctrl(1), g2 = gte_read_ctrl(2),
                     g3 = gte_read_ctrl(3), g4 = gte_read_ctrl(4);
            rb.rotR[0][0] = (int16_t)g0;         rb.rotR[0][1] = (int16_t)(g0 >> 16); rb.rotR[0][2] = (int16_t)g1;
            rb.rotR[1][0] = (int16_t)(g1 >> 16); rb.rotR[1][1] = (int16_t)g2;         rb.rotR[1][2] = (int16_t)(g2 >> 16);
            rb.rotR[2][0] = (int16_t)g3;         rb.rotR[2][1] = (int16_t)(g3 >> 16); rb.rotR[2][2] = (int16_t)g4; }
          // World anchor factored from the LIVE CR5-7 view translation (tr = cam·pos + cam.t for
          // every compose variant ⇒ camᵀ·(tr − camT) = pos exactly) — not a node-field assumption.
          { float camR[3][3], camT[3];
            wq_read_matrix(c, 0x1F8000F8u, camR, camT);
            float tr[3];
            for (int i = 0; i < 3; i++) tr[i] = (float)(int32_t)gte_read_ctrl(5u + (unsigned)i);
            rb.wx = camR[0][0]*(tr[0]-camT[0]) + camR[1][0]*(tr[1]-camT[1]) + camR[2][0]*(tr[2]-camT[2]);
            rb.wy = camR[0][1]*(tr[0]-camT[0]) + camR[1][1]*(tr[1]-camT[1]) + camR[2][1]*(tr[2]-camT[2]);
            rb.wz = camR[0][2]*(tr[0]-camT[0]) + camR[1][2]*(tr[1]-camT[1]) + camR[2][2]*(tr[2]-camT[2]); }
          rb.wColor = c->mem_r32(BUF + 4);
          rb.wUv0 = c->mem_r32(BUF + 12); rb.wUv1 = c->mem_r32(BUF + 20);
          rb.wUv2 = c->mem_r32(BUF + 28); rb.wUv3 = c->mem_r32(BUF + 36);
          rend(c)->mBbRecs.push_back(rb);
        }
        // Diagnostic (PSXPORT_DEBUG=bbord): prove the per-particle registration carries DISTINCT ords
        // (before this change every particle of a manager node shared obj_world_ord(node)'s single ord).
        if (cfg_dbg("bbord")) {
          int gpu_gpu_preseq_present_index(Core*);
          cfg_logf("bbord", "pf=%d call=%08X it=%d part=%08X off=%d,%d nodeOrd=%.6f partOrd=%.6f span=[%08X,%08X)",
                  gpu_gpu_preseq_present_index(c), node, bbIt, particle,
                  c->mem_r8s(particle + 14), c->mem_r8s(particle + 15),
                  obj_world_ord(c, node), pord, packetLo, tail);
        }
      }
    }
  });
}

// ==================================================================================================
namespace {
// Engine/game natives installed into the process-global g_override[] table. These are NOT gated
// here — the gate lives in ONE place (the override registry, runtime/recomp/override_registry.h)
// so it can't be forgotten cluster-by-cluster. engine_set_override_main() installs into that
// registry, which runs the real gen_func_* body on the oracle (psx_fallback) and the native
// everywhere else.
void ov_perObjRenderDispatch(Core* c) { rend(c)->perObjRenderDispatch(); }
void ov_billboardCompose1(Core* c)    { rend(c)->billboardCompose1(); }
void ov_billboardCompose2(Core* c)    { rend(c)->billboardCompose2(); }
void ov_billboardEmit(Core* c)        { rend(c)->billboardEmit(); }
}

// ==================================================================================================
// billboardsRender — the DISPLAY-PASS billboard producer (#67; REDIRECT doctrine: RE+port, not
// stamping). Projects every BbRec billboardEmit captured this logic frame through the SAME float
// camera path the rest of the world uses (Fps60::sceneCam — the choke the fps60 interp present serves
// a lerp(prev,cur) camera through), reproducing the GTE compose exactly in float:
//   CR rotation  = MAT_OUT.R                      -> corner_view = MAT_OUT.R·corner/4096 + t
//   CR translate = CAM2.R·WORLD_POS + CAM2.t      -> t = Rcam·anchor/4096 + Tcam  (CAM2 == sceneCam's
//                                                    scratchpad view matrix, see the header banner)
//   sx = OFX + vx·H/pz, pz = max(H/2, vz)          -> the same projection camWorldScreen/terrain use.
// Emits RQ_WORLD float quads (has_xyf=1 → tier1-owned, rebuilt on the interp present) with real
// per-particle identity (dbg_node = node, records in guest emit order). At the interp re-run
// (Fps60::mObjOverrideOn) each record is component-lerped against the PREVIOUS frame's record for the
// same particle (anchor + corners + rotation), so particle animation interpolates too — the
// per-particle motion the retired screen-space anchor machinery approximated, now derived from state.
// Read-only over guest memory (reads nothing but the camera via sceneCam); host writes only.
// emitRecQuad — shared record-quad emit for billboardsRender's two record kinds. Decodes the FT4
// record's material words RAW (mode/tp_x/tp_y/blend from the tpage half, clut x/y from the clut
// half; neutral texture-window, full draw-area) — NOT from GpuState's live s_tp_*/s_da_* fields,
// which hold unrelated stale state at display time. Float verts + real per-vertex depth + the
// drawWorldQuad draw-offset convention; dbg_node = the owning node (real identity).
static void emitRecQuad(Core* c, uint32_t node, const uint32_t wCol[4],
                        uint32_t wUv0, uint32_t wUv1, uint32_t wUv2, uint32_t wUv3,
                        const float* px, const float* py, const float* dep) {
  const uint8_t  op   = (uint8_t)(wCol[0] >> 24);
  const uint32_t clut = wUv0 >> 16;
  const uint32_t tp   = wUv1 >> 16;
  int us[4] = { (int)(wUv0 & 0xFFu), (int)(wUv1 & 0xFFu), (int)(wUv2 & 0xFFu), (int)(wUv3 & 0xFFu) };
  int vs[4] = { (int)((wUv0 >> 8) & 0xFFu), (int)((wUv1 >> 8) & 0xFFu),
                (int)((wUv2 >> 8) & 0xFFu), (int)((wUv3 >> 8) & 0xFFu) };
  unsigned char rs[4], gsv[4], bs[4];
  for (int i = 0; i < 4; i++) {
    rs[i]  = (unsigned char)(wCol[i] & 0xFF);
    gsv[i] = (unsigned char)((wCol[i] >> 8) & 0xFF);
    bs[i]  = (unsigned char)((wCol[i] >> 16) & 0xFF);
  }
  GpuState& gs = c->game->gpu;
  gs.s_seen3d = 1;
  int xs[4], ys[4]; float xsf[4], ysf[4];
  for (int i = 0; i < 4; i++) {
    xsf[i] = px[i] + (float)gs.s_off_x;
    ysf[i] = py[i] + (float)gs.s_off_y;
    xs[i] = (int)(px[i] < 0 ? px[i] - 0.5f : px[i] + 0.5f) + gs.s_off_x;
    ys[i] = (int)(py[i] < 0 ? py[i] - 0.5f : py[i] + 0.5f) + gs.s_off_y;
  }
  c->rsub.diag.beginObject(node);
  c->game->activeRq().emitOrQueue(c, 1, RQ_WORLD, RQ_OM_DEPTH, 4,
                   (op & 2) ? 1 : 0, (op & 1) ? 1 : 0,
                   xs, ys, xsf, ysf, us, vs, rs, gsv, bs, dep,
                   (int)((tp >> 7) & 3u),
                   (int)(tp & 0xFu) * 64, (int)((tp >> 4) & 1u) * 256,
                   (int)(clut & 0x3Fu) * 16, (int)((clut >> 6) & 0x1FFu),
                   0, 0, 0, 0, 0, 0, 1023, 511, (int)((tp >> 5) & 3u));
  c->rsub.diag.endObject();
}

void Render::billboardsRender() {
  Core* c = mCore;
  if (mBbRecs.empty() && mWqRecs.empty()) return;

  float Ri[3][3], T[3], ofx, ofy, H;
  c->game->fps60.sceneCam(c, Ri, T, ofx, ofy, H);   // raw int16-unit rows, the sceneCam convention
  if (H <= 0.0f) return;
  constexpr float FX = 1.0f / 4096.0f;
  float R[3][3];
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = Ri[i][j] * FX;

  // fps60 interp re-run: WqRecs lerp against the previous frame (stable node/seq identity). BbRec
  // PARTICLES do NOT lerp — the effect sub-lists reuse/walk particle addresses every frame, so no
  // stable cross-frame identity exists; a particle-addr-keyed lerp blended DIFFERENT sprites'
  // positions (USER: "gems rendered at two different places between real and interpolated frames").
  // A particle draws at its own frame's state under the LERPED camera: world-glued, no ghosting;
  // its animation steps at the logic rate, which is what the state actually says.
  const bool interp = c->game->fps60.mObjOverrideOn;
  const float t = c->game->fps60.mT;

  for (const BbRec& rc : mBbRecs) {
    // This frame's own state (no per-particle lerp — see banner above); camera lerp comes via R/T.
    const float wx = rc.wx, wy = rc.wy, wz = rc.wz;
    float cxf[4], cyf[4], rot[3][3];
    for (int i = 0; i < 4; i++) { cxf[i] = rc.cx[i]; cyf[i] = rc.cy[i]; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) rot[i][j] = rc.rotR[i][j] * FX;

    // t = Rcam·anchor + Tcam (the MVMVA CAM2 compose, in float).
    const float ax = R[0][0]*wx + R[0][1]*wy + R[0][2]*wz + T[0];
    const float ay = R[1][0]*wx + R[1][1]*wy + R[1][2]*wz + T[1];
    const float az = R[2][0]*wx + R[2][1]*wy + R[2][2]*wz + T[2];

    float px[4], py[4], dep[4];
    bool behind = false;
    for (int i = 0; i < 4; i++) {
      const float vx = rot[0][0]*cxf[i] + rot[0][1]*cyf[i] + ax;   // corner z==0 in local space
      const float vy = rot[1][0]*cxf[i] + rot[1][1]*cyf[i] + ay;
      const float vz = rot[2][0]*cxf[i] + rot[2][1]*cyf[i] + az;
      if (vz <= 0.0f) { behind = true; break; }
      float pz = H * 0.5f; if (vz > pz) pz = vz;                   // near-plane clamp (world convention)
      const float ph = H / pz;
      px[i] = ofx + vx * ph;
      py[i] = ofy + vy * ph;
      dep[i] = c->rsub.projParams.pzToOrd(vz);
    }
    if (behind) continue;

    { const uint32_t wc[4] = { rc.wColor, rc.wColor, rc.wColor, rc.wColor };
      emitRecQuad(c, rc.node, wc, rc.wUv0, rc.wUv1, rc.wUv2, rc.wUv3, px, py, dep); }
  }

  // ---- WqRecs: the generic composed-CR quad classes (submitQuad callers — render.h WqRec banner).
  // Re-compose each record's FACTORED world transform with the (fps60-lerped) camera and project:
  // corner_view = Rcam·(objR·corner + objT) + Tcam — identical derivation on real and interp frames.
  std::unordered_map<uint64_t, const WqRec*> wprev;
  if (interp) for (const WqRec& p : mWqRecsPrev)
    wprev.emplace(((uint64_t)p.node << 8) | p.seq, &p);
  for (const WqRec& rc : mWqRecs) {
    float objR[3][3], objT[3];
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) objR[i][j] = rc.objR[i][j]; objT[i] = rc.objT[i]; }
    float vxl[4][3];
    for (int i = 0; i < 4; i++) { vxl[i][0] = rc.vx[i]; vxl[i][1] = rc.vy[i]; vxl[i][2] = rc.vz[i]; }
    if (interp) {
      auto it = wprev.find(((uint64_t)rc.node << 8) | rc.seq);
      if (it != wprev.end()) {
        const WqRec& pv = *it->second;
        auto L = [t](float a, float b) { return a + (b - a) * t; };
        for (int i = 0; i < 3; i++) {
          for (int j = 0; j < 3; j++) objR[i][j] = L(pv.objR[i][j], rc.objR[i][j]);
          objT[i] = L(pv.objT[i], rc.objT[i]);
        }
        for (int i = 0; i < 4; i++) {
          vxl[i][0] = L(pv.vx[i], rc.vx[i]); vxl[i][1] = L(pv.vy[i], rc.vy[i]); vxl[i][2] = L(pv.vz[i], rc.vz[i]);
        }
      }
    }
    float px[4], py[4], dep[4];
    bool behind = false;
    for (int i = 0; i < 4; i++) {
      const float wxp = objR[0][0]*vxl[i][0] + objR[0][1]*vxl[i][1] + objR[0][2]*vxl[i][2] + objT[0];
      const float wyp = objR[1][0]*vxl[i][0] + objR[1][1]*vxl[i][1] + objR[1][2]*vxl[i][2] + objT[1];
      const float wzp = objR[2][0]*vxl[i][0] + objR[2][1]*vxl[i][1] + objR[2][2]*vxl[i][2] + objT[2];
      const float vx = R[0][0]*wxp + R[0][1]*wyp + R[0][2]*wzp + T[0];
      const float vy = R[1][0]*wxp + R[1][1]*wyp + R[1][2]*wzp + T[1];
      const float vz = R[2][0]*wxp + R[2][1]*wyp + R[2][2]*wzp + T[2];
      if (vz <= 0.0f) { behind = true; break; }
      float pz = H * 0.5f; if (vz > pz) pz = vz;
      const float ph = H / pz;
      px[i] = ofx + vx * ph;
      py[i] = ofy + vy * ph;
      dep[i] = c->rsub.projParams.pzToOrd(vz);
    }
    if (behind) continue;

    emitRecQuad(c, rc.node, rc.wCol, rc.wUv0, rc.wUv1, rc.wUv2, rc.wUv3, px, py, dep);
  }
}

void perobj_billboard_install() {
  static bool done = false;
  if (done) return;
  done = true;
  // engine_set_override_main (runtime/recomp/override_registry.h) installs into the ONE
  // process-global override registry, which runs gen_func_* on the oracle leg (core B) and the
  // native handler everywhere else — NOT a raw shard_set_override, since these are engine/game
  // natives and the oracle must run the pure recompiled body for them.
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x8003CCA4u, ov_perObjRenderDispatch, gen_func_8003CCA4);
  engine_set_override_main(0x8003C2D4u, ov_billboardCompose1,    gen_func_8003C2D4);
  engine_set_override_main(0x8003C464u, ov_billboardCompose2,    gen_func_8003C464);
  engine_set_override_main(0x8003C8F4u, ov_billboardEmit,        gen_func_8003C8F4);
}
