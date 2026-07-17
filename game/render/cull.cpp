// game/render/cull.cpp — PC-native visibility CULL / LOD subsystem.
// The engine owns the per-object visibility decision (per CLAUDE.md THE BOUNDARY: render ordering /
// visibility is the engine's, with its OWN widescreen-aware margin — it does NOT inherit the stock
// ±34° PSX cone). This module holds the per-object cull body (FUN_8007712C / ov_object_cull) with its
// native decision + verify gate + widescreen margin re-include, the two camera-relative wrappers
// (FUN_8007778C / FUN_80077ACC), and the standalone view-cone cull (FUN_8002B278). Extracted verbatim
// from game_tomba2.cpp (one behavior, byte-identical) into its own module for PC-game code structure.
#include "core.h"
#include "game_ctx.h"
#include "game.h"   // Fps60::current_object
#include "cfg.h"
#include "render.h"   // rend(c)->margin (widescreen margin collect)
#include "cull.h"
#include "override_registry.h"   // overrides::install — the one native-override registry
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);
uint32_t eng_isqrt16(uint32_t);
// g_render_object retired (was defined + written but never read anywhere; dead).

static inline uint16_t obj_r16(Core* c, uint32_t a) { return (uint16_t)(c->mem_r8(a) | (c->mem_r8(a + 1) << 8)); }

// Engine-owned visibility margin (the engine OWNS the cull decision; later-183, user 2026-06-20).
// The recompiled cull FUN_8007712c (RE'd from the disasm: tools/disas.py 0x8007712c → jump table at
// 0x80016cc0, 5 state handlers) culls each object by TWO tests, both reproduced natively below:
//   (1) DISTANCE: dist = isqrt(dx²+dy²+dz²) (object-to-camera). Each handler early-returns culled if
//       dist < ~512 (near) and if dist >= a per-state FAR limit (~0x1001..0x1C01, i.e. 4097..7169).
//   (2) FOV CONE: depth = forward·(dx,dy,dz) [forward = 0x1f8000e8/ea/ec], den = dist*4 (=(dist*4096)>>10);
//       object is kept iff depth/den >= a per-state threshold (the constants 848/856/868/872/880 seen in
//       the handlers — depth/(dist*4) ≈ cos·1024, so ~848 ≈ a ±34° half-cone). That ±34° cone is FAR
//       narrower than even the 4:3 view frustum, so edge objects pop in/out as the camera pans, and the
//       widescreen-widened side/corner geometry (incl. static terrain/water tiles) gets dropped entirely.
// We do NOT inherit the PSX cone. The engine keeps any object that is in front of the camera within an
// extended distance, regardless of aspect — a margin EVEN BEYOND what widescreen needs. After the recomp
// body runs (it cleared the +1 flag for anything it culled), we re-include the dropped objects whose
// (dist, forward-dot) fall inside the engine's own, wider kept region (collected for the post-walk margin
// flush — no +1 poke, so gameplay stays 0-diff; see margin_render.h). Env overrides are diagnostic only.
static unsigned isqrt32(unsigned v) { unsigned r = 0, b = 1u << 30; while (b > v) b >>= 2;
  while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return r; }

// ---- PC-native reimplementation of the recompiled cull body FUN_8007712c ------------------------
// Owns the per-object visibility decision (the per-object CULL/LOD engine system) PC-native instead
// of running the recomp body through the interpreter (it was ~11.2% of hot interp time; the override
// above only WRAPPED it via rec_super_call, so the MIPS body still ran every call). RE'd from the
// disasm (tools/disas.py 0x8007712c → jump table 0x80016cc0, 5 state handlers + a typed sub-dispatch
// in state 0). Decision logic, byte-exact:
//   dist = isqrt16(dx²+dy²+dz²)            (FUN_80077FB0 = ov_isqrt → eng_isqrt16, bit-exact leaf)
//   fwd  = forward vector @0x1f8000e8/ea/ec (s16, scratchpad)
//   if mode byte @0x800bf870 == 4: state @0x1f800084 := 2
//   state = @0x1f800084;  state>=5 → always KEEP
//   else per-state {near, far, fovthr}: KEEP iff near<=dist<far AND (fwd·d)/(dist*4) >= fovthr
//     (the cone quotient is MIPS signed `div`, truncating toward zero — C `/` matches).
//   state 0 sub-dispatches on object type @+0xc: t4→{512,7169,856} t2/9→{512,5121,880}
//     t5→{512,6657,872} other→{512,6145,872};  s1={512,7169,856} s2={768,4097,880}
//     s3={512,4097,848} s4={1024,6657,872}.
// Side effects (the content/render interface — these MUST match): clears the visible flag @obj+1 to 0
// at entry; on KEEP sets it to 1 and, when @0x1f800080==0, pushes the object pointer onto one of three
// type-keyed render queues (stacks growing downward): t2/9→A(ptr 0x1f80013c,cnt 0x1f800144,cap24)
// t4→B(0x1f800148/0x1f800150,cap40) t5→C(0x1f800154/0x1f80015c,cap28). Returns v0 = visible flag.

// ---- Cull-range extension (GitHub issue #22): culling too aggressive, geometry/objects vanish too
// near the camera. We push the FAR limits out so distant and off-screen-but-near things keep both
// rendering AND simulating. Two levers, both in this file:
//   (A) CULL_FAR_MULT  — multiplies every per-state FAR distance limit in cull_decide() below. The
//       stock far limits are 0x1001..0x1C01 (4097..7169 engine units); ×4 pushes them to ~16k..28k
//       so objects the camera is heading toward, and off-screen objects just past the stock edge,
//       stay KEPT. Because cull_decide() drives the visible flag (obj+1) AND the render queues that
//       the per-frame object walk consults, widening these limits keeps far objects simulating, not
//       just drawing. See the pool-overflow RISK note on CULL_FAR (the engine-margin lever) below —
//       the same caution applies: keep the multiplier bounded so the kept/active working set can't
//       blow past the ~52-node active pool (free count @0x800E7E7C).
//   (B) the engine-owned margin re-include (cull_far / cull_fov in ov_object_cull) — see there.
// Tunable, named (not a magic literal): override at runtime with PSXPORT_CULL_FAR_MULT for A/B.
#ifndef CULL_FAR_MULT
#define CULL_FAR_MULT 4   // ×4 the stock per-state far limits (4097..7169 → ~16388..28676)
#endif
// pc_faithful/pc_skip split (2026-07-03): pc_faithful (pc_skip=false) uses the stock (=1) cull
// limits so a side-by-side compare against recomp_path doesn't diverge at 0x800EE489
// (Cull::coneCull2b278 writes obj+1=1 for objects outside the stock cone but inside the ×4
// extended one; recomp culls them). Live pc_skip gameplay still gets the ×4 boost — the two
// modes deliberately do not converge. Same shape as Slip #3 (docs/findings/sbs.md).
int Cull::cullFarMult() { Core* c = core;
  return (c && c->game && !c->game->pc_skip) ? cullFarMultFaithful() : cullFarMultSkip();
}
int Cull::cullFarMultFaithful() {
  return 1;                                             // stock cull limits for substrate parity
}
int Cull::cullFarMultSkip() {
  if (mFarMult < 0) { const char* s = cfg_str("PSXPORT_CULL_FAR_MULT"); int v = s ? atoi(s) : 0;
                      mFarMult = (v > 0) ? v : CULL_FAR_MULT; }
  return mFarMult;
}

static const uint32_t CULL_QPTR[3] = { 0x1f80013cu, 0x1f800148u, 0x1f800154u };
static const uint32_t CULL_QCNT[3] = { 0x1f800144u, 0x1f800150u, 0x1f80015cu };
static const int      CULL_QCAP[3] = { 24, 40, 28 };

// Pure (read-only) cull decision — reproduces FUN_8007712c's control flow without committing writes.
Cull::Decision Cull::decide() { Core* c = core;
  uint32_t obj = c->r[4];
  int32_t dx = (int16_t)c->r[5], dy = (int16_t)c->r[6], dz = (int16_t)c->r[7];   // pos - camera
  uint32_t sum  = (uint32_t)(dx*dx) + (uint32_t)(dy*dy) + (uint32_t)(dz*dz);     // addu-wrap, matches MIPS
  uint32_t dist = eng_isqrt16(sum) & 0xffffu;
  int32_t fx = c->mem_r16s(0x1F8000E8u), fy = c->mem_r16s(0x1F8000EAu), fz = c->mem_r16s(0x1F8000ECu);
  Decision R = { 0, 0, 0 };
  uint32_t state;
  if (c->mem_r8(0x800BF870u) == 4) { R.wrote_state2 = 1; state = 2; }
  else                              state = c->mem_r32(0x1F800084u);
  if (state >= 5) { R.kept = 1; }
  else {
    int nr, fr, thr;
    uint32_t t = c->mem_r8(obj + 0x0c);
    switch (state) {
      case 0:
        if      (t == 4)            { nr = 512;  fr = 7169; thr = 856; }
        else if (t == 2 || t == 9)  { nr = 512;  fr = 5121; thr = 880; }
        else if (t == 5)            { nr = 512;  fr = 6657; thr = 872; }
        else                        { nr = 512;  fr = 6145; thr = 872; }
        break;
      case 1:  nr = 512;  fr = 7169; thr = 856; break;
      case 2:  nr = 768;  fr = 4097; thr = 880; break;
      case 3:  nr = 512;  fr = 4097; thr = 848; break;
      default: nr = 1024; fr = 6657; thr = 872; break;   // state 4
    }
    // Issue #22: extend the far limit (the per-state `fr` above is the byte-exact stock value;
    // we keep it readable and apply the named multiplier here so the kept set reaches further out).
    fr *= cullFarMult();
    if ((int)dist < nr || (int)dist >= fr) { R.kept = 0; }
    else {
      int32_t depth = (int32_t)((uint32_t)(fx*dx) + (uint32_t)(fy*dy) + (uint32_t)(fz*dz));  // addu-wrap
      int32_t den   = (int32_t)((dist * 4096u) >> 10);    // = dist*4 (>0 since dist>=near>=512)
      int32_t q     = depth / den;                        // signed trunc-toward-zero, matches MIPS div
      R.kept = (q >= thr) ? 1 : 0;
    }
  }
  if (R.kept && c->mem_r32(0x1F800080u) == 0) {
    uint32_t t = c->mem_r8(obj + 0x0c);
    if      (t == 4)           R.queue = 2;
    else if (t == 2 || t == 9) R.queue = 1;
    else if (t == 5)           R.queue = 3;
  }
  return R;
}

// Cull::performBaseCull — byte-exact PC-native FUN_8007712C body (no margin re-include). The public
// entry Actor::boundsCull dispatches to (replaces the last rec_dispatch(0x8007712C) in the cull chain).
// Same guest ABI as objectCull: taxi input in r[4]/r[5]/r[6]/r[7], side effects on obj[+1] +
// per-class render-list push, return in r[2]. Was the file-scope `cull_native_body` helper.
void Cull::performBaseCull() { Core* c = core;
  uint32_t obj = c->r[4];
  Decision R = decide();
  c->mem_w8(obj + 1, 0);                                  // prologue `sb zero,1(s3)`
  if (R.wrote_state2) c->mem_w32(0x1F800084u, 2);
  if (!R.kept) { c->r[2] = 0; return; }
  c->mem_w8(obj + 1, 1);
  c->r[2] = 1;
  if (R.queue) {
    int qi = R.queue - 1;
    int32_t cnt = c->mem_r16s(CULL_QCNT[qi]);
    if (cnt < CULL_QCAP[qi]) {
      uint32_t ptr = c->mem_r32(CULL_QPTR[qi]);
      c->mem_w32(CULL_QPTR[qi], ptr - 4);
      c->mem_w32(ptr - 4, obj);
      c->mem_w16(CULL_QCNT[qi], (uint16_t)(cnt + 1));
    }
  }
}

// FUN_8002B278 — standalone view-CONE cull (3.9% field hot). a0 = node. The multiply-form of the same
// cone test as FUN_8007712C with fixed params {near=512, far=7169, thr≡856}: compute the camera-relative
// distance dist = isqrt16(dx²+dy²+dz²) (dx = node->h[0x2C/2E/30] − cam@scratchpad 0x1F8000D2/D6/DA), reject
// (return 0) if dist<512 or dist>=7169, else keep iff fwd·d >= dist*3424 (fwd @0x1F8000E8/EA/EC; 3424 =
// 4*856, the no-divide form of 8007712C's depth/(dist*4) >= 856). On keep, set the visible flag node[1]=1
// and return 1; on reject, return 0 and leave node[1] untouched. Pure leaf (only calls the owned isqrt).
int Cull::coneCullBody(int commit) { Core* c = core;
  uint32_t node = c->r[4];
  int32_t dx = c->mem_r16s(node + 0x2C) - c->mem_r16s(0x1F8000D2u);
  int32_t dy = c->mem_r16s(node + 0x2E) - c->mem_r16s(0x1F8000D6u);
  int32_t dz = c->mem_r16s(node + 0x30) - c->mem_r16s(0x1F8000DAu);
  uint32_t sum  = (uint32_t)(dx*dx) + (uint32_t)(dy*dy) + (uint32_t)(dz*dz);   // addu-wrap, matches MIPS
  uint32_t dist = eng_isqrt16(sum) & 0xffffu;
  int32_t fx = c->mem_r16s(0x1F8000E8u), fy = c->mem_r16s(0x1F8000EAu), fz = c->mem_r16s(0x1F8000ECu);
  // Issue #22: this standalone view-cone cull (FUN_8002B278) shares the 7169 stock far; extend it with
  // the same named CULL_FAR_MULT so distant world geometry on this path also keeps rendering. The cone
  // threshold below stays a relative dot >= dist*3424 (independent of far), so only the far gate moves.
  uint32_t far_lim = 7169u * (uint32_t)cullFarMult();
  if (dist < 512u || dist >= far_lim) return 0;
  int32_t dot = (int32_t)((uint32_t)(fx*dx) + (uint32_t)(fy*dy) + (uint32_t)(fz*dz));  // addu-wrap
  int64_t thr = (int64_t)dist * 3424;                                                  // widened (dist now > 7169 possible)
  if (dot < thr) return 0;
  if (commit) c->mem_w8(node + 1, 1);
  return 1;
}
void Cull::coneCull2b278() { Core* c = core;
  c->r[2] = (uint32_t)coneCullBody(1);
}

// Cull::enqueueVisibleClass4 — PC-native FUN_80077EBC body. Manual push of `obj` onto the class-4
// render list (list ptr @ CULL_QPTR[1] = 0x1F800148, counter @ CULL_QCNT[1] = 0x1F800150, cap 40).
// Byte-exact match to the guest body's slti-40 gate + list-ptr decrement + write + counter bump.
uint32_t Cull::enqueueVisibleClass4(uint32_t obj) { Core* c = core;
  int32_t cnt = c->mem_r16s(CULL_QCNT[1]);
  if (cnt >= CULL_QCAP[1]) return 0;                      // list full — v0 = 0 (matches recomp)
  uint32_t ptr = c->mem_r32(CULL_QPTR[1]);
  c->mem_w32(CULL_QPTR[1], ptr - 4);
  c->mem_w32(ptr - 4, obj);
  uint32_t newCnt = (uint32_t)cnt + 1u;
  c->mem_w16(CULL_QCNT[1], (uint16_t)newCnt);
  return newCnt;
}

// Cull::enqueueByClass — PC-native FUN_8007703C body. Class-keyed queue dispatcher: reads obj[+0xC]
// and routes to the matching queue push (A for classes 2/9, B for class 4, C for class 5). Sets
// obj[+1] = 1 (visible marker) unconditionally before the dispatch (matches the recomp's
// unconditional `sb v0, 1(a2)` right after the prologue, before the switch on obj[+0xC]). RE'd
// verbatim from disas 0x8007703C..0x80077128.
uint32_t Cull::enqueueByClass(uint32_t obj) { Core* c = core;
  c->mem_w8(obj + 1, 1);                                  // 0x8007703C..48: always mark visible
  uint8_t cls = c->mem_r8(obj + 0xC);
  switch (cls) {
    case 4:          return enqueueVisibleClass4(obj);   // 0x800770B8: queue B
    case 2: case 9:  return enqueueQueueA(obj);          // 0x80077084: queue A (v1==2 or v1==9)
    case 5:          return enqueueQueueC(obj);          // 0x800770F0: queue C
    default:         return 0;                            // unknown class → jr ra at 0x80077068
  }
}

// Cull::enqueueQueueA — PC-native FUN_80077E7C body. Manual push of `obj` onto queue A (list ptr @
// CULL_QPTR[0] = 0x1F80013C, counter @ CULL_QCNT[0] = 0x1F800144, cap 24). Sibling of
// enqueueVisibleClass4 for queue B — same slti-24 gate + list-ptr decrement + write + counter bump.
// RE'd from disas 0x80077E7C..0x80077EB8. Six substrate callers: game/world/entity.cpp (4x),
// beh_variant_actor_sm, beh_jumptable_release_trigger.
uint32_t Cull::enqueueQueueA(uint32_t obj) { Core* c = core;
  int32_t cnt = c->mem_r16s(CULL_QCNT[0]);
  if (cnt >= CULL_QCAP[0]) return 0;                      // queue full — v0 = 0 (matches recomp)
  uint32_t ptr = c->mem_r32(CULL_QPTR[0]);
  c->mem_w32(CULL_QPTR[0], ptr - 4);
  c->mem_w32(ptr - 4, obj);
  uint32_t newCnt = (uint32_t)cnt + 1u;
  c->mem_w16(CULL_QCNT[0], (uint16_t)newCnt);
  return newCnt;                                          // v0 = old_counter + 1 (recomp: addiu v0, a1, 1)
}

// Cull::enqueueQueueC — PC-native FUN_80077EFC body. Manual push onto queue C (list ptr @
// CULL_QPTR[2] = 0x1F800154, counter @ CULL_QCNT[2] = 0x1F80015C, cap 28). RE'd verbatim from disas
// 0x80077EFC..0x80077F38. Return convention matches enqueueQueueA (0 on cap-hit, new count on push).
uint32_t Cull::enqueueQueueC(uint32_t obj) { Core* c = core;
  int32_t cnt = c->mem_r16s(CULL_QCNT[2]);
  if (cnt >= CULL_QCAP[2]) return 0;
  uint32_t ptr = c->mem_r32(CULL_QPTR[2]);
  c->mem_w32(CULL_QPTR[2], ptr - 4);
  c->mem_w32(ptr - 4, obj);
  uint32_t newCnt = (uint32_t)cnt + 1u;
  c->mem_w16(CULL_QCNT[2], (uint16_t)newCnt);
  return newCnt;
}

void Cull::objectCull() { Core* c = core;
  uint32_t o = c->r[4];                            // a0 = object* (MIPS arg register $a0)

  if (mObjLog < 0) mObjLog = cfg_dbg("obj") ? 1 : 0;
  if (mObjLog)
    fprintf(stderr, "[objlog] obj=%08x type=%02x pos=(%d,%d,%d)\n", o, c->mem_r8(o + 0x0c),
            (int16_t)obj_r16(c, o + 0x2e), (int16_t)obj_r16(c, o + 0x32), (int16_t)obj_r16(c, o + 0x36));
  int p2 = (int16_t)c->r[5], p3 = (int16_t)c->r[6], p4 = (int16_t)c->r[7];   // pos - camera (s16 each)
  performBaseCull();                                 // PC-native cull (byte-exact FUN_8007712C body)
  // The engine OWNS this margin, so it is ALWAYS active — not gated on widescreen. Even at 4:3 the
  // stock ±34° cone over-culls (edge pop-in), so we keep the wide region in every aspect; widescreen
  // then needs no extra special-casing. Env overrides remain for diagnostics only (PSXPORT_CULL_FAR/_FOV).
  if (mCullEnvRead < 0) { const char* f = cfg_str("PSXPORT_CULL_FAR"); mCullFar = f ? atoi(f) : -1;
                          const char* v = cfg_str("PSXPORT_CULL_FOV"); mCullFov = v ? atoi(v) : -1; mCullEnvRead = 1; }
  int do_cull = 1;
  // FAR limit (engine units, same scale as `dist`) for the engine-owned wide RE-INCLUDE of margin
  // geometry the stock body culled. The widest stock far is 7169 (0x1C01).
  //   CULL_MARGIN_FAR = 0x10000 ≈ 9.1x the stock far — generous per issue #22 so distant scenery /
  //   terrain tiles the camera is heading toward (and off-screen-but-near static world) are kept well
  //   before they pop in. RISK (pool overflow): the active-object pool free count lives @0x800E7E7C
  //   (three active lists, ~stride 0xD0 nodes, ~52 nodes total). The margin re-include itself only
  //   re-RENDERS type-0x03 static world geometry (MarginRenderer::collect, no +1 poke), so it does NOT consume
  //   pool nodes; but the cull_decide far multiplier above DOES affect what stays KEPT/active, so the
  //   real overflow guard is to keep CULL_FAR_MULT bounded (×4 leaves margin) — do not crank it so far
  //   that the whole level stays active at once. Named/tunable, not a magic literal; PSXPORT_CULL_FAR
  //   overrides at runtime for A/B.
  #ifndef CULL_MARGIN_FAR
  #define CULL_MARGIN_FAR 0x10000   // ~9.1x the widest stock far (7169) — generous render-margin reach
  #endif
  int cull_far = mCullFar >= 0 ? mCullFar : CULL_MARGIN_FAR;
  // FOV-cone threshold for depth/(dist*4) [≈ cos·1024]. The engine keeps objects to the EDGE of the view
  // and a bit beyond: 0 = the full forward hemisphere (±90°, well past the widescreen frustum's ~±40°);
  // a small NEGATIVE value extends past 90° so an object whose ORIGIN is just behind the camera plane but
  // whose widened geometry still grazes the screen edge is not dropped (this is the "beyond WS" margin the
  // user asked for). -0x60 ≈ cos(93.4°): ~3.4° past the side, enough to cover wide edge geometry without
  // re-including things squarely behind the camera. (vs the stock 848..880 ≈ only ±34°.)
  // Named/tunable (issue #22): CULL_MARGIN_FOV = -0x60 already keeps the full forward hemisphere and
  // ~3.4° past the side; that is generous enough for off-screen edge geometry, so we keep it but name
  // it. PSXPORT_CULL_FOV overrides at runtime.
  #ifndef CULL_MARGIN_FOV
  #define CULL_MARGIN_FOV (-0x60)   // ≈ cos(93.4°): full hemisphere + ~3.4° past the side
  #endif
  int cull_fov = mCullFov >= 0 ? mCullFov : CULL_MARGIN_FOV;
  // MEASUREMENT (entity-type taxonomy RE, journal later-127 step 1; off by default): restrict the wide
  // re-include to a single entity type (+0xc), or exclude one, so a 4:3-vs-16:9 gameplay RAM self-diff
  // isolates whether re-including THAT type perturbs gameplay logic (static-world) or not (dynamic).
  // PSXPORT_CULL_ONLY_TYPE=<n> re-includes only type n; PSXPORT_CULL_SKIP_TYPE=<n> re-includes all but n.
  if (mOnlyType == -2) { const char* x = cfg_str("PSXPORT_CULL_ONLY_TYPE"); mOnlyType = x ? (int)strtol(x,0,0) : -1;
                         const char* y = cfg_str("PSXPORT_CULL_SKIP_TYPE"); mSkipType = y ? (int)strtol(y,0,0) : -1; }
  int otype = c->mem_r8(o + 0x0c);
  if (mOnlyType >= 0 && otype != mOnlyType) do_cull = 0;
  if (mSkipType >= 0 && otype == mSkipType) do_cull = 0;
  if (do_cull && c->mem_r8(o + 1) == 0) {              // the game CULLED it — reconsider with engine bounds
    unsigned dist = isqrt32((unsigned)(p2*p2 + p3*p3 + p4*p4)) & 0xFFFF;
    // NEAR floor 0x80 (was 0x200): the stock body culls anything closer than ~512, so on-camera objects
    // right by Tomba (e.g. the ground/water tile he stands on) pop at the inner edge too. 0x80 keeps them
    // while staying above the degenerate dist→0 case where the forward-dot direction test is meaningless;
    // the cone test below still rejects near objects that are actually behind the camera.
    if (dist >= 0x80 && dist <= (unsigned)cull_far) {
      int fx = (int16_t)obj_r16(c, 0x1F8000E8), fy = (int16_t)obj_r16(c, 0x1F8000EA), fz = (int16_t)obj_r16(c, 0x1F8000EC);
      long depth = (long)fx*p2 + (long)fy*p3 + (long)fz*p4, den = ((long)dist * 0x1000) >> 10;
      if (den < 1) den = 1;
      if (depth / den >= cull_fov) {
        // NATIVE MARGIN (default, later-133): render this culled object via a post-walk per-node flush
        // instead of poking +1. Poking +1 runs the handler's VISIBLE branch -> gameplay perturbation
        // (5638 B). Collecting + flushing the node's persistent command list touches only render scratch
        // -> margin renders, gameplay 0-diff. A/B: PSXPORT_MARGIN_POKE=1 keeps the old +1 re-include.
        // NOTE (scope of the wider cone above): MarginRenderer::collect() only re-renders type-0x03 world-geometry
        // (terrain/water/static scenery) — the dominant edge/corner pop-in. Dynamic entities are NOT
        // poked visible here (that would perturb their gameplay state), so widening the cone safely
        // un-pops the static world without disturbing enemy/item/NPC logic.
        // WIDESCREEN is render-only (USER 2026-07-10, revised): widening the visual reach must be
        // guest-READ-ONLY. The earlier wide +1 poke (to populate dynamic side-margin entities) perturbed
        // guest state and had to be SBS-suppressed — both removed. Dynamic entities in the extreme wide
        // margins are therefore dropped until each type's read-only render is added (wide de-prioritized).
        // Re-include margin geometry READ-ONLY in every aspect (MarginRenderer host overlay, zero guest
        // writes): no +1 poke, so the object's guest state is untouched — core A stays byte-identical to
        // the oracle core B AND to a standalone run, with NO game->sbs special-casing. Widescreen is a
        // render-only concern (wider visual reach), served by this same read-only path; it must NEVER
        // poke guest state (the old wide +1 poke perturbed 5638 B of object state and forced an SBS
        // special-case — both removed). PSXPORT_MARGIN_POKE=1 keeps the old +1 re-include for A/B diffing.
        if (rend(c)->margin.nativeEnabled())   { rend(c)->margin.collect(c, o); }   // read-only margin, all aspects
        else                                      { c->mem_w8(o + 1, 1); c->r[2] = 1; }   // POKE fallback (A/B diffing only)
        // MEASUREMENT (PSXPORT_DEBUG=cullobj): identify WHAT the margin re-include renders — obj addr,
        // type, model id (+0xe & 0x3fff), model-data ptr (+0x38), pos. Decides static-world vs per-object
        // architecture for approach B. One line per re-include; grep a single frame.
        if (cfg_dbg("cullobj")) {
          int gpu_frame_no(Core*);
          uint32_t cmd = c->mem_r32(o + 0xc0);                       // persistent render-command ptr (later-132)
          uint32_t gb  = cmd ? c->mem_r32(cmd + 0x40) : 0;           // its geomblk
          fprintf(stderr, "[cullobj] f%d obj=%08x type=%02x model=%04x mdata=%08x cmd=%08x geomblk=%08x x18=",
                  gpu_frame_no(c), o, otype, obj_r16(c, o + 0x0e) & 0x3fff, c->mem_r8(o+0x38)|(c->mem_r8(o+0x39)<<8)|(c->mem_r8(o+0x3a)<<16)|(c->mem_r8(o+0x3b)<<24),
                  cmd, gb);
          for (int j = 0; j < 8; j++) fprintf(stderr, "%s%08x", j ? "," : "", cmd ? c->mem_r32(cmd + 0x18 + j*4) : 0);
          fprintf(stderr, " pos=(%d,%d,%d)\n",
                  (int16_t)obj_r16(c, o + 0x2e), (int16_t)obj_r16(c, o + 0x32), (int16_t)obj_r16(c, o + 0x36));
        }
        // MEASUREMENT (PSXPORT_DEBUG=cullinc): per-type tally of objects the wide re-include actually
        // marks visible, per frame. Distinguishes a genuinely static-safe type from a vacuous 0-diff
        // (a type with no culled margin members to re-include). s_frame from gpu_native.
        int gpu_frame_no(Core*); static long s_inc[256]; static int s_lf = -1;
        if (cfg_dbg("cullinc")) {
          if (gpu_frame_no(c) != s_lf && s_lf >= 0) {
            int any = 0; for (int t = 0; t < 256; t++) if (s_inc[t]) any = 1;
            if (any) { fprintf(stderr, "[cullinc] f%d reincluded:", s_lf);
              for (int t = 0; t < 256; t++) if (s_inc[t]) fprintf(stderr, " 0x%02x=%ld", t, s_inc[t]);
              fprintf(stderr, "\n"); }
            for (int t = 0; t < 256; t++) s_inc[t] = 0;
          }
          s_lf = gpu_frame_no(c); s_inc[otype & 0xff]++;
        }
      }
    }
  }
}

// FUN_8007778C — camera-relative cull WRAPPER. Computes obj-cam delta (wrapping s16, sign-extended),
// zeros the cull scratchpad flags 0x1F800080/0x1F800084, then forwards to the per-object cull body
// FUN_8007712C via rec_dispatch (so current-object tracking + the widescreen margin still fire).
// Camera pos @0x1F8000D0 (+2=X,+6=Z,+10=Y, u16). Was ov_cull_wrapper.
// Cull::wrapFrame — shared frame mirror for the whole cullWrapper* family (RE'd instruction-exact
// from generated/shard_{0,1,2,4,5,7}.c: gen_func_8007778C/800777FC/80077ACC/800779D0/80077A4C/
// 800778E4). All six share the IDENTICAL prologue/epilogue shape:
//   addiu sp,sp,-24; sw ra,16(sp); <setup>; move ra,<per-site const>; jal FUN_8007712C;
//   lw ra,16(sp); addiu sp,sp,24; jr ra
// The `sw ra,16(sp)` spills the CALLER's LIVE incoming ra (whatever is currently in c->r[31])
// BEFORE the jal overwrites it with the per-site constant — mirroring that (instead of the old
// "leave unwired" workaround) makes the guest stack bytes byte-match the substrate with no SBS
// exclusion. `raConst` is the RE'd guest return address for this call site's jal to FUN_8007712C.
void Cull::wrapFrame(uint32_t raConst) { Core* c = core;
  uint32_t savedRa = c->r[31];
  c->r[29] -= 24;
  c->mem_w32(c->r[29] + 16, savedRa);
  c->r[31] = raConst;
  performBaseCullFramed();                     // FUN_8007712C native (the jal target) — ITS OWN frame too
  c->r[31] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

// performBaseCullFramed — mirrors FUN_8007712C's OWN real 40-byte guest-stack frame (RE:
// generated/shard_1.c gen_func_8007712C: `addiu sp,-40; sw r19,28(sp) [BEFORE r19<-a0=obj];
// sw r16,16(sp) [BEFORE r16<-a1=dx]; sw r17,20(sp) [BEFORE r17<-a2=dy]; sw r18,24(sp)
// [BEFORE r18<-a3=dz]; sw ra,32(sp)`). Bug found + fixed 2026-07-08: wrapFrame() above only
// mirrored the OUTER wrapper's own frame (cullWrapper etc.'s `addiu sp,-24`); on the real
// substrate, that wrapper's jal to FUN_8007712C ALSO pushes FUN_8007712C's OWN nested -40 frame
// at that depth (visible in generated/shard_1.c) — a gap performBaseCull() (a pure-C++ leaf with
// no r29 use at all) never reproduced. The missing nested frame left the guest-stack bytes at
// that depth untouched on the native side, producing an SBS divergence at 0x801FE904..908
// wherever an UNRELATED later write (from other guest code reusing the same stack depth) differed
// from the substrate's own r16/r19 spill-then-restore. ONLY used from wrapFrame() (the guest-ABI
// path) — performBaseCull() itself stays unframed for its OTHER, native (non-guest-ABI) callers
// (Actor::boundsCull/boundsCullYOffset, cullWrapperFlag2()/cullWrap77acc()'s own unframed bodies),
// same reasoning as cullWrap77acc()/cullWrapperFlag2()'s framed/unframed split above.
void Cull::performBaseCullFramed() { Core* c = core;
  uint32_t save16 = c->r[16], save17 = c->r[17], save18 = c->r[18], save19 = c->r[19], saveRa = c->r[31];
  c->r[29] -= 40;
  c->mem_w32(c->r[29] + 28, save19);           // sw r19,28(sp) — LIVE incoming r19 (before r19<-obj)
  c->mem_w32(c->r[29] + 16, save16);           // sw r16,16(sp) — LIVE incoming r16 (before r16<-dx)
  c->mem_w32(c->r[29] + 20, save17);           // sw r17,20(sp) — LIVE incoming r17 (before r17<-dy)
  c->mem_w32(c->r[29] + 24, save18);           // sw r18,24(sp) — LIVE incoming r18 (before r18<-dz)
  c->mem_w32(c->r[29] + 32, saveRa);           // sw ra,32(sp)

  performBaseCull();

  c->r[31] = c->mem_r32(c->r[29] + 32);
  c->r[18] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[19] = c->mem_r32(c->r[29] + 28);
  c->r[29] += 40;
}

// FUN_8007778C — RA constant for the internal jal to FUN_8007712C (RE: generated/shard_4.c
// gen_func_8007778C, `c->r[31] = 0x800777ECu;`).
static constexpr uint32_t RA_8007778C = 0x800777ECu;
void Cull::cullWrapper() { Core* c = core;
  uint32_t obj = c->r[4];
  uint16_t camx = (uint16_t)c->mem_r16(0x1F8000D2u);
  uint16_t camz = (uint16_t)c->mem_r16(0x1F8000D6u);
  uint16_t camy = (uint16_t)c->mem_r16(0x1F8000DAu);
  c->mem_w32(0x1F800080u, 0);
  c->mem_w32(0x1F800084u, 0);
  c->r[5] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x2E) - camx);
  c->r[6] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x32) - camz);
  c->r[7] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x36) - camy);
  c->r[4] = obj;
  wrapFrame(RA_8007778C);                      // FUN_8007712C native — was rec_dispatch
}

// FUN_80077870 — cull-wrapper variant: byte-identical to cullWrapper (obj in c->r[4], deltas
// computed from obj+0x2E/0x32/0x36 vs cam@0x1F8000D2/D6/DA), but writes 0x1F800084 = 1 (vs 0 for
// cullWrapper, 2 for cullWrapperFlag2, 4 for cullWrap77acc). Reached by a direct func_ call in the
// substrate + by-address from beh_typed_variant_router (via leafr1), never a plain native call.
//
// Its gen body is the SAME shared shape as the whole cullWrapper* family (`addiu sp,-24;
// sw ra,16(sp); ...; move ra,0x800778D4; jal FUN_8007712C; lw ra,16(sp); addiu sp,24`). cullWrapper
// factors that into wrapFrame(); here it is written INLINE in the gen's store order (ra spill first,
// then the two flag writes) so the guest-frame descent/spill + the internal FUN_8007712C call are
// visible to the per-method port_check gate rather than hidden inside the wrapFrame() helper. The
// inner cull runs through performBaseCullFramed() — FUN_8007712C's OWN nested -40 frame — exactly as
// wrapFrame() does, so the guest stack is byte-identical to the substrate (and to cullWrapper).
// port_check verdict is UNPROVABLE, not FAIL: frame sizes (24/24), the store-width sequence
// ([32,32,32]) and the call count + ra-const (0x800778D4) all match the oracle; only the call TARGET
// is unresolvable, because the native reaches FUN_8007712C's body as the owned performBaseCullFramed
// method rather than a substrate func_8007712C(c) — inherent to owning the cull body natively.
// ORACLE: gen_func_80077870
void Cull::cullWrapperFlag1() {
  Core* c = core;
  uint32_t obj = c->r[4];
  uint32_t savedRa = c->r[31];
  c->r[29] -= 24;                                      // addiu sp,-24
  c->mem_w32(c->r[29] + 16, savedRa);                  // sw ra,16(sp) — LIVE incoming ra
  uint16_t camx = (uint16_t)c->mem_r16(0x1F8000D2u);   // subtracted from obj+0x2E → r[5]
  uint16_t camz = (uint16_t)c->mem_r16(0x1F8000D6u);   // subtracted from obj+0x32 → r[6]
  uint16_t camy = (uint16_t)c->mem_r16(0x1F8000DAu);   // subtracted from obj+0x36 → r[7]
  c->mem_w32(0x1F800080u, 0);
  c->mem_w32(0x1F800084u, 1);
  c->r[5] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x2E) - camx);
  c->r[7] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x36) - camy);
  c->r[6] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x32) - camz);
  c->r[4] = obj;
  c->r[31] = 0x800778D4u;                              // move ra,0x800778D4 — jal FUN_8007712C return
  performBaseCullFramed();                             // FUN_8007712C native (its own nested -40 frame)
  c->r[31] = c->mem_r32(c->r[29] + 16);                // lw ra,16(sp)
  c->r[29] += 24;                                      // addiu sp,24
}

// FUN_800777FC — cull-wrapper variant: same taxi shape as cullWrapper (obj in c->r[4], deltas
// computed from obj+0x2E/0x32/0x36 vs cam@0x1F8000D2/D6/DA), but writes 0x1F800084 = 2 (vs 0 for
// cullWrapper). RE'd from disas 0x800777FC..0x8007786C. 3 callers in beh_id_compare_motion_dispatch.
// Deltas match cullWrapper: coord[0] = D2, coord[1] = D6, coord[2] = DA — the local names in
// cullWrapper (camz/camy for D6/DA) are misleading; the recomp subtracts D6 from obj+0x32 (r[6])
// and DA from obj+0x36 (r[7]).
// FUN_800777FC — RA constant (RE: generated/shard_5.c gen_func_800777FC, `c->r[31] = 0x80077860u;`).
static constexpr uint32_t RA_800777FC = 0x80077860u;
// UNFRAMED — the public entry point EXISTING native beh_ callers (beh_id_compare_motion_dispatch.cpp,
// 3 call sites) already use as a plain C++ call. Bug found + fixed 2026-07-08: this method used to
// ALSO wrapFrame() unconditionally, which corrupted an arbitrary guest-stack region for these native
// callers (c->r[29] at their call sites is NOT a real guest sp belonging to this call — it's whatever
// the last unrelated guest call left it at). Framing now lives ONLY in cullWrapperFlag2Framed(),
// reached solely from the guest-ABI shard_set_override trampoline.
void Cull::cullWrapperFlag2() { Core* c = core;
  uint32_t obj = c->r[4];
  uint16_t cam0 = (uint16_t)c->mem_r16(0x1F8000D2u);   // subtracted from obj+0x2E → r[5]
  uint16_t cam1 = (uint16_t)c->mem_r16(0x1F8000D6u);   // subtracted from obj+0x32 → r[6]
  uint16_t cam2 = (uint16_t)c->mem_r16(0x1F8000DAu);   // subtracted from obj+0x36 → r[7]
  c->mem_w32(0x1F800080u, 0);
  c->mem_w32(0x1F800084u, 2);
  c->r[5] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x2E) - cam0);
  c->r[6] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x32) - cam1);
  c->r[7] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x36) - cam2);
  c->r[4] = obj;
  performBaseCull();                            // FUN_8007712C native — no frame (see above)
}
void Cull::cullWrapperFlag2Framed() { Core* c = core;
  uint32_t saveRa = c->r[31];
  c->r[29] -= 24;
  c->mem_w32(c->r[29] + 16, saveRa);
  c->r[31] = RA_800777FC;
  cullWrapperFlag2();
  c->r[31] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

// FUN_80077ACC — cull-wrapper variant, caller-supplied position in a1/a2/a3 (not obj fields), flags
// 0x1F800080=1 / 0x1F800084=4 (vs the 0/0 form above). Makes the position camera-relative then calls
// the cull body 0x8007712C. Was ov_cull_wrap_77acc.
// FUN_80077ACC — RA constant (RE: generated/shard_2.c gen_func_80077ACC, `c->r[31] = 0x80077B28u;`).
static constexpr uint32_t RA_80077ACC = 0x80077B28u;
// UNFRAMED — the public entry point EXISTING native callers (beh_record_list_scanner.cpp,
// script_vm.cpp) already use as a plain C++ call. Same class of bug/fix as cullWrapperFlag2 above:
// framing lives ONLY in cullWrap77accFramed(), reached solely from the guest-ABI trampoline.
void Cull::cullWrap77acc() { Core* c = core;
  c->mem_w32(0x1F800080u, 1);
  c->mem_w32(0x1F800084u, 4);
  uint16_t cx = (uint16_t)c->mem_r16(0x1F8000D2u);
  uint16_t cz = (uint16_t)c->mem_r16(0x1F8000D6u);
  uint16_t cy = (uint16_t)c->mem_r16(0x1F8000DAu);
  c->r[5] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->r[5] - cx);
  c->r[6] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->r[6] - cz);
  c->r[7] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->r[7] - cy);
  performBaseCull();                            // FUN_8007712C native — no frame (see above)
}
void Cull::cullWrap77accFramed() { Core* c = core;
  uint32_t saveRa = c->r[31];
  c->r[29] -= 24;
  c->mem_w32(c->r[29] + 16, saveRa);
  c->r[31] = RA_80077ACC;
  cullWrap77acc();
  c->r[31] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

// FUN_800779D0 — cull-wrapper variant: caller-supplied 3-component offset (a1/a2/a3, entering in
// r[5]/r[6]/r[7]) is ADDED to the object's own position (obj+0x2E/0x32/0x36) BEFORE the camera-
// relative subtraction. Flags 0/0 (same as cullWrapper). RE'd via Ghidra headless
// (scratch/decomp/cluster1.c: FUN_800779d0).
// FUN_800779D0 — RA constant (RE: generated/shard_0.c gen_func_800779D0, `c->r[31] = 0x80077A3Cu;`).
static constexpr uint32_t RA_800779D0 = 0x80077A3Cu;
void Cull::cullWrapperOffset() { Core* c = core;
  uint32_t obj = c->r[4];
  uint32_t offX = c->r[5], offZ = c->r[6], offY = c->r[7];
  uint16_t camx = (uint16_t)c->mem_r16(0x1F8000D2u);
  uint16_t camz = (uint16_t)c->mem_r16(0x1F8000D6u);
  uint16_t camy = (uint16_t)c->mem_r16(0x1F8000DAu);
  c->mem_w32(0x1F800080u, 0);
  c->mem_w32(0x1F800084u, 0);
  c->r[5] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((uint16_t)c->mem_r16(obj + 0x2E) + offX) - camx);
  c->r[6] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((uint16_t)c->mem_r16(obj + 0x32) + offZ) - camz);
  c->r[7] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((uint16_t)c->mem_r16(obj + 0x36) + offY) - camy);
  c->r[4] = obj;
  wrapFrame(RA_800779D0);
}

// FUN_80077A4C — same offset-add shape as cullWrapperOffset, but writes 0x1F800080=1 / 0x1F800084=0
// (vs 0/0). RE'd via Ghidra headless (scratch/decomp/cluster1.c: FUN_80077a4c).
// FUN_80077A4C — RA constant (RE: generated/shard_1.c gen_func_80077A4C, `c->r[31] = 0x80077ABCu;`).
static constexpr uint32_t RA_80077A4C = 0x80077ABCu;
void Cull::cullWrapperOffsetFlag1() { Core* c = core;
  uint32_t obj = c->r[4];
  uint32_t offX = c->r[5], offZ = c->r[6], offY = c->r[7];
  uint16_t camx = (uint16_t)c->mem_r16(0x1F8000D2u);
  uint16_t camz = (uint16_t)c->mem_r16(0x1F8000D6u);
  uint16_t camy = (uint16_t)c->mem_r16(0x1F8000DAu);
  c->mem_w32(0x1F800080u, 1);
  c->mem_w32(0x1F800084u, 0);
  c->r[5] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((uint16_t)c->mem_r16(obj + 0x2E) + offX) - camx);
  c->r[6] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((uint16_t)c->mem_r16(obj + 0x32) + offZ) - camz);
  c->r[7] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((uint16_t)c->mem_r16(obj + 0x36) + offY) - camy);
  c->r[4] = obj;
  wrapFrame(RA_80077A4C);
}

// FUN_800778E4 — cull-wrapper variant: a SINGLE caller-supplied offset (a1, entering in r[5]) is
// added ONLY to the object's Z-field (obj+0x32); X (obj+0x2E) and Y (obj+0x36) use the raw object
// position. Flags 0/0. RE'd via Ghidra headless (scratch/decomp/cluster1.c: FUN_800778e4).
// FUN_800778E4 — RA constant for the (reachable) live path (RE: generated/shard_7.c
// gen_func_800778E4, `c->r[31] = 0x80077948u;` — a second, unreachable dead block follows the
// first `return;` in the generated body; only the first block's ra is ever live).
static constexpr uint32_t RA_800778E4 = 0x80077948u;
void Cull::cullWrapperOffsetY() { Core* c = core;
  uint32_t obj = c->r[4];
  uint32_t offZ = c->r[5];
  uint16_t camx = (uint16_t)c->mem_r16(0x1F8000D2u);
  uint16_t camz = (uint16_t)c->mem_r16(0x1F8000D6u);
  uint16_t camy = (uint16_t)c->mem_r16(0x1F8000DAu);
  c->mem_w32(0x1F800080u, 0);
  c->mem_w32(0x1F800084u, 0);
  c->r[5] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x2E) - camx);
  c->r[6] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((uint16_t)c->mem_r16(obj + 0x32) + offZ) - camz);
  c->r[7] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x36) - camy);
  c->r[4] = obj;
  wrapFrame(RA_800778E4);
}

// ---- Wiring (RESOLVED 2026-07-08): shard_set_override for all 6 camera-relative wrappers -------
// The substrate reaches these via DIRECT `func_<addr>(c)` call sites (jal, not jalr), so
// the override registry's rec_dispatch interception is blind to them; shard_set_override intercepts the
// recompiler's OWN g_override[] table, the same dual-registration pattern as game/math/gte_math.cpp
// and game/object/animation.cpp's applyFrame. psx_fallback-gated: core B (the pure substrate
// reference) always runs the real gen_func_* body.
extern void shard_set_override(uint32_t, void (*)(Core*));

static void eov_cullWrapper(Core* c)             { eng(c).cull.cullWrapper(); }
static void eov_cullWrapperFlag1(Core* c)        { eng(c).cull.cullWrapperFlag1(); }
static void eov_cullWrapperFlag2(Core* c)        { eng(c).cull.cullWrapperFlag2Framed(); }
static void eov_cullWrap77acc(Core* c)           { eng(c).cull.cullWrap77accFramed(); }
static void eov_cullWrapperOffset(Core* c)       { eng(c).cull.cullWrapperOffset(); }
static void eov_cullWrapperOffsetFlag1(Core* c)  { eng(c).cull.cullWrapperOffsetFlag1(); }
static void eov_cullWrapperOffsetY(Core* c)      { eng(c).cull.cullWrapperOffsetY(); }

extern void gen_func_80077870(Core*);
extern void gen_func_8007778C(Core*);
extern void gen_func_800777FC(Core*);
extern void gen_func_80077ACC(Core*);
extern void gen_func_800779D0(Core*);
extern void gen_func_80077A4C(Core*);
extern void gen_func_800778E4(Core*);

void Cull::registerOverrides() {
  using overrides::install;
  install(0x80077870u, "Cull::cullWrapperFlag1",       eov_cullWrapperFlag1,       gen_func_80077870, shard_set_override);
  install(0x8007778Cu, "Cull::cullWrapper",            eov_cullWrapper,            gen_func_8007778C, shard_set_override);
  install(0x800777FCu, "Cull::cullWrapperFlag2",       eov_cullWrapperFlag2,       gen_func_800777FC, shard_set_override);
  install(0x80077ACCu, "Cull::cullWrap77acc",          eov_cullWrap77acc,          gen_func_80077ACC, shard_set_override);
  install(0x800779D0u, "Cull::cullWrapperOffset",      eov_cullWrapperOffset,      gen_func_800779D0, shard_set_override);
  install(0x80077A4Cu, "Cull::cullWrapperOffsetFlag1", eov_cullWrapperOffsetFlag1, gen_func_80077A4C, shard_set_override);
  install(0x800778E4u, "Cull::cullWrapperOffsetY",     eov_cullWrapperOffsetY,     gen_func_800778E4, shard_set_override);
}
