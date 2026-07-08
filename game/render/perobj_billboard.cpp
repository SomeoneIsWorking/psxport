// perobj_billboard.cpp — SUBSTRATE MIRROR for the per-object render-TYPE dispatch (FUN_8003CCA4) and
// 3 of its "special effect" billboard/particle-quad leaf renderers (FUN_8003C2D4, FUN_8003C464,
// FUN_8003C8F4). Same band (0x8003xxxx) as perobj_dispatch.cpp's cmdListDispatch/perModeDispatch;
// same ownership mechanism (shard_set_override — these are reached as PLAIN intra-shard C calls from
// the still-substrate walk cluster gen_func_8003BF00/etc, never through rec_dispatch).
//
// PRIOR STATE: all 4 addresses carried a TRANSPARENT RenderObserver wrapper (render_observer.cpp) that
// ran the literal gen_func_* body then host-side depth-tagged the packet span it wrote
// (obj_world_ord/gpu_obj_depth_add/fps60_bb_node — the billboard-occlusion fix, issue #4 class). Owning
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
// billboardEmit. Both use a shared, non-scratchpad per-instance scratch region at main-RAM 0x800C0000
// (BUF below) that holds ordinary libgte MATRIX structs (m[3][3] int16 row-major + 2 pad bytes + t[3]
// int32 — exactly what Mtx::identity's 8-word write pattern and Math::rotZ/matMul's byte
// reads agree on):
//   BUF+0x00 MAT_A     — C2D4: identity (Mtx::identity). C464: seeded by the still-substrate
//                         FUN_800517BC(node+122/124/126 as s16 x,y,z) instead of identity.
//   BUF+0x20 MAT_ROTZ  — identity, then Z-rotated in place by mem16(node+90) via Math::rotZ.
//   BUF+0x40 MAT_OUT   — Math::matMul(MAT_ROTZ, MAT_A, MAT_OUT) (= MAT_ROTZ for C2D4, since MAT_A is
//                         identity there); .t (=MAT_OUT+0x14) becomes the composed WORLD translation.
//   BUF+0xC0 WORLD_POS — object's world position triple (s16 x3, from node+46/50/54).
//   BUF+0xF8 CAM2      — a PERSISTENT camera MATRIX mirror in main RAM (not the scratchpad SCR/CAM_ROT
//                         perobj_dispatch.cpp already owns) — read-only here, set up elsewhere.
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
#include "game.h"
#include "render.h"
#include "cfg.h"
#include "pkt_span.h"
#include "render_internal.h"   // obj_world_ord / gpu_obj_depth_add / fps60_bb_node

void rec_dispatch(Core*, uint32_t);
void shard_set_override(uint32_t addr, OverrideFn fn);   // generated/shard_disp.c (C++ linkage)

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
constexpr uint32_t BUF          = 0x800C0000u;   // per-instance MATRIX-compose scratch (C2D4/C464/C8F4)
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

// Guest-transparent depth-tag wrap (RenderObserver's obs_body, folded in): PSXPORT_ORACLE runs pure,
// everyone else opens a nested PktSpanSession and tags the packet span this call emits with the
// object's PC-native world depth.
void withDepthTag(Core* c, uint32_t node, void (*body)(Core*)) {
  if (oracle_mode()) { body(c); return; }
  c->mRender->diag.beginObject(node);
  uint32_t slo, shi;
  PktSpanSession sess(c);
  body(c);
  if (sess.close(&slo, &shi)) {
    float od = obj_world_ord(c, node);
    gpu_obj_depth_add(c, slo, shi, od);
    fps60_bb_node(c, slo, shi, node);
  }
  c->mRender->diag.endObject();
}
} // namespace

// ==================================================================================================
// FUN_8003CCA4
void Render::perObjRenderDispatch() {
  Core* c = mCore;
  const uint32_t node = c->r[4];
  withDepthTag(c, node, [](Core* c) {
    GuestFrame frame(c, 32);
    const uint32_t node = c->r[4];
    c->mem_w32(CUR_NODE_SCR, node);
    const uint32_t sel = c->mem_r8(node + 13) & 11u;
    if (sel >= 9u) return;
    constexpr uint32_t TABLE = 0x80014EC8u;
    const uint32_t target = c->mem_r32(TABLE + sel * 4u);
    switch (target) {
      case 0x8003CD00u: {
        c->r[4] = node; c->mRender->cmdListDispatch();
        break;
      }
      case 0x8003CD10u: {
        const uint32_t pre = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->mRender->cmdListDispatch();
        const uint32_t post = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = pre; c->r[6] = post; func_8003D584(c);
        break;
      }
      case 0x8003CD38u: {
        const uint32_t pre = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->mRender->cmdListDispatch();
        const uint32_t post = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = pre; c->r[6] = post; func_8003F344(c);
        break;
      }
      case 0x8003CD60u: {
        const uint32_t pre = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->mRender->cmdListDispatch();
        const uint32_t post = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = pre; c->r[6] = post;
        if (c->mem_r8(node + 27) == 0) func_8003F3F4(c); else func_8003F4C4(c);
        break;
      }
      case 0x8003CDA0u: {
        const uint32_t pre = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->mRender->cmdListDispatch();
        const uint32_t post = c->mem_r32(PKT_POOL_PTR);
        c->r[4] = node; c->r[5] = pre; c->r[6] = post; func_8003F594(c);
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
  c->mRender->billboardEmit();
}

// FUN_8003C2D4
void Render::billboardCompose1() {
  Core* c = mCore;
  const uint32_t node = c->r[4];
  if (c->mem_r32(node + 56) == 0) return;
  withDepthTag(c, node, [](Core* c) {
    GuestFrame frame(c, 40);
    const uint32_t node = c->r[4];
    c->mtx.identity(MAT_A);
    c->mtx.identity(MAT_ROTZ);
    c->math.rotZ((int16_t)c->mem_r16(node + 90), MAT_ROTZ);
    const uint32_t flag = c->mem_r8(node + 71) & 1u;
    c->math.matMul(MAT_ROTZ, MAT_A, MAT_OUT);
    billboardComposeTail(c, node, flag);
  });
}

// FUN_8003C464
void Render::billboardCompose2() {
  Core* c = mCore;
  const uint32_t node = c->r[4];
  if (c->mem_r32(node + 56) == 0) return;
  withDepthTag(c, node, [](Core* c) {
    GuestFrame frame(c, 32);
    const uint32_t node = c->r[4];
    c->r[4] = MAT_A;
    c->r[5] = (uint32_t)c->mem_r16s(node + 122);
    c->r[6] = (uint32_t)c->mem_r16s(node + 124);
    c->r[7] = (uint32_t)c->mem_r16s(node + 126);
    func_800517BC(c);
    c->mtx.identity(MAT_ROTZ);
    c->math.rotZ((int16_t)c->mem_r16(node + 90), MAT_ROTZ);
    const uint32_t flag = c->mem_r8(node + 71) & 1u;
    c->math.matMul(MAT_ROTZ, MAT_A, MAT_OUT);
    billboardComposeTail(c, node, flag);
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
    for (; count != 0; count--, particle += 16u) {
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

      // 2) Off-screen cull: skip if all 4 corners' X>=320 or all 4 corners' Y>=240 (unsigned compares —
      // matches the recomp's zero-extended 16-bit reads).
      bool onX = (uint32_t)c->mem_r16(BUF + 8)  < 320u ||
                 (uint32_t)c->mem_r16(BUF + 16) < 320u ||
                 (uint32_t)c->mem_r16(BUF + 24) < 320u ||
                 (uint32_t)c->mem_r16(BUF + 32) < 320u;
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
      uint32_t tail = c->mem_r32(PKT_POOL_PTR);
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
    }
  });
}

// ==================================================================================================
namespace {
void ov_perObjRenderDispatch(Core* c) { c->mRender->perObjRenderDispatch(); }
void ov_billboardCompose1(Core* c)    { c->mRender->billboardCompose1(); }
void ov_billboardCompose2(Core* c)    { c->mRender->billboardCompose2(); }
void ov_billboardEmit(Core* c)        { c->mRender->billboardEmit(); }
}

void perobj_billboard_install() {
  static bool done = false;
  if (done) return;
  done = true;
  shard_set_override(0x8003CCA4u, ov_perObjRenderDispatch);
  shard_set_override(0x8003C2D4u, ov_billboardCompose1);
  shard_set_override(0x8003C464u, ov_billboardCompose2);
  shard_set_override(0x8003C8F4u, ov_billboardEmit);
}
