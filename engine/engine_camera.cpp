// PC-native engine CAMERA update (the per-frame FOLLOW that tracks the target object). Per the CLAUDE.md
// boundary the camera is ENGINE → reimplemented PC-native. The camera STATE it writes (pos/matrix at the
// scratchpad 0x1f8000d0 block) is a CONTENT/RENDER interface — the still-PSX cull FUN_8007712c and the
// native projection both read it — so these overrides must produce RAM-IDENTICAL camera fields vs the
// recomp reference (A/B gate: override-on vs override-off build, PSXPORT_RAMDUMP + cmp -l, on the free-roam
// MOTION scene PSXPORT_AUTO_SKIP=500 AUTO_WALK=r; the static idle field is A==B and can't exercise it).
// RE: docs/engine_re.md "Camera"; disasm tools/disas.py 0x8006d960 (function-scoped). later-173.
#include "core.h"
#include "cfg.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Shared rate-limiter FUN_8006ce74(delta, maxstep) = clamp(delta, -maxstep, +maxstep) on sign-extended s16.
static inline int32_t cam_clamp(int16_t delta, int16_t maxstep) {
  if (delta >= 0) return delta < maxstep ? delta : maxstep;   // (recomp: a1<a0 ? a1 : a0)
  return delta < (int16_t)(-maxstep) ? (int16_t)(-maxstep) : delta;
}

// One axis of FUN_8006d960: snap the camera accumulator to the target when within ±10 of its integer, else
// step it by clamp(delta,±6144)<<13. accum = 32-bit fixed @ (cam+accOff); its high 16 bits (cam+accOff+2 =
// the integer the cull reads) are what `cur` was read from. Returns 1 if it SNAPPED (settled), 0 if it moved.
//   tgt32 = target's full 32-bit fixed value;  tgtInt/curInt = the two u16 integers being compared.
static inline int cam_axis(Core* c, uint32_t accAddr, uint32_t tgt32Addr, uint16_t tgtInt, uint16_t curInt,
                           int16_t maxstep) {
  int16_t delta = (int16_t)(tgtInt - curInt);          // low-16 sign-extended (recomp: subu then sll/sra16)
  if ((uint16_t)(delta + 10) < 21) {                   // |delta| <= 10  -> snap
    c->mem_w32(accAddr, c->mem_r32(tgt32Addr));
    return 1;
  }
  int32_t step = cam_clamp(delta, maxstep) << 13;      // (recomp: clampResult << 16 >>a 3 = << 13)
  c->mem_w32(accAddr, c->mem_r32(accAddr) + (uint32_t)step);
  return 0;
}

// FUN_8006d960 (0x8006d960) — track the camera X and Z toward the target object a1.
//   X: accum 0x1f8000dc (int 0x1f8000de) toward a1[+0]/a1[+2];  Z: accum 0x1f8000e4 (int 0x1f8000e6) toward
//   a1[+8]/a1[+a].  Returns v0 = 1 iff BOTH axes are settled (snapped) this frame, else 0.
// Debug TELEPORT (REPL `tp X Y Z`): writes Tomba's MASTER position (the 16.16 fixed X/Y/Z at 0x800E7EAC/
// B0/B4 in main RAM — this 2.5D game uses one position for player + camera-center; RE'd 2026-06-21). The
// whole pipeline (player int16 mirrors, camera follow, render) tracks it naturally — a real teleport, not a
// camera poke. NOTE: a position write does NOT trigger a cross-AREA load; it moves Tomba within the loaded
// area (reaching a different area needs the stage/area loader driven). Y is re-derived by the camera floor
// clamp, so pass a sane ground Y for the target area.
#define T2_PLAYER_X 0x800E7EACu   // G+0x2C  (G = 0x800E7E80, the master position struct)
#define T2_PLAYER_Y 0x800E7EB0u   // G+0x30
#define T2_PLAYER_Z 0x800E7EB4u   // G+0x34
#define T2_PLAYER_SPEED 0x800E7EC4u
static int g_tp_pending = 0; static int32_t g_tp_x, g_tp_y, g_tp_z;
void cam_teleport(int x, int y, int z) { g_tp_x = x; g_tp_y = y; g_tp_z = z; g_tp_pending = 1; }
void cam_teleport_off(void) { g_tp_pending = 0; }

static void ov_cam_track_xz(Core* c) {
  uint32_t a1 = c->r[5];
  if (cfg_dbg("cam")) { static int once = 0; if (!once) { once = 1;
    fprintf(stderr, "[cam] ov_cam_track_xz FIRED (a1=0x%08X)\n", a1); } }
  if (g_tp_pending) {                                    // one-shot: write Tomba's master position, camera follows
    g_tp_pending = 0;
    c->mem_w32(T2_PLAYER_X, (uint32_t)g_tp_x << 16);
    c->mem_w32(T2_PLAYER_Y, (uint32_t)g_tp_y << 16);
    c->mem_w32(T2_PLAYER_Z, (uint32_t)g_tp_z << 16);
    c->mem_w32(T2_PLAYER_SPEED, 0);
    fprintf(stderr, "[tp] Tomba -> (%d,%d,%d)\n", g_tp_x, g_tp_y, g_tp_z);
  }
  int snapX = cam_axis(c, 0x1f8000dcu, a1 + 0, c->mem_r16(a1 + 2),  c->mem_r16(0x1f8000deu), 6144);
  int snapZ = cam_axis(c, 0x1f8000e4u, a1 + 8, c->mem_r16(a1 + 10), c->mem_r16(0x1f8000e6u), 6144);
  c->r[2] = (snapX && snapZ) ? 1u : 0u;                // v0 = both-settled flag (recomp's a0 fold)
}

// FUN_8006da54 (0x8006da54) — track the camera Y toward the target a1: accum 0x1f8000e0 (int 0x1f8000e2)
// toward a1[+4]/[+6], maxstep 5632. Single axis. Returns v0 = 1 iff settled (snapped).
static void ov_cam_track_y(Core* c) {
  uint32_t a1 = c->r[5];
  if (cfg_dbg("cam")) { static int once = 0; if (!once) { once = 1;
    fprintf(stderr, "[cam] ov_cam_track_y FIRED (a1=0x%08X)\n", a1); } }
  int snapY = cam_axis(c, 0x1f8000e0u, a1 + 4, c->mem_r16(a1 + 6), c->mem_r16(0x1f8000e2u), 5632);
  c->r[2] = snapY ? 1u : 0u;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_8006e464 — the per-frame camera ROTATION / LOOK-AT builder (the matrix-adjacent pitch fields).
// RE: docs/engine_re.md "Camera". Big multi-mode function: it picks a camera MODE (two jump tables) to
// compute a target heading ANGLE + a RADIUS, derives a LOOK POINT around the scene center
// (0x1f800160/164), then re-derives yaw (ratan2) + distance (isqrt) to that look point and folds them
// into the pitch accumulators 0x1f8000d0 / 0x1f8000d8. Per the boundary the camera is ENGINE: we OWN
// the control flow + arithmetic PC-native, but CALL the retained libgte trig (rsin/rcos/ratan2/isqrt)
// via rec_dispatch rather than reproduce their LUTs — they are a math library, not PSX hardware.
//
// RADIUS = sext16(-mem_r16(0x1f8000ee)). The recomp sets s2=-ee in the DELAY SLOT at e4d8 (so it
// executes UNCONDITIONALLY, before the mode dispatch) and sign-extends s2 to 16 bits right before each
// mult — so every path uses the same radius; only the e518 path overrides the raw value to -ee-600.
// (Mis-reading that delay slot as an ancestor-supplied s2 was the first wrong model — corrected.)
//
// Output (0x1f8000d0/d8) is a CONTENT/RENDER interface and lives in SCRATCHPAD, which the main-RAM A/B
// dump is BLIND to. The gate is therefore a per-call comparator (PSXPORT_DEBUG=camverify): run native,
// snapshot its scratchpad writes, restore, run the recomp ORACLE on the same inputs, compare. Verified
// 0 mismatches with d0 actively accumulating on the free-roam MOTION scene WITH CONTINUOUS movement (a
// stopped scene is degenerate, increment 0). The default field path (table1 case-0 pattern) is what
// gameplay exercises; the special-camera modes are ported faithfully but stay latent until driven. later-175.

static const uint32_t T2_RSIN   = 0x80083e80u;   // rsin(angle12) -> Q12
static const uint32_t T2_RCOS   = 0x80083f50u;   // rcos(angle12) -> Q12
static const uint32_t T2_ISQRT  = 0x80084080u;   // isqrt(x)      -> floor(sqrt)
static const uint32_t T2_RATAN2 = 0x80085690u;   // ratan2(y,x)   -> angle12
static const uint32_t T2_ANGCMP = 0x80077768u;   // helper used by camera mode-2-case-1

// trig-call spy (DS_SPY): record (fn,a0,a1,result) of every cam_call so the dist-solver verify can diff the
// native call-sequence vs the oracle's. Temporary diagnostic.
static inline int32_t cam_call(Core* c, uint32_t fn, int32_t a0, int32_t a1, int32_t a2) {
  c->r[4] = (uint32_t)a0; c->r[5] = (uint32_t)a1; c->r[6] = (uint32_t)a2;
  rec_dispatch(c, fn);
  return (int32_t)c->r[2];
}

// COMMON look-at tail @0x8006e7c8: given a heading `theta` and a `radius`, place a look point around the
// scene center, then fold yaw+distance into the pitch accumulators. (cam = camera object, for the +0x66
// "settled" flag.)
static void cam_lookat_tail(Core* c, uint32_t cam, int32_t theta, int32_t radius) {
  int32_t rc    = cam_call(c, T2_RCOS, theta, 0, 0);
  int32_t lookX = (int32_t)c->mem_r16(0x1f800160u) + ((rc * radius) >> 12);
  int32_t rs    = cam_call(c, T2_RSIN, theta, 0, 0);
  int32_t lookZ = (int32_t)c->mem_r16(0x1f800164u) - ((rs * radius) >> 12);

  int32_t camZ = (int32_t)c->mem_r16(0x1f8000dau);
  int32_t camX = (int32_t)c->mem_r16(0x1f8000d2u);
  int32_t dz   = (int16_t)(lookZ - camZ);
  int32_t dx   = (int16_t)(lookX - camX);
  int32_t yaw  = (int16_t)cam_call(c, T2_RATAN2, -dz, dx, 0);
  int32_t dist = (int16_t)cam_call(c, T2_ISQRT, dx * dx + dz * dz, 0, 0);   // sext16, as the recomp does

  int32_t rc2 = cam_call(c, T2_RCOS, yaw, 0, 0);
  c->mem_w32(0x1f8000d0u, c->mem_r32(0x1f8000d0u) + ((rc2 * dist) >> 1));
  int32_t rs2 = cam_call(c, T2_RSIN, yaw, 0, 0);
  c->mem_w32(0x1f8000d8u, c->mem_r32(0x1f8000d8u) - ((rs2 * dist) >> 1));
  if (dist < 401) c->mem_w8(cam + 0x66, c->mem_r8(cam + 0x66) | 1);
}

// JOIN @0x8006e640: theta = G[+0x140] + cam[+0x52] + delta (stored back to cam[+0x8c]); look-at with the
// incoming radius. Used by the table-1 modes + the c114&2 path.
static void cam_join_e640(Core* c, uint32_t cam, uint32_t G, int32_t delta, int32_t radius) {
  int32_t  sum    = (int32_t)c->mem_r16(G + 0x140) + (int32_t)c->mem_r16(cam + 0x52) + delta;
  uint16_t theta16 = (uint16_t)sum;
  c->mem_w16(cam + 0x8c, theta16);
  cam_lookat_tail(c, cam, (int16_t)theta16, radius);
}

// table-1 "e570" pattern: ±cam[+0x56] depending on whether G[+0x140]==G[+0x56].
static inline int32_t cam_t1_e570(Core* c, uint32_t cam, uint32_t G) {
  int32_t g140 = (int16_t)c->mem_r16(G + 0x140), g56 = (int16_t)c->mem_r16(G + 0x56);
  int32_t cam56 = (int32_t)c->mem_r16(cam + 0x56);
  return (g140 == g56) ? cam56 : -cam56;
}

// TABLE 1 (dispatch on G[+0x164], range<13 else the "e614"==e570 body) -> the heading delta for e640.
static int32_t cam_table1_delta(Core* c, uint32_t cam, uint32_t G) {
  uint8_t idx = c->mem_r8(G + 0x164);
  switch (idx) {
    case 1: case 9: case 11: {                                  // e58c
      if (c->mem_r32(G + 0x158) == 0) return cam_t1_e570(c, cam, G);
      int32_t g140 = (int16_t)c->mem_r16(G + 0x140), g56 = (int16_t)c->mem_r16(G + 0x56);
      int32_t cam56 = (int32_t)c->mem_r16(cam + 0x56);
      return (g140 == g56) ? -cam56 : cam56;                    // inverted sense
    }
    case 3: {                                                   // e5b8
      if (c->mem_r8(cam + 0x77) != 0)      return cam_t1_e570(c, cam, G);
      if (c->mem_r8(0x800bf870u) == 6)     return cam_t1_e570(c, cam, G);
      int32_t g140 = (int16_t)c->mem_r16(G + 0x140), g56 = (int16_t)c->mem_r16(G + 0x56);
      int32_t cam56 = (int32_t)c->mem_r16(cam + 0x56);
      int32_t g168  = (int32_t)c->mem_r8(G + 0x168) << 6;
      return (g140 != g56) ? (g168 - cam56) : (cam56 - g168);
    }
    default:                                                    // 0,2,4..8,10,12,>=13 -> e570/e614
      return cam_t1_e570(c, cam, G);
  }
}

// TABLE 2 (dispatch on ((G[+0x61]&0xff)>>4)-1, range<8 else default): special-camera headings; all end
// in SHAPE B — theta masked to 12 bits, look-at with the incoming radius.
static void cam_table2(Core* c, uint32_t cam, uint32_t G, int32_t radius) {
  uint32_t a1   = c->mem_r8(G + 0x61);
  uint32_t idx2 = ((a1 & 0xff) >> 4) - 1;        // unsigned; >=8 -> default
  int32_t  theta;
  if (idx2 == 0) {                                                       // e6a8
    if (a1 & 1) theta = (int32_t)c->mem_r16(0x1f800196u) + 512;
    else {
      int32_t s196 = (int16_t)c->mem_r16(0x1f800196u);
      int32_t g140 = (int16_t)c->mem_r16(G + 0x140);
      int32_t d    = ((s196 - g140) & 0xfff) >> 1;
      theta = (int32_t)c->mem_r16(0x1f800196u) + d;
    }
  } else if (idx2 == 1) {                                                // e6f8
    if (a1 & 1) {
      int32_t r = cam_call(c, T2_ANGCMP, (int16_t)c->mem_r16(G + 0x56),
                                         (int16_t)c->mem_r16(G + 0x140), 0);
      theta = (int32_t)c->mem_r16(G + 0x140) + (r == 0 ? 512 : 1536);
    } else {
      int32_t a = (int16_t)c->mem_r16(0x1f800194u);
      int32_t b = (int16_t)c->mem_r16(0x1f800196u);
      theta = (int32_t)c->mem_r16(G + 0x140) + (a != b ? 512 : 1536);
    }
  } else if (idx2 == 2) {                                                // e750
    int32_t vu = (int32_t)c->mem_r16(0x1f800196u);
    theta = vu + ((int16_t)vu / 2);
  } else if (idx2 == 3) {                                                // e77c
    theta = (int32_t)c->mem_r16(0x1f800196u) + 512;
  } else if (idx2 == 7) {                                                // e78c (both branches +1024)
    theta = (int32_t)c->mem_r16(G + 0x140) + 1024;
  } else {                                                               // e7b4 default (4,5,6,>=8)
    theta = (int32_t)c->mem_r16(G + 0x140) + 512;
  }
  cam_lookat_tail(c, cam, theta & 0xfff, radius);
}

static void ov_cam_rotbuild(Core* c) {
  uint32_t cam = c->r[4];
  const uint32_t G = 0x800e7e80u;

  if (cfg_dbg("cam")) { static int once = 0; if (!once) { once = 1;
    fprintf(stderr, "[cam] ov_cam_rotbuild FIRED (cam=0x%08X)\n", cam); } }

  if (c->mem_r8(cam + 0x76)) return;                     // e494: disabled this frame

  uint32_t a1 = c->mem_r8(G + 0x61);
  uint32_t a0 = a1 & 0xff;
  if (a0 != 0 && (c->mem_r8(cam + 0x72) & 0x80)) return; // e4bc

  // RADIUS = -mem_r16(0x1f8000ee), SIGN-EXTENDED TO 16 BITS at the point of use (the recomp does
  // `sll/sra 16` on s2 right before the mult). s2 is set to -ee in the DELAY SLOT at e4d8 (executes
  // unconditionally regardless of the e4d4 branch), so EVERY path uses this radius — only the e518
  // path overrides the raw value to -ee-600 (then likewise sext16'd at use).
  int32_t negEE  = -(int32_t)c->mem_r16(0x1f8000eeu);    // full 32-bit (e.g. -63786)
  int32_t radius = (int16_t)negEE;                       // sext16 -> e.g. 1750
  uint8_t c114   = c->mem_r8(cam + 0x72);

  if (c114 & 0x40) {                                     // e4d4 not taken: SHAPE A, theta=cam[+0x8c]
    cam_lookat_tail(c, cam, (int16_t)c->mem_r16(cam + 0x8c), radius);
    return;
  }
  if (a0 != 0 && (a1 & 1) == 0) {                        // e504: TABLE 2
    cam_table2(c, cam, G, radius);
    return;
  }
  if (c->mem_r8(G + 0x17a)) {                            // e518: delta 0, radius override -ee-600
    cam_join_e640(c, cam, G, 0, (int16_t)(negEE - 600));
    return;
  }
  if (c114 & 2) {                                        // e524
    int32_t delta = (c114 & 1) ? -(int32_t)c->mem_r16(cam + 0x56)
                               :  (int32_t)c->mem_r16(cam + 0x56);
    cam_join_e640(c, cam, G, delta, radius);
    return;
  }
  cam_join_e640(c, cam, G, cam_table1_delta(c, cam, G), radius);   // e540: TABLE 1
}

// PSXPORT_DEBUG=camverify — per-call A/B gate for the rotation builder. The output (0x1f8000d0/d8) is
// scratchpad, so the main-RAM dump is blind to it; instead run MY native code, snapshot its writes,
// restore, run the recomp ORACLE on the same inputs, and compare directly. The only writes are the 4
// locations checked here (sw 0x1f8000d0, sw 0x1f8000d8, sh cam+0x8c, sb cam+0x66).
static void ov_cam_rotbuild_verify(Core* c) {
  uint32_t cam = c->r[4];
  // Snapshot the FULL scratchpad (1KB) + register file so the two runs are truly independent — the
  // libgte trig helpers scribble scratchpad temporaries that would otherwise leak from run 1 into run 2.
  uint32_t spad[256]; for (int i = 0; i < 256; i++) spad[i] = c->mem_r32(0x1f800000u + i * 4);
  uint8_t  s_66 = c->mem_r8(cam + 0x66);
  uint32_t rsave[32]; memcpy(rsave, c->r, sizeof rsave);

  ov_cam_rotbuild(c);                                   // native
  uint32_t m_d0 = c->mem_r32(0x1f8000d0u), m_d8 = c->mem_r32(0x1f8000d8u);
  uint16_t m_8c = c->mem_r16(cam + 0x8c);  uint8_t m_66 = c->mem_r8(cam + 0x66);

  for (int i = 0; i < 256; i++) c->mem_w32(0x1f800000u + i * 4, spad[i]);   // restore + rerun oracle
  c->mem_w8(cam + 0x66, s_66);
  memcpy(c->r, rsave, sizeof rsave);
  rec_interp(c, 0x8006e464u);                            // recomp oracle on the same inputs
  uint32_t o_d0 = c->mem_r32(0x1f8000d0u), o_d8 = c->mem_r32(0x1f8000d8u);
  uint16_t o_8c = c->mem_r16(cam + 0x8c);  uint8_t o_66 = c->mem_r8(cam + 0x66);

  static int nbad = 0, ngood = 0;
  if (m_d0 != o_d0 || m_d8 != o_d8 || m_8c != o_8c || m_66 != o_66) {
    if (nbad++ < 40)
      fprintf(stderr, "[camverify] MISMATCH d0 %08x/%08x d8 %08x/%08x 8c %04x/%04x 66 %02x/%02x (mine/oracle)\n",
              m_d0, o_d0, m_d8, o_d8, m_8c, o_8c, m_66, o_66);
  } else if ((ngood++ % 200) == 0) {
    fprintf(stderr, "[camverify] match #%d (d0=%08x d8=%08x)\n", ngood, o_d0, o_d8);
  }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_8006d2ac — the per-frame camera DISTANCE / ZOOM solver (first sub-fn of orchestrator FUN_8006e0f0).
// RE: docs/engine_re.md "Camera". cam = a0 (main-RAM camera object), G = 0x800e7e80. It:
//   1. sets a "settle timer" cam[+0x22] (240 normally, 0 when blocked by various flags),
//   2. picks a TARGET planar point (tX,tZ) — a 13-entry jump table on G[+0x164] selects the source
//      (world center G[+0x2c]/[+0x34], a sub-object G[+0x10], scratchpad 0x1f800200/204, or G[+0x14c]/[+0x150]),
//      plus a cam[+0x72]&2 fast path; a "mode" byte picks whether to add 2048 (180°) to the base heading,
//   3. derives the current camera→look-point planar distance (isqrt) and the angular error (ratan2),
//   4. smooths cam[+0x14] toward ±distance limits (±65536/frame, clamped to ±0x280000) and accumulates it
//      into the distance cam[+0x58], then places the camera planar position cam[+0x08]=X / cam[+0x10]=Z at
//      that distance along the heading G[+0x140] from (tX,tZ).
// Per the boundary the camera is ENGINE → own control flow + arithmetic native; CALL the libgte trig
// (rsin/rcos/ratan2/isqrt) via rec_dispatch. Outputs are MAIN-RAM cam fields (visible to the A/B dump),
// but gated robustly per-call by the camverify comparator below (snapshot, run native, restore, run oracle).
//
// All arithmetic mirrors the recomp's exact 32-bit mult-lo / arithmetic-shift behaviour (helper `mlo`).

static inline int32_t mlo(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

static void ov_cam_dist_solve(Core* c) {
  uint32_t cam = c->r[4];
  const uint32_t G = 0x800e7e80u;

  if (cfg_dbg("cam")) { static int once = 0; if (!once) { once = 1;
    fprintf(stderr, "[cam] ov_cam_dist_solve FIRED (cam=0x%08X)\n", cam); } }

  // 1. settle timer cam[+0x22]
  uint8_t g61 = c->mem_r8(G + 0x61);
  int16_t timer;
  if      (g61 & 0x80)                  timer = 0;
  else if (c->mem_r8(G + 0x17a))        timer = 0;
  else if ((g61 & 0xff) == 0)           timer = 240;
  else if (g61 & 1)                     timer = 240;
  else if (c->mem_r8(0x800bf816u))      timer = 0;
  else                                  timer = 240;
  c->mem_w16(cam + 0x22, (uint16_t)timer);
  uint8_t flags = c->mem_r8(cam + 0x72);
  if (flags & 4) c->mem_w16(cam + 0x22, 0);
  flags = c->mem_r8(cam + 0x72);

  // 2. target planar point (tX,tZ in 16.16) + heading "mode" byte
  int32_t tX, tZ, mode;
  if (flags & 2) {
    tX = (int32_t)c->mem_r32(G + 0x2c);
    tZ = (int32_t)c->mem_r32(G + 0x34);
    mode = flags & 1;
  } else {
    uint8_t idx = c->mem_r8(G + 0x164);
    uint8_t g147 = c->mem_r8(G + 0x147);
    if (idx >= 13) idx = 8;                              // >=13 -> case-8 body
    switch (idx) {
      case 2: case 3: {                                  // 8006d3bc: a sub-object at G[+0x10]
        uint32_t p = c->mem_r32(G + 0x10);
        tX = (int32_t)(c->mem_r32(p + 0x2c) << 16);
        tZ = (int32_t)(c->mem_r32(p + 0x34) << 16);
        mode = g147;
        break;
      }
      case 12: {                                         // 8006d3d8: scratchpad 0x1f800200/204
        tX = (int32_t)(c->mem_r16(0x1f800200u) << 16);
        tZ = (int32_t)(c->mem_r16(0x1f800204u) << 16);
        mode = g147;
        break;
      }
      case 7: {                                          // 8006d3f4: G[+0x14c]/[+0x150]
        tX = (int32_t)(c->mem_r16(G + 0x14c) << 16);
        tZ = (int32_t)(c->mem_r16(G + 0x150) << 16);
        mode = g147;
        break;
      }
      case 8: {                                          // 8006d40c (and idx>=13): world center, plain mode
        tX = (int32_t)c->mem_r32(G + 0x2c);
        tZ = (int32_t)c->mem_r32(G + 0x34);
        mode = g147;
        break;
      }
      default: {                                         // 8006d39c: 0,1,4,5,6,9,10,11 — world center
        tX = (int32_t)c->mem_r32(G + 0x2c);
        tZ = (int32_t)c->mem_r32(G + 0x34);
        mode = (c->mem_r32(G + 0x158) == 0) ? (int32_t)g147 : (1 - (int32_t)g147);
        break;
      }
    }
  }

  // 3. base heading + the look-point offset using the settle timer
  int32_t baseAng = (int32_t)c->mem_r16(G + 0x140);
  if (mode & 0xff) baseAng += 2048;
  int32_t s0a   = (int16_t)(uint16_t)baseAng;            // sext16 (recomp: sll/sra 16)
  int32_t cam22 = (int16_t)c->mem_r16(cam + 0x22);
  int32_t s2 = tX + (mlo(cam_call(c, T2_RCOS, s0a, 0, 0), cam22) << 4);
  int32_t s1 = tZ - (mlo(cam_call(c, T2_RSIN, s0a, 0, 0), cam22) << 4);

  // current camera planar position along heading G[+0x140] at distance cam[+0x58]
  int32_t g140s = (int16_t)c->mem_r16(G + 0x140);
  int32_t cam58 = (int32_t)c->mem_r32(cam + 0x58);
  int32_t coordX = tX + (mlo(cam_call(c, T2_RCOS, g140s, 0, 0), cam58) >> 4);
  int32_t coordZ = tZ - (mlo(cam_call(c, T2_RSIN, g140s, 0, 0), cam58) >> 4);
  int32_t s2q = (s2 - coordX) >> 8;                      // Q8 planar deltas to the look point
  int32_t s1q = (s1 - coordZ) >> 8;
  int32_t s0d = cam_call(c, T2_ISQRT, mlo(s2q, s2q) + mlo(s1q, s1q), 0, 0) << 8;
  int32_t ang = cam_call(c, T2_RATAN2, -s1q, s2q, 0);
  int32_t angd = (ang - (int32_t)c->mem_r16(G + 0x140) - 1024) & 0xfff;

  // 4. smooth cam[+0x14] toward the distance, accumulate into cam[+0x58], place camera X/Z
  int32_t cam14;
  int32_t cur = (int32_t)c->mem_r32(cam + 0x14);
  if (0x140000 < s0d) {                                  // far: step ±65536/frame toward a limit
    if (angd < 2048) {                                   // look point ahead — pull NEGATIVE
      int32_t neg = -s0d;
      if (cur < neg)        cam14 = ((int32_t)0xffd80000 < neg) ? neg : (int32_t)0xffd80000;
      else                  cam14 = (cur > 0 ? 0 : cur) - 65536;   // blez: zero only when cur>0
    } else {                                             // behind — push POSITIVE
      if (s0d < cur)        cam14 = (0x0027ffff < s0d) ? 0x00280000 : s0d;
      else                  cam14 = (cur < 0 ? 0 : cur) + 65536;
    }
  } else {
    // near: snap to the distance — but the far/near branch's DELAY SLOT (slti v0,angd,2048) re-loads v0, so
    // the 8006d5b8 test is `angd<2048`, which NEGATES s0d when the look point is ahead (recomp 8006d5c0).
    cam14 = (angd < 2048) ? -s0d : s0d;
  }
  c->mem_w32(cam + 0x14, (uint32_t)cam14);

  int32_t cam58n = cam58 + (cam14 >> 8);
  c->mem_w32(cam + 0x58, (uint32_t)cam58n);
  c->mem_w32(cam + 0x08, (uint32_t)(tX + (mlo(cam_call(c, T2_RCOS, g140s, 0, 0), cam58n) >> 4)));
  c->mem_w32(cam + 0x10, (uint32_t)(tZ - (mlo(cam_call(c, T2_RSIN, g140s, 0, 0), cam58n) >> 4)));
}

// PSXPORT_DEBUG=camverify — per-call A/B gate for the distance solver. Snapshot the cam struct + scratchpad,
// run my native code, save the post-state, restore, run the recomp ORACLE on the same inputs, diff EVERY
// word of the cam struct + scratchpad (so no written field is missed). later-176.
static void ov_cam_dist_solve_verify(Core* c) {
  uint32_t cam = c->r[4];
  uint32_t cam0[64], sp0[256], rsave[32];
  for (int i = 0; i < 64;  i++) cam0[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) sp0[i]  = c->mem_r32(0x1f800000u + i * 4);
  memcpy(rsave, c->r, sizeof rsave);

  ov_cam_dist_solve(c);
  uint32_t camM[64]; for (int i = 0; i < 64; i++) camM[i] = c->mem_r32(cam + i * 4);

  for (int i = 0; i < 64;  i++) c->mem_w32(cam + i * 4, cam0[i]);
  for (int i = 0; i < 256; i++) c->mem_w32(0x1f800000u + i * 4, sp0[i]);
  memcpy(c->r, rsave, sizeof rsave);
  rec_interp(c, 0x8006d2acu);
  uint32_t camO[64]; for (int i = 0; i < 64; i++) camO[i] = c->mem_r32(cam + i * 4);

  static int nbad = 0, ngood = 0; int bad = 0;
  for (int i = 0; i < 64; i++) if (camM[i] != camO[i]) {
    bad = 1;
    if (nbad++ < 60)
      fprintf(stderr, "[distverify] MISMATCH cam+0x%02x  mine=%08x oracle=%08x\n", i * 4, camM[i], camO[i]);
  }
  if (!bad && (ngood++ % 200) == 0)
    fprintf(stderr, "[distverify] match #%d (x=%08x z=%08x d58=%08x)\n",
            ngood, camO[2], camO[4], camO[0x16]);
}

// FUN_8006e010 — camera ANGLE-ACCUMULATOR step (sub-fn of orchestrator FUN_8006e0f0, called when
// cam[+0x77]==0). Picks a target for the signed-32 field cam[+0x34] from the world mode bytes
// G[+0x164]/G[+0x168] (5-entry jump table @0x8001697c), then rate-limits cam[+0x34] toward it in ±8 steps,
// snapping on overshoot. Pure integer; NO trig / NO rec_dispatch. Output cam+0x34 is MAIN-RAM. a0 = cam.
// RE this session (gen_func_8006E010, shard_2.c); delay slot @9154 precomputes cur+8 for the step-up path.
static void ov_cam_angle_step(Core* c) {
  uint32_t cam = c->r[4];
  const uint32_t G = 0x800e7e80u;
  int32_t cur = (int32_t)c->mem_r32(cam + 0x34);
  if (c->mem_r8(G + 0x164) != 3) {                    // not mode 3: drive cam[+0x34] toward 0 by ±8
    if (cur <= 0) { int32_t r = cur + 8; c->mem_w32(cam + 0x34, (uint32_t)r); if (r >= 0) c->mem_w32(cam + 0x34, 0); }
    else          { int32_t r = cur - 8; c->mem_w32(cam + 0x34, (uint32_t)r); if (r <= 0) c->mem_w32(cam + 0x34, 0); }
    return;
  }
  uint8_t idx = c->mem_r8(G + 0x168);                 // mode 3: target from the sub-mode jump table (all <=0)
  int32_t target;
  switch (idx >= 5 ? 4 : idx) {
    case 0: case 1: target = 0;    break;
    case 2:         target = -128; break;
    case 3:         target = -256; break;
    default:        target = -384; break;             // jt idx 4 and idx>=5
  }
  if (target < cur) { int32_t r = cur - 8; c->mem_w32(cam + 0x34, (uint32_t)r); if (r < target) c->mem_w32(cam + 0x34, (uint32_t)target); }
  else              { int32_t r = cur + 8; c->mem_w32(cam + 0x34, (uint32_t)r); if (target < r) c->mem_w32(cam + 0x34, (uint32_t)target); }
}
// PSXPORT_DEBUG=camverify — per-call A/B gate (cam-struct diff; output cam+0x34 is main-RAM).
static void ov_cam_angle_step_verify(Core* c) {
  uint32_t cam = c->r[4];
  uint32_t cam0[64], sp0[256], rsave[32];
  for (int i = 0; i < 64;  i++) cam0[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) sp0[i]  = c->mem_r32(0x1f800000u + i * 4);
  memcpy(rsave, c->r, sizeof rsave);
  ov_cam_angle_step(c);
  uint32_t camM[64]; for (int i = 0; i < 64; i++) camM[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 64;  i++) c->mem_w32(cam + i * 4, cam0[i]);
  for (int i = 0; i < 256; i++) c->mem_w32(0x1f800000u + i * 4, sp0[i]);
  memcpy(c->r, rsave, sizeof rsave);
  rec_interp(c, 0x8006e010u);
  uint32_t camO[64]; for (int i = 0; i < 64; i++) camO[i] = c->mem_r32(cam + i * 4);
  static int nbad = 0, ngood = 0; int bad = 0;
  for (int i = 0; i < 64; i++) if (camM[i] != camO[i]) { bad = 1;
    if (nbad++ < 60) fprintf(stderr, "[anglestepverify] MISMATCH cam+0x%02x mine=%08x oracle=%08x\n", i * 4, camM[i], camO[i]); }
  if (!bad && (ngood++ % 200) == 0) fprintf(stderr, "[anglestepverify] match #%d (cam+34=%08x)\n", ngood, camO[0x0d]);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_8006c80c — camera-Y FLOOR clamp (sub-fn of orchestrator FUN_8006e0f0). Clamps the integer
// camera-Y at scratchpad 0x1f8000e2 to a per-render-mode limit. a0=cam is UNUSED (reads globals only).
// Selector = mem_r8(0x800bf870)-1 dispatched through a 13-entry jump table @0x80016874 — only modes
// 1,4,6,10,13 clamp; every other mode returns untouched. Output: SCRATCHPAD 0x1f8000e2 (sh), so the
// main-RAM A/B dump is BLIND — camverify must diff scratchpad. NO trig / NO sub-calls.
// (shard_6.c gen_func_8006C80C; branch SENSES transcribed line-for-line — some arms write on Y<N via
//  `beq`, others write on Y>=N via `bne`; the constant set in the delay slot is the value written.)
static void ov_cam_y_floor(Core* c) {
  uint8_t mode1 = c->mem_r8(0x800bf870u);
  uint32_t idx = (uint32_t)(uint8_t)(mode1 - 1);
  if (idx >= 13) return;                               // sltiu v1,13 guard
  const uint32_t YA = 0x1f8000e2u;
  int32_t Y = (int16_t)c->mem_r16(YA);
  switch (idx) {
    case 0:                                            // mode1=1 (0x8006c844): beq → write when Y<-10140
      if (Y < -10140) c->mem_w16(YA, (uint16_t)(int16_t)-10140);
      break;
    case 3: {                                          // mode1=4 (0x8006c868)
      if (c->mem_r8(0x800bf871u) == 7) {
        if ((int16_t)c->mem_r16(0x800e7eb6u) < 6800) {
          if (Y < -7299) {                             // bne → L_8006C8B8 with r2=(Y<-6499)
            if (!(Y < -6499)) c->mem_w16(YA, (uint16_t)(int16_t)-6500);  // (dead: Y<-7299 ⇒ Y<-6499)
          } else {
            c->mem_w16(YA, (uint16_t)(int16_t)-7300);  // fall-through: Y >= -7299
          }
        }
        // g7eb6 >= 6800 → no clamp (beq → RET)
      } else {                                         // !=7 → L_8006C8C8: bne → write when Y>=-6599
        if (!(Y < -6599)) c->mem_w16(YA, (uint16_t)(int16_t)-6600);
      }
      break;
    }
    case 5:                                            // mode1=6 (0x8006c8e8): both arms beq → write when Y<N
      if (c->mem_r8(0x1f800207u) == 14) {
        if (Y < -7200) c->mem_w16(YA, (uint16_t)(int16_t)-7200);
      } else {
        if (Y < -9200) c->mem_w16(YA, (uint16_t)(int16_t)-9200);
      }
      break;
    case 9:                                            // mode1=10 (0x8006c93c): beq → write when Y<-2160
      if (Y < -2160) c->mem_w16(YA, (uint16_t)(int16_t)-2160);
      break;
    case 12:                                           // mode1=13 (0x8006c960): bne → write when Y>=-1399
      if (!(Y < -1399)) c->mem_w16(YA, (uint16_t)(int16_t)-1400);
      break;
    default:                                           // modes 2,3,5,7,8,9,11,12 → no clamp
      break;
  }
}
// PSXPORT_DEBUG=camverify — per-call A/B gate. Output is SCRATCHPAD, so snapshot+diff the full scratchpad.
static void ov_cam_y_floor_verify(Core* c) {
  uint32_t sp0[256], rsave[32];
  for (int i = 0; i < 256; i++) sp0[i] = c->mem_r32(0x1f800000u + i * 4);
  memcpy(rsave, c->r, sizeof rsave);
  ov_cam_y_floor(c);
  uint32_t spM[256]; for (int i = 0; i < 256; i++) spM[i] = c->mem_r32(0x1f800000u + i * 4);
  for (int i = 0; i < 256; i++) c->mem_w32(0x1f800000u + i * 4, sp0[i]);
  memcpy(c->r, rsave, sizeof rsave);
  rec_interp(c, 0x8006c80cu);
  uint32_t spO[256]; for (int i = 0; i < 256; i++) spO[i] = c->mem_r32(0x1f800000u + i * 4);
  static int nbad = 0, ngood = 0; int bad = 0;
  for (int i = 0; i < 256; i++) if (spM[i] != spO[i]) { bad = 1;
    if (nbad++ < 60) fprintf(stderr, "[yfloorverify] MISMATCH sp+0x%03x mine=%08x oracle=%08x\n", i * 4, spM[i], spO[i]); }
  if (!bad && (ngood++ % 200) == 0) fprintf(stderr, "[yfloorverify] match #%d (y=%04x)\n", ngood, c->mem_r16(0x1f8000e2u));
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_8006d654 — camera PITCH / vertical-look smoother (sub-fn of orchestrator FUN_8006e0f0). a0=cam,
// G=0x800e7e80. Picks a TARGET "base height" r5 (13-entry jump table @0x8001690c on G[+0x164]) from one
// of several sources, applies a fixed -600<<16 bias, computes delta = target - cam[+0x0c], then with a
// dead-band + asymmetric PULL/PUSH clamp arms rate-limits the velocity cam[+0x18] and accumulates
// cam[+0x0c] += cam[+0x18]. Pure integer; NO trig / NO scratchpad / NO sub-calls. Outputs cam[+0x18],
// cam[+0x0c] (both MAIN-RAM). Transcribed line-for-line from shard_0.c gen_func_8006D654 (disas.py
// mis-scopes it; the recomp body is authoritative). later-176-class delay-slot hazards (delta in the
// `bltz` delay slot, the dir flag in the two dead-band branch delay slots) are folded explicitly.
static void ov_cam_pitch(Core* c) {
  uint32_t cam = c->r[4];
  const uint32_t G = 0x800e7e80u;
  int32_t g30  = (int32_t)c->mem_r32(G + 0x30);
  int16_t g17e = (int16_t)c->mem_r16(G + 0x17e);
  int sign = (g17e & 0x8000) != 0;                     // sign bit of G[+0x17e]

  // Phase A: base height r5 (folded with its per-path offset); `viaC8` adds +200<<16 (L_8006D7C8).
  int32_t r5 = g30; int viaC8 = 0;
  uint8_t idx = c->mem_r8(G + 0x164);
  if (idx >= 13) idx = 7;                              // >=13 → L_8006D7C4 (== case 7/8 body)
  switch (idx) {
    case 2: {                                          // 0x8006d778
      uint32_t p = c->mem_r32(G + 0x10);
      r5 = (int32_t)(c->mem_r32(p + 0x30) << 16); viaC8 = 1; break;
    }
    case 3: {                                          // 0x8006d78c (→ L_8006D7D0, no +200)
      uint32_t p = c->mem_r32(G + 0x10);
      r5 = (int32_t)(c->mem_r32(p + 0x30) << 16); break;
    }
    case 12: {                                         // 0x8006d7a0: round-to-0 avg of scratchpad word + g30
      int32_t a = (int16_t)c->mem_r16(0x1f800202u);
      int32_t sum = (a << 16) + g30;
      r5 = (int32_t)((sum + (int32_t)((uint32_t)sum >> 31)) >> 1); viaC8 = 1; break;
    }
    case 7: case 8:                                    // L_8006D7C4 (and idx>=13)
      r5 = g30; viaC8 = 1; break;
    default: {                                         // 0,1,4,5,6,9,10,11 → 0x8006d690
      uint8_t m  = c->mem_r8(0x800bf821u);
      uint8_t gm = c->mem_r8(G + 0x145);
      if (m == 1) {
        r5 = g30 - (200 << 16);
      } else if (m != 0) {                             // m >= 2
        if (gm == 2) r5 = g30 + (sign ? -(240 << 16) : (40 << 16));
        else         r5 = g30 + (sign ? -(310 << 16) : -(240 << 16));
      } else {                                         // m == 0
        if (gm == 2) r5 = sign ? g30 : (g30 + (200 << 16));   // sign → L_8006D760 (r5=g30)
        else         r5 = sign ? (g30 - (70 << 16)) : g30;    // !sign → L_8006D760 (r5=g30)
      }
      break;
    }
  }
  if (viaC8) r5 += (200 << 16);
  int32_t target = r5 - (600 << 16);                   // bias is -600<<16 on every path

  // Phase B: dead-band + PULL/PUSH velocity clamp.
  int32_t delta = target - (int32_t)c->mem_r32(cam + 0x0c);
  int32_t r7 = (g17e < 0) ? (20 << 16)  : (140 << 16);
  int32_t r4 = (g17e < 0) ? (580 << 16) : (460 << 16);

  int dir; int32_t r3;
  int32_t t = delta + r4;
  if (t >= 0) { dir = 0; r3 = t; }
  else {
    int32_t t2 = t + r7;
    if (t2 < 0) { dir = 1; r3 = t2; }
    else { c->mem_w32(cam + 0x18, 0); return; }        // dead band: zero the velocity
  }

  if (dir == 0) {                                      // PULL arm (r3 = delta + r4)
    if (!(0x80000 < r3)) {                             // L_8006D884: snap cam[+0x0c], leave velocity
      c->mem_w32(cam + 0x0c, (uint32_t)(r5 + r4 - (600 << 16)));
      return;
    }
    int32_t cv = (int32_t)c->mem_r32(cam + 0x18);
    if (!(r3 < cv)) {                                  // L_8006D864: step up +16<<16 (clamp negatives to 0)
      if (cv < 0) c->mem_w32(cam + 0x18, 0);
      c->mem_w32(cam + 0x18, (uint32_t)((int32_t)c->mem_r32(cam + 0x18) + (16 << 16)));
    } else if (0x4FFFFF < r3) {
      c->mem_w32(cam + 0x18, (uint32_t)(80 << 16));
    } else {                                           // L_8006D90C
      c->mem_w32(cam + 0x18, (uint32_t)r3);
    }
  } else {                                             // PUSH arm (r3 = delta + r4 + r7)
    uint8_t g145 = c->mem_r8(G + 0x145);
    if (g145 != 0) {                                   // L_8006D8EC
      if (!(r3 < -(256 << 16))) return;                // L_8006D92C: no change
      r3 += (256 << 16);
      if (-(96 << 16) < r3) c->mem_w32(cam + 0x18, (uint32_t)r3);            // D90C
      else                  c->mem_w32(cam + 0x18, (uint32_t)-(96 << 16));   // D914
    } else {                                           // g145 == 0
      if (!(r3 < -(22 << 16))) {                       // r3 >= -22<<16 → D90C
        c->mem_w32(cam + 0x18, (uint32_t)r3);
      } else {
        int32_t cv = (int32_t)c->mem_r32(cam + 0x18);
        if (!(cv < r3)) {                              // cv >= r3 → D8D0: step down -90112 (clamp >0 to 0)
          if (cv > 0) c->mem_w32(cam + 0x18, 0);
          c->mem_w32(cam + 0x18, (uint32_t)((int32_t)c->mem_r32(cam + 0x18) + (int32_t)0xFFFEA000u));
        } else {                                       // cv < r3 → velocity = -22<<16
          c->mem_w32(cam + 0x18, (uint32_t)-(22 << 16));
        }
      }
    }
  }
  // L_8006D918: cam[+0x0c] += cam[+0x18]
  c->mem_w32(cam + 0x0c, (uint32_t)((int32_t)c->mem_r32(cam + 0x0c) + (int32_t)c->mem_r32(cam + 0x18)));
}
// PSXPORT_DEBUG=camverify — per-call A/B gate (cam-struct + scratchpad diff; outputs are main-RAM cam fields).
static void ov_cam_pitch_verify(Core* c) {
  uint32_t cam = c->r[4];
  uint32_t cam0[64], sp0[256], rsave[32];
  for (int i = 0; i < 64;  i++) cam0[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) sp0[i]  = c->mem_r32(0x1f800000u + i * 4);
  memcpy(rsave, c->r, sizeof rsave);
  ov_cam_pitch(c);
  uint32_t camM[64]; for (int i = 0; i < 64; i++) camM[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 64;  i++) c->mem_w32(cam + i * 4, cam0[i]);
  for (int i = 0; i < 256; i++) c->mem_w32(0x1f800000u + i * 4, sp0[i]);
  memcpy(c->r, rsave, sizeof rsave);
  rec_interp(c, 0x8006d654u);
  uint32_t camO[64]; for (int i = 0; i < 64; i++) camO[i] = c->mem_r32(cam + i * 4);
  static int nbad = 0, ngood = 0; int bad = 0;
  for (int i = 0; i < 64; i++) if (camM[i] != camO[i]) { bad = 1;
    if (nbad++ < 60) fprintf(stderr, "[pitchverify] MISMATCH cam+0x%02x mine=%08x oracle=%08x\n", i * 4, camM[i], camO[i]); }
  if (!bad && (ngood++ % 200) == 0) fprintf(stderr, "[pitchverify] match #%d (c=%08x v=%08x)\n", ngood, camO[3], camO[6]);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_8006dcf4 — camera HEADING tracker (sub-fn of orchestrator FUN_8006e0f0). a0=cam, G=0x800e7e80,
// S=0x1f8000d0 (scratchpad cam-state). Selects a heading offset `off` (g61 fast path, else a 13-entry
// jump table @0x80016944 on G[+0x164]), forms a delta toward S[+0x06]+off-cam[+0x26]-S[+0x12], then
// either SNAPS the integer heading S[+0x06] or STEPS the heading accumulator S[+0x04]; a tail clamps
// S[+0x06] to cam[+0x4a] under the cam[+0x74] &2/&8 gates. Outputs: S[+0x04]/S[+0x06] (SCRATCHPAD),
// cam[+0x66]|=2 (main-RAM) → camverify diffs cam + scratchpad. NO trig / NO sub-calls (jump table
// internal). Transcribed line-for-line from shard_7.c gen_func_8006DCF4. NB the case-1/11 "special"
// path is off=1100 (the bne delay-slot r3=1100 SURVIVES into L_8006DE20 — it is NOT dead, and NOT a
// "no-offset" path as an earlier note guessed).
static void ov_cam_heading(Core* c) {
  uint32_t cam = c->r[4];
  const uint32_t G = 0x800e7e80u;
  const uint32_t S = 0x1f8000d0u;

  if (c->mem_r8(cam + 0x74) & 4) {                     // 8828 early exit → L_8006DED8
    c->mem_w8(cam + 0x66, c->mem_r8(cam + 0x66) | 2);
    return;
  }

  // Phase A: heading offset `off`.
  int32_t off = 0;
  uint8_t g61 = c->mem_r8(G + 0x61);
  int useTable;
  if (g61 == 0)            useTable = 1;
  else if ((g61 & 1) == 0) { off = 768; useTable = 0; }   // skip table
  else                     useTable = 1;

  if (useTable) {
    uint8_t idx = c->mem_r8(G + 0x164);
    if (idx >= 13) return;                             // 8836 → L_8006DEE8
    switch (idx) {
      case 0: case 4: {                                // 0x8006dd54
        if (c->mem_r8(G + 0x145) == 0) { off = 320; break; }
        int32_t g4a_s = (int16_t)c->mem_r16(G + 0x4a);
        uint32_t r3v  = (uint16_t)c->mem_r16(G + 0x4a);
        if (g4a_s < 0) r3v = (uint32_t)(0 - r3v);
        r3v += (c->mem_r8(G + 0x165) == 0) ? (uint32_t)-14976 : (uint32_t)-16384;
        int32_t s = (int32_t)((uint32_t)r3v << 16) >> 21;
        off = 320 - s;
        break;
      }
      case 1: case 11: {                               // 0x8006dda8
        uint32_t sub = c->mem_r32(G + 0x158);
        off = (c->mem_r8(sub + 0xc) == 4 && c->mem_r8(sub + 0x2) != 0) ? 1100 : 700;
        break;
      }
      case 9:  off = -320; break;                      // 0x8006ddd8
      case 3:  off = 700;  break;                      // 0x8006dde0 (L_8006DDE0)
      case 12: {                                       // 0x8006dde8
        uint32_t diff = (uint32_t)(uint16_t)c->mem_r16(G + 0x32)
                      - (uint32_t)(uint16_t)c->mem_r16(0x1f800202u);
        off = ((int16_t)diff < 501) ? (int32_t)diff : 500;
        break;
      }
      default: return;                                 // 2,5,6,7,8,10 → L_8006DEE8
    }
  }

  // Phase B: smoother (L_8006DE20), low-16 wrap arithmetic like the recomp.
  int32_t s06 = (uint16_t)c->mem_r16(S + 6);
  int32_t c26 = (uint16_t)c->mem_r16(cam + 0x26);
  int32_t s12 = (uint16_t)c->mem_r16(S + 0x12);
  int32_t r5  = s06 + off - c26 - s12;
  if ((uint16_t)(r5 + 10) < 21) {                      // SNAP (L_8006DE68)
    c->mem_w16(S + 6, (uint16_t)(c26 + (s12 - off)));
    c->mem_w8(cam + 0x66, c->mem_r8(cam + 0x66) | 2);
  } else {                                             // STEP: S[+0x04] -= sext16(r5)<<13
    int32_t step = (int32_t)((uint32_t)r5 << 16) >> 3;
    c->mem_w32(S + 4, (uint32_t)((int32_t)c->mem_r32(S + 4) - step));
  }

  // Tail (L_8006DE80): clamp S[+0x06] to cam[+0x4a] under the cam[+0x74] &2/&8 gates.
  uint8_t f = c->mem_r8(cam + 0x74);
  int cond = 0, active = 1;
  if (f & 2)      cond = ((int16_t)c->mem_r16(S + 6) < (int16_t)c->mem_r16(cam + 0x4a));
  else if (f & 8) cond = ((int16_t)c->mem_r16(cam + 0x4a) < (int16_t)c->mem_r16(S + 6));
  else            active = 0;
  if (active && !cond) {                               // L_8006DEC8 false-arm: clamp + cam[+0x66]|=2
    c->mem_w16(S + 6, (uint16_t)c->mem_r16(cam + 0x4a));
    c->mem_w8(cam + 0x66, c->mem_r8(cam + 0x66) | 2);
  }
}
// PSXPORT_DEBUG=camverify — per-call A/B gate (cam-struct + scratchpad diff).
static void ov_cam_heading_verify(Core* c) {
  uint32_t cam = c->r[4];
  uint32_t cam0[64], sp0[256], rsave[32];
  for (int i = 0; i < 64;  i++) cam0[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) sp0[i]  = c->mem_r32(0x1f800000u + i * 4);
  memcpy(rsave, c->r, sizeof rsave);
  ov_cam_heading(c);
  uint32_t camM[64], spM[256];
  for (int i = 0; i < 64;  i++) camM[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) spM[i]  = c->mem_r32(0x1f800000u + i * 4);
  for (int i = 0; i < 64;  i++) c->mem_w32(cam + i * 4, cam0[i]);
  for (int i = 0; i < 256; i++) c->mem_w32(0x1f800000u + i * 4, sp0[i]);
  memcpy(c->r, rsave, sizeof rsave);
  rec_interp(c, 0x8006dcf4u);
  uint32_t camO[64], spO[256];
  for (int i = 0; i < 64;  i++) camO[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) spO[i]  = c->mem_r32(0x1f800000u + i * 4);
  static int nbad = 0, ngood = 0; int bad = 0;
  for (int i = 0; i < 64; i++) if (camM[i] != camO[i]) { bad = 1;
    if (nbad++ < 60) fprintf(stderr, "[hdgverify] MISMATCH cam+0x%02x mine=%08x oracle=%08x\n", i * 4, camM[i], camO[i]); }
  for (int i = 0; i < 256; i++) if (spM[i] != spO[i]) { bad = 1;
    if (nbad++ < 60) fprintf(stderr, "[hdgverify] MISMATCH sp+0x%03x mine=%08x oracle=%08x\n", i * 4, spM[i], spO[i]); }
  if (!bad && (ngood++ % 200) == 0)
    fprintf(stderr, "[hdgverify] match #%d (s04=%08x s06=%04x)\n", ngood, spO[0x35], (uint16_t)spO[0x35]);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_8006d02c — camera ORIENT / LOOK-AT matrix builder (sub-fn of orchestrator FUN_8006e0f0). a0=cam,
// S=0x1f8000d0. Builds the camera basis matrix: deltas dX/dZ/dY = fwd-target int - pos; isqrt the 3D
// and XZ distances into cam[+0x5C]/[+0x60]; derive pitch (ratan2) + yaw (ratan2), assemble the two
// rotation matrices, compose them (MulMatrix0), extract the basis rows, and transform the negated camera
// position (ApplyMatrixLV) into the matrix translation column, then CopyMatrix to 0x1f800118.
// Per the CLAUDE.md RENDER boundary this owns the CONTROL FLOW + integer arithmetic native and CALLS the
// retained libgte/GTE helpers (isqrt/ratan2/MR_init + MulMatrix0/ApplyMatrixLV/CopyMatrix) via
// rec_dispatch — they are the math library (a TRUE float-native matrix lift is a SEPARATE later step,
// flagged here, not silently transcribed). NB isqrt here is 0x80077FB0 (NOT rotbuild's 0x80084080).
// Outputs: mostly SCRATCHPAD + cam[+0x5C]/[+0x60] → camverify diffs cam + full scratchpad.
// Transcribed from shard_6.c gen_func_8006D02C (the cpu_div sequences use the runtime div; the s18/s19!=0
// guards mean the recomp's div-by-zero/overflow rec_breaks never fire).
static const uint32_t LA_ISQRT  = 0x80077fb0u;   // isqrt (DIFFERENT from rotbuild's 0x80084080)
static const uint32_t LA_RATAN2 = 0x80085690u;
static const uint32_t LA_MRINIT = 0x80051794u;   // MR_init (identity)
static const uint32_t LA_MULMAT = 0x80084250u;   // MulMatrix0(a0,a1)   [GTE]
static const uint32_t LA_APPLYLV= 0x80084470u;   // ApplyMatrixLV(m,v,out) [GTE]
static const uint32_t LA_COPYMAT= 0x800847b0u;   // CopyMatrix(a0,a1)

static inline int32_t cam_idiv(Core* c, int32_t num, int32_t den) {
  cpu_div(c, (uint32_t)num, (uint32_t)den);
  return (int32_t)c->lo;
}

static void ov_cam_lookat(Core* c) {
  uint32_t cam = c->r[4];
  const uint32_t S = 0x1f8000d0u;                       // r21

  int32_t dX = (int16_t)c->mem_r16(S + 14) - (int16_t)c->mem_r16(S + 2);   // 0x..de - 0x..d2
  int32_t dZ = (int16_t)c->mem_r16(S + 22) - (int16_t)c->mem_r16(S + 10);  // 0x..e6 - 0x..da
  int32_t dY = (int16_t)c->mem_r16(S + 18) - (int16_t)c->mem_r16(S + 6);   // 0x..e2 - 0x..d6
  int32_t xz = dX * dX + dZ * dZ;
  int32_t s18 = cam_call(c, LA_ISQRT, xz + dY * dY, 0, 0) & 0xffff;        // 3D dist
  int32_t s19 = cam_call(c, LA_ISQRT, xz, 0, 0) & 0xffff;                  // XZ dist
  c->mem_w32(cam + 0x5c, (uint32_t)s18);
  c->mem_w32(cam + 0x60, (uint32_t)s19);

  if (s18 != 0) {
    cam_call(c, LA_MRINIT, S + 40, 0, 0);               // identity @0x1f8000f8 (pitch matrix base)
    int32_t sinp = cam_idiv(c, dY  << 12, s18);
    int32_t cosp = cam_idiv(c, s19 << 12, s18);
    int32_t pitch = (int16_t)cam_call(c, LA_RATAN2, sinp, cosp, 0);
    c->mem_w16(S + 32, (uint16_t)pitch);                // 0x1f8000f0
    c->mem_w16(S + 48, (uint16_t)cosp);                 // m[4] @0x1f800100
    c->mem_w16(S + 56, (uint16_t)cosp);                 // m[8] @0x1f800108
    c->mem_w16(S + 50, (uint16_t)(-sinp));              // m[5] @0x1f800102
    c->mem_w16(S + 54, (uint16_t)sinp);                 // m[7] @0x1f800106

    if (s19 != 0) {
      int32_t a = cam_idiv(c, (-dX) << 12, s19);        // r16 = (-dX<<12)/s19
      int32_t b = cam_idiv(c, dZ    << 12, s19);        // r17 = (dZ<<12)/s19
      int32_t yaw = (int16_t)cam_call(c, LA_RATAN2, a, b, 0);
      c->mem_w16(S + 34, (uint16_t)yaw);                // 0x1f8000f2
      cam_call(c, LA_MRINIT, 0x1f800000u, 0, 0);        // identity @0x1f800000 (yaw matrix base)
      c->mem_w16(0x1f800000u, (uint16_t)b);             // m[0]
      c->mem_w16(0x1f800004u, (uint16_t)a);             // m[2]
      c->mem_w16(0x1f800010u, (uint16_t)b);             // m[8]
      c->mem_w16(0x1f80000cu, (uint16_t)(-a));          // m[6]
      cam_call(c, LA_MULMAT, S + 40, 0x1f800000u, 0);   // MulMatrix0(pitch, yaw)
    }
  }

  // L_8006D214 tail (also reached by the s18==0 / s19==0 early exits).
  uint32_t M = S + 40;                                  // 0x1f8000f8 composed matrix
  uint16_t r104 = c->mem_r16(S + 52);                   // 0x1f800104
  uint16_t r106 = c->mem_r16(S + 54);                   // 0x1f800106
  uint16_t r108 = c->mem_r16(S + 56);                   // 0x1f800108
  c->mem_w16(S + 24, r104);                             // 0x1f8000e8
  c->mem_w16(S + 26, r106);                             // 0x1f8000ea
  c->mem_w16(S + 28, r108);                             // 0x1f8000ec
  c->mem_w16(0x1f8000c0u, (uint16_t)(0u - (uint32_t)c->mem_r16(S + 2)));    // -posX
  c->mem_w16(0x1f8000c2u, (uint16_t)(0u - (uint32_t)c->mem_r16(S + 6)));    // -posY
  c->mem_w16(0x1f8000c4u, (uint16_t)(0u - (uint32_t)c->mem_r16(S + 10)));   // -posZ
  cam_call(c, LA_APPLYLV, M, 0x1f8000c0u, 0x1f80010cu); // ApplyMatrixLV(M, -pos, out=0x1f80010c)
  cam_call(c, LA_COPYMAT, M, S + 72, 0);                // CopyMatrix(M → 0x1f800118)
}
// PSXPORT_DEBUG=camverify — per-call A/B gate (cam-struct + scratchpad diff). The libgte/GTE calls reload
// their operands from memory each invocation, so restoring scratchpad+regs before the oracle suffices.
static void ov_cam_lookat_verify(Core* c) {
  uint32_t cam = c->r[4];
  uint32_t cam0[64], sp0[256], rsave[32];
  for (int i = 0; i < 64;  i++) cam0[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) sp0[i]  = c->mem_r32(0x1f800000u + i * 4);
  memcpy(rsave, c->r, sizeof rsave);
  ov_cam_lookat(c);
  uint32_t camM[64], spM[256];
  for (int i = 0; i < 64;  i++) camM[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) spM[i]  = c->mem_r32(0x1f800000u + i * 4);
  for (int i = 0; i < 64;  i++) c->mem_w32(cam + i * 4, cam0[i]);
  for (int i = 0; i < 256; i++) c->mem_w32(0x1f800000u + i * 4, sp0[i]);
  memcpy(c->r, rsave, sizeof rsave);
  rec_interp(c, 0x8006d02cu);
  uint32_t camO[64], spO[256];
  for (int i = 0; i < 64;  i++) camO[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) spO[i]  = c->mem_r32(0x1f800000u + i * 4);
  static int nbad = 0, ngood = 0; int bad = 0;
  for (int i = 0; i < 64; i++) if (camM[i] != camO[i]) { bad = 1;
    if (nbad++ < 60) fprintf(stderr, "[lookatverify] MISMATCH cam+0x%02x mine=%08x oracle=%08x\n", i * 4, camM[i], camO[i]); }
  for (int i = 0; i < 256; i++) if (spM[i] != spO[i]) { bad = 1;
    if (nbad++ < 60) fprintf(stderr, "[lookatverify] MISMATCH sp+0x%03x mine=%08x oracle=%08x\n", i * 4, spM[i], spO[i]); }
  if (!bad && (ngood++ % 200) == 0)
    fprintf(stderr, "[lookatverify] match #%d (5c=%08x 60=%08x)\n", ngood, camO[0x17], camO[0x18]);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// Per-MODE camera ORCHESTRATORS — FUN_8006e0f0 / FUN_8006e228 / FUN_8006e3f4. A camera-mode selector
// calls one per frame; each drives the owned position/orient sub-fns in sequence plus a little glue.
// We own the sequence native, calling the owned sub-fns DIRECTLY as C (so the orchestrator verify can
// compare native-everything vs a PURE-gen oracle without the override table in the loop). The two
// still-unowned e228 sub-fns (FUN_8006dad8 / FUN_8006def0 — they consume only a0) route via rec_dispatch.
// RE: shard_3.c gen_func_8006E0F0 / gen_func_8006E3F4, shard_6.c gen_func_8006E228.

// FUN_8006e0f0 — the MAIN follow mode: dist/zoom, track XZ, pitch, track Y, Y-floor, (heading if
// cam[+0x76]==0 && G[+0x17a]==0), look-at, (angle-step if cam[+0x77]==0), then accumulate
// scratchpad[0x1f800114] -= cam[+0x28]+cam[+0x34].
static void ov_cam_orch_e0f0(Core* c) {
  uint32_t cam = c->r[4];
  const uint32_t G = 0x800e7e80u, S = 0x1f8000d0u;
  c->r[4] = cam;                  ov_cam_dist_solve(c);
  c->r[4] = cam; c->r[5] = cam+8; ov_cam_track_xz(c);
  c->r[4] = cam;                  ov_cam_pitch(c);
  c->r[4] = cam; c->r[5] = cam+8; ov_cam_track_y(c);
  c->r[4] = cam;                  ov_cam_y_floor(c);
  if (c->mem_r8(cam + 0x76) == 0 && c->mem_r8(G + 0x17a) == 0) { c->r[4] = cam; ov_cam_heading(c); }
  c->r[4] = cam;                  ov_cam_lookat(c);
  if (c->mem_r8(cam + 0x77) == 0) { c->r[4] = cam; ov_cam_angle_step(c); }
  int32_t acc = (int32_t)c->mem_r32(cam + 0x28) + (int32_t)c->mem_r32(cam + 0x34);
  c->mem_w32(S + 0x44, (uint32_t)((int32_t)c->mem_r32(S + 0x44) - acc));
}

// FUN_8006e3f4 — a SIMPLE mode (a0=cam, a1=target): track XZ then Y (settled flags → cam[+0x66]
// |1 / |2), then look-at.
static void ov_cam_orch_e3f4(Core* c) {
  uint32_t cam = c->r[4], tgt = c->r[5];
  c->r[4] = cam; c->r[5] = tgt; ov_cam_track_xz(c);
  if (c->r[2] != 0) c->mem_w8(cam + 0x66, c->mem_r8(cam + 0x66) | 1);
  c->r[4] = cam; c->r[5] = tgt; ov_cam_track_y(c);
  if (c->r[2] != 0) c->mem_w8(cam + 0x66, c->mem_r8(cam + 0x66) | 2);
  c->r[4] = cam; ov_cam_lookat(c);
}

// FUN_8006e228 — a mode (a0=cam, a1=target): track XZ then Y, latch cam[+0x0e]=scratchpad[0x1f8000e2],
// then (if cam[+0x76]==0) the two unowned sub-fns FUN_8006dad8/FUN_8006def0, then look-at.
static void ov_cam_orch_e228(Core* c) {
  uint32_t cam = c->r[4], tgt = c->r[5];
  c->r[4] = cam; c->r[5] = tgt; ov_cam_track_xz(c);
  c->r[4] = cam; c->r[5] = tgt; ov_cam_track_y(c);
  c->mem_w16(cam + 0x0e, c->mem_r16(0x1f8000e2u));
  if (c->mem_r8(cam + 0x76) == 0) {
    c->r[4] = cam; rec_dispatch(c, 0x8006dad8u);     // FUN_8006dad8 (unowned, reads a0 only)
    c->r[4] = cam; rec_dispatch(c, 0x8006def0u);     // FUN_8006def0 (unowned, reads a0 only)
  }
  c->r[4] = cam; ov_cam_lookat(c);
}

// The 9 owned sub-fns — used to register and to CLEAR around the orchestrator oracle (so rec_interp of
// the orchestrator runs the pure recomp bodies, not our overrides).
static const struct { uint32_t a; void (*f)(Core*); } CAM_SUBS[] = {
  {0x8006d960u, ov_cam_track_xz}, {0x8006da54u, ov_cam_track_y},
  {0x8006e464u, ov_cam_rotbuild}, {0x8006d2acu, ov_cam_dist_solve},
  {0x8006e010u, ov_cam_angle_step}, {0x8006c80cu, ov_cam_y_floor},
  {0x8006d654u, ov_cam_pitch}, {0x8006dcf4u, ov_cam_heading},
  {0x8006d02cu, ov_cam_lookat},
};
static void cam_subs_clear(int clear) {
  // Override system removed (2026-06-22): there is no override table to clear/restore. No-op.
  (void)clear; (void)CAM_SUBS;
}

// Generic orchestrator camverify: native-everything vs PURE-gen oracle (sub-fn overrides cleared).
static void cam_orch_verify(Core* c, void (*nat)(Core*), uint32_t addr, const char* tag) {
  uint32_t cam = c->r[4];
  uint32_t cam0[64], sp0[256], rsave[32];
  for (int i = 0; i < 64;  i++) cam0[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) sp0[i]  = c->mem_r32(0x1f800000u + i * 4);
  memcpy(rsave, c->r, sizeof rsave);
  nat(c);
  uint32_t camM[64], spM[256];
  for (int i = 0; i < 64;  i++) camM[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) spM[i]  = c->mem_r32(0x1f800000u + i * 4);
  for (int i = 0; i < 64;  i++) c->mem_w32(cam + i * 4, cam0[i]);
  for (int i = 0; i < 256; i++) c->mem_w32(0x1f800000u + i * 4, sp0[i]);
  memcpy(c->r, rsave, sizeof rsave);
  cam_subs_clear(1);                                 // pure-gen oracle
  rec_interp(c, addr);
  cam_subs_clear(0);
  uint32_t camO[64], spO[256];
  for (int i = 0; i < 64;  i++) camO[i] = c->mem_r32(cam + i * 4);
  for (int i = 0; i < 256; i++) spO[i]  = c->mem_r32(0x1f800000u + i * 4);
  static int nbad = 0, ngood = 0; int bad = 0;
  for (int i = 0; i < 64; i++) if (camM[i] != camO[i]) { bad = 1;
    if (nbad++ < 80) fprintf(stderr, "[%s] MISMATCH cam+0x%02x mine=%08x oracle=%08x\n", tag, i * 4, camM[i], camO[i]); }
  for (int i = 0; i < 256; i++) if (spM[i] != spO[i]) { bad = 1;
    if (nbad++ < 80) fprintf(stderr, "[%s] MISMATCH sp+0x%03x mine=%08x oracle=%08x\n", tag, i * 4, spM[i], spO[i]); }
  if (!bad && (ngood++ % 200) == 0) fprintf(stderr, "[%s] match #%d\n", tag, ngood);
}
static void ov_cam_orch_e0f0_verify(Core* c) { cam_orch_verify(c, ov_cam_orch_e0f0, 0x8006e0f0u, "orch_e0f0"); }
static void ov_cam_orch_e3f4_verify(Core* c) { cam_orch_verify(c, ov_cam_orch_e3f4, 0x8006e3f4u, "orch_e3f4"); }
static void ov_cam_orch_e228_verify(Core* c) { cam_orch_verify(c, ov_cam_orch_e228, 0x8006e228u, "orch_e228"); }

void engine_camera_register(void) {
  // Override system removed (2026-06-22): camera sub-fns are direct-call targets to be wired top-down.
  // Registration deleted; the ov_cam_* defs and *_verify wrappers are kept for future direct calls.
  (void)cfg_dbg;
}
