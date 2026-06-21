// Tomba!2-specific native overrides (per-game tier). Generic mechanisms live in timing.c /
// cd_override.c; this file holds glue tied to MAIN.EXE's own addresses.
//
// VBlank pacing: port the dwell to PC (don't dwell)
// ------------------------------------------------
// The StrPlayer main loop FUN_80050b08 paces each displayed frame with a busy-wait at
// 0x80050CE4:  DAT_800e809c = 0;  ... ;  do {} while (DAT_800e809c < DAT_1f800235);
// On hardware the VBlank IRQ bumps DAT_800e809c (0x800E809C, u16) until it reaches the
// per-frame quota DAT_1f800235 (scratchpad u8, =2 => the engine's 30 fps logic rate). This
// is pure frame-rate pacing. In a PC port frame pacing belongs to the host present loop, not
// a self-spinning counter, and we deliver no preemptive VBlank IRQ — so we make the loop NOT
// dwell: FUN_800788ac is the per-frame state update called exactly once per iteration (its
// only caller is the loop, right after the counter reset and before the dwell), so after its
// real body we set the display counter to the quota the dwell tests => the dwell falls
// through on its first check. This is exactly the state the real VBlank handler would have
// produced (the cb at 0x800506B4 only increments that counter), computed directly.
// (When a host present loop exists it will pace frames; this just removes the busy-wait.)
#include "core.h"
#include "game.h"   // Fps60State::current_object (was g_current_object)
#include "cfg.h"
#include "margin_render.hpp"
#include <stdlib.h>
#include <stdio.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (super-call / A/B oracle)
void fps60_init(void);            // fps60: read PSXPORT_FPS60
// geomblk capture probe (engine_submit.c): the LAST object the cull ran on. Unlike g_current_object this is
// NOT restored on return — a handler calls its cull (FUN_8007712c) then immediately submits its geometry, so
// across that submit g_render_object identifies the rendering object. Pure probe key; no gameplay effect.
uint32_t g_render_object = 0;
extern int g_fps60_on;            // fps60: capture enabled (PSXPORT_FPS60)
extern "C" void spu_audio_frame(void);        // SPU: advance the mixer one frame + feed the audio device
void rec_dispatch(Core*, uint32_t);  // hybrid call: recomp body if emitted, else interpret

#define DISPLAY_COUNTER 0x800E809Cu   // DAT_800e809c (u16) — the dwell's vblank counter
#define VBLANK_QUOTA    0x1F800235u   // DAT_1f800235 (u8)  — vblanks per displayed frame

// libsnd music-sequencer tick (RE: docs/journal.md later-53; SsSetTickMode = FUN_80090750).
// Tomba2 sequences its in-game/menu BGM with the libsnd sequencer, ticked from the VBlank IRQ
// (tick mode 5, RCnt3/vblank). The IRQ runs the tick wrapper FUN_800909c0, which chains the
// optional per-vblank user callback (DAT_800ac430) then the sequencer SsSeqCalled (DAT_800ac42c).
// The port delivers NO preemptive IRQ and collapses the pace-dwell the IRQ would fire in, so on
// hardware-faithful boot the sequencer never ticks -> zero per-note KON -> silent SPU (verified
// vs the oracle, later-53: the oracle writes KON from this very ISR while parked in the dwell).
// FIX (port the HW interrupt work to PC, per the busy-wait-porting rule — NOT simulate the IRQ):
// run the same tick wrapper natively once per vblank. The wrapper/sequencer are NOT emitted by
// the static recompiler (only reached via the IRQ callback pointer, never a direct jal), so we
// invoke them through rec_dispatch -> the hybrid interpreter (bit-identical to recomp); it runs
// FUN_800909c0 to its `jr ra` and returns. Caller-saved regs it clobbers are dead across the
// FUN_800788ac call site by MIPS convention, so this is safe to run right after the super-call.
#define SEQ_TICK_WRAPPER 0x800909C0u  // FUN_800909c0: per-vblank libsnd tick (user cb + SsSeqCalled)
#define SEQ_FUNC_PTR     0x800AC42Cu  // DAT_800ac42c: SsSeqCalled pointer (0 until SsStart inits)

static void ov_frame_update(Core* c) {
  rec_super_call(c, 0x800788ACu);                    // real per-frame state update
  // Per-VBLANK audio work. On hardware the libsnd sequencer ticks once per VBlank IRQ (60 Hz NTSC)
  // and the SPU plays in realtime. One ov_frame_update is one *logic frame*, which on hardware spans
  // DAT_1f800235 (=quota) VBlanks (=2 => Tomba2's 30 fps). So the per-vblank work — the sequencer
  // tick AND the SPU's 1/60 s field advance (spu_audio_frame) — must run `quota` times per logic
  // frame to stay at the hardware 60 Hz rate in real time. later-54 ran BOTH once (matching each
  // other but at half real-time); windowed that plays audio at HALF tempo — the user heard the
  // menu-cursor tick too slow (the headless WAV hid it: its timeline is field-count, not wall-clock,
  // so 1 tick/1 field there is still 60:60 = correct-sounding). Running both quota× fixes real-time
  // playback and keeps the WAV's tick:field ratio unchanged (just a longer, more correct duration).
  // Sequencer guard: pointer initialized + sane code address (never call through null pre-SsStart).
  // Opt out (A/B): PSXPORT_T2_NOSEQTICK. Adaptive: a true-60fps scene (quota=1) ticks once.
  int quota = c->mem_r8(VBLANK_QUOTA); if (quota < 1) quota = 1;
  uint32_t seqfn = c->mem_r32(SEQ_FUNC_PTR);
  int seq_ok = !cfg_on("PSXPORT_T2_NOSEQTICK")
               && (seqfn & 0x1FFFFFFFu) >= 0x10000u && (seqfn & 0x1FFFFFFFu) < 0x200000u;
  for (int v = 0; v < quota; v++) {                  // once per VBlank this logic frame spans
    if (seq_ok) rec_dispatch(c, SEQ_TICK_WRAPPER);   // libsnd per-vblank tick (user cb + SsSeqCalled)
    spu_audio_frame();                               // advance SPU one 1/60 s field + feed device
  }
  c->mem_w16(DISPLAY_COUNTER, c->mem_r8(VBLANK_QUOTA));    // satisfy the pacing dwell immediately
  // fps60 (when enabled) OWNS presentation: it presents the previous real frame + the interpolated
  // frame (60 fps, 1 frame behind) and paces both halves — see fps60_present. The faithful path
  // presents frame B once and paces a full frame.
  fps60_frame_commit(c);
  if (!g_fps60_on) { gpu_present(c); gpu_pace_frame(c); }
}

// fps60 object tag: the universal per-object cull/LOD dispatcher (a0 = object*, once per logic
// frame for every live drawable). Every RTP op fired in its call tree is tagged with this object's
// stable pool-pointer id (the join key). Super-call the recomp body unchanged; clear on exit.
// PSXPORT_OBJLOG=1: dump every object the cull dispatcher visits (addr + type@+0xc +
// pos@+0x2e/32/36). Empirically maps the active-object pool/list for the native entity
// manager (Phase 1) — more reliable than static-tracing the overlay handler dispatch.
int gpu_vk_wide_engine(void);   // gpu_vk.c — genuine engine-wide active (PSXPORT_WIDE_ENGINE && aspect!=4:3)
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
uint32_t eng_isqrt16(uint32_t);

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
  int32_t fx = (int16_t)c->mem_r16(0x1F8000E8u), fy = (int16_t)c->mem_r16(0x1F8000EAu), fz = (int16_t)c->mem_r16(0x1F8000ECu);
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

// Native body: commit the decision (the live path).
static void cull_native_body(Core* c) {
  uint32_t obj = c->r[4];
  CullDecision R = cull_decide(c);
  c->mem_w8(obj + 1, 0);                                  // prologue `sb zero,1(s3)`
  if (R.wrote_state2) c->mem_w32(0x1F800084u, 2);
  if (!R.kept) { c->r[2] = 0; return; }
  c->mem_w8(obj + 1, 1);
  c->r[2] = 1;
  if (R.queue) {
    int qi = R.queue - 1;
    int32_t cnt = (int16_t)c->mem_r16(CULL_QCNT[qi]);
    if (cnt < CULL_QCAP[qi]) {
      uint32_t ptr = c->mem_r32(CULL_QPTR[qi]);
      c->mem_w32(CULL_QPTR[qi], ptr - 4);
      c->mem_w32(ptr - 4, obj);
      c->mem_w16(CULL_QCNT[qi], (uint16_t)(cnt + 1));
    }
  }
}

// PSXPORT_DEBUG=cullverify — per-call gate: predict native (pure), let the recomp body do the real
// writes, then compare the observed effects (visible flag @obj+1, state word, queue count deltas +
// the pushed pointer) against the prediction. Scratchpad-blind to a plain RAM diff, so this is the
// gate. 0 mismatches over many k live calls ⇒ flip to cull_native_body.
static void cull_verify_body(Core* c) {
  uint32_t obj = c->r[4];
  uint32_t cnt_b[3], ptr_b[3];
  for (int i = 0; i < 3; i++) { cnt_b[i] = (uint16_t)c->mem_r16(CULL_QCNT[i]); ptr_b[i] = c->mem_r32(CULL_QPTR[i]); }
  CullDecision R = cull_decide(c);
  rec_super_call(c, 0x8007712Cu);                         // authoritative writes
  static long ngood = 0, nbad = 0;
  int bad = 0;
  int kept_a = c->mem_r8(obj + 1);
  if (kept_a != R.kept) bad = 1;
  if ((uint32_t)c->r[2] != (uint32_t)R.kept) bad = 1;
  if (R.wrote_state2 && c->mem_r32(0x1F800084u) != 2) bad = 1;
  // which queue actually changed (by count delta)?
  int aq = 0; uint32_t pushed = 0;
  for (int i = 0; i < 3; i++) {
    uint32_t cnt_a = (uint16_t)c->mem_r16(CULL_QCNT[i]);
    if (cnt_a != cnt_b[i]) {
      aq = i + 1;
      if (cnt_a != cnt_b[i] + 1) bad = 1;                 // exactly +1
      uint32_t ptr_a = c->mem_r32(CULL_QPTR[i]);
      if (ptr_a != ptr_b[i] - 4) bad = 1;                 // ptr advanced by -4
      pushed = c->mem_r32(ptr_b[i] - 4);
      if (pushed != obj) bad = 1;                         // obj stored at the new slot
    }
  }
  // predicted queue: pushes only when not at cap (cap-skip leaves count unchanged → aq==0)
  int pred_q = R.queue;
  if (pred_q) { int qi = pred_q - 1; if (cnt_b[qi] >= (uint32_t)CULL_QCAP[qi]) pred_q = 0; }
  if (aq != pred_q) bad = 1;
  if (bad) {
    if (nbad++ < 60)
      fprintf(stderr, "[cullverify] MISMATCH obj=%08x type=%02x kept(n=%d a=%d) q(n=%d a=%d) state2=%d\n",
              obj, c->mem_r8(obj + 0xc), R.kept, kept_a, pred_q, aq, R.wrote_state2);
  } else if (++ngood % 20000 == 0) {
    fprintf(stderr, "[cullverify] %ld matches (last obj=%08x kept=%d q=%d)\n", ngood, obj, R.kept, pred_q);
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
  int32_t dx = (int16_t)c->mem_r16(node + 0x2C) - (int16_t)c->mem_r16(0x1F8000D2u);
  int32_t dy = (int16_t)c->mem_r16(node + 0x2E) - (int16_t)c->mem_r16(0x1F8000D6u);
  int32_t dz = (int16_t)c->mem_r16(node + 0x30) - (int16_t)c->mem_r16(0x1F8000DAu);
  uint32_t sum  = (uint32_t)(dx*dx) + (uint32_t)(dy*dy) + (uint32_t)(dz*dz);   // addu-wrap, matches MIPS
  uint32_t dist = eng_isqrt16(sum) & 0xffffu;
  int32_t fx = (int16_t)c->mem_r16(0x1F8000E8u), fy = (int16_t)c->mem_r16(0x1F8000EAu), fz = (int16_t)c->mem_r16(0x1F8000ECu);
  if (dist < 512u || dist >= 7169u) return 0;
  int32_t dot = (int32_t)((uint32_t)(fx*dx) + (uint32_t)(fy*dy) + (uint32_t)(fz*dz));  // addu-wrap
  int32_t thr = (int32_t)(dist * 3424u);                                              // dist<7169 → no overflow
  if (dot < thr) return 0;
  if (commit) c->mem_w8(node + 1, 1);
  return 1;
}
// PSXPORT_DEBUG=conecull — per-call gate: native (no commit), restore, recomp body, compare v0 + node[1].
static void ov_cone_cull_2b278_verify(Core* c) {
  uint32_t rs[32]; memcpy(rs, c->r, sizeof rs);
  uint32_t node = c->r[4];
  uint8_t b1_before = c->mem_r8(node + 1);
  int mine_v0 = cone_cull_2b278(c, 0);
  uint8_t mine_b1 = (mine_v0 ? 1 : b1_before);                 // native would set node[1]=1 only on keep
  rec_super_call(c, 0x8002B278u);                              // authoritative writes
  int orc_v0 = (int)c->r[2];
  uint8_t orc_b1 = c->mem_r8(node + 1);
  static long ngood = 0, nbad = 0;
  if (mine_v0 != orc_v0 || mine_b1 != orc_b1) {
    if (nbad++ < 60) fprintf(stderr, "[conecull] MISMATCH node=%08x v0(n=%d o=%d) b1(n=%02x o=%02x)\n", node, mine_v0, orc_v0, mine_b1, orc_b1);
  } else if (++ngood % 20000 == 0) fprintf(stderr, "[conecull] %ld matches (last node=%08x v0=%d)\n", ngood, node, orc_v0);
  memcpy(c->r, rs, sizeof rs);
  c->mem_w8(node + 1, mine_b1); c->r[2] = (uint32_t)mine_v0;   // keep native result live
}
void ov_cone_cull_2b278(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("conecull") ? 1 : 0;
  if (s_v) { ov_cone_cull_2b278_verify(c); return; }
  c->r[2] = (uint32_t)cone_cull_2b278(c, 1);
}

// FUN_8009A450 — the platform PRNG (`rand`): the classic glibc LCG state*0x41C64E6D + 12345, state at
// 0x80105EE8, returns (state>>16)&0x7FFF. Called from many hot per-frame loops (particle/effect jitter,
// range-random 0x80032A44). Pure platform primitive — exact native reimplementation (mult low word =
// mflo). `randverify` (lazy REPL gate) snapshots/restores the state word and A/B's v0 + new state vs the
// recomp body.
static inline uint32_t rand_lcg(Core* c) {
  uint32_t st = c->mem_r32(0x80105EE8u) * 0x41C64E6Du + 12345u;
  c->mem_w32(0x80105EE8u, st);
  return (st >> 16) & 0x7FFFu;
}
// Trig LUTs (FUN_80083E80 sin / FUN_80083F50 cos / FUN_80083EBC sin-quadrant lookup) — pure functions
// over the angle tables in guest RAM (12-bit angle 0..4095). Hot: ~5-9k calls each/run, feeding the
// transform/anim math. Faithful native reimpl reads the SAME guest tables via mem_r16 at the SAME
// addresses the asm computes, so it's bit-exact by construction. `trigverify` (lazy gate) A/B's v0.
static inline int trig_lut(Core* c, int a0) {            // FUN_80083EBC: a0 in 0..4095
  if (a0 < 2049) {
    if (a0 < 1025) return (int16_t)c->mem_r16(0x800A5AF0u + 2u * (uint32_t)a0);
    return (int16_t)c->mem_r16(0x800A5AF0u + 2u * (uint32_t)(2048 - a0));
  }
  if (a0 < 3073) return -(int)(int16_t)c->mem_r16(0x800A4AF0u + 2u * (uint32_t)a0);
  return -(int)(int16_t)c->mem_r16(0x800A5AF0u + 2u * (uint32_t)(4096 - a0));
}
static inline int trig_sin(Core* c, int a0) {            // FUN_80083E80
  int neg = a0 < 0; int aa = (neg ? -a0 : a0) & 0xFFF;
  int r = trig_lut(c, aa); return neg ? -r : r;
}
static inline int trig_cos(Core* c, int a0) {            // FUN_80083F50
  if (a0 < 0) a0 = -a0;
  a0 &= 0xFFF;
  if (a0 < 2049) {
    if (a0 < 1025) return (int16_t)c->mem_r16(0x800A5AF0u + 2u * (uint32_t)(1024 - a0));
    return -(int)(int16_t)c->mem_r16(0x800A52F0u + 2u * (uint32_t)a0);
  }
  if (a0 < 3073) return -(int)(int16_t)c->mem_r16(0x800A5AF0u + 2u * (uint32_t)(3072 - a0));
  return (int16_t)c->mem_r16(0x800A42F0u + 2u * (uint32_t)a0);   // q3 jumps past the negate (j 0x80083fe8)
}
static void trig_verify(Core* c, uint32_t mine, uint32_t addr, const char* nm) {
  rec_super_call(c, addr);
  static long ng = 0, nb = 0;
  if ((uint32_t)c->r[2] != mine) { if (nb++ < 20) fprintf(stderr, "[trigverify] %s MISMATCH mine=%x oracle=%x\n", nm, mine, (uint32_t)c->r[2]); }
  else if (++ng % 20000 == 0) fprintf(stderr, "[trigverify] %ld matches\n", ng);
  c->r[2] = mine;
}
void ov_trig_sin(Core* c) { static int v = -1; if (v<0) v = cfg_dbg("trigverify")?1:0;
  uint32_t r = (uint32_t)trig_sin(c, (int)c->r[4]); if (v) trig_verify(c, r, 0x80083E80u, "sin"); else c->r[2] = r; }
void ov_trig_cos(Core* c) { static int v = -1; if (v<0) v = cfg_dbg("trigverify")?1:0;
  uint32_t r = (uint32_t)trig_cos(c, (int)c->r[4]); if (v) trig_verify(c, r, 0x80083F50u, "cos"); else c->r[2] = r; }
void ov_trig_lut(Core* c) { static int v = -1; if (v<0) v = cfg_dbg("trigverify")?1:0;
  uint32_t r = (uint32_t)trig_lut(c, (int)c->r[4]); if (v) trig_verify(c, r, 0x80083EBCu, "lut"); else c->r[2] = r; }

// FUN_8004D7EC — pure bitmap bit-test (~2%, 6.8k calls): byte = bitmap[(int16)(a0/8)] then return
// byte & (1 << ((int16)(a0%8) & 31)); bitmap base is 0x800BFD34 when (a1&0xff)!=0 else 0x800BFCB4.
// Pure function over a guest bitmap — exact native reimpl. `bitverify` (lazy gate) A/B's v0.
void ov_bittest_4d7ec(Core* c) {
  static int v = -1; if (v < 0) v = cfg_dbg("bitverify") ? 1 : 0;
  int a0 = (int)c->r[4]; uint32_t a1 = c->r[5];
  int q  = (a0 >= 0) ? a0 : (a0 + 7);
  int a2 = q >> 3;                        // a0/8 toward zero
  int a3 = a0 - (a2 << 3);                // a0%8
  uint32_t base = (a1 & 0xff) ? (0x800BF870u + 1220u) : (0x800BF870u + 1092u);
  uint8_t byte = c->mem_r8(base + (uint32_t)(int32_t)(int16_t)a2);
  uint32_t mine = (uint32_t)byte & (1u << ((uint32_t)(int32_t)(int16_t)a3 & 31u));
  if (v) {
    rec_super_call(c, 0x8004D7ECu);
    static long ng = 0, nb = 0;
    if ((uint32_t)c->r[2] != mine) { if (nb++ < 20) fprintf(stderr, "[bitverify] MISMATCH a0=%d a1=%x mine=%x oracle=%x\n", a0, a1, mine, (uint32_t)c->r[2]); }
    else if (++ng % 20000 == 0) fprintf(stderr, "[bitverify] %ld matches\n", ng);
  }
  c->r[2] = mine;
}

void ov_rand(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("randverify") ? 1 : 0;
  if (!s_v) { c->r[2] = rand_lcg(c); return; }
  uint32_t st0 = c->mem_r32(0x80105EE8u);
  uint32_t mine = rand_lcg(c); uint32_t st_n = c->mem_r32(0x80105EE8u);
  c->mem_w32(0x80105EE8u, st0);                      // restore
  rec_super_call(c, 0x8009A450u);
  static long ng = 0, nb = 0;
  if (c->r[2] != mine || c->mem_r32(0x80105EE8u) != st_n) {
    if (nb++ < 20) fprintf(stderr, "[randverify] MISMATCH v0 mine=%x oracle=%x state mine=%x oracle=%x\n",
                          mine, (uint32_t)c->r[2], st_n, c->mem_r32(0x80105EE8u));
  } else if (++ng % 20000 == 0) fprintf(stderr, "[randverify] %ld matches\n", ng);
  c->r[2] = mine; c->mem_w32(0x80105EE8u, st_n);     // keep native
}

// FUN_80031780 — list-tail resolver / reset. Walks the 8-byte-stride linked list rooted at
// a0[52] (off 0x34), reading the tag word at entry+4 each step, until a tag has bit30|bit31
// (0xC0000000) set. If that terminator tag has bit30 (0x40000000) set -> clear the list
// (a0[52]=a0[56]=0); else set the tail pointer a0[56] (off 0x38)=found entry. If a0[52]==0 at
// entry it is a no-op. Pure guest-pointer/integer walk, no GP0/OT. `listscan` (lazy gate) A/B's
// the two written words.
void ov_list_scan_31780(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("listscan") ? 1 : 0;
  uint32_t a0 = c->r[4];
  uint32_t o52 = c->mem_r32(a0 + 52), o56 = c->mem_r32(a0 + 56);
  uint32_t n52 = o52, n56 = o56, v0 = c->r[2];
  if (o52 != 0) {
    uint32_t v1 = o52, a1;
    for (;;) { a1 = c->mem_r32(v1 + 4); bool brk = (a1 & 0xC0000000u) != 0; v1 += 8; if (brk) break; }  // +8 is the loop's delay slot — runs even on exit
    v0 = a1 & 0x40000000u;
    if (v0) { n56 = 0; n52 = 0; } else { n56 = v1; }
  }
  if (s_v) {
    rec_super_call(c, 0x80031780u);                  // memory untouched above -> oracle writes
    uint32_t r52 = c->mem_r32(a0 + 52), r56 = c->mem_r32(a0 + 56);
    static long ng = 0, nb = 0;
    if (r52 != n52 || r56 != n56) { if (nb++ < 20) fprintf(stderr, "[listscan] MISMATCH a0=%x 52 mine=%x oracle=%x  56 mine=%x oracle=%x\n", a0, n52, r52, n56, r56); }
    else if (++ng % 5000 == 0) fprintf(stderr, "[listscan] %ld matches\n", ng);
    return;                                          // keep oracle result
  }
  c->mem_w32(a0 + 52, n52); c->mem_w32(a0 + 56, n56); c->r[2] = v0;
}

// FUN_80049968 — collision-grid ROW-POINTER setup. a0 = grid/layer index (&0xff). Reads the table
// base ptr @0x1F8001C8, indexes table[a0] (halfword offset) to a per-grid record, then writes 5
// scratchpad row pointers from the record's halfword fields:
//   0x1F8001CC = rec+0x14;  0x1F8001D0/D4/D8/DC = rec + rec[12/14/16/18]*2
// Pure pointer arithmetic over scratchpad + guest record data. `gridsetup` A/B's the 5 written words.
void ov_grid_setup_49968(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("gridsetup") ? 1 : 0;
  uint32_t a0   = c->r[4] & 0xffu;
  uint32_t base = c->mem_r32(0x1F8001C8u);
  uint32_t rec  = base + (uint32_t)c->mem_r16(base + a0 * 2) * 2;
  uint32_t cc = rec + 20;
  uint32_t d0 = rec + (uint32_t)c->mem_r16(rec + 12) * 2;
  uint32_t d4 = rec + (uint32_t)c->mem_r16(rec + 14) * 2;
  uint32_t d8 = rec + (uint32_t)c->mem_r16(rec + 16) * 2;
  uint32_t dc = rec + (uint32_t)c->mem_r16(rec + 18) * 2;
  if (s_v) {
    rec_super_call(c, 0x80049968u);
    static long ng = 0, nb = 0;
    uint32_t o_cc = c->mem_r32(0x1F8001CCu), o_d0 = c->mem_r32(0x1F8001D0u), o_d4 = c->mem_r32(0x1F8001D4u),
             o_d8 = c->mem_r32(0x1F8001D8u), o_dc = c->mem_r32(0x1F8001DCu);
    if (o_cc != cc || o_d0 != d0 || o_d4 != d4 || o_d8 != d8 || o_dc != dc) {
      if (nb++ < 20) fprintf(stderr, "[gridsetup] MISMATCH a0=%x cc=%x/%x d0=%x/%x d4=%x/%x d8=%x/%x dc=%x/%x\n",
                             a0, cc, o_cc, d0, o_d0, d4, o_d4, d8, o_d8, dc, o_dc);
    } else if (++ng % 5000 == 0) fprintf(stderr, "[gridsetup] %ld matches\n", ng);
    return;
  }
  c->mem_w32(0x1F8001CCu, cc); c->mem_w32(0x1F8001D0u, d0); c->mem_w32(0x1F8001D4u, d4);
  c->mem_w32(0x1F8001D8u, d8); c->mem_w32(0x1F8001DCu, dc);
}

// FUN_80047CBC — collision-grid CELL QUERY / neighbor-walk. Converts the probe position
// (sh[0x1BC],sh[0x1C0]) relative to grid origin (sh[0x1AA],sh[0x1AC]) into grid indices (>>6),
// bounds-checks against the row table (w[0x1CC]), looks up the cell record (w[0x1D0] + idx*8) and
// reads its tag. Then loops following the tag bits: 0x8000=keep walking, 0x4000=follow the cell's
// link/child list (inner sub-scan against u16[0x1BE]-32), else step ONE cell in +/-X (sh[0x1C0]) or
// +/-Z (sh[0x1BC]) per the low 3 tag bits, recompute the cell, repeat. Returns 0 (off-grid/blocked)
// or 1 (resolved). Writes scratchpad ONLY (0x08C idx, 0x1A8 tag, 0x1BC/0x1C0 stepped coords,
// 0x1E0/E4 cursor ptrs). t6=w[0x1D4], t7=u16[0x1BE], MASK=~63 (the -64 grid-snap mask).
static uint32_t grid_query_47cbc(Core* c) {
  const uint32_t SP = 0x1F800000u, MASK = 0xFFFFFFC0u;
  // ---- phase A: initial cell from probe vs origin ----
  int32_t t1 = ((int32_t)(int16_t)c->mem_r16(SP+0x1BC) - (int32_t)(int16_t)c->mem_r16(SP+0x1AA)) >> 6;  // grid Z idx (a3/t1)
  int32_t a3 = t1;
  uint32_t row0 = c->mem_r32(SP+0x1CC);
  uint32_t a1   = row0 + (uint32_t)(t1 << 2);                    // &row0[t1] (4-byte stride)
  int32_t t0   = ((int32_t)(int16_t)c->mem_r16(SP+0x1C0) - (int32_t)(int16_t)c->mem_r16(SP+0x1AC)) >> 6;  // grid X idx (t0)
  uint32_t A1_0 = c->mem_r16(a1+0);
  if (t0 < (int32_t)A1_0) return 0;
  uint32_t a2 = (c->mem_r16(a1+2) + (uint32_t)t0) - A1_0;
  int32_t limit = (int32_t)((uint32_t)c->mem_r16(SP+0x1AE) >> 6) - 2;
  if (a3 < limit) { if (!((a2 & 0xffff) < (uint32_t)c->mem_r16(a1+6))) return 0; }
  // ---- L_d64: latch the cell record + tag ----
  uint32_t idx = a2 & 0xffff;
  c->mem_w32(SP+0x08C, idx);
  uint32_t ptr = c->mem_r32(SP+0x1D0) + (idx << 3);
  a2 = c->mem_r16(ptr+0);
  c->mem_w32(SP+0x1E4, ptr);
  c->mem_w32(SP+0x1E0, ptr);
  c->mem_w16(SP+0x1A8, (uint16_t)a2);
  if ((a2 & 0xc000u) != 0xc000u) c->mem_w16(SP+0x1A8, 0);
  if ((a2 & 0x8000u) == 0) return 1;
  uint32_t t6 = c->mem_r32(SP+0x1D4);
  uint32_t t7 = c->mem_r16(SP+0x1BE);
  // ---- walk ----
  for (;;) {
    if (a2 & 0x4000u) {
      // ARM A: follow link / child list
      uint32_t rec = c->mem_r32(SP+0x1E0);                       // original record (a1)
      c->mem_w32(SP+0x1E0, t6 + ((uint32_t)c->mem_r16(rec+2) << 3));
      if (a2 & 0x0001u) {
        int32_t a0 = 1;
        uint32_t cnt = c->mem_r16(rec+4);
        if (1 < (int32_t)cnt) {
          int32_t a3p = (int32_t)t7 - 32;
          for (;;) {
            uint32_t cur = c->mem_r32(SP+0x1E0) + 8;
            uint32_t iv = c->mem_r16(cur+4);
            c->mem_w32(SP+0x1E0, cur);
            uint32_t iw = c->mem_r16(cur+6);
            if (((iv - (uint32_t)a3p) & 0xffff) < iw) break;
            a0 += 1;
            if (!(a0 < (int32_t)cnt)) break;
          }
        }
        uint32_t t = c->mem_r16(rec+6);
        bool hit = ((uint32_t)a0 == (t & 0xff)) || ((uint32_t)a0 == (t >> 8));
        if (!hit && (uint32_t)a0 == (uint32_t)c->mem_r16(rec+4)) hit = true;
        if (hit) c->mem_w32(SP+0x1E0, t6 + ((uint32_t)c->mem_r16(rec+2) << 3));
      }
      a2 = c->mem_r16(c->mem_r32(SP+0x1E0) + 0);
    } else {
      // ARM B: step one grid cell, recompute
      if (a2 & 0x0004u) {
        switch (a2 & 3u) {
          case 1: c->mem_w16(SP+0x1BC, (uint16_t)((c->mem_r16(SP+0x1BC) + 64) & MASK)); t1++; break;
          case 0: c->mem_w16(SP+0x1BC, (uint16_t)((c->mem_r16(SP+0x1BC) & MASK) - 1));  t1--; break;
          case 2: c->mem_w16(SP+0x1C0, (uint16_t)((c->mem_r16(SP+0x1C0) & MASK) - 1));  t0--; break;
          case 3: c->mem_w16(SP+0x1C0, (uint16_t)((c->mem_r16(SP+0x1C0) + 64) & MASK)); t0++; break;
        }
      } else if ((uint32_t)c->mem_r16(SP+0x1AE) < (uint32_t)c->mem_r16(SP+0x1B0)) {
        if (a2 & 0x0002u) { c->mem_w16(SP+0x1BC, (uint16_t)((c->mem_r16(SP+0x1BC) + 64) & MASK)); t1++; }
        else              { c->mem_w16(SP+0x1BC, (uint16_t)((c->mem_r16(SP+0x1BC) & MASK) - 1));  t1--; }
      } else {
        if (a2 & 0x0001u) { c->mem_w16(SP+0x1C0, (uint16_t)((c->mem_r16(SP+0x1C0) + 64) & MASK)); t0++; }
        else              { c->mem_w16(SP+0x1C0, (uint16_t)((c->mem_r16(SP+0x1C0) & MASK) - 1));  t0--; }
      }
      // L_f9c: recompute cell from stepped indices
      uint32_t a1b = c->mem_r32(SP+0x1CC) + (uint32_t)(((int32_t)(int16_t)t1) * 4);
      uint32_t A1b0 = c->mem_r16(a1b+0);
      if (A1b0 == 0xffff) return 0;
      if ((int32_t)(int16_t)t0 < (int32_t)A1b0) return 0;
      uint32_t a2v = (c->mem_r16(a1b+2) + (uint32_t)t0) - A1b0;
      uint32_t a0b = a2v & 0xffff;
      if (!(a0b < (uint32_t)c->mem_r16(a1b+6))) return 0;
      uint32_t ptrB = c->mem_r32(SP+0x1D0) + (a0b << 3);
      a2 = c->mem_r16(ptrB+0);
      c->mem_w32(SP+0x08C, a0b);
      c->mem_w32(SP+0x1E4, ptrB);
      c->mem_w32(SP+0x1E0, ptrB);
      c->mem_w16(SP+0x1A8, (uint16_t)a2);
    }
    if ((a2 & 0x8000u) == 0) return 1;
  }
}

void ov_grid_query_47cbc(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("gridquery") ? 1 : 0;
  if (!s_v) { c->r[2] = grid_query_47cbc(c); return; }
  const uint32_t LO = 0x1F800080u, HI = 0x1F8001F0u, N = HI - LO;
  uint8_t snap[0x170], after[0x170];
  for (uint32_t a = LO; a < HI; a++) snap[a-LO] = c->mem_r8(a);
  uint32_t mine = grid_query_47cbc(c);
  for (uint32_t a = LO; a < HI; a++) { after[a-LO] = c->mem_r8(a); c->mem_w8(a, snap[a-LO]); }  // capture+restore
  rec_super_call(c, 0x80047CBCu);
  uint32_t oracle = c->r[2];
  int firstoff = -1;
  for (uint32_t a = LO; a < HI; a++) if (c->mem_r8(a) != after[a-LO]) { firstoff = (int)(a-LO); break; }
  static long ng = 0, nb = 0;
  if (firstoff >= 0 || mine != oracle) {
    if (nb++ < 30) fprintf(stderr, "[gridquery] MISMATCH ret mine=%x oracle=%x scratchdiff@+%x\n", mine, oracle, firstoff);
  } else if (++ng % 2000 == 0) fprintf(stderr, "[gridquery] %ld matches\n", ng);
  (void)N;
  c->r[2] = oracle;                                            // keep oracle scratchpad state
}

// FUN_800498C8 — collision-grid RESOLVE LOOP (top of the grid family; pairs with the owned
// FUN_80049968 setup + FUN_80047CBC query). a0 = probe object. Iterates:
//   jal 0x8004798C(obj)                    -- per-step grid-origin/index setup (kept dispatched; non-trivial)
//   jal 0x80049968(u8 @0x1F8001FE)         -- row-pointer setup (owned ov_grid_setup_49968)
//   v0 = jal 0x80047CBC()                  -- cell query/neighbor-walk (owned ov_grid_query_47cbc)
//   if v0 == 0 -> return 0                  (query found nothing / off-grid -> done)
//   v1 = w[0x1F8001E0] (the cell record ptr the query latched)
//   if (h[v1] & 0x4000) == 0 -> return 1   (resolved cell is terminal -> done, keep)
//   obj[42] = b[v1]                         (record the resolved cell's tag byte onto the probe object)
//   reload v1' = w[0x1F8001E0]; if (h[v1'] & 0x4000) != 0 -> LOOP (descend further)
//   else -> return 1
// Pure control flow over scratchpad + object memory; ONE object write (obj+42); NO GTE, NO render
// packets. The three callees stay PSX via rec_dispatch (the two grid leaves honor their own owned
// override identically in the dispatched path). Return: 0 only when the query returns 0; otherwise 1.
static uint32_t grid_resolve_498c8(Core* c) {
  const uint32_t obj = c->r[4];
  for (;;) {
    c->r[4] = obj; rec_dispatch(c, 0x8004798Cu);                 // setup (dispatched)
    c->r[4] = (uint32_t)c->mem_r8(0x1F8001FEu); rec_dispatch(c, 0x80049968u);  // row-ptr setup (owned)
    rec_dispatch(c, 0x80047CBCu);                                // cell query (owned)
    if (c->r[2] == 0) return 0;
    uint32_t v1 = c->mem_r32(0x1F8001E0u);
    if ((c->mem_r16(v1) & 0x4000u) == 0) return 1;
    c->mem_w8(obj + 42, c->mem_r8(v1));                          // record tag byte onto the object
    uint32_t v1b = c->mem_r32(0x1F8001E0u);
    if ((c->mem_r16(v1b) & 0x4000u) != 0) continue;             // bne v0,zero,0x800498e8 -> loop
    return 1;
  }
}

void ov_grid_resolve_498c8(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("gridresolve") ? 1 : 0;
  if (!s_v) { c->r[2] = grid_resolve_498c8(c); return; }
  // Full RAM+scratchpad A/B vs rec_super_call. The native path runs first, its writes are snapshotted
  // and rolled back, then the recomp body runs and we diff. The dispatched callees (incl. the deep
  // FUN_8004798C tree) run in BOTH passes; FUN_800498C8's own 32-byte stack frame [sp-32, sp) is dead
  // below sp on return (gen saves regs there; native never touches the guest stack) -> excluded.
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  c->r[2] = grid_resolve_498c8(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x800498C8u);
  uint32_t v0_o = c->r[2];
  // Exclude pure stack scratch below entry sp: the dispatched callee tree (FUN_8004798C -> 0x80048ecc/
  // 0x80048fc4 + the grid leaves) runs in BOTH passes and leaves transient values in its OWN frames below
  // sp; because FUN_800498C8's native frame is absent, the residual bytes there differ harmlessly (same as
  // scriptvm/player). The exclusion window is the top-of-RAM stack (sp-0x800, sp) — far above ALL game
  // data (sp ~0x1FE9xx, RAM end 0x200000); a real behavioral divergence would alter persistent state below.
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[gridresolve] MISMATCH obj=%08x v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           obj, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 2000 == 0) fprintf(stderr, "[gridresolve] %ld matches\n", ng);
}

// FUN_8004798C — collision-grid PER-STEP ORIGIN/INDEX SETUP (the remaining dispatched callee inside
// the owned FUN_800498C8 resolve loop; completes the grid family with FUN_80049968 setup / FUN_80047CBC
// query / FUN_800498C8 resolve). a0 = probe object. Pure scratchpad halfword arithmetic + two dispatched
// callees; NO GTE, NO render packets. Scratchpad fields (base 0x1F800000):
//   0x1AA,0x1AC   = grid origin (X,Z)
//   0x1AE,0x1B0   = grid extents (X,Z)    [used unsigned in the select/clamp tests]
//   0x1B2,0x1B4   = grid cell base (X,Z)
//   0x1BA         = grid cell pitch       [signed; the >>14 fixed-point recompute multiplier]
//   0x1BC,0x1C0   = working probe coords (X,Z)
//   0x1FE (byte)  = current grid id
// Control flow:
//   if (obj[42] != byte[0x1FE]) jal 0x80048ecc(a0 = obj[42])    -- reload grid for this id (dispatched)
//   SELECT/RANGE TEST: if (h[0x1AE] u< h[0x1B0]) use the Z range else the X range; if probe is past the
//     selected range, jal 0x80048fc4(a0 = obj, a1 = 1)          -- re-resolve (dispatched)
//   CLAMP + RECOMPUTE: on (h[0x1AE] u< h[0x1B0]) -> Z branch (clamp 0x1C0 into [0x1AC, 0x1AC+0x1B0],
//     recompute 0x1BC) else X branch (clamp 0x1BC into [0x1AA, 0x1AA+0x1AE], recompute 0x1C0).
//   recompute writes the OTHER coord = cellbase + (((clamped - cellbase2) * pitch) >> 14) (signed mult,
//   low word). NB the >>14 is an arithmetic shift of the 32-bit low product (sra).
// `gridstep` gate = full RAM+scratchpad A/B vs rec_super_call (the two dispatched callees run in BOTH
// passes; this fn's own [sp-24, sp) stack frame + the callees' frames below sp differ harmlessly, so the
// gate excludes [sp-0x800, sp) — same family rationale as gridresolve/scriptvm).
static void grid_step_4798c(Core* c) {
  const uint32_t SP = 0x1F800000u, obj = c->r[4];
  // ---- block 1: reload grid if the object's recorded id differs ----
  uint32_t v1 = c->mem_r8(obj + 42);
  uint32_t gid = c->mem_r8(SP + 0x1FE);
  if (v1 != gid) { c->r[4] = v1; rec_dispatch(c, 0x80048eccu); }
  // ---- block 2: select range (Z if h[0x1AE] u< h[0x1B0], else X), test, maybe re-resolve ----
  uint32_t aE = c->mem_r16(SP + 0x1AE);   // h[0x1AE] (a1)
  uint32_t b0 = c->mem_r16(SP + 0x1B0);   // h[0x1B0] (a0)
  uint32_t test;
  if (aE < b0) {                          // sltu(a1,a0) != 0 -> Z range
    uint32_t d = (c->mem_r16(SP + 0x1C0) - c->mem_r16(SP + 0x1AC)) & 0xffffu;
    test = (b0 < d) ? 1u : 0u;            // sltu(a0, d)
  } else {                                // X range
    uint32_t d = (c->mem_r16(SP + 0x1BC) - c->mem_r16(SP + 0x1AA)) & 0xffffu;
    test = (aE < d) ? 1u : 0u;            // sltu(a1, d)
  }
  if (test != 0) { c->r[4] = obj; c->r[5] = 1; rec_dispatch(c, 0x80048fc4u); }
  // ---- block 3: clamp the in-range coord, then recompute the other from it ----
  uint32_t lo = c->mem_r16(SP + 0x1AE), hi = c->mem_r16(SP + 0x1B0);
  if (lo < hi) {
    // Z branch: clamp 0x1C0 into [0x1AC, 0x1AC + 0x1B0], then recompute 0x1BC
    int32_t  a2 = (int16_t)c->mem_r16(SP + 0x1C0);
    int32_t  a1 = (int16_t)c->mem_r16(SP + 0x1AC);
    uint32_t v1u = c->mem_r16(SP + 0x1AC);
    if (a2 < a1) {
      c->mem_w16(SP + 0x1C0, (uint16_t)v1u);
    } else {
      uint32_t a0u = c->mem_r16(SP + 0x1B0);
      if ((int32_t)((uint32_t)a1 + a0u) < a2) c->mem_w16(SP + 0x1C0, (uint16_t)(v1u + a0u));
    }
    int32_t  cv  = (int16_t)c->mem_r16(SP + 0x1C0);
    uint32_t cb  = c->mem_r16(SP + 0x1B4);
    int32_t  pit = (int16_t)c->mem_r16(SP + 0x1BA);
    int32_t  prod = (int32_t)((uint32_t)((uint32_t)cv - cb) * (uint32_t)pit);  // lo(mult)
    int32_t  v = prod >> 14;
    c->mem_w16(SP + 0x1BC, (uint16_t)(c->mem_r16(SP + 0x1B2) + (uint32_t)v));
  } else {
    // X branch: clamp 0x1BC into [0x1AA, 0x1AA + 0x1AE], then recompute 0x1C0
    int32_t  a2 = (int16_t)c->mem_r16(SP + 0x1BC);
    int32_t  a1 = (int16_t)c->mem_r16(SP + 0x1AA);
    uint32_t v1u = c->mem_r16(SP + 0x1AA);
    if (a2 < a1) {
      c->mem_w16(SP + 0x1BC, (uint16_t)v1u);
    } else {
      uint32_t a0u = c->mem_r16(SP + 0x1AE);
      if ((int32_t)((uint32_t)a1 + a0u) < a2) c->mem_w16(SP + 0x1BC, (uint16_t)(v1u + a0u));
    }
    int32_t  cv  = (int16_t)c->mem_r16(SP + 0x1BC);
    uint32_t cb  = c->mem_r16(SP + 0x1B2);
    int32_t  pit = (int16_t)c->mem_r16(SP + 0x1BA);
    int32_t  prod = (int32_t)((uint32_t)((uint32_t)cv - cb) * (uint32_t)pit);  // lo(mult)
    int32_t  v = prod >> 14;
    c->mem_w16(SP + 0x1C0, (uint16_t)(c->mem_r16(SP + 0x1B4) + (uint32_t)v));
  }
}

void ov_grid_step_4798c(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("gridstep") ? 1 : 0;
  if (!s_v) { grid_step_4798c(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  grid_step_4798c(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x8004798Cu);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[gridstep] MISMATCH obj=%08x ram@%x spad@%x sp=%x\n", obj, ro, so, sp);
  } else if (++ng % 2000 == 0) fprintf(stderr, "[gridstep] %ld matches\n", ng);
}

// FUN_80040410 — per-object CHILD-NODE SPAWN / sub-object builder (a callee of the per-object state
// machine FUN_80040558's state-0 handler). a0 = obj, a1 = group index (low byte). NO GTE, NO render
// packets; pure control flow + object/child-node memory writes, with 2 dispatched callees.
//   obj[8] = 2 (child count, set unconditionally on entry).
//   if ((int16)*0x800ed098 < 2): obj[4] = 3; return 0  (global gate — not ready yet).
//   else:
//     obj[9]=2, obj[13]=0, obj[11]=0, sh obj[84]=obj[86]=obj[88]=0.
//     count = obj[8] (the 2 just written); s0 (per-child base) = obj; for i in [0,count):
//       node = jal 0x8007aae8()          (child-node allocator, dispatched -> v0 = node ptr)
//       s0[0xC0] = node                  (store the child ptr at obj+0xC0 + 4*i)
//       node[6] = (i - 1) as s16         (0xFFFF on the first child)
//       node[0] = u16 tblA[6*i + 0],  node[2] = u16 tblA[6*i + 2],  node[4] = u16 tblA[6*i + 4]
//                                        (tblA = 0x800a3b1c, stride 6)
//       node[8] = node[0xA] = node[0xC] = 0
//       a2 = lh tblB[2*((a1&0xff) + i)]  (tblB = 0x800a3b28, stride 2, base index = a1&0xff)
//       jal 0x80051b04(a0=node, a1=1, a2)  (transform/geom setup -> writes node[0x40], dispatched)
//     return 1.
// CONTROL FLOW + every memory write owned native; the allocator 0x8007aae8 and the setup 0x80051b04 stay
// PSX via rec_dispatch (each honors its own override identically in the super-call path). GOTCHA: the
// child count read (v1=obj[8]) is the value just stored (2) — re-read from memory; the loop counter
// increments BEFORE the tblA stores complete but AFTER node[6]=s2-1 is stored (delay-slot ordering),
// so node[6] uses the PRE-increment index. `child40410` gate = full RAM+scratchpad A/B vs rec_super_call.
static uint32_t child_spawn_40410(Core* c) {
  const uint32_t obj = c->r[4];
  const uint32_t a1  = c->r[5] & 0xffu;
  c->mem_w8(obj + 8, 2);
  if ((int16_t)c->mem_r16(0x800ed098u) < 2) { c->mem_w8(obj + 4, 3); return 0; }
  c->mem_w8(obj + 9, 2);
  c->mem_w8(obj + 13, 0);
  c->mem_w8(obj + 11, 0);
  c->mem_w16(obj + 84, 0);
  c->mem_w16(obj + 86, 0);
  c->mem_w16(obj + 88, 0);
  uint32_t count = c->mem_r8(obj + 8);
  uint32_t s0 = obj;                 // per-child base for obj[0xC0 + 4*i]
  uint32_t s1 = 0x800a3b1cu;         // tblA cursor (stride 6)
  uint32_t s3 = a1 << 2;             // tblB byte offset = (a1&0xff)*4, +2 per iter
  const uint32_t s5 = 0x800a3b28u;   // tblB base
  for (uint32_t i = 0; i < count; i++) {
    c->r[4] = 0; rec_dispatch(c, 0x8007aae8u);     // allocate child node
    uint32_t node = c->r[2];
    c->mem_w32(s0 + 0xC0, node);
    c->mem_w16(node + 6, (uint16_t)(i - 1));        // node[6] = (i-1) as s16
    c->mem_w16(node + 0, c->mem_r16(s1 + 0));
    c->mem_w16(node + 2, c->mem_r16(s1 + 2));
    c->mem_w16(node + 4, c->mem_r16(s1 + 4));
    c->mem_w16(node + 8, 0);
    c->mem_w16(node + 0xA, 0);
    c->mem_w16(node + 0xC, 0);
    uint32_t a2 = (uint32_t)(int32_t)(int16_t)c->mem_r16(s5 + s3);
    c->r[4] = node; c->r[5] = 1; c->r[6] = a2;
    rec_dispatch(c, 0x80051b04u);                   // transform/geom setup
    s1 += 6; s3 += 2; s0 += 4;
  }
  return 1;
}

void ov_child_spawn_40410(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("child40410") ? 1 : 0;
  if (!s_v) { c->r[2] = child_spawn_40410(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  c->r[2] = child_spawn_40410(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80040410u);
  uint32_t v0_o = c->r[2];
  // Same family rationale as the grid/scriptvm gates: the dispatched callees (0x8007aae8 allocator,
  // 0x80051b04 setup) run in BOTH passes and leave transient residue in their own stack frames below
  // entry sp; FUN_80040410's own 48-byte frame is also dead below sp on return. Exclude [sp-0x800, sp)
  // (sp ~0x1FE9xx, RAM end 0x200000 — far above ALL game data; a real divergence alters persistent state).
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[child40410] MISMATCH obj=%08x v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           obj, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 10 == 0) fprintf(stderr, "[child40410] %ld matches\n", ng);
}

// FUN_80040558 — per-object STATE-MACHINE HEAD (the dispatcher whose state-0 handler calls the just-owned
// child-spawn FUN_80040410; owning the head advances the whole behavior family). a0 = obj. Pure control
// flow + object byte/halfword writes + global/scratchpad reads; NO GTE, NO render packets. Every `jal` is
// a sub-behavior kept PSX via rec_dispatch (each honors its own override identically in the super-call
// path). Dispatch on the state byte obj[4]: 0 / 1 / 2 / 3 / else.
//   STATE 3 (@a40): jal 0x8007a624(obj); return.
//   STATE 0 (@5ac): sub-dispatch on obj[5]:
//     obj[5]==0 (@5cc): v0 = jal 0x80040410(obj, a1=obj[3]); if v0!=0 -> obj[5]++; then ALWAYS:
//        sh obj[128]=64, sh obj[130]=128, sb obj[41]=0, sb obj[43]=0, sb obj[95]=0,
//        sh obj[132]=150, sh obj[134]=150, sb obj[70]=0; return.  (the +64 in obj[128] comes from the
//        delay-slot constant whether or not obj[5] was bumped).
//     obj[5]==1 (@620): v1=obj[94]; if v1<8 -> jump table 0x800152e0[v1], else v0=1 (@6c0).
//        jt[0]@650 jal 0x8003fbc4; v0=1.   jt[1]@660 jal 0x8003fc00; v0=1.
//        jt[2]@670 jal 0x801286f4; v0=1.   jt[3]@6c0 v0=1.   jt[4]@680 jal 0x8003fc78; v0=1.
//        jt[5]@690 jal 0x80120188; v0=1.   jt[6]@6a0 jal 0x8003fc8c; v0=ret. jt[7]@6b0 jal 0x801146e8; v0=ret.
//        @6b8: if v0==0 return; @6c4: sb obj[4]=v0, sb obj[5]=0, sb obj[0]=v0, sb obj[41]=0; return.
//     else: return.
//   STATE 1 (@6d8): g870=*0x800bf870; if g870==18 { if *0x800bfa59==0 return; } else if g870==19 { if
//      *0x800bf871==19 return; }. Then @720: v1=obj[5]; if v1<6 -> jt 0x80015300[v1] (jal one of
//      0x8003fd10/fed8/ffcc/4022c/40390/0x80114934), else fall to @7a8.
//      @7a8: v1=obj[94]; if v1<8 -> jt 0x80015318[v1], else @8c8. jt: [0,1,3,4,6]->@7e0, [2]->@888,
//      [5]->@8c0, [7]->@7d8 (=@7e0 prefixed by jal 0x8012b118).
//        @7e0: if *0x800bf816!=0 && *0x800bf817==obj[106](s16) && (obj[40]&0x80) { sb obj[1]=1;
//              jal 0x80077e7c(obj); goto @878 } else goto @834.
//        @834: if (obj[40]&0x80) goto @8c8; else if *0x800bf870==8 v0=jal 0x8012e168(obj) else
//              v0=jal 0x8007778c(obj); if v0==0 goto @8c8; @878 jal 0x800517f8(obj); sb obj[41]=0; return.
//        @888: v0=*(*obj[16] + 1)(u8); sb obj[1]=v0; if (v0&0xff)==0 goto @8c8; else jal 0x8012866c(obj),
//              jal 0x80077e7c(obj); sb obj[41]=0; return.
//        @8c0: jal 0x801201e0(obj); (fall to @8c8).   @8c8: sb obj[41]=0; return.
//   STATE 2 (@8d4): v1=obj[5]; if v1<5 -> jt 0x80015338[v1], else @964.
//      jt[0]/jt[4]=@964, jt[1]=@904, jt[2]=@94c (jal 0x8003fe00), jt[3]=@95c (jal 0x8003fed8 -> @964).
//        @904: if obj[3]==0 && *0x800bfad1==0 -> jal 0x80040b48(a0=56); @92c: if obj[94]==2 { v1b=*obj[16];
//              sb *(v1b+94)=1; } goto @964.
//        @964: if obj[94]==2 { v0=*(*obj[16]+1)(u8); sb obj[1]=v0; jal 0x8012866c(obj); jal 0x80077e7c(obj);
//              return } else @99c (mirrors state-1's @7e0..tail with the same global checks +
//              jal 0x8012e168/0x8007778c + jal 0x800517f8, sb obj[1]=1 path).
// `sm40558` gate = full RAM+scratchpad A/B vs rec_super_call (same family rationale as child40410: every
// dispatched callee runs in BOTH passes leaving residue below entry sp, and this fn's own 24-byte frame is
// dead there on return -> exclude [sp-0x800, sp), far above all game data).
static void sm40558(Core* c) {
  const uint32_t obj = c->r[4];
  const uint32_t G = 0x800BF870u;                 // global block base (0x800bf870 = mode byte)
  uint32_t st = c->mem_r8(obj + 4);

  if (st == 3) { c->r[4] = obj; rec_dispatch(c, 0x8007A624u); return; }

  if (st == 0) {
    uint32_t s5 = c->mem_r8(obj + 5);
    if (s5 == 0) {
      c->r[4] = obj; c->r[5] = c->mem_r8(obj + 3);
      rec_dispatch(c, 0x80040410u);
      if (c->r[2] != 0) c->mem_w8(obj + 5, (uint8_t)(c->mem_r8(obj + 5) + 1));
      c->mem_w16(obj + 128, 64);
      c->mem_w16(obj + 130, 128);
      c->mem_w8 (obj + 41, 0);
      c->mem_w8 (obj + 43, 0);
      c->mem_w8 (obj + 95, 0);
      c->mem_w16(obj + 132, 150);
      c->mem_w16(obj + 134, 150);
      c->mem_w8 (obj + 70, 0);
      return;
    }
    if (s5 == 1) {
      uint32_t v94 = c->mem_r8(obj + 94);
      uint32_t v0;
      switch (v94) {
        case 0: c->r[4]=obj; rec_dispatch(c, 0x8003FBC4u); v0=1; break;
        case 1: c->r[4]=obj; rec_dispatch(c, 0x8003FC00u); v0=1; break;
        case 2: c->r[4]=obj; rec_dispatch(c, 0x801286F4u); v0=1; break;
        case 3: v0=1; break;
        case 4: c->r[4]=obj; rec_dispatch(c, 0x8003FC78u); v0=1; break;
        case 5: c->r[4]=obj; rec_dispatch(c, 0x80120188u); v0=1; break;
        case 6: c->r[4]=obj; rec_dispatch(c, 0x8003FC8Cu); v0=c->r[2]; break;
        case 7: c->r[4]=obj; rec_dispatch(c, 0x801146E8u); v0=c->r[2]; break;
        default: v0=1; break;                    // v94>=8 -> @6c0 (v0=1)
      }
      if (v94 == 6 || v94 == 7) { if (v0 == 0) return; }   // @6b8 (only the ret-valued blocks gate here)
      c->mem_w8(obj + 4, (uint8_t)v0);
      c->mem_w8(obj + 5, 0);
      c->mem_w8(obj + 0, (uint8_t)v0);
      c->mem_w8(obj + 41, 0);
      return;
    }
    return;                                       // obj[5] other -> @a48
  }

  if (st == 1) {
    uint32_t g870 = c->mem_r8(G + 0);
    if (g870 == 18) { if (c->mem_r8(0x800BFA59u) == 0) return; }
    else if (g870 == 19) { if (c->mem_r8(G + 1) == 19) return; }
    // @720: obj[5] sub-dispatch (jt 0x80015300, 6 entries)
    uint32_t s5 = c->mem_r8(obj + 5);
    if (s5 < 6) {
      static const uint32_t JT1[6] = { 0x8003FD10u, 0x8003FED8u, 0x8003FFCCu, 0x8004022Cu, 0x80040390u, 0x80114934u };
      c->r[4] = obj; rec_dispatch(c, JT1[s5]);
    }
    // @7a8: obj[94] sub-dispatch (jt 0x80015318, 8 entries)
    uint32_t v94 = c->mem_r8(obj + 94);
    int go888 = 0, go8c0 = 0;                      // selected tail block
    if (v94 < 8) {
      if (v94 == 7) { c->r[4] = obj; rec_dispatch(c, 0x8012B118u); }   // @7d8 prefix
      if (v94 == 2) go888 = 1;
      else if (v94 == 5) go8c0 = 1;
      // else (0,1,3,4,6,7) -> the @7e0 common block
    } else {
      // v94 >= 8 -> @8c8
      c->mem_w8(obj + 41, 0);
      return;
    }
    if (go888) {
      // @888
      uint32_t p = c->mem_r32(obj + 16);
      uint32_t v0 = c->mem_r8(p + 1);
      c->mem_w8(obj + 1, (uint8_t)v0);
      if ((v0 & 0xff) == 0) { c->mem_w8(obj + 41, 0); return; }        // @8c8
      c->r[4]=obj; rec_dispatch(c, 0x8012866Cu);
      c->r[4]=obj; rec_dispatch(c, 0x80077E7Cu);
      c->mem_w8(obj + 41, 0);
      return;
    }
    if (go8c0) {
      // @8c0
      c->r[4]=obj; rec_dispatch(c, 0x801201E0u);
      c->mem_w8(obj + 41, 0);                       // fall to @8c8
      return;
    }
    // @7e0 common block
    if (c->mem_r8(0x800BF816u) != 0
        && c->mem_r8(0x800BF817u) == (uint32_t)(uint16_t)(int16_t)c->mem_r16(obj + 106)) {
      if (c->mem_r8(obj + 40) & 0x80) {
        c->mem_w8(obj + 1, 1);
        c->r[4]=obj; rec_dispatch(c, 0x80077E7Cu);
        // @878
        c->r[4]=obj; rec_dispatch(c, 0x800517F8u);
        c->mem_w8(obj + 41, 0);
        return;
      }
      // (obj[40]&0x80)==0 -> @8c8
      c->mem_w8(obj + 41, 0);
      return;
    }
    // @834 (g816==0, or obj[817]!=obj[106])
    if (c->mem_r8(obj + 40) & 0x80) { c->mem_w8(obj + 41, 0); return; }   // @8c8
    {
      uint32_t v0;
      if (c->mem_r8(G + 0) == 8) { c->r[4]=obj; rec_dispatch(c, 0x8012E168u); v0=c->r[2]; }
      else                        { c->r[4]=obj; rec_dispatch(c, 0x8007778Cu); v0=c->r[2]; }
      if (v0 == 0) { c->mem_w8(obj + 41, 0); return; }                    // @8c8
      // @878
      c->r[4]=obj; rec_dispatch(c, 0x800517F8u);
      c->mem_w8(obj + 41, 0);
      return;
    }
  }

  if (st == 2) {
    uint32_t s5 = c->mem_r8(obj + 5);
    if (s5 < 5) {
      // jt 0x80015338: [0]/[4]=@964, [1]=@904, [2]=@94c, [3]=@95c
      if (s5 == 1) {
        // @904
        if (c->mem_r8(obj + 3) == 0 && c->mem_r8(0x800BFAD1u) == 0) { c->r[4] = 56; rec_dispatch(c, 0x80040B48u); }
        // @92c
        if (c->mem_r8(obj + 94) == 2) {
          uint32_t v1b = c->mem_r32(obj + 16);
          c->mem_w8(v1b + 94, 1);
        }
        // fall to @964
      } else if (s5 == 2) {
        c->r[4]=obj; rec_dispatch(c, 0x8003FE00u);    // @94c -> @964
      } else if (s5 == 3) {
        c->r[4]=obj; rec_dispatch(c, 0x8003FED8u);    // @95c -> @964
      }
      // s5==0 or 4 -> @964 directly
    }
    // @964
    if (c->mem_r8(obj + 94) == 2) {
      uint32_t p = c->mem_r32(obj + 16);
      uint32_t v0 = c->mem_r8(p + 1);
      c->mem_w8(obj + 1, (uint8_t)v0);
      c->r[4]=obj; rec_dispatch(c, 0x8012866Cu);
      c->r[4]=obj; rec_dispatch(c, 0x80077E7Cu);
      return;
    }
    // @99c: mirror of state-1 @7e0..tail (global checks + cull/transform), with obj fields
    if (c->mem_r8(0x800BF816u) != 0
        && c->mem_r8(0x800BF817u) == (uint32_t)(uint16_t)(int16_t)c->mem_r16(obj + 106)) {
      if (c->mem_r8(obj + 40) & 0x80) {
        c->mem_w8(obj + 1, 1);
        c->r[4]=obj; rec_dispatch(c, 0x80077E7Cu);
        // @a30
        c->r[4]=obj; rec_dispatch(c, 0x800517F8u);
        return;
      }
      return;                                         // (obj[40]&0x80)==0 -> @a48
    }
    // @9ec
    if (c->mem_r8(obj + 40) & 0x80) return;           // @a48
    {
      uint32_t v0;
      if (c->mem_r8(G + 0) == 8) { c->r[4]=obj; rec_dispatch(c, 0x8012E168u); v0=c->r[2]; }
      else                        { c->r[4]=obj; rec_dispatch(c, 0x8007778Cu); v0=c->r[2]; }
      if (v0 == 0) return;                            // @a48
      // @a30
      c->r[4]=obj; rec_dispatch(c, 0x800517F8u);
      return;
    }
  }

  // st other (>3) -> @a48
}

void ov_sm40558(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("sm40558") ? 1 : 0;
  if (!s_v) { sm40558(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  sm40558(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80040558u);
  // Same family rationale as child40410/gridresolve: every dispatched callee runs in BOTH passes and leaves
  // transient residue in its own stack frame below entry sp; FUN_80040558's own 24-byte frame is also dead
  // below sp on return. Exclude [sp-0x800, sp) (sp ~0x1FE9xx, RAM end 0x200000 — far above all game data;
  // a real divergence alters persistent state below). This fn returns void (no v0 to compare).
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[sm40558] MISMATCH obj=%08x st=%d s5=%d v94=%d ram@%x spad@%x sp=%x\n",
                           obj, c->mem_r8(obj+4), c->mem_r8(obj+5), c->mem_r8(obj+94), ro, so, sp);
  } else if (++ng % 200 == 0) fprintf(stderr, "[sm40558] %ld matches\n", ng);
}

// FUN_8003FD10 — per-object OSCILLATE / FRAME-TOGGLE sub-behavior (one of sm40558 STATE-1's obj[5] jump-table
// handlers JT1[0], reached ~thousands×/run from the hot active-behavior path). a0 = obj. NO GTE, NO render
// packets — pure object/scratchpad memory ops + ONE dispatched callee (0x8009A450 = ov_rand, owned). A
// 3-way micro state-machine on the phase byte obj[6]:
//   obj[6]==0 (@fd40): if obj[43]==0 return; else obj[6]=1, obj[43]=0, obj[64](sh)=16, return.
//   obj[6]==1 (@fd64): if obj[43]!=0 { obj[43]=0; obj[64](sh)=16; }  @fd7c: v0=obj[64](lhu); v0--; obj[64]=v0;
//     if (int16)v0 == -1 obj[6] += -1 (i.e. obj[6]--);  @fdb0: r = ((u16*)0x1F80017C & 1); node=*(obj+0xC0);
//     node[2](sh) = r*6; rr = ov_rand(); node[0](sh) = ((rr&3)-2)*6; return.
//   obj[6] other (@fdf0): return.
// GOTCHAs: (1) the `sh v1,2(node)` at 0x8003fdd0 is in the ov_rand jal DELAY SLOT — node and v1 (=r*6) are
//   computed BEFORE the call, the store happens with the pre-call values (node loaded @0x8003fdc4). (2) the
//   obj[6]-- at @fdac uses v1=-1 added to obj[6] (only on the v0==-1 branch). (3) node[2]/[0] are halfword
//   stores of v0*6 == (v0*3)<<1. `fd10` gate = full RAM+scratchpad A/B vs rec_super_call (same family
//   rationale as sm40558: the dispatched ov_rand runs in BOTH passes + this fn's 24-byte frame is dead below
//   entry sp on return -> exclude [sp-0x800, sp)).
static void osc_fd10(Core* c) {
  const uint32_t obj = c->r[4];
  uint8_t p6 = c->mem_r8(obj + 6);
  if (p6 == 0) {                                  // @fd40
    if (c->mem_r8(obj + 43) == 0) return;         // @fdf0
    c->mem_w8 (obj + 6, 1);
    c->mem_w16(obj + 64, 16);
    c->mem_w8 (obj + 43, 0);
    return;
  }
  if (p6 != 1) return;                            // @fdf0
  // @fd64
  if (c->mem_r8(obj + 43) != 0) {
    c->mem_w8 (obj + 43, 0);
    c->mem_w16(obj + 64, 16);
  }
  // @fd7c
  uint16_t cnt = c->mem_r16(obj + 64);
  cnt = (uint16_t)(cnt - 1);
  c->mem_w16(obj + 64, cnt);
  if ((int16_t)cnt == -1) {                       // @fda4 (obj[6] += -1)
    c->mem_w8(obj + 6, (uint8_t)(c->mem_r8(obj + 6) - 1));
  }
  // @fdb0
  uint32_t r = c->mem_r16(0x1F80017Cu) & 1u;      // scratchpad halfword & 1
  uint32_t node = c->mem_r32(obj + 0xC0);
  c->mem_w16(node + 2, (uint16_t)(r * 6u));       // sh in the ov_rand delay slot (pre-call node/value)
  c->r[4] = obj; rec_dispatch(c, 0x8009A450u);    // ov_rand
  uint32_t rr = c->r[2] & 3u;
  uint32_t v0 = (uint32_t)((int32_t)rr - 2);
  c->mem_w16(node + 0, (uint16_t)(v0 * 6u));
}

void ov_osc_fd10(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("fd10") ? 1 : 0;
  if (!s_v) { osc_fd10(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  osc_fd10(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x8003FD10u);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[fd10] MISMATCH obj=%08x p6=%d ram@%x spad@%x sp=%x\n",
                           obj, c->mem_r8(obj+6), ro, so, sp);
  } else if (++ng % 200 == 0) fprintf(stderr, "[fd10] %ld matches\n", ng);
}

// FUN_8004CE14 — per-object SCRIPT-VM tick (the MOST-CALLED field function, ~14900 calls/run). a0 = obj.
// Dispatches on the state byte obj[4]: 2 -> no-op; 3 -> jal 0x8007A624(obj); >3 -> no-op; 0 -> if the
// global enable byte 0x800BF873!=0 set obj[4]=3 & return, else INIT (obj[4]=1, obj[0]=1, load the per-obj
// behavior fn ptr from table 0x800A3F00[obj[3]] into obj[108], obj[116]=0, jal 0x8004B354(obj,0)) then
// fall into state 1. State 1 is the VM: a pause/mode gate (global 0x800BF870/871 + scratchpad 0x1F800207
// + the per-obj run-condition obj[3]) decides whether to run the command loop. The loop walks the
// 16-byte-stride command stream at cursor obj[108] (s4): opcode lbu[s4]==0xFF terminates; else a flag
// byte s4[2] bit7 picks predicate 0x8004D7EC (clear) vs 0x8004D868 (set), gated by the per-slot mask
// obj[116] & (1<<idx); a passing entry executes either 0x80111CCC(s4[12]) (when 0x800BF870==1 &&
// 0x800BF871>=15) or the cull/anim call 0x80077ACC(obj, s4[4], s4[6], s4[8]); a nonzero return ORs the
// slot bit into obj[112]. On terminator: obj[106]=slot count, obj[11]=31, obj[1]=1, jal 0x80077EFC(obj).
// CONTROL FLOW + memory ops are owned native; every jal sub-behavior stays interpreted via rec_dispatch
// (each honors its own override identically in the super-call path). `scriptvm` gate = full RAM+scratchpad
// A/B vs rec_super_call (each path runs once from one checkpoint; the native run is rolled back).
static void script_vm_4ce14(Core* c) {
  const uint32_t obj = c->r[4];
  const uint32_t s5  = obj + 96;
  uint8_t state = c->mem_r8(obj + 4);
  if (state == 2) { c->r[2] = 2; return; }
  if (state == 3) { c->r[4] = obj; rec_dispatch(c, 0x8007A624u); return; }   // v0 = sub return
  if (state > 3)  { c->r[2] = 3; return; }
  if (state == 0) {
    if (c->mem_r8(0x800BF873u) != 0) { c->mem_w8(obj + 4, 3); c->r[2] = 3; return; }  // global not enabled yet
    uint32_t fnptr = c->mem_r32(0x800A3F00u + (uint32_t)c->mem_r8(obj + 3) * 4);
    c->mem_w8(obj + 4, 1);
    c->mem_w8(obj + 0, 1);
    c->mem_w32(s5 + 20, 0);          // obj[116] = 0 (slot mask)
    c->mem_w32(s5 + 12, fnptr);      // obj[108] = behavior fn ptr (cursor base)
    c->r[4] = obj; c->r[5] = 0; rec_dispatch(c, 0x8004B354u);
    // fall through into state 1
  }
  // ---- STATE 1: pause/mode gate, then the command loop ----
  uint8_t G0 = c->mem_r8(0x800BF870u);
  if (G0 == 0) {
    uint8_t o3 = c->mem_r8(obj + 3);
    if (o3 == 1) {
      if (c->mem_r8(0x1F800207u) >= 28) { c->r[2] = 0; return; }      // run only when scratch<28
    } else if (o3 == 2) {
      if (c->mem_r8(0x1F800207u) < 28)  { c->r[2] = 1; return; }      // run only when scratch>=28
    }
    // o3 not 1/2 -> always run
  } else {
    if (G0 == 6 && c->mem_r8(0x800BF871u) == 19) { c->r[2] = 19; return; }
    // else run
  }
  // ---- command loop ----
  uint32_t s4 = c->mem_r32(s5 + 12);   // cursor
  c->mem_w32(s5 + 16, 0);              // obj[112] = 0 (result accumulator)
  uint32_t s3 = 0;                     // slot index
  uint32_t v0_ret = 0;                 // v0 at function exit (from the terminator sub-call)
  for (;;) {
    if (c->mem_r8(s4) == 0xFF) {       // terminator opcode
      c->mem_w16(s5 + 10, (uint16_t)s3);   // obj[106] = slot count
      c->mem_w8(obj + 11, 31);
      c->mem_w8(obj + 1, 1);
      c->r[4] = obj; rec_dispatch(c, 0x80077EFCu);
      v0_ret = c->r[2];
      break;
    }
    uint32_t mask = 1u << (s3 & 31);
    uint8_t  flag = c->mem_r8(s4 + 2);
    bool s2set = (flag & 0x80) != 0;
    bool skip = false;
    if (!s2set) {                                        // bit7 clear -> predicate 0x8004D7EC
      c->r[4] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s4 + 10); c->r[5] = 0;
      rec_dispatch(c, 0x8004D7ECu);
      if (c->r[2] != 0) skip = true;
    }
    if (!skip && (c->mem_r32(s5 + 20) & mask)) skip = true;   // slot already done
    if (!skip && s2set) {                                // bit7 set -> predicate 0x8004D868
      c->r[4] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s4 + 10); c->r[5] = 0;
      rec_dispatch(c, 0x8004D868u);
      if (c->r[2] != 0) skip = true;
    }
    if (!skip && (c->mem_r32(s5 + 20) & mask)) skip = true;   // re-check (predicate may have set it)
    if (!skip) {
      uint32_t ret;
      if (c->mem_r8(0x800BF870u) == 1 && c->mem_r8(0x800BF871u) >= 15) {
        c->r[4] = (uint32_t)c->mem_r8(s4 + 12);
        rec_dispatch(c, 0x80111CCCu);
        ret = c->r[2];
      } else {
        c->r[4] = obj;
        c->r[5] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s4 + 4);
        c->r[6] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s4 + 6);
        c->r[7] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s4 + 8);
        rec_dispatch(c, 0x80077ACCu);
        ret = c->r[2];
      }
      if (ret != 0) c->mem_w32(s5 + 16, c->mem_r32(s5 + 16) | mask);
    }
    s3++; s4 += 16;
  }
  c->r[2] = v0_ret;
}

void ov_script_vm_4ce14(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("scriptvm") ? 1 : 0;
  if (!s_v) { script_vm_4ce14(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  script_vm_4ce14(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x8004CE14u);
  uint32_t v0_o = c->r[2];
  // Ignore FUN_8004CE14's OWN 56-byte stack frame [sp-56, sp): the gen prologue saves regs there and the
  // native body never touches the guest stack, so those bytes are dead-below-sp on return. (Sub-call stack
  // frames are identical between paths — both use the guest stack via rec_dispatch / interpreted jals.)
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 56) ? sp - 56 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[scriptvm] MISMATCH obj=%08x state=%u v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           obj, c->mem_r8(obj + 4), v0_n, v0_o, ro, so, sp);
  } else if (++ng % 2000 == 0) fprintf(stderr, "[scriptvm] %ld matches\n", ng);
}

// FUN_800931C0 — per-frame INPUT/controller-state processor (the heaviest un-owned resident fn, ~12% of
// field time). Five phases over the global tables at 0x80105xxx / 0x801054xx:
//  P0  advance the 16-slot ring index 0x80105BAC (=(idx+1)&15), clear new ring slot 0x80105BB0[idx].
//  P1  for each object [0, (int8)0x80105CEC): jal 0x8009A1D0(s0, &rec[s0]) (rec base 0x801054CE, stride 56);
//      if rec[s0].h0==0 set bit s0 in ring slot 0x80105BB0[idx] (the "was-present this frame" accumulator).
//  P2  if (int8)0x80105D28==0: acc = AND of ring slots 0..14 (a 15-frame coherence window); for each object,
//      if (acc>>s0)&1 and rec-byte 0x801054E5[s0*56]==2 -> jal 0x80097E10(0, mask) (mask = (int16)(1<<s0)
//      for s0<16, else ((1<<(s0-16))&0xff)<<16); clear that byte.
//  P3  0x801054B8 &= ~0x80105BF0; 0x801054BA &= ~0x80105BF2; for s0 in [0,24): if h[0x801054E6+s0*56]!=0
//      call (*0x80105BA8)(s0); if h[0x801054F2+s0*56]!=0 call (*0x80105A20)(s0)  (indirect fn-ptr globals).
//  P4  for s0 in [0,24): marshal a struct on the guest stack (base sp+16) from the per-slot flag byte
//      0x80105A08[s0] + the halfword fields at 0x80105A28+s0*16, set field+4 bits (1->3, 4->|0x10,
//      8->|0x80, 0x10->|0x60000); if field+4 != 0 jal 0x80099970(struct); clear the flag byte.
//  P5  four channel flushes: 0x80098F90(0, bf2:bf0), 0x80098F90(1, 54ba:54b8), 0x80098DB0(8, 54be:54bc),
//      0x80097E10(8, 54c2:54c0); then zero 0x80105BF0/BF2 and 0x801054B8/BA/C0/C2.
// CONTROL FLOW + memory ops owned native; every jal (incl. the P3 indirect fn-ptrs) stays interpreted via
// rec_dispatch. We mirror the gen 120-byte stack frame (sp -= 120) so the P4 struct lands where 0x80099970
// expects it AND every sub-call's stack frame aligns with the gen body. `pad931c0` gate = full RAM+
// scratchpad A/B vs rec_super_call, excluding the fn's own frame [old_sp-120, old_sp). v0 carries the last
// sub-call's (0x80097E10) return, matching the gen epilogue.
static void input_dispatch_931c0(Core* c) {
  uint32_t old_sp = c->r[29];
  uint32_t sp = old_sp - 120;
  c->r[29] = sp;
  // P0: advance ring
  uint32_t ridx = (c->mem_r32(0x80105BACu) + 1) & 0xf;
  c->mem_w32(0x80105BACu, ridx);
  c->mem_w32(0x80105BB0u + ridx * 4, 0);
  // P1
  for (int s0 = 0; s0 < (int)(int8_t)c->mem_r8(0x80105CECu); s0++) {
    uint32_t rec = 0x801054CEu + (uint32_t)s0 * 56;
    c->r[4] = (uint32_t)s0; c->r[5] = rec; rec_dispatch(c, 0x8009A1D0u);
    if (c->mem_r16(rec) == 0) {
      uint32_t a = 0x80105BB0u + c->mem_r32(0x80105BACu) * 4;
      c->mem_w32(a, c->mem_r32(a) | (1u << (s0 & 31)));
    }
  }
  // P2
  if ((int8_t)c->mem_r8(0x80105D28u) == 0) {
    uint32_t acc = 0xFFFFFFFFu;
    for (int k = 0; k < 15; k++) acc &= c->mem_r32(0x80105BB0u + (uint32_t)k * 4);
    for (int s0 = 0; s0 < (int)(int8_t)c->mem_r8(0x80105CECu); s0++) {
      if (acc & (1u << (s0 & 31))) {
        uint32_t recb = 0x801054E5u + (uint32_t)s0 * 56;
        if ((int8_t)c->mem_r8(recb) == 2) {
          uint32_t a1 = (s0 < 16) ? (uint32_t)(int32_t)(int16_t)(uint16_t)(1u << s0)
                                  : (((1u << ((s0 - 16) & 31)) & 0xffu) << 16);
          c->r[4] = 0; c->r[5] = a1; rec_dispatch(c, 0x80097E10u);
        }
        c->mem_w8(recb, 0);
      }
    }
  }
  // P3
  c->mem_w16(0x801054B8u, (uint16_t)(c->mem_r16(0x801054B8u) & (uint16_t)~(uint16_t)c->mem_r16(0x80105BF0u)));
  c->mem_w16(0x801054BAu, (uint16_t)(c->mem_r16(0x801054BAu) & (uint16_t)~(uint16_t)c->mem_r16(0x80105BF2u)));
  for (int s0 = 0; s0 < 24; s0++) {
    uint32_t off = (uint32_t)s0 * 56;
    if (c->mem_r16(0x801054E6u + off) != 0) { c->r[4] = (uint32_t)s0; rec_dispatch(c, c->mem_r32(0x80105BA8u)); }
    if (c->mem_r16(0x801054F2u + off) != 0) { c->r[4] = (uint32_t)s0; rec_dispatch(c, c->mem_r32(0x80105A20u)); }
  }
  // P4
  for (int s0 = 0; s0 < 24; s0++) {
    uint32_t off = (uint32_t)s0 * 16;
    c->mem_w32(sp + 16, 1u << (s0 & 31));
    c->mem_w32(sp + 20, 0);
    uint32_t f20 = 0;
    uint8_t flags = c->mem_r8(0x80105A08u + (uint32_t)s0);
    if (flags & 1) {
      f20 = 3; c->mem_w32(sp + 20, f20);
      c->mem_w16(sp + 24, c->mem_r16(0x80105A28u + off));
      c->mem_w16(sp + 26, c->mem_r16(0x80105A2Au + off));
    }
    if (c->mem_r8(0x80105A08u + (uint32_t)s0) & 4) {
      f20 |= 0x10; c->mem_w32(sp + 20, f20);
      c->mem_w16(sp + 36, c->mem_r16(0x80105A2Cu + off));
    }
    if (c->mem_r8(0x80105A08u + (uint32_t)s0) & 8) {
      f20 |= 0x80; c->mem_w32(sp + 20, f20);
      c->mem_w32(sp + 44, (uint32_t)c->mem_r16(0x80105A2Eu + off) << 3);
    }
    if (c->mem_r8(0x80105A08u + (uint32_t)s0) & 0x10) {
      f20 |= 0x60000; c->mem_w32(sp + 20, f20);
      c->mem_w16(sp + 74, c->mem_r16(0x80105A30u + off));
      c->mem_w16(sp + 76, c->mem_r16(0x80105A32u + off));
    }
    if (c->mem_r32(sp + 20) != 0) { c->r[4] = sp + 16; rec_dispatch(c, 0x80099970u); }
    c->mem_w8(0x80105A08u + (uint32_t)s0, 0);
  }
  // P5
  c->r[4] = 0; c->r[5] = ((uint32_t)c->mem_r8(0x80105BF2u) << 16) | (uint32_t)c->mem_r16(0x80105BF0u); rec_dispatch(c, 0x80098F90u);
  c->r[4] = 1; c->r[5] = ((uint32_t)c->mem_r8(0x801054BAu) << 16) | (uint32_t)c->mem_r16(0x801054B8u); rec_dispatch(c, 0x80098F90u);
  c->r[4] = 8; c->r[5] = ((uint32_t)c->mem_r8(0x801054BEu) << 16) | (uint32_t)c->mem_r16(0x801054BCu); rec_dispatch(c, 0x80098DB0u);
  c->r[4] = 8; c->r[5] = ((uint32_t)c->mem_r8(0x801054C2u) << 16) | (uint32_t)c->mem_r16(0x801054C0u); rec_dispatch(c, 0x80097E10u);
  c->mem_w16(0x80105BF0u, 0); c->mem_w16(0x80105BF2u, 0);
  c->mem_w16(0x801054B8u, 0); c->mem_w16(0x801054BAu, 0);
  c->mem_w16(0x801054C0u, 0); c->mem_w16(0x801054C2u, 0);
  c->r[29] = old_sp;
}

void ov_input_dispatch_931c0(Core* c) {
  // NB: re-check the channel each call (not a one-shot static) — this fn runs from BOOT, before the REPL's
  // `debug pad931c0` is processed, so a first-call latch would pin the gate OFF. cfg_dbg is free when unset.
  if (!cfg_dbg("pad931c0")) { input_dispatch_931c0(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  // Exclude scratch STACK below the entry sp (the fn's own 120-byte frame + its callees' frames). The ABI
  // guarantees nothing below sp is live across the call, so an A/B that double-runs the sub-calls
  // legitimately leaves different scratch there (a deep callee reads a transient mid-fn global / hardware
  // value that is reconciled before return — the struct passed in is byte-identical, all persistent state
  // matches). A real behavioral bug would alter persistent state (a game table / object / scratchpad) and
  // still be caught. The stack lives at the top of RAM (~0x801FFFxx); the window is far above all game data.
  uint32_t old_sp = regs0[29] & 0x1FFFFFu;
  uint32_t slo = (old_sp >= 0x1000) ? old_sp - 0x1000 : 0;
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  input_dispatch_931c0(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x800931C0u);
  uint32_t v0_o = c->r[2];
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= slo && a < old_sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[pad931c0] MISMATCH v0 n=%x o=%x ram@%x spad@%x sp=%x\n", v0_n, v0_o, ro, so, old_sp);
  } else if (++ng % 1000 == 0) fprintf(stderr, "[pad931c0] %ld matches\n", ng);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// FUN_80076D68 — per-object ANIMATION-SEQUENCE VM stepper (resident, no GTE; ~3.6% of field interp
// time, a frequency leader on motion scenes). a0 (s0) = anim object; field s0+0x0E (h) is the frame
// COUNTDOWN/duration; field s0+0x38 (cur) is the cursor (word ptr) into an 8-byte-stride keyframe
// stream; each keyframe holds a tag/payload halfword at +6 and a jump pointer (word) at +8. The
// control word s0+0x0E is read once at entry as `ctrl`; its bit 0x1000 is the "do NOT advance the
// cursor this step" flag (sequence freeze/loop).
//
// Logic (transcribed 1:1 from the disasm — control flow + memory ops owned native, the 3 sub-fns
// (0x80075f0c per-frame applier, 0x80076904 frame loader, 0x80075ff8 frame executor) stay PSX,
// dispatched via rec_dispatch so each honors any later override identically in the super-call path):
//  - low12(ctrl) > 1  -> DELAY: still counting down this frame. The cursor's SIGN BIT is a flag:
//    cur<0 (bit31 set) = freeze in place -> sh[s0+14] = (low12-1) | (ctrl & 0x1000), return 0; else
//    apply the current frame (0x80075f0c, a1 = low12-1 — it sets the cursor's KSEG0 bit when a1==1)
//    and sh[s0+14] = (low12-1) | (post-call s0[14] & 0x1000), return 2.
//  - low12(ctrl) == 1 -> STEP: read the keyframe tag at [cur+6]&0xc000 and dispatch (freeze =
//    ctrl & 0x1000; when frozen the cursor is NOT advanced in any block):
//      tag 0x0000: advance cur += 8 (unless frozen), load the new frame's duration into s0+14
//                  (low12 of [cur+6]) via 0x80076904, then if the LOADED frame has the 0x2000
//                  "execute" flag, run its executor (0x80075ff8) keyed by the new tag; return 0.
//      tag 0x4000: FOLLOW the jump pointer cur = [cur+8] (unless frozen), reload duration, same exec;
//                  return 0. (The transient cur+8 store the asm makes is dead — overwritten by [cur+8].)
//      tag 0x8000: terminal/hold — sh[s0+14] = [cur+6]&0xfff (no advance), return 1.
//      tag 0xc000: FOLLOW cur = [cur+8] (unless frozen), reload duration, same exec; return 1 (and on
//                  the no-exec-flag path stores the loaded [cur+6]&0xfff like a hold).
//  The executor calls (L_f40/L_f28/L_ff0/L_7000) differ only in whether the executor's a1 is the
//  cursor's jump target ([cur+8]) or the cursor itself +8, and whether v0 returns 0 or 1.
// `animvm` gate = full RAM+scratchpad A/B vs rec_super_call (each path runs once from one checkpoint;
//  the native run is rolled back; the fn's own 40-byte stack frame [sp-40,sp) is excluded — the gen
//  prologue saves regs there, the native body never touches the guest stack).
static void anim_vm_76d68(Core* c) {
  const uint32_t s0   = c->r[4];
  const uint32_t ctrl = c->mem_r16(s0 + 14);             // a0reg (read once)
  const uint32_t low12 = ctrl & 0x0fffu;
  const uint32_t cnt   = (low12 - 1) & 0xffffffffu;      // s1 = low12 - 1
  const uint32_t freeze = ctrl & 0x1000u;                // bit set => do not advance the cursor

  // ---- DELAY branch: low12 != 1 (counter still running) ----
  if (cnt != 0) {
    int32_t cur = (int32_t)c->mem_r32(s0 + 56);
    if (cur < 0) {                                       // cursor sign bit set -> freeze in place
      // cur<0 path (0x80076dd0): h = (low12-1) + (ctrl & 0x1000); return 0. The +0x1000 term comes from
      // the bltz delay slot `andi v0,a0,0x1000` (a0 = the entry ctrl word) — NOT a literal +2.
      c->mem_w16(s0 + 14, (uint16_t)(cnt + freeze));
      c->r[2] = 0; return;
    }
    // 0x80075f0c takes a1 = cnt (the counter, s1 at the jal) — it uses (int16_t)a1==1 to decide whether
    // to set the KSEG0 bit on the cursor (s0+56). The entry register a1 was `addu a1,s1,zero` (=cnt).
    c->r[4] = s0; c->r[5] = cnt; rec_dispatch(c, 0x80075f0cu);   // apply current frame
    // s0+14 is RE-READ here (the applier 0x80075f0c may have modified it); only bit 0x1000 is kept.
    uint32_t fz = c->mem_r16(s0 + 14) & 0x1000u;
    c->mem_w16(s0 + 14, (uint16_t)(cnt + fz));           // (low12-1) | (post-call s0[14] & 0x1000)
    c->r[2] = 2; return;
  }

  // ---- STEP branch: low12 == 1 (frame elapsed) ----
  uint32_t cur = c->mem_r32(s0 + 56);
  uint32_t op  = c->mem_r16(cur + 6);                    // lhu; tag bits + payload
  uint32_t opu = op;                                     // lhu copy (a1)
  uint32_t tag = op & 0xc000u;                           // s1

  // Run the executor tail and set v0 (mirrors L_f40/L_f28/L_ff0/L_7000):
  //   jump_target: true -> executor a1 = [cur+8]; false -> a1 = cur+8
  //   retval: function return value
  auto exec_tail = [&](uint32_t cur_in, bool jump_target, uint32_t retval) {
    uint32_t a1 = jump_target ? c->mem_r32(cur_in + 8) : (cur_in + 8);
    c->r[4] = s0; c->r[5] = a1; c->r[6] = (uint32_t)(int32_t)(int16_t)c->mem_r16(s0 + 14);
    rec_dispatch(c, 0x80075ff8u);
    c->r[2] = retval;
  };

  if (tag == 0x4000u) {                                  // block T4000 (0x80076e9c)
    // NOT frozen -> follow the jump pointer cur=[cur+8] (the cur+8 sw is dead, overwritten); frozen ->
    // leave the cursor unchanged (the bne skips the whole follow).
    if (!freeze) { cur = c->mem_r32(cur + 8); c->mem_w32(s0 + 56, cur); }
    cur = c->mem_r32(s0 + 56);
    uint32_t dur = c->mem_r16(cur + 6) & 0x0fffu;        // duration low12
    c->mem_w16(s0 + 14, (uint16_t)dur);                  // sh BEFORE the call (delay slot)
    c->r[4] = s0; rec_dispatch(c, 0x80076904u);          // load frame
    uint32_t a1c = c->mem_r32(s0 + 56);
    uint32_t v1  = c->mem_r16(a1c + 6);
    if ((v1 & 0x2000u) == 0) { c->r[2] = 0; return; }    // no exec flag
    uint32_t t = v1 & 0xc000u;
    if (t == 0x4000u)      { exec_tail(a1c, true, 0); return; }   // -> L_f40
    if (t < 0x4001u) {                                            // t == 0
      if (t == 0)          { exec_tail(a1c, false, 0); return; }  // -> L_f28
      c->r[2] = 0; return;
    }
    if (t == 0x8000u)      { c->r[2] = 0; return; }               // -> L_701c
    if (t == 0xc000u)      { exec_tail(a1c, true, 0); return; }   // -> L_f40
    c->r[2] = 0; return;
  }

  if (tag < 0x4001u) {                                   // tag == 0 -> block T0 (0x80076e2c)
    if (tag != 0) { c->r[2] = 0; return; }               // (unreachable for &0xc000, kept faithful)
    if (!freeze) { cur = cur + 8; c->mem_w32(s0 + 56, cur); }
    cur = c->mem_r32(s0 + 56);
    uint32_t dur = c->mem_r16(cur + 6) & 0x0fffu;
    c->mem_w16(s0 + 14, (uint16_t)dur);                  // sh in jal delay slot
    c->r[4] = s0; rec_dispatch(c, 0x80076904u);          // load frame
    uint32_t a1c = c->mem_r32(s0 + 56);
    uint32_t v1  = c->mem_r16(a1c + 6);
    if ((v1 & 0x2000u) == 0) { c->r[2] = 0; return; }
    uint32_t t = v1 & 0xc000u;
    if (t == 0x4000u)      { exec_tail(a1c, true, 0); return; }   // == s2 -> L_f40
    if (t < 0x4001u) {                                            // t == 0
      if (t == 0)          { exec_tail(a1c, false, 0); return; }  // -> L_f28
      c->r[2] = 0; return;
    }
    if (t == 0x8000u)      { c->r[2] = 0; return; }               // -> L_701c
    if (t == 0xc000u)      { exec_tail(a1c, true, 0); return; }   // -> L_f40
    c->r[2] = 0; return;
  }

  // tag is 0x8000 or 0xc000
  if (tag == 0x8000u) {                                  // block T8000 (0x80076f58)
    c->mem_w16(s0 + 14, (uint16_t)(opu & 0x0fffu));
    c->r[2] = 1; return;
  }
  // tag == 0xc000 -> block TC000 (0x80076f64)
  {
    // Same follow-jump structure as T4000: NOT frozen -> cur=[cur+8]; frozen -> unchanged.
    if (!freeze) { cur = c->mem_r32(cur + 8); c->mem_w32(s0 + 56, cur); }
    cur = c->mem_r32(s0 + 56);
    uint32_t dur = c->mem_r16(cur + 6) & 0x0fffu;
    c->mem_w16(s0 + 14, (uint16_t)dur);
    c->r[4] = s0; rec_dispatch(c, 0x80076904u);
    uint32_t a1c = c->mem_r32(s0 + 56);
    uint32_t v1  = c->mem_r16(a1c + 6);
    if ((v1 & 0x2000u) == 0) {                                    // beq -> 0x80076f5c (hold-store + return 1)
      c->mem_w16(s0 + 14, (uint16_t)(opu & 0x0fffu)); c->r[2] = 1; return;
    }
    uint32_t t = v1 & 0xc000u;
    if (t == 0x4000u)      { exec_tail(a1c, true, 1); return; }   // == s2 -> L_7000
    if (t < 0x4001u) {                                            // t == 0
      if (t == 0)          { exec_tail(a1c, false, 1); return; }  // -> L_ff0
      c->r[2] = 1; return;                                        // (delay v0=1) -> 0x80077020
    }
    if (t == 0x8000u)      { c->r[2] = 1; return; }               // == s3 -> 0x80077020 (v0=1)
    if (t == 0xc000u)      { exec_tail(a1c, true, 1); return; }   // == s1 -> L_7000
    c->r[2] = 1; return;
  }
}

void ov_anim_vm_76d68(Core* c) {
  // Lazy gate (re-check each call): this fn can run before the REPL `debug animvm` is processed.
  if (!cfg_dbg("animvm")) { anim_vm_76d68(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t s0 = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  anim_vm_76d68(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80076D68u);
  uint32_t v0_o = c->r[2];
  // Exclude FUN_80076D68's OWN 40-byte stack frame [sp-40, sp) — gen saves regs there, native never
  // touches the guest stack. Sub-call frames are identical (both interpret the same jals).
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 40) ? sp - 40 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[animvm] MISMATCH s0=%08x v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           s0, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 2000 == 0) fprintf(stderr, "[animvm] %ld matches\n", ng);
}


static void ov_object_cull(Core* c) {
  uint32_t prev = c->game->fps60.current_object;
  uint32_t o = c->r[4];                            // a0 = object* (MIPS arg register $a0)
  c->game->fps60.current_object = o;
  g_render_object  = o;                            // probe key: NOT restored — the submit that follows is o's

  if (s_objlog < 0) s_objlog = cfg_dbg("obj") ? 1 : 0;
  if (s_objlog)
    fprintf(stderr, "[objlog] obj=%08x type=%02x pos=(%d,%d,%d)\n", o, c->mem_r8(o + 0x0c),
            (int16_t)obj_r16(c, o + 0x2e), (int16_t)obj_r16(c, o + 0x32), (int16_t)obj_r16(c, o + 0x36));
  int p2 = (int16_t)c->r[5], p3 = (int16_t)c->r[6], p4 = (int16_t)c->r[7];   // pos - camera (s16 each)
  static int s_cullverify = -1;
  if (s_cullverify < 0) s_cullverify = cfg_dbg("cullverify") ? 1 : 0;
  if (s_cullverify) cull_verify_body(c);           // diagnostic: predict native, recomp writes, compare
  else              cull_native_body(c);            // PC-native cull (replaces the recomp body)
  // The engine OWNS this margin, so it is ALWAYS active — not gated on widescreen. Even at 4:3 the
  // stock ±34° cone over-culls (edge pop-in), so we keep the wide region in every aspect; widescreen
  // then needs no extra special-casing. Env overrides remain for diagnostics only (PSXPORT_CULL_FAR/_FOV).
  if (s_cull < 0) { const char* f = cfg_str("PSXPORT_CULL_FAR"); s_cull_far = f ? atoi(f) : -1;
                    const char* v = cfg_str("PSXPORT_CULL_FOV"); s_cull_fov = v ? atoi(v) : -1; s_cull = 1; }
  int do_cull = 1;
  // FAR limit (engine units, same scale as `dist`): the widest stock far is 7169 (0x1C01). 0x8000 ≈ 4.6x
  // that, so distant scenery / terrain tiles the camera is heading toward are kept well before they pop in,
  // while still bounding the working set (we never re-include the whole level — perf guard, see risk note).
  int cull_far = s_cull_far >= 0 ? s_cull_far : 0x8000;
  // FOV-cone threshold for depth/(dist*4) [≈ cos·1024]. The engine keeps objects to the EDGE of the view
  // and a bit beyond: 0 = the full forward hemisphere (±90°, well past the widescreen frustum's ~±40°);
  // a small NEGATIVE value extends past 90° so an object whose ORIGIN is just behind the camera plane but
  // whose widened geometry still grazes the screen edge is not dropped (this is the "beyond WS" margin the
  // user asked for). -0x60 ≈ cos(93.4°): ~3.4° past the side, enough to cover wide edge geometry without
  // re-including things squarely behind the camera. (vs the stock 848..880 ≈ only ±34°.)
  int cull_fov = s_cull_fov >= 0 ? s_cull_fov : -0x60;
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

// FUN_8007778C — camera-relative cull WRAPPER. Computes the object's delta from the camera
// (obj pos − cam pos: a wrapping 16-bit subtraction, sign-extended), zeros the two cull scratchpad
// flags (0x1F800080 push-enable, 0x1F800084 state), then forwards to the per-object cull body
// FUN_8007712c (a0=object, a1=dx(X) → r5, a2=dz(Z) → r6, a3=dy(Y) → r7). Camera pos is the scratchpad
// block @0x1F8000D0 (+2=X,+6=Z,+10=Y, u16). The cull body is itself owned (ov_object_cull) and re-reads
// r5..r7 for the engine margin, so we route through it via rec_dispatch (NOT cull_native_body directly)
// to keep current-object tracking + the widescreen margin. Returns v0 = visible (set by the cull body).
static void ov_cull_wrapper_prep(Core* c) {
  uint32_t obj = c->r[4];
  uint16_t camx = (uint16_t)c->mem_r16(0x1F8000D2u);
  uint16_t camz = (uint16_t)c->mem_r16(0x1F8000D6u);
  uint16_t camy = (uint16_t)c->mem_r16(0x1F8000DAu);
  c->mem_w32(0x1F800080u, 0);
  c->mem_w32(0x1F800084u, 0);
  c->r[5] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x2E) - camx);  // dx (X)
  c->r[6] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x32) - camz);  // dz (Z)
  c->r[7] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->mem_r16(obj + 0x36) - camy);  // dy (Y)
  c->r[4] = obj;
}
// PSXPORT_DEBUG=cullwrap — per-call gate. The only NEW logic here is the delta arithmetic + the two
// flag zero-writes; the inner cull (ov_object_cull) is already verified. Run native (prep + dispatch),
// capture the gameplay-relevant outputs, restore them, run the recomp wrapper, and diff. The inner cull
// is deterministic given r5..7 + scratchpad, so any divergence reflects a wrong delta. (Render-margin
// scratch is applied in both paths and not restored — diagnostic frames only.)
static void ov_cull_wrapper_verify(Core* c) {
  uint32_t rs[32]; memcpy(rs, c->r, sizeof rs);
  uint32_t obj = c->r[4];
  uint32_t cnt_b[3], ptr_b[3];
  for (int i = 0; i < 3; i++) { cnt_b[i] = (uint16_t)c->mem_r16(CULL_QCNT[i]); ptr_b[i] = c->mem_r32(CULL_QPTR[i]); }
  uint32_t kept_b = c->mem_r8(obj + 1), f84_b = c->mem_r32(0x1F800084u), f80_b = c->mem_r32(0x1F800080u);
  // NATIVE
  ov_cull_wrapper_prep(c); rec_dispatch(c, 0x8007712Cu);
  uint32_t kept_n = c->mem_r8(obj + 1), v0_n = c->r[2], f84_n = c->mem_r32(0x1F800084u), f80_n = c->mem_r32(0x1F800080u);
  uint32_t cnt_n[3], ptr_n[3];
  for (int i = 0; i < 3; i++) { cnt_n[i] = (uint16_t)c->mem_r16(CULL_QCNT[i]); ptr_n[i] = c->mem_r32(CULL_QPTR[i]); }
  // RESTORE outputs to the pre-call state
  memcpy(c->r, rs, sizeof rs);
  c->mem_w8(obj + 1, (uint8_t)kept_b); c->mem_w32(0x1F800084u, f84_b); c->mem_w32(0x1F800080u, f80_b);
  for (int i = 0; i < 3; i++) { c->mem_w16(CULL_QCNT[i], (uint16_t)cnt_b[i]); c->mem_w32(CULL_QPTR[i], ptr_b[i]); }
  // RECOMP
  rec_super_call(c, 0x8007778Cu);
  uint32_t kept_o = c->mem_r8(obj + 1), v0_o = c->r[2], f84_o = c->mem_r32(0x1F800084u), f80_o = c->mem_r32(0x1F800080u);
  uint32_t cnt_o[3], ptr_o[3];
  for (int i = 0; i < 3; i++) { cnt_o[i] = (uint16_t)c->mem_r16(CULL_QCNT[i]); ptr_o[i] = c->mem_r32(CULL_QPTR[i]); }
  static long ngood = 0, nbad = 0; int bad = 0;
  if (kept_n != kept_o || v0_n != v0_o || f84_n != f84_o || f80_n != f80_o) bad = 1;
  for (int i = 0; i < 3; i++) if (cnt_n[i] != cnt_o[i] || ptr_n[i] != ptr_o[i]) bad = 1;
  if (bad) { if (nbad++ < 60) fprintf(stderr, "[cullwrap] MISMATCH obj=%08x kept(n=%u o=%u) v0(n=%08x o=%08x) f84(n=%08x o=%08x) f80(n=%08x o=%08x)\n",
                                       obj, kept_n, kept_o, v0_n, v0_o, f84_n, f84_o, f80_n, f80_o); }
  else if (++ngood % 20000 == 0) fprintf(stderr, "[cullwrap] %ld matches (last obj=%08x)\n", ngood, obj);
}
static void ov_cull_wrapper(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("cullwrap") ? 1 : 0;
  if (s_v) { ov_cull_wrapper_verify(c); return; }
  ov_cull_wrapper_prep(c);
  rec_dispatch(c, 0x8007712Cu);
}

// FUN_80077ACC — a second cull-wrapper variant (~2.3%, 9k calls). Unlike ov_cull_wrapper it takes the
// position in a1/a2/a3 (caller-supplied, not obj fields) and sets the cull flags 0x1F800080=1 /
// 0x1F800084=4 (vs 0/0), then makes the position camera-relative (a1 -= *0x1F8000D2, a2 -= *0x1F8000D6,
// a3 -= *0x1F8000DA; sign-extend low16) and calls the owned cull body 0x8007712C. The cull is already
// verified, so the only new logic is the flag writes + delta arithmetic. `cullwrap2` gate A/B's the
// cull's outputs (obj+1, v0, flags, the 3 render queues) native vs the recomp wrapper.
static void cull_wrap_77acc_prep(Core* c) {
  c->mem_w32(0x1F800080u, 1);
  c->mem_w32(0x1F800084u, 4);
  uint16_t cx = (uint16_t)c->mem_r16(0x1F8000D2u);
  uint16_t cz = (uint16_t)c->mem_r16(0x1F8000D6u);
  uint16_t cy = (uint16_t)c->mem_r16(0x1F8000DAu);
  c->r[5] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->r[5] - cx);
  c->r[6] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->r[6] - cz);
  c->r[7] = (uint32_t)(int32_t)(int16_t)(uint16_t)((uint16_t)c->r[7] - cy);
}
static void ov_cull_wrap_77acc_verify(Core* c) {
  uint32_t rs[32]; memcpy(rs, c->r, sizeof rs);
  uint32_t obj = c->r[4];
  uint32_t cnt_b[3], ptr_b[3];
  for (int i = 0; i < 3; i++) { cnt_b[i] = (uint16_t)c->mem_r16(CULL_QCNT[i]); ptr_b[i] = c->mem_r32(CULL_QPTR[i]); }
  uint32_t kept_b = c->mem_r8(obj + 1), f84_b = c->mem_r32(0x1F800084u), f80_b = c->mem_r32(0x1F800080u);
  cull_wrap_77acc_prep(c); rec_dispatch(c, 0x8007712Cu);
  uint32_t kept_n = c->mem_r8(obj + 1), v0_n = c->r[2], f84_n = c->mem_r32(0x1F800084u), f80_n = c->mem_r32(0x1F800080u);
  uint32_t cnt_n[3], ptr_n[3];
  for (int i = 0; i < 3; i++) { cnt_n[i] = (uint16_t)c->mem_r16(CULL_QCNT[i]); ptr_n[i] = c->mem_r32(CULL_QPTR[i]); }
  memcpy(c->r, rs, sizeof rs);
  c->mem_w8(obj + 1, (uint8_t)kept_b); c->mem_w32(0x1F800084u, f84_b); c->mem_w32(0x1F800080u, f80_b);
  for (int i = 0; i < 3; i++) { c->mem_w16(CULL_QCNT[i], (uint16_t)cnt_b[i]); c->mem_w32(CULL_QPTR[i], ptr_b[i]); }
  rec_super_call(c, 0x80077ACCu);
  uint32_t kept_o = c->mem_r8(obj + 1), v0_o = c->r[2], f84_o = c->mem_r32(0x1F800084u), f80_o = c->mem_r32(0x1F800080u);
  uint32_t cnt_o[3], ptr_o[3];
  for (int i = 0; i < 3; i++) { cnt_o[i] = (uint16_t)c->mem_r16(CULL_QCNT[i]); ptr_o[i] = c->mem_r32(CULL_QPTR[i]); }
  static long ngood = 0, nbad = 0; int bad = 0;
  if (kept_n != kept_o || v0_n != v0_o || f84_n != f84_o || f80_n != f80_o) bad = 1;
  for (int i = 0; i < 3; i++) if (cnt_n[i] != cnt_o[i] || ptr_n[i] != ptr_o[i]) bad = 1;
  if (bad) { if (nbad++ < 60) fprintf(stderr, "[cullwrap2] MISMATCH obj=%08x kept(n=%u o=%u) v0(n=%08x o=%08x) f84(n=%08x o=%08x) f80(n=%08x o=%08x)\n",
                                       obj, kept_n, kept_o, v0_n, v0_o, f84_n, f84_o, f80_n, f80_o); }
  else if (++ngood % 20000 == 0) fprintf(stderr, "[cullwrap2] %ld matches (last obj=%08x)\n", ngood, obj);
}
void ov_cull_wrap_77acc(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("cullwrap2") ? 1 : 0;
  if (s_v) { ov_cull_wrap_77acc_verify(c); return; }
  cull_wrap_77acc_prep(c);
  rec_dispatch(c, 0x8007712Cu);
}

// PC-owned LZ image decompressor — replaces recompiled FUN_80044D8C (0x80044D8C). This routine
// rebuilds the per-frame CLUTs (0x801FCDC0) and sprite/texture data from compressed area assets.
// It was the source of the gameplay 2D-sprite corruption: the SAME function gave correct output
// when recompiled but ZEROS when flat-interpreted by the coroutine interpreter (rec_coro_run) at
// runtime — a recompiler-vs-interpreter divergence. A pure decompressor belongs to the PC side,
// so we own it natively here (one implementation, reached identically from both engines).
//
// ABI (matches the MIPS at 0x80044D8C, verified by disassembly):
//   a0=descriptor, a1=dest, a2=src, a3=srclen. Returns v0 = bytes written.
//   Setup: build 8 back-ref offsets from the static table at 0x800153C8, scaled by the per-call
//   stride at desc+4:  offset[i] = base[i] + 2*(factor[i]*stride)  (2D image predictors: mode 1 =
//   previous byte, modes 2-7 = previous-row neighbours; row pitch = stride).
//   Stream of control bytes: len=ctrl>>3, mode=ctrl&7.  mode==0 -> literal copy `len` bytes from
//   src (ctrl byte 0 / len 0 terminates).  mode!=0 -> back-ref copy `len` bytes from dest+offset
//   [mode], BYTE-granular so overlapping copies replicate (RLE), exactly as the original loop.
#define LZ_OFFTAB_BASE 0x800153C8u
static uint32_t lz_decompress(Core* c, uint32_t desc, uint32_t dst, uint32_t src0, uint32_t srclen) {
  const uint32_t src_end = src0 + srclen;
  const int32_t stride = (int16_t)c->mem_r16(desc + 4);
  int32_t offtab[8];
  for (int i = 0; i < 8; i++) {
    const int32_t base   = (int32_t)c->mem_r32(LZ_OFFTAB_BASE + i * 8 + 0);
    const int32_t factor = (int32_t)c->mem_r32(LZ_OFFTAB_BASE + i * 8 + 4);
    offtab[i] = base + 2 * (factor * stride);
  }
  uint32_t src = src0, out = dst;
  while (src < src_end) {
    const uint8_t ctrl = c->mem_r8(src++);
    const uint32_t len = ctrl >> 3, mode = ctrl & 7u;
    if (mode != 0) {                                  // back-reference into the output so far
      uint32_t bsrc = out + (uint32_t)offtab[mode];
      for (uint32_t k = 0; k < len; k++) c->mem_w8(out++, c->mem_r8(bsrc++));
    } else {                                          // literal run from the source
      if (len == 0) break;                            // terminator
      for (uint32_t k = 0; k < len; k++) c->mem_w8(out++, c->mem_r8(src++));
    }
  }
  return out - dst;                                   // total bytes written
}
static void ov_lz_decompress(Core* c) {
  c->r[2] = lz_decompress(c, c->r[4], c->r[5], c->r[6], c->r[7]);
}

// PC-owned texture-group unpacker — replaces recompiled FUN_80044E84 (0x80044E84). Verified by
// disassembly: a0 = descriptor table base, a1 = scratch-end anchor (0x1FD000). Layout: [count:4]
// then [pad:4] then `count` 12-byte entries each { stride:2(@+4 from entry head), field:2(@+6),
// srclen:4(@+8) }; source data starts 0x800 after the table base and advances by srclen per entry.
// For each entry: dst = anchor - 2*stride*field (outputs stack ending at the anchor — transient
// scratch), decompress the entry's image there, then upload it (FUN_80081218) and run the post
// step (FUN_80080f6c). Non-gameplay (asset unpack) → PC-owned, calling the native decompressor
// directly; the two gfx-library sub-calls still route through the recomp/dispatch for now.
void rec_dispatch(Core*, uint32_t);
static void ov_unpack_group(Core* c) {
  const uint32_t table = c->r[4], anchor = c->r[5];
  const int32_t count = (int32_t)c->mem_r32(table);
  uint32_t entry = table + 4;            // first 12-byte descriptor entry
  uint32_t src = table + 0x800;          // packed source data follows the table
  int dbg = cfg_dbg("unpack") != 0;
  if (dbg) fprintf(stderr, "[unpack] table=0x%08X count=%d src0=0x%08X ra=0x%08X\n",
                   table, count, src, c->r[31]);
  // PSXPORT_UNPACKDUMP=dir — dump the LIVE compressed input (table + 0x30000 bytes) the moment the
  // unpacker reads it, sequence-numbered, so it can be checked against the disc / oracle exactly.
  { const char* dd = cfg_str("PSXPORT_UNPACKDUMP");
    if (dd) { static int seq = 0; char p[300]; snprintf(p, sizeof p, "%s/unpack_%03d_c%d.bin", dd, seq++, count);
      FILE* uf = fopen(p, "wb"); if (uf) {
        // Dump from the staging base to the end of RAM (archives can be up to ~0x76000 from 0x8018A000).
        uint32_t off = table & 0x1FFFFF, len = 0x200000u - off;
        fwrite(&c->ram[off], 1, len, uf); fclose(uf);
        fprintf(stderr, "[unpack] dumped live input -> %s (table=0x%08X count=%d)\n", p, table, count); } } }
  for (int32_t i = 0; i < count; i++) {
    const uint32_t desc   = entry;
    const int32_t  stride = (int16_t)c->mem_r16(desc + 4);
    const int32_t  field  = (int16_t)c->mem_r16(desc + 6);
    const uint32_t srclen = c->mem_r32(desc + 8);
    const uint32_t dst    = anchor - (uint32_t)(2 * stride * field);
    if (dbg) fprintf(stderr, "[unpack]  e%d dst=(%d,%d) %dx%d src=0x%08X len=%u srcbytes:"
                     " %02X %02X %02X %02X\n", i, (int16_t)c->mem_r16(desc), (int16_t)c->mem_r16(desc+2),
                     stride, field, src, srclen, c->mem_r8(src), c->mem_r8(src+1), c->mem_r8(src+2), c->mem_r8(src+3));
    lz_decompress(c, desc, dst, src, srclen);            // native decompress into transient scratch
    src   += srclen;
    entry += 12;
    c->r[4] = desc; c->r[5] = dst; rec_dispatch(c, 0x80081218u);  // FUN_80081218(desc, dst): upload
    // Per-image post-step FUN_80080f6c(0) = libgs GPU DrawSync between uploads — meaningless for our
    // synchronous native upload (no async DMA to drain). Owned as a skip; see note above ov_upload_image.
    if (cfg_dbg("unpacksync")) { c->r[4] = 0; rec_dispatch(c, 0x80080F6Cu); }
  }
}

// PC-native TEXTURE-GROUP LOADER — owns the asset-load ORCHESTRATION FUN_80044F58 (0x80044F58): the
// per-group loader a level uses to stream a texture set into VRAM. RE (tools/disas.py + gen_func_80044F58):
// the current task selects a set via task[0x6D]=mode / task[0x6E]=set, then
//   1. CD-load a 2KB HEADER from sector (filebase0 = *0x800BE0F0) + set  [+ a 4/26 bias in mode 2] -> 0x800EF478
//   2. CD-load the compressed ARCHIVE from sector (filebase1 = *0x800BE0F8) + (hdr[0]>>11), len hdr[1]-hdr[0]
//      -> the fixed staging buffer 0x8018A000 (descriptor table in its first 0x800 bytes, packed data after)
//   3. UNPACK the archive (ov_unpack_group) -> decompress + upload each image to its VRAM (x,y) (owned)
//   4. copy a 42-word metadata table from hdr+0x100 (0x800EF578) -> 0x800FB170 (per-set sprite/CLUT meta the
//      game content reads back)
//   5. (mode==0 only) set _DAT_1f80019b=1 and run the task TERMINAL YIELD FUN_80051FB4 (suspend until next
//      frame — this is what streams the groups one-per-frame).
// ENGINE asset orchestration -> reimplemented PC-native; the CD read (ov_cd_loadfile, via 0x8001DC40) and the
// terminal task yield (the scheduler) stay the retained platform/content mechanism (called, not transcribed).
// The mode/set inputs and the 0x800FB170 metadata are CONTENT-interface state, so this is gated on the main-
// RAM A/B diff (later-177). For mode==0 the terminal yield does not return (ov_switch longjmps mid-game),
// exactly like eng_stage_transition's tail; there is no code after it.
static void ov_load_texgroup(Core* c) {
  uint32_t ra = c->r[31], sp = c->r[29], s0 = c->r[16];   // preserve for the (non-yield) epilogue
  uint32_t task = c->mem_r32(0x1F800138u);
  uint32_t mode = c->mem_r8(task + 0x6Du);
  uint32_t set  = c->mem_r8(task + 0x6Eu);
  uint32_t hdr_sector = c->mem_r32(0x800BE0F0u) + set;             // filebase0 + set
  if (mode == 2) {                                                 // mode-2 per-set 4/26-sector bias
    uint16_t mask = c->mem_r16(0x800BFE56u);
    hdr_sector += ((mask >> (set & 31)) & 1) ? 26u : 4u;
  }
  const uint32_t HDR = 0x800EF478u;
  c->r[4] = HDR; c->r[5] = hdr_sector; c->r[6] = 2048;
  rec_dispatch(c, 0x8001DC40u);                                    // 1. CD-load 2KB header (platform)
  uint32_t h0 = c->mem_r32(HDR + 0), h1 = c->mem_r32(HDR + 4);
  c->r[4] = 0x8018A000u; c->r[5] = c->mem_r32(0x800BE0F8u) + (h0 >> 11); c->r[6] = h1 - h0;
  rec_dispatch(c, 0x8001DC40u);                                    // 2. CD-load compressed archive -> staging
  c->r[4] = 0x8018A000u; c->r[5] = 0x1FD000u;
  rec_dispatch(c, 0x80044E84u);                                    // 3. unpack -> decompress + VRAM upload (owned)
  for (uint32_t i = 0; i < 42; i++)                                // 4. per-set metadata table
    c->mem_w32(0x800FB170u + i * 4, c->mem_r32(HDR + 0x100u + i * 4));
  c->r[16] = s0; c->r[29] = sp; c->r[31] = ra;
  if (mode == 0) {                                                 // 5. terminal yield (streams one group/frame)
    c->mem_w8(0x1F80019Bu, 1);
    rec_dispatch(c, 0x80051FB4u);                                  // ov_switch tail — does not return mid-game
  }
}

// Per-image post-step FUN_80080f6c(0) = the game's libgs frame DrawSync: FUN_80083364(0) waits for the
// GPU's ordering-table DMA to drain and the GPU to go idle (polls GPUSTAT @0x800a5ab4 bit 0x01000000 /
// @0x800a5aa8 bit 0x04000000), having queued the (now-empty, since ov_upload_image bypasses it) ring.
// IMPORTANT: this same function is the MAIN-LOOP per-frame DrawSync, so it must NOT be globally overridden
// (a global no-op stalls all presentation). Inside the unpack loop, however, it is a between-uploads GPU
// sync that is meaningless for our SYNCHRONOUS native VRAM upload — there is no async DMA to wait on — so
// the unpack loop owns it as a skip. A/B-verified VRAM+RAM-identical across a full menu+seaside load
// (later-177): the unpack call site only ever passes a0=0 with an empty ring. DIAG: PSXPORT_DEBUG=unpacksync
// restores the per-image super-call to prove equivalence.

// PC-native CPU->VRAM upload — replaces the game's libgs-style upload library FUN_80081218
// (0x80081218). RE (verified empirically vs the A0 upload log, later-62/63): a0 = descriptor
// { x:s16@0, y:s16@2, w:s16@4, h:s16@6 }, a1 = source pixel data (w*h contiguous 16-bit pixels,
// row-major). The recomp body ENQUEUES an entry into the GsSortObject ring at 0x800A5AC8 (head/
// tail @0x800A5AC8/5ACC) which is DMA'd to the GPU later as a 0xA0 packet. It is the SINGLE
// chokepoint for BOTH the scene-load texture atlas (256x256/192x256/… into the texpages the
// characters sample) AND every per-frame 16x1 CLUT — 5300+ calls per attract run. The user's
// directive: the GPU library must be PC-native, not a faithful recomp. So we write the rect
// straight into native VRAM here and DO NOT enqueue (the later ring flush/sync then no-ops over
// an empty ring). Ordering is preserved: the upload still happens before this frame's draws are
// processed, and CLUTs are double-buffered across frames (parity-alternated slots), so no draw
// reads a slot mid-overwrite.
static void ov_upload_image(Core* c) {
  const uint32_t desc = c->r[4], src = c->r[5];
  const int x = (int16_t)c->mem_r16(desc + 0), y = (int16_t)c->mem_r16(desc + 2);
  const int w = (int16_t)c->mem_r16(desc + 4), h = (int16_t)c->mem_r16(desc + 6);
  if (w > 0 && h > 0) gpu_native_load_image(c, x, y, w, h, src);
}

// --- Native ownership of the GTE projection setters (libgte) -------------------------------------
// The engine configures its projection via libgte SetGeomOffset/SetGeomScreen (RE: docs/engine_re.md,
// FUN_800509B4 -> screen center (160,120), focal length H=350). We reimplement them in native C
// (byte-identical to gen_func_800846D0/800846F0) so the PROJECTION is ours: this is the genuine
// widescreen FOV lever (widen OFX + the draw-env clip; no squish, no renderer re-center) and the
// reference point the 60fps/hi-res paths build on. Owned unconditionally (no gating). Was the
// recomp bodies for A/B. A one-time log prints the configured projection to confirm equivalence.
static void ov_set_geom_offset(Core* c) {       // SetGeomOffset(ofx, ofy)
  // FAITHFUL: write the game's exact projection offset. We do NOT widen OFX here anymore — CR24 is
  // SHARED GTE state that the GAME's OWN logic reads back (its on-screen tests / placement run RTPS and
  // consume the projected SXY). Widening it globally shifted those read-backs and corrupted the game.
  // Genuine widescreen now happens ONLY inside our owned render submit (engine_submit.c), isolated from
  // this guest-visible state — the PC engine drives the wide render; the game's logic stays untouched.
  uint32_t ofx = c->r[4], ofy = c->r[5];
  gte_write_ctrl(24, ofx << 16);                 // OFX
  gte_write_ctrl(25, ofy << 16);                 // OFY
  static int logged = 0;
  if (!logged++) fprintf(stderr, "[geom] native SetGeomOffset OFX=%u OFY=%u (CR24=%08X CR25=%08X)\n",
                         ofx, ofy, gte_read_ctrl(24), gte_read_ctrl(25));
}
static void ov_set_geom_screen(Core* c) {       // SetGeomScreen(h) — projection-plane distance (FOV)
  gte_write_ctrl(26, c->r[4]);                   // H
  static int logged = 0;
  if (!logged++) fprintf(stderr, "[geom] native SetGeomScreen H=%u (CR26=%08X)\n", c->r[4], gte_read_ctrl(26));
}

// Native ownership of DrawOTag (libgpu FUN_80081560, the per-frame draw kick): the recomp body just
// programs the GPU linked-list DMA to walk the ordering table at a0 — which our renderer already does
// natively in gpu_dma2_linked_list (walk OT -> decode each primitive -> rasterize). Overriding it routes
// the draw straight through our native walk (synchronous), instead of the DMA-register emulation dance.
// This is the engine's draw submission, owned.
void gpu_blank_display(Core* core);
static void ov_draw_otag(Core* c) {
  // #7/#11 finish: while the DEMO/title front-end is still LOADING its assets (sub-SM task0+0x48 < 2, the
  // s4a load ramp), the title composites its menu/font over whatever VRAM the FMV/SCEA splash left — so the
  // SCEA text / FMV last-frame show through (the one-time hand-off clear can't cover the multi-frame ramp).
  // Blank the display FB to black BEFORE this frame's prims draw, every loading frame, so the title's
  // partial 2D layer always sits on opaque black. Once loaded (s48>=2) the title owns a full background and
  // this is a no-op-equivalent (its bg overwrites the black). Engine-owned, keyed on the stage's own signal.
  if (c->mem_r32(0x801FE00Cu) == 0x801062E4u && c->mem_r8(0x801FE048u) < 2) gpu_blank_display(c);
  // Engine-owned ordering (the one render path): owned world geometry was queued during submit; the OT
  // walk queues the guest 2D / un-owned submit variants (instead of drawing them inline); then the queue
  // drains in ENGINE order (layer: background < world < overlay < hud; depth within world). The guest OT
  // is read here ONLY to enumerate the leftover guest prims — its draw ORDER is discarded. M3 captures
  // those at submit time and retires this read. (PSXPORT_SBS debug compare keeps an inline path; its
  // queue stays empty so the flush is a no-op.)
  gpu_dma2_linked_list(c, c->r[4]);
  rq_flush(c);
}

// ---- Replace the game's in-game Options menu with our PC-native (ImGui) menu (later-112) ----
// RE: the in-game pause menu is a task in the GAME overlay. Its body is the dispatcher at 0x8010810C,
// which reads the page byte task+0x6B (task ptr = *(u32)0x1F800138), bounds-checks <0xC, and jumps
// through a 12-entry table at 0x801062EC. Page 1 draws the main pause menu "Options / Load data /
// Quit game" (FUN_8007eae4) and, on Cross over "Options", sets task+0x6B = 3. Page 3's handler
// (0x801082C0) calls FUN_8007b45c — the game's Options submenu controller (Messages / Sound /
// Screen adjust / Controls), the options the user deemed not worth keeping. So we REPLACE that
// controller: while page 3 runs, show OUR overlay (g_mods toggles) instead, and own the SAME
// back-navigation FUN_8007b45c uses, including its menu SFX:
//   Circle (0x2000)   -> task+0x6B = 1 (back to the pause menu); SFX FUN_80074590(0x14, 0xFFF7, 0).
//   Triangle (0x1000) -> task+0x6B = 2 (close the pause menu);   SFX FUN_80074590(0x11, 0, 0).
// Faithful fallback: if our overlay isn't actually up (headless / window-less), super-call the real
// menu so nothing is lost. The overlay is up by default for windowed runs (no flag needed).
#define T2_PAD_EDGE    0x800E7E68u  // DAT_800e7e68 — this-frame pressed-button edges (u16, active-high)
#define T2_TASK_PTR    0x1F800138u  // _DAT_1f800138 — current task struct pointer (scratchpad word)
#define T2_MENU_CURSOR 0x800BF808u  // DAT_800bf808 — shared menu cursor byte
#define T2_MENU_DIRTY  0x1F800136u  // DAT_1f800136 — "menu changed" flag the pause task watches
#define T2_SFX_FN      0x80074590u  // FUN_80074590 — the menu sound-effect trigger
#define PAD_TRIANGLE   0x1000u
#define PAD_CIRCLE     0x2000u
extern "C" void imgui_overlay_set_visible(int v);
extern "C" void imgui_overlay_set_options_mode(int v);
extern "C" int  imgui_overlay_inited(void);

// Invoke a guest function with up to 3 args, preserving the override's a0-a2.
static void t2_call3(Core* c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2) {
  uint32_t s4 = c->r[4], s5 = c->r[5], s6 = c->r[6];
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2;
  rec_dispatch(c, addr);
  c->r[4] = s4; c->r[5] = s5; c->r[6] = s6;
}

static void ov_options_menu(Core* c) {
  if (cfg_dbg("ui")) {                                // PSXPORT_DEBUG=ui: confirm the page-3 handler is reached
    static int n = 0; if (!n++) fprintf(stderr, "[ui] FUN_8007b45c reached (game Options selected)\n");
  }
  if (!imgui_overlay_inited()) { rec_super_call(c, 0x8007B45Cu); return; }  // no overlay -> faithful menu
  static int announced = 0;
  if (!announced++) fprintf(stderr, "[ui] in-game Options -> PC-native overlay (Circle=back, Triangle=close)\n");
  imgui_overlay_set_options_mode(1);
  imgui_overlay_set_visible(1);                       // OUR menu IS the options screen now (no game draw)
  uint16_t edge = c->mem_r16(T2_PAD_EDGE);
  uint32_t task = c->mem_r32(T2_TASK_PTR);
  if (edge & PAD_CIRCLE) {                            // back to the pause menu (FUN_8007b45c substate-0 cancel)
    t2_call3(c, T2_SFX_FN, 0x14, 0xFFF7, 0);
    c->mem_w8(task + 0x6B, 1);
    c->mem_w8(T2_MENU_CURSOR, 0);
    c->mem_w8(T2_MENU_DIRTY, 1);
    imgui_overlay_set_options_mode(0);
    imgui_overlay_set_visible(0);
  } else if (edge & PAD_TRIANGLE) {                   // close the whole pause menu (FUN_8007b45c Triangle exit)
    t2_call3(c, T2_SFX_FN, 0x11, 0, 0);
    c->mem_w8(task + 0x6B, 2);
    imgui_overlay_set_options_mode(0);
    imgui_overlay_set_visible(0);
  }
}

void games_tomba2_init(void) {
  // ONE behavior = the PC game. The native engine path is registered UNCONDITIONALLY — no FAITHFUL master
  // switch, no per-override *_RECOMP / NO_* opt-outs (those were faithful-first A/B scaffolding; the user
  // directive is no gating + drive toward removing the interpreter entirely, so they are retired). Every
  // override below IS the behavior; the user verifies it via ./run.sh.
  // Hand-written native C++ for the boot→first-cutscene path (engine/native_path.cpp).
  void games_native_path_init(void);
  games_native_path_init();
  rec_set_override(0x800788ACu, ov_frame_update);    // per-frame state update + present + audio
  // Replace the game's in-game Options menu with our overlay. The override falls back to the real menu
  // (super-call) when the overlay isn't up (headless / PSXPORT_UI=0), so it self-activates with the overlay.
  rec_set_override(0x8007B45Cu, ov_options_menu);
  // Own the GTE projection setup natively.
  rec_set_override(0x800846D0u, ov_set_geom_offset);
  rec_set_override(0x800846F0u, ov_set_geom_screen);
  rec_set_override(0x80081560u, ov_draw_otag);       // own DrawOTag (the per-frame draw kick) natively
  { void engine_camera_register(void); engine_camera_register(); }   // per-frame camera X/Z follow native
  { void engine_math_register(void);   engine_math_register();   }   // hot libgte-style math leaves (isqrt)
  // PC-owned asset codecs.
  rec_set_override(0x80044D8Cu, ov_lz_decompress);   // LZ image decompressor
  rec_set_override(0x80044F58u, ov_load_texgroup);   // texture-group LOADER orchestration (header+archive+unpack)
  rec_set_override(0x80044E84u, ov_unpack_group);    // texture-group unpacker (drives the above)
  rec_set_override(0x80081218u, ov_upload_image);    // PC-native CPU->VRAM upload (libgs upload lib)
  // Own the geometry submit path natively.
  {
    void ov_submit_poly_gt3(Core*), ov_submit_poly_gt4(Core*), ov_submit_poly_gt4_bp(Core*),
         engine_submit_register_autodetect(void);
    rec_set_override(0x8007FDB0u, ov_submit_poly_gt3);   // POLY_GT3 (gouraud-textured triangle) submit
    rec_set_override(0x8008007Cu, ov_submit_poly_gt4);   // POLY_GT4 (gouraud-textured quad) submit
    rec_set_override(0x80027768u, ov_submit_poly_gt4_bp);// byte-packed POLY_GT4 (field's dominant emitter)
    engine_submit_register_autodetect();                 // + own the same library in runtime-loaded overlays
  }
  // Own the PER-OBJECT render flush natively (gen_func_8003CDD8 — the world/margin render submission):
  // compose the camera×object transform + dispatch the geomblk to the native submitter, with NO guest
  // render code (later-133).
  {
    void ov_perobj_flush(Core*), ov_perobj_render(Core*), ov_render_walk(Core*), ov_terrain(Core*);
    void ov_render_walk_snapshot(Core*);
    rec_set_override(0x8003CDD8u, ov_perobj_flush);
    rec_set_override(0x8003CCA4u, ov_perobj_render);   // per-object render dispatch
    rec_set_override(0x8003C048u, ov_render_walk);     // phase-2 render-list walk
    rec_set_override(0x8003BB50u, ov_render_walk_snapshot);  // snapshot-queue object walk + world-pos depth
    void ov_collectable_quad(Core*);
    rec_set_override(0x8003C8F4u, ov_collectable_quad);  // collectable billboard-quad drawer: world-pos depth
    rec_set_override(0x8002AB5Cu, ov_terrain);         // field terrain renderer (float transform + real depth)
    void ov_build_xform(Core*);
    rec_set_override(0x80051C8Cu, ov_build_xform);     // per-object transform builder
    void ov_xform_propagate(Core*);
    rec_set_override(0x80051464u, ov_xform_propagate); // child-node transform propagation (orchestrates owned prims)
    void ov_xform51128(Core*);
    rec_set_override(0x80051128u, ov_xform51128);      // per-object child-node transform loop (sibling of xform_propagate; orchestrates owned prims)
    void ov_orch597AC(Core*);
    rec_set_override(0x800597ACu, ov_orch597AC);       // per-object world-transform orchestrator (build matrix + secondary + child propagate)
    { void ov_cone_cull_2b278(Core*);
      rec_set_override(0x8002B278u, ov_cone_cull_2b278); }  // view-cone cull (lazy conecull gate)
    { void ov_rand(Core*); rec_set_override(0x8009A450u, ov_rand); }   // platform PRNG (rand LCG)
    { void ov_bittest_4d7ec(Core*); rec_set_override(0x8004D7ECu, ov_bittest_4d7ec); }  // bitmap bit-test
    { void ov_trig_sin(Core*), ov_trig_cos(Core*), ov_trig_lut(Core*);
      rec_set_override(0x80083E80u, ov_trig_sin);                      // sin LUT
      rec_set_override(0x80083F50u, ov_trig_cos);                      // cos LUT
      rec_set_override(0x80083EBCu, ov_trig_lut); }                    // sin-quadrant lookup
    { void ov_cull_wrap_77acc(Core*); rec_set_override(0x80077ACCu, ov_cull_wrap_77acc); }  // cull wrapper variant (flags 1/4)
    { void ov_list_scan_31780(Core*); rec_set_override(0x80031780u, ov_list_scan_31780); }  // list-tail resolver/reset
    { void ov_grid_setup_49968(Core*); rec_set_override(0x80049968u, ov_grid_setup_49968); }  // collision-grid row-ptr setup
    { void ov_grid_query_47cbc(Core*); rec_set_override(0x80047CBCu, ov_grid_query_47cbc); }  // collision-grid cell query/walk
    { void ov_grid_resolve_498c8(Core*); rec_set_override(0x800498C8u, ov_grid_resolve_498c8); }  // collision-grid resolve loop (control flow owned)
    { void ov_grid_step_4798c(Core*); rec_set_override(0x8004798Cu, ov_grid_step_4798c); }  // collision-grid per-step origin/index setup (the last dispatched grid callee)
    { void ov_script_vm_4ce14(Core*); rec_set_override(0x8004CE14u, ov_script_vm_4ce14); }  // per-object script-VM tick (control flow owned; sub-behaviors dispatched)
    { void ov_input_dispatch_931c0(Core*); rec_set_override(0x800931C0u, ov_input_dispatch_931c0); }  // per-frame input/controller-state processor (control flow owned)
    // PC-native PLAYER velocity-integrate handler (engine/engine_player.cpp): FUN_80056B48 integrates
    // speed×dir into the master position (16.16 X/Y/Z at 0x800E7EAC/B0/B4). CONTENT (post-boundary), owned
    // native; `playerverify` full RAM+scratchpad A/B gate. The settle helper 0x80054650 stays dispatched.
    { void ov_player_move(Core*); rec_set_override(0x80056B48u, ov_player_move); }
    { void ov_anim_vm_76d68(Core*); rec_set_override(0x80076D68u, ov_anim_vm_76d68); }  // per-object animation-sequence VM stepper (control flow owned; 3 frame sub-fns dispatched)
    { void ov_child_spawn_40410(Core*); rec_set_override(0x80040410u, ov_child_spawn_40410); }  // per-object child-node spawn/sub-object builder (control flow owned; allocator+setup dispatched)
    { void ov_sm40558(Core*); rec_set_override(0x80040558u, ov_sm40558); }  // per-object state-machine head (control flow owned; all sub-behaviors dispatched)
    { void ov_osc_fd10(Core*); rec_set_override(0x8003FD10u, ov_osc_fd10); }  // sm40558 STATE-1 obj[5]=0 oscillate/frame-toggle handler (control flow owned; ov_rand dispatched)
  }
  // PC-native LEVEL/STAGE LOADER (engine/engine_level.cpp): the engine's overlay loader FUN_800450bc —
  // load a stage's overlay off the disc + set its entry, synchronous (no PSX CD-wait yield).
  { void ov_load_stage(Core*); rec_set_override(0x800450BCu, ov_load_stage); }
  // PC-native STAGE TRANSITION (engine/engine_level.cpp): FUN_80052078 — load the next stage + restart the
  // task at its new entry. Thread plumbing replaced by the native scheduler (state=3 == restart); the
  // terminal yield is the existing ov_switch. Exercised at the START->DEMO->GAME boot transitions.
  { void ov_stage_transition(Core*); rec_set_override(0x80052078u, ov_stage_transition); }
  // PC-native task-0 BOOTSTRAP (engine/engine_level.cpp): FUN_800499e8 — resolve \BIN\START.BIN, record its
  // (LBA,size) in the stage table, transition to stage 0. CD-directory lookup stays the platform mechanism.
  { void ov_task0_boot(Core*); rec_set_override(0x800499E8u, ov_task0_boot); }
  // PC-native FONT/TEXT init (engine/engine_font.cpp): FUN_80075130 is called directly from native_boot,
  // but register the orchestrator + its 3 owned engine callees so any other dispatcher to them uses native
  // (the 8 libgpu/sound callees stay PSX, dispatched in-context). later (font frontier).
  { void ov_font_init(Core*), ov_font_bank_select(Core*), ov_font_bank2_store(Core*), ov_font_glyphclass_fill(Core*);
    rec_set_override(0x80075130u, ov_font_init);
    rec_set_override(0x800963a0u, ov_font_bank_select);
    rec_set_override(0x80096370u, ov_font_bank2_store);
    rec_set_override(0x800752b4u, ov_font_glyphclass_fill); }
  fps60_init();
  // cull tap: genuine-wide is the default wide path and the overlay can toggle aspect LIVE, so the
  // widened-frustum re-include is always available. ov_object_cull is a faithful super-call + a wide-only
  // re-include (no-op at 4:3).
  rec_set_override(0x8007712Cu, ov_object_cull);
  rec_set_override(0x8007778Cu, ov_cull_wrapper);    // camera-relative delta + flag reset → dispatch the cull body
  // PC-native per-object DEPTH at the render-command dispatcher (the universal chokepoint): every queued
  // render command passes through 0x8003F698 with the composed camera×object transform live in the GTE, so
  // ov_render_cmd computes the object's world-position view-depth there and tags the command's packet-pool
  // span → a 2D billboard prim occludes by real world depth, not sprite order. (Also carries the rcmd debug
  // dump when PSXPORT_DEBUG=rcmd.) Always on — one behavior. later-130/this session.
  { void ov_render_cmd(Core*); rec_set_override(0x8003F698u, ov_render_cmd); }
  // Enqueue tap (PSXPORT_DEBUG=enq): the render-command push, called per-object in phase 1 → g_current_object
  // names the source object, the attribution the downstream oracle lacks. later-131. Gated (super-call).
  if (cfg_dbg("enq")) { void ov_enqueue_probe(Core*); rec_set_override(0x80077EBCu, ov_enqueue_probe); }
  // Flush tap (PSXPORT_DEBUG=flush): dump the command-struct addresses (list+0xc0[i]) the flush drains, to
  // trace the still-open render-command enqueue. later-131. Gated (super-call).
  if (cfg_dbg("flush")) { void ov_flush_probe(Core*); rec_set_override(0x8003F174u, ov_flush_probe); }
  // Major flush tap (PSXPORT_DEBUG=flush2): the world/margin flush gen_func_8003CDD8 (later-133). Gated.
  if (cfg_dbg("flush2")) { void ov_flush2_probe(Core*); rec_set_override(0x8003CDD8u, ov_flush2_probe); }
  // Command-enqueue tap (PSXPORT_DEBUG=cmdenq): gen_func_80051B70, validates obj/(group,sub)→geomblk. later-132.
  if (cfg_dbg("cmdenq")) { void ov_cmdenq_probe(Core*); rec_set_override(0x80051B04u, ov_cmdenq_probe); }
  // Submitter call-counter (PSXPORT_DEBUG=subcnt): which un-owned submit variants fire per scene. Gated.
  if (cfg_dbg("subcnt")) { void ov_subcnt_b320(Core*), ov_subcnt_c8f4(Core*);
    rec_set_override(0x8003B320u, ov_subcnt_b320); rec_set_override(0x8003C8F4u, ov_subcnt_c8f4); }
  // Per-object dispatch case histogram (PSXPORT_DEBUG=ccase): which gen_func_8003CCA4 cases fire. Gated.
  if (cfg_dbg("ccase")) { void ov_ccase_probe(Core*); rec_set_override(0x8003CCA4u, ov_ccase_probe); }
  // Phase-2 render-walk caller counter (PSXPORT_DEBUG=rwalk): which orchestrator drives 8003CCA4. Gated.
  if (cfg_dbg("rwalk")) {
    void ov_rwalk_b588(Core*), ov_rwalk_bb50(Core*), ov_rwalk_bcf4(Core*),
         ov_rwalk_bf00(Core*), ov_rwalk_c048(Core*), ov_rwalk_eec0(Core*);
    rec_set_override(0x8003B588u, ov_rwalk_b588); rec_set_override(0x8003BB50u, ov_rwalk_bb50);
    rec_set_override(0x8003BCF4u, ov_rwalk_bcf4); rec_set_override(0x8003BF00u, ov_rwalk_bf00);
    rec_set_override(0x8003C048u, ov_rwalk_c048); rec_set_override(0x8003EEC0u, ov_rwalk_eec0);
  }
  // Render-list node-type dump (PSXPORT_DEBUG=rlist): the full type set 8003C048 must handle. Gated.
  if (cfg_dbg("rlist")) { void ov_rlist_probe(Core*); rec_set_override(0x8003C048u, ov_rlist_probe); }
  // Issue #4: own the AUXILIARY render walks PC-native (engine/engine_submit.cpp) so flame/rope/effect
  // billboards get a real WORLD-POSITION depth (gpu_obj_depth_add) instead of falling to the flat 2D band
  // and drawing over occluding foliage. Faithful per-node lift of each recomp body + per-node depth tag,
  // mirroring ov_render_walk_snapshot. Skip when PSXPORT_DEBUG=rwalk is on (the diagnostic counters above
  // own these addresses then; the override table is last-registration-wins, so this guard avoids a clash).
  if (!cfg_dbg("rwalk")) {
    void ov_rwalk_aux_bcf4(Core*), ov_rwalk_aux_bf00(Core*), ov_rwalk_aux_eec0(Core*);
    rec_set_override(0x8003BCF4u, ov_rwalk_aux_bcf4);
    rec_set_override(0x8003BF00u, ov_rwalk_aux_bf00);
    rec_set_override(0x8003EEC0u, ov_rwalk_aux_eec0);
  }
  void engine_tomba2_init(void);
  engine_tomba2_init();                            // native engine layer (Phase 1: object-list walk)
}
