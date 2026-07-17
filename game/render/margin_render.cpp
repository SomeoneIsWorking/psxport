// Tomba2Engine — native widescreen-margin renderer. See margin_render.h for the architecture.
// RE: docs/journal.md later-133, docs/engine_re.md "Deferred render pipeline".
//
// flush() used to dispatch two guest functions (T2_BUILD_XFORM=0x80051C8C, T2_PEROBJ_RENDER=0x8003CCA4)
// to refresh a culled margin node's stale render transform and submit its command list. Both WRITE guest
// RAM (node+0x98/0xAC rotation+translation cache, cmd+0x18/0x2C Robj/Tobj) — a violation of the
// pc_render read-only-overlay rule (CLAUDE.md). This is a HOST-ONLY reimplementation: it reads the same
// node/cmd fields those guest functions read (euler angles, position, the command array) and rebuilds
// the transform in plain host float, then submits through the already-native, already guest-write-free
// Render::gt3gt4 -> submitPolyGt3Native/submitPolyGt4Native path (the same submit path
// Render::perObjFlush uses for every live object, render_walk.cpp). No mem_w*, no rec_dispatch, no GTE
// writes anywhere in this file.
//
// Math note: 0x80051C8C seeds an identity MATRIX at node+0x98 then composes 3 sequential fixed-point
// axis rotations (rotX, then rotY, then rotZ) from node+0x54/56/58 (euler, 4096 units/circle), overwrites
// the translation with node+0x2E/32/36 (raw position). Its callee func_80051464 does the same per-command
// local rotation (cmd+0x08/0x0A/0x0C) and composes it against either the node's matrix (ROOT, cmd+6==-1)
// or an earlier sibling command's matrix (cmd+6==sentinel index into node+0xC0), then transforms the
// command's local anchor (cmd+0x00/02/04) through the PARENT matrix (not the just-computed one — a real
// guest quirk, see NodeXform::propagate for the same pattern) to get the world translation. The margin
// path is a host-only overlay (never SBS byte-compared — it's OFF at 4:3, where SBS runs), so we
// reproduce this in plain float (real sin/cos of angle*2*pi/4096) rather than bit-exact PSX fixed-point:
// the picture is what matters here, not a byte match.
#include "margin_render.h"
#include "game_ctx.h"
#include "render.h"
#include "projection.h"
#include "game.h"
#include "cfg.h"
#include <vector>
#include <unordered_set>
#include <cstdio>
#include <cmath>

// mem_r8/mem_w8 come from r3000.h (via margin_render.h).
int gpu_frame_no(Core*);                             // present-frame counter (gpu_native.cpp)

// Entity type (node+0xc) of the static world-geometry objects rendered via this per-object path.
// later-133: exactly these account for the +24 widescreen-margin commands at the field.
#define T2_WORLDGEO_TYPE 0x03

// ---- HOST-ONLY mirror of 0x80051C8C + func_80051464's rotation/translation compose ------------------
// Unit (unscaled, diag=1) float rotation matrices — NOT the guest's 1.3.12 fixed-point (diag=4096)
// convention. Robj is scaled up to that convention only at the Render::projComposeObjectHost call site,
// to match Rcam's raw-int16 scale (see projection.cpp). Tobj/anchor stay in raw position units throughout
// (matches applyMatlv's net effect: rotate-then->>12, i.e. exactly a unit-matrix * raw-vector multiply).
static constexpr float kMarginTau = 6.283185307179586f;   // 2*pi — 4096 angle units per full circle

// rowA/rowB are ROW INDICES (0,1,2) into a host float[3][3] — mirrors gte_math.cpp's rotpair, but in
// plain float (no >>12 truncation): sequentially rotates the matrix's rowA/rowB pair by `angle`.
static void margin_rotpair_f(float angleUnits, float m[3][3], int rowA, int rowB, int posSin) {
  float theta = angleUnits * (kMarginTau / 4096.0f);
  float s = sinf(theta) * (float)posSin, co = cosf(theta);
  float A[3] = { m[rowA][0], m[rowA][1], m[rowA][2] };
  float B[3] = { m[rowB][0], m[rowB][1], m[rowB][2] };
  for (int i = 0; i < 3; i++) m[rowA][i] = co * A[i] - s * B[i];
  for (int i = 0; i < 3; i++) m[rowB][i] = s * A[i] + co * B[i];
}
static inline void margin_rotX(float angle, float m[3][3]) { margin_rotpair_f(angle, m, 1, 2, +1); } // FUN_80084D10
static inline void margin_rotY(float angle, float m[3][3]) { margin_rotpair_f(angle, m, 0, 2, -1); } // FUN_80084EB0
static inline void margin_rotZ(float angle, float m[3][3]) { margin_rotpair_f(angle, m, 0, 1, +1); } // FUN_80085050
static inline void margin_identity(float m[3][3]) {
  m[0][0] = 1.f; m[0][1] = 0.f; m[0][2] = 0.f;
  m[1][0] = 0.f; m[1][1] = 1.f; m[1][2] = 0.f;
  m[2][0] = 0.f; m[2][1] = 0.f; m[2][2] = 1.f;
}
// P = R * M (unit rotation matrices; mirrors Math::matMul's row-times-column shape, no fixed-point scale).
static void margin_matmul(const float R[3][3], const float M[3][3], float P[3][3]) {
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      P[i][j] = R[i][0] * M[0][j] + R[i][1] * M[1][j] + R[i][2] * M[2][j];
}
// out = R * v (unit rotation matrix times a raw-unit position vector — mirrors applyMatlv's net effect).
static void margin_applymatlv(const float R[3][3], const float v[3], float out[3]) {
  for (int i = 0; i < 3; i++) out[i] = R[i][0] * v[0] + R[i][1] * v[1] + R[i][2] * v[2];
}

// One command's host-computed world transform (mirrors cmd+0x18 Robj / cmd+0x2C..34 Tobj).
struct MarginCmdXform { float Robj[3][3]; float Tobj[3]; bool valid = false; };

// MarginRenderer::buildHostTransforms — HOST reimpl of 0x80051C8C + func_80051464 for one margin node.
// Fills `out[i]` for every cmd index i in [0, node+8), bound node+9 (same dual-bound loop
// Render::perObjFlush/func_80051464 use over node+0xC0). NO guest writes anywhere in this function.
static void margin_build_host_transforms(Core* c, uint32_t node, std::vector<MarginCmdXform>& out) {
  // --- 0x80051C8C: node rotation (identity + rotX/Y/Z) + raw translation, HOST-only, unit-scale. ---
  float Mnode[3][3]; margin_identity(Mnode);
  margin_rotX((float)c->mem_r16s(node + 0x54), Mnode);
  margin_rotY((float)c->mem_r16s(node + 0x56), Mnode);
  margin_rotZ((float)c->mem_r16s(node + 0x58), Mnode);
  float Tnode[3] = { (float)c->mem_r16s(node + 0x2E), (float)c->mem_r16s(node + 0x32),
                      (float)c->mem_r16s(node + 0x36) };

  // --- func_80051464: per-command local rotation + parent/sibling compose, HOST-only. ---
  uint8_t count = c->mem_r8(node + 8);
  uint8_t bound = c->mem_r8(node + 9);
  out.assign(count, MarginCmdXform{});
  for (uint32_t i = 0; i < count && i < bound; i++) {
    uint32_t cmd = c->mem_r32(node + 0xC0 + i * 4);
    float work[3][3]; margin_identity(work);
    margin_rotX((float)c->mem_r16s(cmd + 0x08), work);
    margin_rotY((float)c->mem_r16s(cmd + 0x0A), work);
    margin_rotZ((float)c->mem_r16s(cmd + 0x0C), work);
    int16_t sentinel = (int16_t)c->mem_r16s(cmd + 0x06);
    float anchor[3] = { (float)c->mem_r16s(cmd + 0x00), (float)c->mem_r16s(cmd + 0x02),
                         (float)c->mem_r16s(cmd + 0x04) };
    MarginCmdXform& x = out[i];
    float Tanchor[3];
    if (sentinel == -1) {
      margin_matmul(Mnode, work, x.Robj);
      margin_applymatlv(Mnode, anchor, Tanchor);       // parent's OWN matrix, not the just-computed x.Robj
      x.Tobj[0] = Tanchor[0] + Tnode[0]; x.Tobj[1] = Tanchor[1] + Tnode[1]; x.Tobj[2] = Tanchor[2] + Tnode[2];
    } else {
      // Sibling refers to an EARLIER entry in this same node's cmd array (our own host results — never
      // stale guest cmd+0x18, since we never wrote it). Content is built root-first so this always holds
      // in practice; guard anyway and skip (rather than read garbage) if it doesn't.
      if ((uint32_t)sentinel >= i || !out[sentinel].valid) { x.valid = false; continue; }
      const MarginCmdXform& p = out[sentinel];
      margin_matmul(p.Robj, work, x.Robj);
      margin_applymatlv(p.Robj, anchor, Tanchor);
      x.Tobj[0] = Tanchor[0] + p.Tobj[0]; x.Tobj[1] = Tanchor[1] + p.Tobj[1]; x.Tobj[2] = Tanchor[2] + p.Tobj[2];
    }
    x.valid = true;
  }
}

// Record a re-include-eligible node (deduped within the frame). FILTER to entity type 0x03 (the
// static world-geometry object type): later-133 proved that of all the wide-frustum re-include-
// eligible objects, ONLY type-0x03 nodes actually render in the real +1 path — they render via the
// per-object flush gen_func_8003CDD8(node, 0), and the 10 type-0x03 margin nodes reproduce EXACTLY
// the +24 margin commands (matching geomblks). Other types (02/04/05/09) carry command lists too but
// their handlers render through different paths (or not at all in the margin), so flushing them
// over-renders. The type is the correct semantic gate, not a magic offset.
void MarginRenderer::collect(Core* c, uint32_t node) {
  if (node == 0) return;
  if (c->mem_r8(node + 0xc) != T2_WORLDGEO_TYPE) return;   // only world-geometry objects render here
  if (!seen_.insert(node).second) return;               // already collected this frame
  nodes_.push_back(node);
}

// Render every collected node's persistent command list, then reset for the next frame. HOST-ONLY:
// rebuilds each node's transform in float (margin_build_host_transforms) and submits through the
// native, guest-write-free Render::gt3gt4 path — mirrors Render::perObjFlush's own loop shape, fed by
// host-computed Robj/Tobj instead of (stale, for a culled node) cmd+0x18/0x2C.
void MarginRenderer::flush(Core* c) {
  for (uint32_t node : nodes_) {
    if (dbg_ && gpu_frame_no(c) == 2900)
      fprintf(stderr, "[margin]   node=%08x type=%02x cnt=%u\n",
              node, (unsigned)c->mem_r8(node + 0xc), (unsigned)c->mem_r8(node + 8));
    if (c->mem_r8(node + 8) == 0 || c->mem_r8(node + 9) == 0) continue;   // no render commands (perObjFlush parity)

    std::vector<MarginCmdXform> xf;
    margin_build_host_transforms(c, node, xf);

    uint32_t otbase_ptr = c->mem_r32(0x800ED8C8u);   // *0x800ED8C8 — the active OT base (READ only)
    uint8_t cnt = c->mem_r8(node + 8);
    for (uint32_t i = 0; i < cnt; i++) {
      if (!xf[i].valid) continue;
      uint32_t cmd = c->mem_r32(node + 0xC0 + i * 4);
      uint32_t geomblk = c->mem_r32(cmd + 0x40);
      if (!geomblk) continue;
      // Scale the unit rotation matrix up to the raw ~4096 fixed-point convention projComposeCore
      // expects for Robj (matches Rcam's raw-int16 scale read from scratchpad) — Tobj stays raw units.
      float Robj4096[3][3];
      for (int r = 0; r < 3; r++) for (int col = 0; col < 3; col++) Robj4096[r][col] = xf[i].Robj[r][col] * 4096.0f;
      EObjXform w;
      rend(c)->projComposeObjectHost(Robj4096, xf[i].Tobj, &w);
      rend(c)->projSetActive(&w);
      uint32_t otbase = otbase_ptr;
      if ((c->mem_r8(node + 0xD) & 0xF) == 4) otbase = otbase_ptr + ((c->mem_r8s(cmd + 0x3F)) << 2);
      rend(c)->gt3gt4(geomblk, otbase);           // fully-native GT3/GT4 submit — no guest writes
      rend(c)->projClearActive();
    }
  }
  if (dbg_ && !nodes_.empty())
    fprintf(stderr, "[margin] f%d rendered %zu margin nodes\n", gpu_frame_no(c), nodes_.size());
  nodes_.clear();
  seen_.clear();
}

int MarginRenderer::nativeEnabled() {
  if (mNativeEnabled < 0) {
    // Default ON (this IS the widescreen margin path). PSXPORT_MARGIN_POKE=1 falls back to the old
    // +1 re-include (gameplay-perturbing) for A/B diffing.
    mNativeEnabled = cfg_on("PSXPORT_MARGIN_POKE") ? 0 : 1;
    dbg_ = cfg_dbg("margin") != 0;
  }
  return mNativeEnabled;
}
