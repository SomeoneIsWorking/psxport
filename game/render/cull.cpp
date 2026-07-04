// engine/cull.cpp — PC-native visibility CULL / LOD subsystem.
// The engine owns the per-object visibility decision (per CLAUDE.md THE BOUNDARY: render ordering /
// visibility is the engine's, with its OWN widescreen-aware margin — it does NOT inherit the stock
// ±34° PSX cone). This module holds the per-object cull body (FUN_8007712C / ov_object_cull) with its
// native decision + verify gate + widescreen margin re-include, the two camera-relative wrappers
// (FUN_8007778C / FUN_80077ACC), and the standalone view-cone cull (FUN_8002B278). Extracted verbatim
// from game_tomba2.cpp (one behavior, byte-identical) into its own module for PC-game code structure.
#include "core.h"
#include "game.h"   // Fps60::current_object
#include "cfg.h"
#include "margin_render.hpp"
#include "cull.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);
uint32_t eng_isqrt16(uint32_t);
// g_render_object retired (was defined + written but never read anywhere; dead).

static int s_objlog = -1;
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
// flush — no +1 poke, so gameplay stays 0-diff; see margin_render.hpp). Env overrides are diagnostic only.
static int s_cull = -1, s_cull_far, s_cull_fov;
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
static int cull_far_mult(Core* c) {
  if (c && c->game && !c->game->pc_skip) return 1;      // pc_faithful — stock cull for substrate parity
  static int m = -1;
  if (m < 0) { const char* s = cfg_str("PSXPORT_CULL_FAR_MULT"); int v = s ? atoi(s) : 0;
               m = (v > 0) ? v : CULL_FAR_MULT; }
  return m;
}

struct CullDecision { int kept; int wrote_state2; int queue; };  // queue: 0=none,1=A,2=B,3=C
static const uint32_t CULL_QPTR[3] = { 0x1f80013cu, 0x1f800148u, 0x1f800154u };
static const uint32_t CULL_QCNT[3] = { 0x1f800144u, 0x1f800150u, 0x1f80015cu };
static const int      CULL_QCAP[3] = { 24, 40, 28 };

// Pure (read-only) cull decision — reproduces FUN_8007712c's control flow without committing writes.
static CullDecision cull_decide(Core* c) {
  uint32_t obj = c->r[4];
  int32_t dx = (int16_t)c->r[5], dy = (int16_t)c->r[6], dz = (int16_t)c->r[7];   // pos - camera
  uint32_t sum  = (uint32_t)(dx*dx) + (uint32_t)(dy*dy) + (uint32_t)(dz*dz);     // addu-wrap, matches MIPS
  uint32_t dist = eng_isqrt16(sum) & 0xffffu;
  int32_t fx = c->mem_r16s(0x1F8000E8u), fy = c->mem_r16s(0x1F8000EAu), fz = c->mem_r16s(0x1F8000ECu);
  CullDecision R = { 0, 0, 0 };
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
    fr *= cull_far_mult(c);
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
  CullDecision R = cull_decide(c);
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
static int cone_cull_2b278(Core* c, int commit) {
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
  uint32_t far_lim = 7169u * (uint32_t)cull_far_mult(c);
  if (dist < 512u || dist >= far_lim) return 0;
  int32_t dot = (int32_t)((uint32_t)(fx*dx) + (uint32_t)(fy*dy) + (uint32_t)(fz*dz));  // addu-wrap
  int64_t thr = (int64_t)dist * 3424;                                                  // widened (dist now > 7169 possible)
  if (dot < thr) return 0;
  if (commit) c->mem_w8(node + 1, 1);
  return 1;
}
void Cull::coneCull2b278() { Core* c = core;
  c->r[2] = (uint32_t)cone_cull_2b278(c, 1);
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
  uint32_t prev = c->game->fps60.current_object;
  uint32_t o = c->r[4];                            // a0 = object* (MIPS arg register $a0)
  c->game->fps60.current_object = o;               // fps60: tag every subsequent RTP with this object

  if (s_objlog < 0) s_objlog = cfg_dbg("obj") ? 1 : 0;
  if (s_objlog)
    fprintf(stderr, "[objlog] obj=%08x type=%02x pos=(%d,%d,%d)\n", o, c->mem_r8(o + 0x0c),
            (int16_t)obj_r16(c, o + 0x2e), (int16_t)obj_r16(c, o + 0x32), (int16_t)obj_r16(c, o + 0x36));
  int p2 = (int16_t)c->r[5], p3 = (int16_t)c->r[6], p4 = (int16_t)c->r[7];   // pos - camera (s16 each)
  performBaseCull();                                 // PC-native cull (byte-exact FUN_8007712C body)
  // The engine OWNS this margin, so it is ALWAYS active — not gated on widescreen. Even at 4:3 the
  // stock ±34° cone over-culls (edge pop-in), so we keep the wide region in every aspect; widescreen
  // then needs no extra special-casing. Env overrides remain for diagnostics only (PSXPORT_CULL_FAR/_FOV).
  if (s_cull < 0) { const char* f = cfg_str("PSXPORT_CULL_FAR"); s_cull_far = f ? atoi(f) : -1;
                    const char* v = cfg_str("PSXPORT_CULL_FOV"); s_cull_fov = v ? atoi(v) : -1; s_cull = 1; }
  int do_cull = 1;
  // FAR limit (engine units, same scale as `dist`) for the engine-owned wide RE-INCLUDE of margin
  // geometry the stock body culled. The widest stock far is 7169 (0x1C01).
  //   CULL_MARGIN_FAR = 0x10000 ≈ 9.1x the stock far — generous per issue #22 so distant scenery /
  //   terrain tiles the camera is heading toward (and off-screen-but-near static world) are kept well
  //   before they pop in. RISK (pool overflow): the active-object pool free count lives @0x800E7E7C
  //   (three active lists, ~stride 0xD0 nodes, ~52 nodes total). The margin re-include itself only
  //   re-RENDERS type-0x03 static world geometry (margin_collect, no +1 poke), so it does NOT consume
  //   pool nodes; but the cull_decide far multiplier above DOES affect what stays KEPT/active, so the
  //   real overflow guard is to keep CULL_FAR_MULT bounded (×4 leaves margin) — do not crank it so far
  //   that the whole level stays active at once. Named/tunable, not a magic literal; PSXPORT_CULL_FAR
  //   overrides at runtime for A/B.
  #ifndef CULL_MARGIN_FAR
  #define CULL_MARGIN_FAR 0x10000   // ~9.1x the widest stock far (7169) — generous render-margin reach
  #endif
  int cull_far = s_cull_far >= 0 ? s_cull_far : CULL_MARGIN_FAR;
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
  int cull_fov = s_cull_fov >= 0 ? s_cull_fov : CULL_MARGIN_FOV;
  // MEASUREMENT (entity-type taxonomy RE, journal later-127 step 1; off by default): restrict the wide
  // re-include to a single entity type (+0xc), or exclude one, so a 4:3-vs-16:9 gameplay RAM self-diff
  // isolates whether re-including THAT type perturbs gameplay logic (static-world) or not (dynamic).
  // PSXPORT_CULL_ONLY_TYPE=<n> re-includes only type n; PSXPORT_CULL_SKIP_TYPE=<n> re-includes all but n.
  static int s_only = -2, s_skip = -2;
  if (s_only == -2) { const char* x = cfg_str("PSXPORT_CULL_ONLY_TYPE"); s_only = x ? (int)strtol(x,0,0) : -1;
                      const char* y = cfg_str("PSXPORT_CULL_SKIP_TYPE"); s_skip = y ? (int)strtol(y,0,0) : -1; }
  int otype = c->mem_r8(o + 0x0c);
  if (s_only >= 0 && otype != s_only) do_cull = 0;
  if (s_skip >= 0 && otype == s_skip) do_cull = 0;
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
        // NOTE (scope of the wider cone above): margin_collect() only re-renders type-0x03 world-geometry
        // (terrain/water/static scenery) — the dominant edge/corner pop-in. Dynamic entities are NOT
        // poked visible here (that would perturb their gameplay state), so widening the cone safely
        // un-pops the static world without disturbing enemy/item/NPC logic.
        if (margin_native_enabled()) { margin_collect(c, o); }
        else { c->mem_w8(o + 1, 1); c->r[2] = 1; }                          // re-include: mark visible
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
  c->game->fps60.current_object = prev;
}

// FUN_8007778C — camera-relative cull WRAPPER. Computes obj-cam delta (wrapping s16, sign-extended),
// zeros the cull scratchpad flags 0x1F800080/0x1F800084, then forwards to the per-object cull body
// FUN_8007712C via rec_dispatch (so current-object tracking + the widescreen margin still fire).
// Camera pos @0x1F8000D0 (+2=X,+6=Z,+10=Y, u16). Was ov_cull_wrapper.
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
  performBaseCull();                           // FUN_8007712C native — was rec_dispatch
}

// FUN_800777FC — cull-wrapper variant: same taxi shape as cullWrapper (obj in c->r[4], deltas
// computed from obj+0x2E/0x32/0x36 vs cam@0x1F8000D2/D6/DA), but writes 0x1F800084 = 2 (vs 0 for
// cullWrapper). RE'd from disas 0x800777FC..0x8007786C. 3 callers in beh_id_compare_motion_dispatch.
// Deltas match cullWrapper: coord[0] = D2, coord[1] = D6, coord[2] = DA — the local names in
// cullWrapper (camz/camy for D6/DA) are misleading; the recomp subtracts D6 from obj+0x32 (r[6])
// and DA from obj+0x36 (r[7]).
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
  performBaseCull();                           // FUN_8007712C native
}

// FUN_80077ACC — cull-wrapper variant, caller-supplied position in a1/a2/a3 (not obj fields), flags
// 0x1F800080=1 / 0x1F800084=4 (vs the 0/0 form above). Makes the position camera-relative then calls
// the cull body 0x8007712C. Was ov_cull_wrap_77acc.
void Cull::cullWrap77acc() { Core* c = core;
  c->mem_w32(0x1F800080u, 1);
  c->mem_w32(0x1F800084u, 4);
  uint16_t cx = (uint16_t)c->mem_r16(0x1F8000D2u);
  uint16_t cz = (uint16_t)c->mem_r16(0x1F8000D6u);
  uint16_t cy = (uint16_t)c->mem_r16(0x1F8000DAu);
  c->r[5] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->r[5] - cx);
  c->r[6] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->r[6] - cz);
  c->r[7] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->r[7] - cy);
  performBaseCull();                           // FUN_8007712C native — was rec_dispatch
}
