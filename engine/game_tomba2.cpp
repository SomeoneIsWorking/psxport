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
  rec_super_call(c, 0x8007712Cu);                  // the game's cull (sets +1 visible flag, queues)
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
static void ov_draw_otag(Core* c) {
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
