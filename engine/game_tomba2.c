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
#include "r3000.h"
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>

void rec_super_call(R3000*, uint32_t);   // interpret the original PSX body (super-call / A/B oracle)
void fps60_frame_commit(void);    // fps60: per-logic-frame fence (rate detect / interp)
void fps60_init(void);            // fps60: read PSXPORT_FPS60
extern uint32_t g_current_object;  // fps60: object* whose RTP ops are being tagged
extern int g_fps60_on;            // fps60: capture enabled (PSXPORT_FPS60)
void gpu_present(void);            // native GPU: present the displayed VRAM region
void gpu_pace_frame(void);         // native GPU: throttle to game pace when windowed (no-op headless)
void spu_audio_frame(void);        // SPU: advance the mixer one frame + feed the audio device
void rec_dispatch(R3000*, uint32_t);  // hybrid call: recomp body if emitted, else interpret

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

static void ov_frame_update(R3000* c) {
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
  int quota = mem_r8(VBLANK_QUOTA); if (quota < 1) quota = 1;
  uint32_t seqfn = mem_r32(SEQ_FUNC_PTR);
  int seq_ok = !cfg_on("PSXPORT_T2_NOSEQTICK")
               && (seqfn & 0x1FFFFFFFu) >= 0x10000u && (seqfn & 0x1FFFFFFFu) < 0x200000u;
  for (int v = 0; v < quota; v++) {                  // once per VBlank this logic frame spans
    if (seq_ok) rec_dispatch(c, SEQ_TICK_WRAPPER);   // libsnd per-vblank tick (user cb + SsSeqCalled)
    spu_audio_frame();                               // advance SPU one 1/60 s field + feed device
  }
  mem_w16(DISPLAY_COUNTER, mem_r8(VBLANK_QUOTA));    // satisfy the pacing dwell immediately
  // fps60 (when enabled) OWNS presentation: it presents the previous real frame + the interpolated
  // frame (60 fps, 1 frame behind) and paces both halves — see fps60_present. The faithful path
  // presents frame B once and paces a full frame.
  fps60_frame_commit();
  if (!g_fps60_on) { gpu_present(); gpu_pace_frame(); }
}

// fps60 object tag: the universal per-object cull/LOD dispatcher (a0 = object*, once per logic
// frame for every live drawable). Every RTP op fired in its call tree is tagged with this object's
// stable pool-pointer id (the join key). Super-call the recomp body unchanged; clear on exit.
// PSXPORT_OBJLOG=1: dump every object the cull dispatcher visits (addr + type@+0xc +
// pos@+0x2e/32/36). Empirically maps the active-object pool/list for the native entity
// manager (Phase 1) — more reliable than static-tracing the overlay handler dispatch.
extern uint8_t mem_r8(uint32_t);
extern void    mem_w8(uint32_t, uint8_t);
static int s_objlog = -1;
static inline uint16_t obj_r16(uint32_t a) { return (uint16_t)(mem_r8(a) | (mem_r8(a + 1) << 8)); }

// Extended culling (PSXPORT_CULL=1): the game's FUN_8007712c culls each object by distance AND by a
// FOV cone (depth/dist < ~0x370 ≈ ±77°). That over-culls — distant objects pop in, and in widescreen
// the wider edges get dropped. After the game's cull runs, re-include objects it dropped that are
// within an EXTENDED distance + WIDER FOV cone (just mark visible@+1 + return 1). RE: docs/engine_re.md.
static int s_cull = -1, s_cull_far, s_cull_fov;
static unsigned isqrt32(unsigned v) { unsigned r = 0, b = 1u << 30; while (b > v) b >>= 2;
  while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return r; }

static void ov_object_cull(R3000* c) {
  uint32_t prev = g_current_object;
  uint32_t o = c->r[4];                            // a0 = object* (MIPS arg register $a0)
  g_current_object = o;
  if (s_objlog < 0) s_objlog = cfg_dbg("obj") ? 1 : 0;
  if (s_objlog)
    fprintf(stderr, "[objlog] obj=%08x type=%02x pos=(%d,%d,%d)\n", o, mem_r8(o + 0x0c),
            (int16_t)obj_r16(o + 0x2e), (int16_t)obj_r16(o + 0x32), (int16_t)obj_r16(o + 0x36));
  int p2 = (int16_t)c->r[5], p3 = (int16_t)c->r[6], p4 = (int16_t)c->r[7];   // pos - camera (s16 each)
  rec_super_call(c, 0x8007712Cu);                  // the game's cull (sets +1 visible flag, queues)
  if (s_cull < 0) {
    // Widescreen widens the horizontal FOV ~1.34x, so the re-include cone+distance MUST widen to match
    // or the new edge/corner geometry (incl. the static terrain/water tiles, which also go through this
    // per-object cull) is dropped -> black wedges. Couple the defaults to PSXPORT_WIDE: wide => widest
    // cone (fov 0) + extended far; plain CULL keeps the conservative 4:3 values. Both env-overridable.
    int wide = cfg_str("PSXPORT_WIDE") && atoi(cfg_str("PSXPORT_WIDE")) != 0;
    s_cull = (cfg_on("PSXPORT_CULL") || wide) ? 1 : 0;
    const char* f = cfg_str("PSXPORT_CULL_FAR"); s_cull_far = f ? atoi(f) : (wide ? 0x8000 : 0x6000);
    const char* v = cfg_str("PSXPORT_CULL_FOV"); s_cull_fov = v ? atoi(v) : (wide ? 0x00 : 0x80); }
  if (s_cull && mem_r8(o + 1) == 0) {              // the game CULLED it — reconsider with extended bounds
    unsigned dist = isqrt32((unsigned)(p2*p2 + p3*p3 + p4*p4)) & 0xFFFF;
    if (dist >= 0x200 && dist <= (unsigned)s_cull_far) {   // keep near/behind culling intact
      int fx = (int16_t)obj_r16(0x1F8000E8), fy = (int16_t)obj_r16(0x1F8000EA), fz = (int16_t)obj_r16(0x1F8000EC);
      long depth = (long)fx*p2 + (long)fy*p3 + (long)fz*p4, den = ((long)dist * 0x1000) >> 10;
      if (den < 1) den = 1;
      if (depth / den >= s_cull_fov) { mem_w8(o + 1, 1); c->r[2] = 1; }   // re-include: mark visible
    }
  }
  g_current_object = prev;
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
static uint32_t lz_decompress(uint32_t desc, uint32_t dst, uint32_t src0, uint32_t srclen) {
  const uint32_t src_end = src0 + srclen;
  const int32_t stride = (int16_t)mem_r16(desc + 4);
  int32_t offtab[8];
  for (int i = 0; i < 8; i++) {
    const int32_t base   = (int32_t)mem_r32(LZ_OFFTAB_BASE + i * 8 + 0);
    const int32_t factor = (int32_t)mem_r32(LZ_OFFTAB_BASE + i * 8 + 4);
    offtab[i] = base + 2 * (factor * stride);
  }
  uint32_t src = src0, out = dst;
  while (src < src_end) {
    const uint8_t ctrl = mem_r8(src++);
    const uint32_t len = ctrl >> 3, mode = ctrl & 7u;
    if (mode != 0) {                                  // back-reference into the output so far
      uint32_t bsrc = out + (uint32_t)offtab[mode];
      for (uint32_t k = 0; k < len; k++) mem_w8(out++, mem_r8(bsrc++));
    } else {                                          // literal run from the source
      if (len == 0) break;                            // terminator
      for (uint32_t k = 0; k < len; k++) mem_w8(out++, mem_r8(src++));
    }
  }
  return out - dst;                                   // total bytes written
}
static void ov_lz_decompress(R3000* c) {
  c->r[2] = lz_decompress(c->r[4], c->r[5], c->r[6], c->r[7]);
}

// PC-owned texture-group unpacker — replaces recompiled FUN_80044E84 (0x80044E84). Verified by
// disassembly: a0 = descriptor table base, a1 = scratch-end anchor (0x1FD000). Layout: [count:4]
// then [pad:4] then `count` 12-byte entries each { stride:2(@+4 from entry head), field:2(@+6),
// srclen:4(@+8) }; source data starts 0x800 after the table base and advances by srclen per entry.
// For each entry: dst = anchor - 2*stride*field (outputs stack ending at the anchor — transient
// scratch), decompress the entry's image there, then upload it (FUN_80081218) and run the post
// step (FUN_80080f6c). Non-gameplay (asset unpack) → PC-owned, calling the native decompressor
// directly; the two gfx-library sub-calls still route through the recomp/dispatch for now.
void rec_dispatch(R3000*, uint32_t);
static void ov_unpack_group(R3000* c) {
  const uint32_t table = c->r[4], anchor = c->r[5];
  const int32_t count = (int32_t)mem_r32(table);
  uint32_t entry = table + 4;            // first 12-byte descriptor entry
  uint32_t src = table + 0x800;          // packed source data follows the table
  int dbg = cfg_dbg("unpack") != 0;
  if (dbg) fprintf(stderr, "[unpack] table=0x%08X count=%d src0=0x%08X ra=0x%08X\n",
                   table, count, src, c->r[31]);
  // PSXPORT_UNPACKDUMP=dir — dump the LIVE compressed input (table + 0x30000 bytes) the moment the
  // unpacker reads it, sequence-numbered, so it can be checked against the disc / oracle exactly.
  { const char* dd = cfg_str("PSXPORT_UNPACKDUMP");
    if (dd) { static int seq = 0; char p[300]; snprintf(p, sizeof p, "%s/unpack_%03d_c%d.bin", dd, seq++, count);
      FILE* uf = fopen(p, "wb"); if (uf) { extern uint8_t g_ram[];
        fwrite(&g_ram[table & 0x1FFFFF], 1, 0x30000, uf); fclose(uf);
        fprintf(stderr, "[unpack] dumped live input -> %s (table=0x%08X count=%d)\n", p, table, count); } } }
  for (int32_t i = 0; i < count; i++) {
    const uint32_t desc   = entry;
    const int32_t  stride = (int16_t)mem_r16(desc + 4);
    const int32_t  field  = (int16_t)mem_r16(desc + 6);
    const uint32_t srclen = mem_r32(desc + 8);
    const uint32_t dst    = anchor - (uint32_t)(2 * stride * field);
    if (dbg) fprintf(stderr, "[unpack]  e%d dst=(%d,%d) %dx%d src=0x%08X len=%u srcbytes:"
                     " %02X %02X %02X %02X\n", i, (int16_t)mem_r16(desc), (int16_t)mem_r16(desc+2),
                     stride, field, src, srclen, mem_r8(src), mem_r8(src+1), mem_r8(src+2), mem_r8(src+3));
    lz_decompress(desc, dst, src, srclen);            // native decompress into transient scratch
    src   += srclen;
    entry += 12;
    c->r[4] = desc; c->r[5] = dst; rec_dispatch(c, 0x80081218u);  // FUN_80081218(desc, dst): upload
    c->r[4] = 0;                         rec_dispatch(c, 0x80080F6Cu);  // FUN_80080f6c(0): post step
  }
}

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
// reads a slot mid-overwrite. A/B: PSXPORT_LZ_RECOMP=1 keeps the recomp upload library.
void gpu_native_load_image(int x, int y, int w, int h, uint32_t src);
static void ov_upload_image(R3000* c) {
  const uint32_t desc = c->r[4], src = c->r[5];
  const int x = (int16_t)mem_r16(desc + 0), y = (int16_t)mem_r16(desc + 2);
  const int w = (int16_t)mem_r16(desc + 4), h = (int16_t)mem_r16(desc + 6);
  if (w > 0 && h > 0) gpu_native_load_image(x, y, w, h, src);
}

// --- Native ownership of the GTE projection setters (libgte) -------------------------------------
// The engine configures its projection via libgte SetGeomOffset/SetGeomScreen (RE: docs/engine_re.md,
// FUN_800509B4 -> screen center (160,120), focal length H=350). We reimplement them in native C
// (byte-identical to gen_func_800846D0/800846F0) so the PROJECTION is ours: this is the genuine
// widescreen FOV lever (widen OFX + the draw-env clip; no squish, no renderer re-center) and the
// reference point the 60fps/hi-res paths build on. Faithful-first: PSXPORT_GEOM_RECOMP=1 keeps the
// recomp bodies for A/B. A one-time log prints the configured projection to confirm equivalence.
static void ov_set_geom_offset(R3000* c) {       // SetGeomOffset(ofx, ofy)
  uint32_t ofx = c->r[4], ofy = c->r[5];
  gte_write_ctrl(24, ofx << 16);                 // OFX
  gte_write_ctrl(25, ofy << 16);                 // OFY
  static int logged = 0;
  if (!logged++) fprintf(stderr, "[geom] native SetGeomOffset OFX=%u OFY=%u (CR24=%08X CR25=%08X)\n",
                         ofx, ofy, gte_read_ctrl(24), gte_read_ctrl(25));
}
static void ov_set_geom_screen(R3000* c) {       // SetGeomScreen(h) — projection-plane distance (FOV)
  gte_write_ctrl(26, c->r[4]);                   // H
  static int logged = 0;
  if (!logged++) fprintf(stderr, "[geom] native SetGeomScreen H=%u (CR26=%08X)\n", c->r[4], gte_read_ctrl(26));
}

// Native ownership of DrawOTag (libgpu FUN_80081560, the per-frame draw kick): the recomp body just
// programs the GPU linked-list DMA to walk the ordering table at a0 — which our renderer already does
// natively in gpu_dma2_linked_list (walk OT -> decode each primitive -> rasterize). Overriding it routes
// the draw straight through our native walk (synchronous), instead of the DMA-register emulation dance.
// This is the engine's draw submission, owned. Faithful-first: PSXPORT_OT_RECOMP=1 keeps the recomp body.
void gpu_dma2_linked_list(uint32_t madr);
static void ov_draw_otag(R3000* c) { gpu_dma2_linked_list(c->r[4]); }

void games_tomba2_init(void) {
  rec_set_override(0x800788ACu, ov_frame_update);
  if (!cfg_on("PSXPORT_GEOM_RECOMP")) {           // own the GTE projection setup natively (faithful-first)
    rec_set_override(0x800846D0u, ov_set_geom_offset);
    rec_set_override(0x800846F0u, ov_set_geom_screen);
  }
  if (!cfg_on("PSXPORT_OT_RECOMP"))               // own DrawOTag (the per-frame draw kick) natively
    rec_set_override(0x80081560u, ov_draw_otag);
  // PC-owned asset codecs (A/B: PSXPORT_LZ_RECOMP=1 keeps the recomp bodies for comparison).
  if (!cfg_on("PSXPORT_LZ_RECOMP")) {
    rec_set_override(0x80044D8Cu, ov_lz_decompress);  // LZ image decompressor
    rec_set_override(0x80044E84u, ov_unpack_group);   // texture-group unpacker (drives the above)
    rec_set_override(0x80081218u, ov_upload_image);   // PC-native CPU->VRAM upload (libgs upload lib)
  }
  if (!cfg_on("PSXPORT_SUBMIT_RECOMP")) {          // own the geometry submit path natively (faithful-first)
    void ov_submit_poly_gt3(R3000*), ov_submit_poly_gt4(R3000*), ov_submit_poly_gt4_bp(R3000*),
         engine_submit_register_autodetect(void);
    rec_set_override(0x8007FDB0u, ov_submit_poly_gt3);   // POLY_GT3 (gouraud-textured triangle) submit
    rec_set_override(0x8008007Cu, ov_submit_poly_gt4);   // POLY_GT4 (gouraud-textured quad) submit
    rec_set_override(0x80027768u, ov_submit_poly_gt4_bp);// byte-packed POLY_GT4 (field's dominant emitter)
    engine_submit_register_autodetect();                 // + own the same library in runtime-loaded overlays
  }
  fps60_init();
  if (g_fps60_on || cfg_dbg("obj") || cfg_on("PSXPORT_CULL"))   // cull tap: fps60 / objlog / extended-cull
    rec_set_override(0x8007712Cu, ov_object_cull);
  void engine_tomba2_init(void);
  engine_tomba2_init();                            // native engine layer (Phase 1: object-list walk)
}
