// Native ownership of the engine's geometry SUBMIT path (Tomba2Engine).
//
// These are the resident routines that take an object's pre-built primitive-record list, GTE-project
// each record's model vertices, backface/frustum-cull, compute an ordering-table (OT) bucket, write the
// screen-space GPU packet, and link it into the OT. The recompiled MIPS bodies threw away the float
// view-space depth (only integer SXY survives into the packet), which is the ONLY reason the value-keyed
// "attach" measurement-hack existed (recovering depth by correlating projected SXY against memory
// stores). By owning the submit code natively we compute the projection and KEEP the real per-vertex
// view-Z, carrying it straight to the renderer's depth path — no correlation, no bridge.
//
// Faithful-first: the native routine reproduces the recomp body BYTE-FOR-BYTE (identical packets, OT
// links, packet-pool advance, cull decisions, return value), verified 0-diff vs the recomp body on real
// field gameplay. PSXPORT_SUBMIT_RECOMP=1 keeps the recomp bodies for A/B. The GTE math itself stays a
// platform primitive (gte_op → the Beetle GTE), exactly as the recomp body called it, so projection
// results are bit-identical; we own the control flow, record decode, packet assembly and OT insertion.
//
// RE (recomp bodies gen_func_8007FDB0 / gen_func_8008007C, decoded into clean form — docs/engine_re.md):
//   args: a0 = primitive-record array, a1 = OT base, a2 = record count;  returns a0 advanced past the array.
//   global packet-pool write pointer at 0x800BF544 (advanced past each committed packet).
#include "core.h"
#include "cfg.h"
#include "native_dl.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)

#define PKT_POOL_PTR 0x800BF544u   // DAT_800bf544: current free GPU-packet write pointer
#define COL_MASK     0xFFF0F0F0u   // low-nibble-per-byte clear applied to packet RGB words

// Packet write target. The submit code assembles a primitive packet either straight into GUEST RAM
// (faithful pre-port behaviour; P.guest = the OT-pool node addr, P.w = NULL) or into a NATIVE word
// buffer that becomes a NativePrim (the port default; P.w = the buffer). Offsets index packet bytes;
// the native buffer mirrors the packet byte layout so the same 8/16/32-bit accesses line up on this
// little-endian host (and the byte/halfword reads the cull does come back identical). The 1-word OT
// link node still goes to guest RAM directly (not through this) since it is the engine's own ordering.
typedef struct { uint32_t* w; uint32_t guest; Core* c; } PkTgt;
static inline void     pk_w32(PkTgt* p, uint32_t o, uint32_t v) { if (p->w) p->w[o >> 2] = v;             else p->c->mem_w32(p->guest + o, v); }
static inline void     pk_w16(PkTgt* p, uint32_t o, uint16_t v) { if (p->w) ((uint16_t*)p->w)[o >> 1] = v; else p->c->mem_w16(p->guest + o, v); }
static inline void     pk_w8 (PkTgt* p, uint32_t o, uint8_t  v) { if (p->w) ((uint8_t*)p->w)[o] = v;        else p->c->mem_w8(p->guest + o, v); }
static inline uint16_t pk_r16(PkTgt* p, uint32_t o) { return p->w ? ((uint16_t*)p->w)[o >> 1] : p->c->mem_r16(p->guest + o); }
static inline uint8_t  pk_r8 (PkTgt* p, uint32_t o) { return p->w ? ((uint8_t*)p->w)[o]       : p->c->mem_r8(p->guest + o); }

static int s_submit_recomp = -1;   // PSXPORT_SUBMIT_RECOMP=1 -> keep the recomp body (A/B)
static int submit_recomp(void) { if (s_submit_recomp < 0) s_submit_recomp = cfg_on("PSXPORT_SUBMIT_RECOMP") ? 1 : 0;
                                 return s_submit_recomp; }

// Right-edge frustum-cull threshold. The submit drops a prim only if ALL its verts are off the right of
// the screen. In 4:3 that's SX>=320 (faithful). Genuine engine-wide (gpu_vk_wide_engine) extends the
// screen to the wide width (428@16:9), so geometry projected into the [320,wide) right band is ON-screen
// and MUST NOT be dropped — widen the threshold to the wide width. THIS is why the right-side terrain was
// missing in wide: the engine's own submit culled it to 4:3. (Vertical 240 cull unchanged.) later-119.
int gpu_vk_wide_engine(void), gpu_vk_wide_engine_w(void);
static int submit_xmax(void) { return gpu_vk_wide_engine() ? gpu_vk_wide_engine_w() : 320; }

// PSXPORT_DEBUG=geomblk — geometry-record CAPTURE probe. Dumps the RAW primitive records of every geomblk
// submitted through the three natively-owned submitters (GT3 0x8007FDB0, GT4 0x8008007C, GT4bp 0x80027768).
// The raw record bytes are the canonical INPUT a native render-half must reproduce byte-for-byte (the 0-diff
// gate). stride = per-prim record size (36 GT3 / 44 GT4 / 36 GT4bp).
//
// IMPORTANT — object attribution is NOT available from here (journal later-130). Geometry SUBMISSION is a
// DEFERRED FLUSH phase, decoupled from the per-object entity walk: at the field, all owned submits run with
// g_current_object == 0 (outside any handler's node_call context) and AFTER every per-object cull. So neither
// the walk-tap (g_current_object → 0 at flush) nor the cull-tap (g_render_object → stuck at the last-culled
// object) names the geomblk's source object. We log both as weak context only; true attribution needs tapping
// the geomblk ENQUEUE site (where a handler registers its render command, while g_current_object = its node).
// At the field the owned path carries the world/map renderer's geometry; the 78 margin objects enqueue via
// UN-owned submitter variants and are invisible here. Off by default; pure logging, no state change.
extern uint32_t g_current_object, g_render_object;
extern int s_frame;
// Optional single-frame gate (PSXPORT_GEOMBLK_FRAME=<s_frame>) shared by the geomblk + rcmd probes: bound the
// firehose to one present frame so a 2900-frame headless run to the field doesn't emit gigabytes. Unset = every.
static int s_probe_frame = -2;
static inline int probe_frame_ok(void) {
  if (s_probe_frame == -2) { const char* f = cfg_str("PSXPORT_GEOMBLK_FRAME"); s_probe_frame = f ? atoi(f) : -1; }
  return s_probe_frame < 0 || s_frame == s_probe_frame;
}
static int s_geomblk = -1;
static inline int geomblk_on(void) {
  if (s_geomblk < 0) s_geomblk = cfg_dbg("geomblk") ? 1 : 0;
  return s_geomblk && probe_frame_ok();
}
static void geomblk_dump(Core* c, const char* kind, uint32_t rec, uint32_t count, uint32_t stride) {
  if (!geomblk_on()) return;
  uint32_t o = g_render_object;                    // weak hint: last-culled object (see header — not the source)
  uint32_t handler = o ? c->mem_r32(o + 0x1c) : 0;
  uint8_t  type    = o ? c->mem_r8(o + 0x0c) : 0xff;
  fprintf(stderr, "[geomblk] f%d cur=%08x lastcull=%08x type=%02x handler=%08x %s n=%u\n",
          s_frame, g_current_object, o, type, handler, kind, count);
  for (uint32_t i = 0; i < count; i++) {
    fprintf(stderr, "[geomblk]   rec%u:", i);
    for (uint32_t b = 0; b < stride; b++) fprintf(stderr, "%s%02x", (b & 3) ? "" : " ", c->mem_r8(rec + i*stride + b));
    fprintf(stderr, "\n");
  }
}

// PSXPORT_DEBUG=rcmd — RENDER-COMMAND capture probe (the complete oracle, later-130). Taps the deferred-flush
// mode dispatcher gen_func_8003F698: every queued render command passes through here as a SELF-CONTAINED unit
// — mode (*0x800BF870 → which submitter variant runs), geomblk (a0), OT base (a1), flag (a2), and the per-
// object GTE transform the flush just loaded into the GTE control regs (CR0-7: CR0-4 rotation, CR5-7
// translation). This is the full input a native render-half must reproduce, for ALL modes (incl. the overlay
// variants the margin handlers feed), not just the natively-owned GT3/GT4 path the geomblk probe decodes.
// Registered only when the channel is on (game_tomba2.c init), so zero cost otherwise; super-calls the original.
void ov_render_cmd_probe(Core* c) {
  if (cfg_dbg("rcmd") && probe_frame_ok()) {
    uint8_t mode = c->mem_r8(0x800BF870u);
    fprintf(stderr, "[rcmd] f%d mode=%02x geomblk=%08x ot=%08x flag=%08x ra=%08x M=",
            s_frame, mode, c->r[4], c->r[5], c->r[6], c->r[31]);
    for (int i = 0; i < 8; i++) fprintf(stderr, "%s%08x", i ? "," : "", (uint32_t)gte_read_ctrl(i));
    fprintf(stderr, "\n");
  }
  rec_super_call(c, 0x8003F698u);
}

// PSXPORT_DEBUG=enq — ENQUEUE tap (later-131 NEXT). The render-command PUSH gen_func_80077EBC appends its a0
// to the per-frame render list (scratchpad write-ptr 0x1F800148, count 0x1F800150, cap 40). Called by the
// per-object handlers during phase 1 (entity walk), so g_current_object names the SOURCE object — the
// attribution the rcmd/geomblk oracle can't get downstream. We dump that object + the pushed pointer a0 and
// its candidate command fields (a0+0x40 geomblk, a0+0x18 transform word) to confirm a0 IS the command struct
// and to build the object→command→geomblk map needed to enqueue margin commands natively. Super-calls original.
void ov_enqueue_probe(Core* c) {
  if (cfg_dbg("enq") && probe_frame_ok()) {
    uint32_t a0 = c->r[4], o = g_current_object;
    fprintf(stderr, "[enq] f%d obj=%08x type=%02x handler=%08x a0=%08x a0+40=%08x a0+18=%08x\n",
            s_frame, o, o ? c->mem_r8(o + 0x0c) : 0xff, o ? c->mem_r32(o + 0x1c) : 0,
            a0, c->mem_r32(a0 + 0x40), c->mem_r32(a0 + 0x18));
  }
  rec_super_call(c, 0x80077EBCu);
}

// PSXPORT_DEBUG=flush — render-command FLUSH tap (later-131 NEXT). Taps gen_func_8003F174: a0 = a command list
// whose header has the command count at +8 and a command-pointer array at +0xc0. Dumps each command's ADDRESS
// (list+0xc0[i]) + its geomblk (cmd+0x40) so the still-open render-command ENQUEUE can be traced (the writer
// of those cmd structs). The cmd address is the thing the dispatcher/rcmd tap can't see. Super-calls original.
void ov_flush_probe(Core* c) {
  if (cfg_dbg("flush") && probe_frame_ok()) {
    uint32_t list = c->r[4];
    uint32_t count = c->mem_r8(list + 8);
    fprintf(stderr, "[flush] f%d list=%08x count=%u\n", s_frame, list, count);
    for (uint32_t i = 0; i < count; i++) {
      uint32_t cmd = c->mem_r32(list + 0xc0 + i*4);
      fprintf(stderr, "[flush]   cmd[%u]=%08x geomblk=%08x x18=%08x\n",
              i, cmd, cmd ? c->mem_r32(cmd + 0x40) : 0, cmd ? c->mem_r32(cmd + 0x18) : 0);
    }
  }
  rec_super_call(c, 0x8003F174u);
}

// PSXPORT_DEBUG=flush2 — the MAJOR flush tap (gen_func_8003CDD8). later-133: the dispatcher-caller histogram
// (rcmd ra=8003d07c, 100 cmds) traced the world/margin flush to this function, NOT the minor 0x8003F174
// (24 static-decor cmds, ra=8003f230). Args: a0=command list, a1=OT base. Loop (0x8003ce40): count=list+8,
// cmd = list[0xc0 + i*4], geomblk=cmd+0x40; the GTE transform is camera(0x1f8000f8) × object-matrix(cmd+0x18)
// composed at flush, translation from cmd+0x2c/0x30/0x34. Dumps each command STRUCT address + its geomblk +
// the object-local matrix/translation so the +24 margin command structs can be located (compare ON vs OFF)
// and their stability/persistence + the per-frame append site established. Super-calls original.
void ov_flush2_probe(Core* c) {
  if (cfg_dbg("flush2") && probe_frame_ok()) {
    uint32_t list = c->r[4];
    uint32_t count = c->mem_r8(list + 8);
    fprintf(stderr, "[flush2] f%d list=%08x count=%u ot=%08x\n", s_frame, list, count, c->r[5]);
    for (uint32_t i = 0; i < count; i++) {
      uint32_t cmd = c->mem_r32(list + 0xc0 + i*4);
      uint32_t gb  = cmd ? c->mem_r32(cmd + 0x40) : 0;
      fprintf(stderr, "[flush2]   cmd[%u]=%08x geomblk=%08x objM=%08x,%08x,%08x trans=%04x,%04x,%04x\n",
              i, cmd, gb,
              cmd ? c->mem_r32(cmd + 0x18) : 0, cmd ? c->mem_r32(cmd + 0x1c) : 0, cmd ? c->mem_r32(cmd + 0x20) : 0,
              cmd ? c->mem_r16(cmd + 0x2c) : 0, cmd ? c->mem_r16(cmd + 0x30) : 0, cmd ? c->mem_r16(cmd + 0x34) : 0);
    }
  }
  rec_super_call(c, 0x8003CDD8u);
}

// PSXPORT_DEBUG=cmdenq — render-command ENQUEUE tap (later-132). gen_func_80051B70 enqueues one object's
// render command: a0=object node, a1=model group idx, a2=model sub idx. It stores the cmd ptr at node+0xc0,
// allocs via 0x8007AAE8, and resolves the geomblk from the two-level model table @0x800ECF58 via leaf
// 0x80051B04: geomblk = T + *(T + sub*4 + 4), where T = *(0x800ECF58 + group*4). We log obj + (group,sub) +
// the geomblk we compute the SAME way, to validate the decode (cross-check vs rcmd geomblks) before porting.
// Tapped at the LEAF resolver gen_func_80051B04(cmd=a0, group=a1, sub=a2): geomblk = T + *(T+sub*4+4),
// T = *(0x800ECF58 + group*4), stored to cmd+0x40. Validates the two-level model-table decode (the f2900
// commands enqueue through this leaf, not the single-object 0x80051B70). g_current_object names the object.
void ov_cmdenq_probe(Core* c) {
  if (cfg_dbg("cmdenq") && probe_frame_ok()) {
    uint32_t cmd = c->r[4], group = c->r[5], sub = c->r[6], o = g_current_object;
    uint32_t T = c->mem_r32(0x800ECF58u + group*4);
    uint32_t geomblk = T + c->mem_r32(T + sub*4 + 4);
    fprintf(stderr, "[cmdenq] f%d obj=%08x type=%02x group=%u sub=%u T=%08x geomblk=%08x cmd=%08x\n",
            s_frame, o, o ? c->mem_r8(o + 0x0c) : 0xff, group, sub, T, geomblk, cmd);
  }
  rec_super_call(c, 0x80051B04u);
}

// PC-native per-vertex depth (Phase 2): because we OWN the projection, we know each vertex's real
// view-space Z (the SZ the GTE just produced) — record it keyed by the packet vertex word's address so
// the renderer's D32 depth buffer does true per-pixel occlusion (PSXPORT_NATIVE_DEPTH / the SBS A/B
// view) instead of OT-submission order. No correlation, no value-matching: the engine that emits the
// vertex writes the depth for the exact address it stored the SXY to. Off (faithful) by default.
int  attach_enabled(void);                       // native-depth path live (gte_beetle.c)
void projprim_set_pz(uint32_t addr, float pz);   // record a vertex's view-Z at its packet word address
void proj_set_H(uint16_t h);                     // tell proj_pz_to_ord the projection-plane H (CR26)
static int s_depth = -1;
static int depth_on(void) { if (s_depth < 0) s_depth = attach_enabled() ? 1 : 0; return s_depth; }

// GTE opcodes used by the submit path (cop2 instruction encodings, matching the recomp bodies).
#define GTE_RTPS  0x4A180001u   // project 1 vertex (V0)            -> SXY2/SZ3
#define GTE_RTPT  0x4A280030u   // project 3 vertices (V0,V1,V2)    -> SXY0/1/2, SZ1/2/3
#define GTE_NCLIP 0x4B400006u   // signed area of (SXY0,SXY1,SXY2)  -> MAC0 (>0 front-facing)
#define GTE_AVSZ3 0x4B58002Du   // average Z of the 3 SZ           -> OTZ (DR7), scaled by ZSF3
#define GTE_AVSZ4 0x4B68002Eu   // average Z of the 4 SZ           -> OTZ (DR7), scaled by ZSF4

// OT-bucket depth from the SZ FIFO. `nz` SZ regs starting at DR `zbase` (tri: 3 @ DR17; quad: 4 @ DR16).
// The record's code byte selects: type 1 -> farthest (max SZ)>>2, type 2 -> nearest (min SZ)>>2,
// else -> hardware AVSZ average. (Verified by exhaustively tracing each recomp body's branch tree —
// both type paths reduce to a pure min/max over all the SZ.)
static uint32_t ot_depth(Core* c, uint32_t code, int zbase, int nz, uint32_t avsz) {
  uint32_t type = (code >> 24) & 3u;
  if (type == 1 || type == 2) {
    int32_t z = (int32_t)gte_read_data(zbase);
    for (int i = 1; i < nz; i++) { int32_t zi = (int32_t)gte_read_data(zbase + i);
      if (type == 1 ? (zi > z) : (zi < z)) z = zi; }
    return (uint32_t)(z >> 2);
  }
  gte_op(c, avsz);
  return gte_read_data(7);
}

// Logarithmic OT-bucket compression + range clamp, exactly as the recomp body. Returns the final OT
// index, or 0xFFFFFFFF (negative) if out of the drawable range [4,2047] (prim culled, not linked).
// Uses ARITHMETIC shifts to match the recomp bodies bit-for-bit when otz can be negative (the
// byte-packed variants add a signed per-call OT-Z bias before this; for the non-negative AVSZ inputs
// of the resident GT3/GT4 library this is identical to a logical shift).
static uint32_t ot_compress(uint32_t otz) {
  int32_t z = (int32_t)otz;
  int32_t sh = z >> 10;
  int32_t idx = (z >> (sh & 31)) + (sh << 9);
  if ((uint32_t)(idx - 4) < 2044u) return (uint32_t)idx;   // in range
  return 0xFFFFFFFFu;                                       // out of range -> -1 -> skip
}

// gen_func_8007FDB0 — POLY_GT3 (gouraud-textured triangle) submit.
// Record = 36 bytes: {+0 rgb0|code, +4 rgb1 (rgb2 = rgb1<<4), +8 uv0|clut, +12 uv1|tpage,
//   +16 VXY0, +20 VZ0(lo)|VZ1(hi), +24 VXY1, +28 VXY2, +32 VZ2(lo)|uv2(hi)}.
// Packet = 40 bytes POLY_GT3: {tag, rgb0|code, SXY0, uv0|clut, rgb1, SXY1, uv1|tpage, rgb2, SXY2, uv2}.
static void submit_poly_gt3(Core* c) {
  uint32_t rec = c->r[4], ot = c->r[5], count = c->r[6];
  uint32_t pkt = c->mem_r32(PKT_POOL_PTR);
  int depth = depth_on(); if (depth) proj_set_H((uint16_t)gte_read_ctrl(26));
  int dl = ndl_active();                           // build into a native prim instead of a guest packet
  geomblk_dump(c, "GT3", rec, count, 36);             // capture probe (oracle): raw records keyed by render obj
  uint32_t W[16];
  for (uint32_t i = 0; i < count; i++, rec += 36) {
    PkTgt P; P.c = c; if (dl) { memset(W, 0, sizeof W); P.w = W; P.guest = 0; } else { P.w = 0; P.guest = pkt; }
    float pz0 = 0, pz1 = 0, pz2 = 0;
    // load the 3 model vertices into the GTE input regs (V0..V2), then project all three (RTPT).
    uint32_t vz01 = c->mem_r32(rec + 20);
    gte_write_data(0, c->mem_r32(rec + 16));          // VXY0
    gte_write_data(1, vz01 & 0xFFFFu);             // VZ0
    gte_write_data(2, c->mem_r32(rec + 24));          // VXY1
    gte_write_data(3, vz01 >> 16);                 // VZ1
    gte_write_data(4, c->mem_r32(rec + 28));          // VXY2
    gte_write_data(5, c->mem_r32(rec + 32));          // VZ2 (low 16)
    uint32_t code = c->mem_r32(rec + 4);
    pk_w32(&P, 4,  c->mem_r32(rec + 0));              // rgb0|code
    gte_op(c, GTE_RTPT);
    pk_w32(&P, 12, c->mem_r32(rec + 8));              // uv0|clut
    pk_w32(&P, 24, c->mem_r32(rec + 12));             // uv1|tpage
    if ((int32_t)gte_read_ctrl(31) < 0) continue;  // GTE FLAG: projection error/overflow -> drop
    gte_op(c, GTE_NCLIP);
    pk_w32(&P, 16, code & COL_MASK);               // rgb1
    if ((int32_t)gte_read_data(24) <= 0) continue; // MAC0 = signed area <= 0 -> backface -> drop
    pk_w32(&P, 8,  gte_read_data(12));             // SXY0
    pk_w32(&P, 20, gte_read_data(13));             // SXY1
    pk_w32(&P, 32, gte_read_data(14));             // SXY2
    // frustum cull (right/bottom edges only, as the original): drop if all 3 SX>=xmax or all 3 SY>=240.
    int xmax = submit_xmax();
    uint16_t sx0 = pk_r16(&P, 8),  sx1 = pk_r16(&P, 20), sx2 = pk_r16(&P, 32);
    if (sx0 >= xmax && sx1 >= xmax && sx2 >= xmax) continue;
    uint16_t sy0 = pk_r16(&P, 10), sy1 = pk_r16(&P, 22), sy2 = pk_r16(&P, 34);
    if (sy0 >= 240 && sy1 >= 240 && sy2 >= 240) continue;
    pk_w32(&P, 28, (code << 4) & COL_MASK);        // rgb2
    uint32_t idx = ot_compress(ot_depth(c, code, 17, 3, GTE_AVSZ3));
    if ((int32_t)idx < 0) continue;                // out of OT range -> drop
    pk_w16(&P, 36, c->mem_r16(rec + 34));             // uv2 (high half of rec+32 word)
    pz0 = (float)(int32_t)gte_read_data(17);       // SXY0 -> SZ0  (native per-vertex view-Z)
    pz1 = (float)(int32_t)gte_read_data(18);       // SXY1 -> SZ1
    pz2 = (float)(int32_t)gte_read_data(19);       // SXY2 -> SZ2
    uint32_t otaddr = ot + (idx << 2);
    if (dl) {
      // NATIVE ORDERING: append to the host per-bucket list keyed by the OT anchor — write NO guest OT/pool.
      NativePrim* np = ndl_alloc(otaddr & 0x1FFFFC);
      if (np) { np->nwords = 9; np->npz = 3;
        for (int k = 0; k < 9; k++) np->words[k] = W[k + 1];   // W[1..9] = the 9 GP0 payload words
        np->pz[0] = pz0; np->pz[1] = pz1; np->pz[2] = pz2; }
    } else {
      c->mem_w32(pkt + 0, c->mem_r32(otaddr) | 0x09000000u);  // tag: link old head + length (9 words)
      c->mem_w32(otaddr, pkt);                        // OT head -> this packet
      if (depth) {                                 // record each vertex's real view-Z at its packet addr
        projprim_set_pz(pkt + 8,  pz0); projprim_set_pz(pkt + 20, pz1); projprim_set_pz(pkt + 32, pz2);
      }
      pkt += 40;
    }
  }
  if (!dl) c->mem_w32(PKT_POOL_PTR, pkt);           // native ordering writes no guest pool
  c->r[2] = rec;                                   // return: record pointer advanced past the array
}

void ov_submit_poly_gt3(Core* c) {
  if (submit_recomp() || cfg_on("PSXPORT_GT3_RECOMP")) { rec_super_call(c, 0x8007FDB0u); return; }
  submit_poly_gt3(c);
}

// gen_func_8008007C — POLY_GT4 (gouraud-textured quad) submit.
// Record = 44 bytes: {+0 rgb0(rgb1=<<4), +4 rgb2(rgb3=<<4), +8 uv0|clut, +12 uv1|tpage,
//   +16 uv2(lo)|uv3(hi), +20 VXY0, +24 VZ0(lo)|VZ1(hi), +28 VXY1, +32 VXY2, +36 VZ2(lo)|VZ3(hi), +40 VXY3}.
// Packet = 52 bytes POLY_GT4: {tag, rgb0,SXY0,uv0|clut, rgb1,SXY1,uv1|tpage, rgb2,SXY2,uv2, rgb3,SXY3,uv3}.
// The 4th vertex (V3) is projected alone via RTPS first, then the front tri (V0,V1,V2) via RTPT.
static void submit_poly_gt4(Core* c) {
  uint32_t rec = c->r[4], ot = c->r[5], count = c->r[6];
  uint32_t pkt = c->mem_r32(PKT_POOL_PTR);
  int depth = depth_on(); if (depth) proj_set_H((uint16_t)gte_read_ctrl(26));
  int dl = ndl_active();
  geomblk_dump(c, "GT4", rec, count, 44);             // capture probe (oracle): raw records keyed by render obj
  uint32_t W[16];
  for (uint32_t i = 0; i < count; i++, rec += 44) {
    PkTgt P; P.c = c; if (dl) { memset(W, 0, sizeof W); P.w = W; P.guest = 0; } else { P.w = 0; P.guest = pkt; }
    float pz0 = 0, pz1 = 0, pz2 = 0, pz3 = 0;
    // project the lone 4th vertex (V3) first (RTPS): result SXY in DR14.
    uint32_t code2 = c->mem_r32(rec + 4);
    gte_write_data(0, c->mem_r32(rec + 40));          // VXY3
    gte_write_data(1, c->mem_r32(rec + 36) >> 16);    // VZ3
    pk_w32(&P, 28, code2 & COL_MASK);              // rgb2
    gte_op(c, GTE_RTPS);
    pk_w32(&P, 40, (code2 << 4) & COL_MASK);       // rgb3
    pk_w32(&P, 24, c->mem_r32(rec + 12));             // uv1|tpage
    if ((int32_t)gte_read_ctrl(31) < 0) continue;  // GTE FLAG: V3 projection error -> drop
    pk_w32(&P, 44, gte_read_data(14));             // SXY3
    // project the front triangle (V0,V1,V2) via RTPT.
    uint32_t vz01 = c->mem_r32(rec + 24);
    gte_write_data(0, c->mem_r32(rec + 20));          // VXY0
    gte_write_data(1, vz01 & 0xFFFFu);             // VZ0
    gte_write_data(3, vz01 >> 16);                 // VZ1
    gte_write_data(2, c->mem_r32(rec + 28));          // VXY1
    gte_write_data(4, c->mem_r32(rec + 32));          // VXY2
    gte_write_data(5, c->mem_r32(rec + 36) & 0xFFFFu);// VZ2
    uint32_t code0 = c->mem_r32(rec + 0);
    pk_w32(&P, 4, code0 & COL_MASK);               // rgb0
    gte_op(c, GTE_RTPT);
    pk_w32(&P, 16, (code0 << 4) & COL_MASK);       // rgb1
    if ((int32_t)gte_read_ctrl(31) < 0) continue;  // GTE FLAG -> drop
    gte_op(c, GTE_NCLIP);
    pk_w32(&P, 12, c->mem_r32(rec + 8));              // uv0|clut
    if ((int32_t)gte_read_data(24) <= 0) continue; // backface (front-tri signed area <= 0) -> drop
    pk_w32(&P, 8,  gte_read_data(12));             // SXY0
    pk_w32(&P, 20, gte_read_data(13));             // SXY1
    pk_w32(&P, 32, gte_read_data(14));             // SXY2
    // frustum cull (right/bottom edges) over all 4 verts.
    int xmax = submit_xmax();
    uint16_t sx0 = pk_r16(&P, 8), sx1 = pk_r16(&P, 20), sx2 = pk_r16(&P, 32), sx3 = pk_r16(&P, 44);
    if (sx0 >= xmax && sx1 >= xmax && sx2 >= xmax && sx3 >= xmax) continue;
    uint16_t sy0 = pk_r16(&P, 10), sy1 = pk_r16(&P, 22), sy2 = pk_r16(&P, 34), sy3 = pk_r16(&P, 46);
    if (sy0 >= 240 && sy1 >= 240 && sy2 >= 240 && sy3 >= 240) continue;
    uint32_t idx = ot_compress(ot_depth(c, code2, 16, 4, GTE_AVSZ4));  // 4 SZ in DR16..19
    if ((int32_t)idx < 0) continue;                // out of OT range -> drop
    uint32_t uv23 = c->mem_r32(rec + 16);
    pk_w32(&P, 36, uv23);                           // uv2 (low half used by GPU)
    pk_w32(&P, 48, uv23 >> 16);                     // uv3
    pz0 = (float)(int32_t)gte_read_data(17);        // SXY0 -> SZ0  (SZ FIFO: DR16=SZ3, DR17-19=SZ0-2)
    pz1 = (float)(int32_t)gte_read_data(18);        // SXY1 -> SZ1
    pz2 = (float)(int32_t)gte_read_data(19);        // SXY2 -> SZ2
    pz3 = (float)(int32_t)gte_read_data(16);        // SXY3 -> SZ3
    uint32_t otaddr = ot + (idx << 2);
    if (dl) {
      // NATIVE ORDERING: append to the host per-bucket list keyed by the OT anchor — write NO guest OT/pool.
      NativePrim* np = ndl_alloc(otaddr & 0x1FFFFC);
      if (np) { np->nwords = 12; np->npz = 4;
        for (int k = 0; k < 12; k++) np->words[k] = W[k + 1];  // W[1..12] = the 12 GP0 payload words
        np->pz[0] = pz0; np->pz[1] = pz1; np->pz[2] = pz2; np->pz[3] = pz3; }
    } else {
      c->mem_w32(pkt + 0, c->mem_r32(otaddr) | 0x0C000000u);  // tag: link old head + length (12 words)
      c->mem_w32(otaddr, pkt);                        // OT head -> this packet
      if (depth) {
        projprim_set_pz(pkt + 8,  pz0); projprim_set_pz(pkt + 20, pz1);
        projprim_set_pz(pkt + 32, pz2); projprim_set_pz(pkt + 44, pz3);
      }
      pkt += 52;
    }
  }
  if (!dl) c->mem_w32(PKT_POOL_PTR, pkt);           // native ordering writes no guest pool
  c->r[2] = rec;                                   // return: record pointer advanced past the array
}

void ov_submit_poly_gt4(Core* c) {
  if (submit_recomp() || cfg_on("PSXPORT_GT4_RECOMP")) { rec_super_call(c, 0x8008007Cu); return; }
  submit_poly_gt4(c);
}

// =====================================================================================================
// Byte-packed POLY_GT4 submit variant — gen_func_80027768 (resident MAIN). A DISTINCT submitter from
// the GT3/GT4 library above: it is the field's dominant world-poly emitter (~252 GT4/frame) and ran
// interpreted, so it carried no native depth. Decoded from the recomp body (docs/engine_re.md):
//   ABI:  a0 = record array; a1 = CLUT-Y bank offset (added <<22 to the uv0|clut word);
//         a2 = OT-Z bias (sign-extended s16, added to AVSZ4 OTZ before the log-compress);
//         a3 = U-texture offset (added to the U byte of all four uv words).
//   NO count arg: the loop runs record-by-record and continues while the record's CONTROL word
//   (rec+4) is > 0 (its sign marks the last record, which is still drawn).
//   OT base is a GLOBAL (*0x800ED8C8), NOT an arg. IR0 depth-cue factor is read from scratchpad
//   0x1F800090 each iteration. Returns r2 = 0x800C0000 (the body leaves the pool-base reg there).
// Record (36 bytes): vertex X/Y/Z are SIGNED bytes scaled <<8 (a u16 GTE coord); the top byte of each
//   RGB word doubles as that vertex's Z, so X/Y live in rec[0x1C..0x23] and Z in the RGB words:
//     rec+0x00 uv0|clut(word) ->pkt+12   rec+0x04 control: low23 -> pkt+24 (uv1|clut), bit30 = semi-trans,
//     value>0 = more records follow      rec+0x08 uv2(lo)|uv3(hi) -> pkt+36/+48
//     rec+0x0C rgb0(+VZ0 in top byte) rec+0x10 rgb1(+VZ1) rec+0x14 rgb2(+VZ2) rec+0x18 rgb3(+VZ3)
//     X: rec+0x1C=VX0 0x1D=VX1 0x20=VX2 0x21=VX3   Y: 0x1E=VY0 0x1F=VY1 0x22=VY2 0x23=VY3
//     Z (top byte of the rgb word): 0x0F=VZ0 0x13=VZ1 0x17=VZ2 0x1B=VZ3
//   Colors: DPCT depth-cues rgb0/rgb1/rgb2, DPCS depth-cues rgb3 (both toward FAR_COLOR via IR0).
// Faithful-first: this reproduces the recomp body's writes/cull/return exactly (0-diff gate); when the
// native-depth path is live it additionally records each vertex's real view-Z (the SZ FIFO) keyed by
// its packet SXY-word address, exactly like the GT3/GT4 library above.
#define OTBASE_PTR   0x800ED8C8u             // *this = the active ordering-table base for these variants
#define IR0_SCRATCH  0x1F800090u             // depth-cue interpolation factor (IR0) staged in scratchpad
#define GTE_DPCT     0x4AF8002Au             // depth-cue 3 colors (rgb0,rgb1,rgb2) toward FAR_COLOR
#define GTE_DPCS     0x4A780010u             // depth-cue 1 color  (rgb)            toward FAR_COLOR

// byte b at `addr`, scaled <<8 into a 16-bit GTE coordinate (signedness is irrelevant after the 16-bit
// truncation: (b<<8)&0xFFFF == (sext8(b)<<8)&0xFFFF for any byte b — the recomp body sext's some lanes
// and not others, all yielding the same halfword).
static inline uint32_t vcoord(Core* c, uint32_t addr) { return ((uint32_t)c->mem_r8(addr) << 8) & 0xFFFFu; }
// add the per-call U offset (a3) to the low (U) byte of a packet uv word, in place (mod 256).
static inline void uoff_add(PkTgt* p, uint32_t off, uint32_t uoff) {
  pk_w8(p, off, (uint8_t)(pk_r8(p, off) + uoff));
}

static void submit_poly_gt4_bp(Core* c) {
  uint32_t rec = c->r[4];
  uint32_t xoff = c->r[5] << 22;                       // CLUT-Y bank offset
  int32_t  zoff = (int32_t)(int16_t)c->r[6];           // OT-Z bias (sign-extended)
  uint32_t uoff = c->r[7];                             // U-texture offset
  uint32_t pkt  = c->mem_r32(PKT_POOL_PTR);
  uint32_t otbase = c->mem_r32(OTBASE_PTR);
  int depth = depth_on(); if (depth) proj_set_H((uint16_t)gte_read_ctrl(26));
  int dl = ndl_active();
  if (geomblk_on()) {                              // count the control-terminated record list, then dump
    uint32_t n = 0, r = rec; for (;;) { n++; if ((int32_t)c->mem_r32(r + 4) <= 0) break; r += 36; }
    geomblk_dump(c, "GT4bp", rec, n, 36);
  }
  uint32_t W[16];
  for (;;) {
    PkTgt P; P.c = c; if (dl) { memset(W, 0, sizeof W); P.w = W; P.guest = 0; } else { P.w = 0; P.guest = pkt; }
    uint32_t ctl = c->mem_r32(rec + 4);                   // control word (sign = last record)
    // front triangle (V0,V1,V2) -> RTPT
    gte_write_data(0, vcoord(c, rec + 0x1C) | (vcoord(c, rec + 0x1E) << 16));  // VXY0
    gte_write_data(1, vcoord(c, rec + 0x0F));                                // VZ0
    gte_write_data(2, vcoord(c, rec + 0x1D) | (vcoord(c, rec + 0x1F) << 16));  // VXY1
    gte_write_data(3, vcoord(c, rec + 0x13));                                // VZ1
    gte_write_data(4, vcoord(c, rec + 0x20) | (vcoord(c, rec + 0x22) << 16));  // VXY2
    gte_write_data(5, vcoord(c, rec + 0x17));                                // VZ2
    gte_op(c, GTE_RTPT);
    if ((int32_t)gte_read_ctrl(31) >= 0) {
      pk_w32(&P, 8,  gte_read_data(12));               // SXY0
      pk_w32(&P, 20, gte_read_data(13));               // SXY1
      pk_w32(&P, 32, gte_read_data(14));               // SXY2
      // 4th vertex (V3) -> RTPS
      gte_write_data(0, vcoord(c, rec + 0x21) | (vcoord(c, rec + 0x23) << 16)); // VXY3
      gte_write_data(1, vcoord(c, rec + 0x1B));                               // VZ3
      pk_w32(&P, 12, c->mem_r32(rec + 0) + xoff);         // uv0|clut + CLUT bank
      gte_op(c, GTE_RTPS);
      if ((int32_t)gte_read_ctrl(31) >= 0) {
        pk_w32(&P, 44, gte_read_data(14));             // SXY3
        pk_w32(&P, 24, ctl & 0x7FFFFFu);               // uv1|clut (control low 23 bits)
        gte_op(c, GTE_AVSZ4);
        uint32_t idx = ot_compress((uint32_t)((int32_t)gte_read_data(7) + zoff));
        if ((int32_t)idx >= 0) {
          uint32_t uv = c->mem_r32(rec + 8);
          pk_w32(&P, 36, uv);                          // uv2
          pk_w32(&P, 48, (uint32_t)((int32_t)uv >> 16)); // uv3 (sign-extended high half)
          uoff_add(&P, 12, uoff); uoff_add(&P, 24, uoff);
          uoff_add(&P, 36, uoff); uoff_add(&P, 48, uoff);
          // depth-cued colors: DPCT(rgb0,rgb1,rgb2) then DPCS(rgb3)
          gte_write_data(8,  c->mem_r32(IR0_SCRATCH));    // IR0 (depth-cue factor)
          gte_write_data(20, c->mem_r32(rec + 0x0C));     // RGB0
          gte_write_data(21, c->mem_r32(rec + 0x10));     // RGB1
          gte_write_data(22, c->mem_r32(rec + 0x14));     // RGB2
          gte_write_data(6,  c->mem_r32(rec + 0x14));     // RGBC (= rgb2; written by the body)
          gte_op(c, GTE_DPCT);
          pk_w32(&P, 4,  gte_read_data(20));           // rgb0 out
          pk_w32(&P, 16, gte_read_data(21));           // rgb1 out
          pk_w32(&P, 28, gte_read_data(22));           // rgb2 out
          pk_w8(&P, 7, (ctl & 0x40000000u) ? 0x3E : 0x3C);  // code: semi-trans vs opaque GT4
          gte_write_data(6, c->mem_r32(rec + 0x18));      // RGB3 in
          gte_op(c, GTE_DPCS);
          pk_w32(&P, 40, gte_read_data(22));           // rgb3 out
          float pz0 = (float)(int32_t)gte_read_data(16);   // SZ FIFO: DR16..19 = SZ0..3
          float pz1 = (float)(int32_t)gte_read_data(17);
          float pz2 = (float)(int32_t)gte_read_data(18);
          float pz3 = (float)(int32_t)gte_read_data(19);
          uint32_t otaddr = otbase + (idx << 2);
          if (dl) {
            // NATIVE ORDERING: append to the host per-bucket list keyed by the OT anchor — no guest OT/pool write.
            NativePrim* np = ndl_alloc(otaddr & 0x1FFFFC);
            if (np) { np->nwords = 12; np->npz = 4;
              for (int k = 0; k < 12; k++) np->words[k] = W[k + 1];
              np->pz[0] = pz0; np->pz[1] = pz1; np->pz[2] = pz2; np->pz[3] = pz3; }
          } else {
            c->mem_w32(pkt + 0, c->mem_r32(otaddr) | 0x0C000000u);  // tag: link old head + len 12 (GT4)
            c->mem_w32(otaddr, pkt);
            if (depth) {
              projprim_set_pz(pkt + 8,  pz0); projprim_set_pz(pkt + 20, pz1);
              projprim_set_pz(pkt + 32, pz2); projprim_set_pz(pkt + 44, pz3);
            }
            pkt += 52;
          }
        }
      }
    }
    if ((int32_t)ctl <= 0) break;                      // control sign marks the last record
    rec += 36;
  }
  if (!dl) c->mem_w32(PKT_POOL_PTR, pkt);           // native ordering writes no guest pool
  c->r[2] = 0x800C0000u;                               // return value the recomp body leaves in r2
}

void ov_submit_poly_gt4_bp(Core* c) {
  if (submit_recomp() || cfg_on("PSXPORT_GT4BP_RECOMP")) { rec_super_call(c, 0x80027768u); return; }
  submit_poly_gt4_bp(c);
}

// =====================================================================================================
// NATIVE PER-OBJECT RENDER FLUSH — gen_func_8003CDD8 (THE world/margin render submission, later-133).
//
// This is the heart of "make it a PC game": the engine's per-object render — composing the camera ×
// object-local transform and dispatching each object's persistent render-command list to the geometry
// submitter — reimplemented in native C so NO guest render code runs (no gen_func_8003CDD8, no
// gen_func_8003F698 dispatcher, no gen_func_800803DC) and NO guest packet/VRAM is touched beyond the
// 1-word OT ordering node the native submitters already own. Decoded byte-for-byte from the recomp body
// (docs/engine_re.md "Deferred render pipeline" / journal later-133):
//
//   gen_func_8003CDD8(a0=node, a1=flag): for each render command in the node's persistent list
//   (count at node+8 / node+9, cmd-ptr ARRAY at node+0xc0[i]):
//     - geomblk = cmd+0x40; skip the command if it is 0.
//     - COMPOSE the GTE transform: camera-rotation (scratch 0x1F8000F8 → CR0-4) × the object-local
//       matrix (cmd+0x18, 3 columns at +0x18/+0x1a/+0x1c, each col 3 halfwords at +0,+6,+0xc) via one
//       MVMVA (0x4A49E012, mx=0 rotation, v=3 IR vector) per column → composed rotation matrix.
//     - TRANSFORM the object translation (cmd+0x2c/0x30/0x34) by the camera (MVMVA 0x4A486012, v=0 V0)
//       then ADD the camera translation offset (scratch 0x1F80010C/110/114) → composed translation.
//     - Load the composed rotation into CR0-4 and translation into CR5-7.
//     - Dispatch geomblk to the per-mode renderer with OT base *0x800ED8C8 (+cmd[0x3f]*4 when
//       node[0xd]&0xf == 4) and the flush flag.
//
// The MVMVA matrix math stays a platform primitive (gte_op → the Beetle GTE), exactly as the recomp
// body called it, so the composed CR0-7 are bit-identical. The scratchpad temps (0x1F8000xx) are the
// SAME the recomp body uses — pure CPU scratch, not render packet/VRAM. The dispatch routes the common
// world path natively (native_dispatch → native_gt3gt4 → the native ov_submit_poly_gt3/gt4 above);
// the per-scene OVERLAY submitter variants (mode-table entries other than the GT3/GT4 path) are NOT yet
// owned, so for those modes the original per-mode renderer is invoked (rec_dispatch) — the documented
// next RE target (engine_re "OPEN — full field depth coverage"). A/B: PSXPORT_PEROBJ_RECOMP=1.
#define SCR          0x1F800000u             // PSX scratchpad base (the engine's GTE-compose temp area)
#define MODE_BYTE    0x800BF870u             // *this = render-mode select (DAT_800bf870, 0..0x15)
#define MODE_FORCE   0x1F800234u             // *this != 0 forces the generic GT3/GT4 path
#define MODE_TABLE   0x80015268u             // 22-entry jump table: mode → per-mode renderer
#define MVMVA_ROTCOL 0x4A49E012u             // MVMVA: camera-rot(CR0-4) × IR vector → composed col
#define MVMVA_TRANS  0x4A486012u             // MVMVA: camera-rot × V0 (object translation)

void rec_dispatch(Core*, uint32_t);         // interpret/run a guest fn (unowned overlay-variant modes)

// gen_func_800803DC's first body (the generic GT3/GT4 renderer): split the geomblk's packed prim counts
// (low16 tri, high16 quad), point past the 16-byte header to the record array, and run the two native
// submitters in sequence (tri-submit returns the advanced record pointer = the quad array base).
static void native_gt3gt4(Core* c, uint32_t geomblk, uint32_t otbase) {
  uint32_t counts = c->mem_r32(geomblk + 0);
  c->r[4] = geomblk + 16; c->r[5] = otbase; c->r[6] = counts & 0xFFFFu;
  ov_submit_poly_gt3(c);
  c->r[4] = c->r[2];      c->r[5] = otbase; c->r[6] = counts >> 16;
  ov_submit_poly_gt4(c);
}

// gen_func_8003F698: route geomblk to the per-mode renderer. The generic GT3/GT4 path (forced flag /
// force-byte / mode≥22 / a table entry that IS gen_func_800803DC) is owned natively; any other mode
// (a per-scene overlay submitter variant we don't own yet) runs its original per-mode renderer.
// PSXPORT_DEBUG=pdisp — per-mode dispatch coverage: count native (generic GT3/GT4) vs fallback
// (unowned overlay-variant) dispatches per present frame, and which mode/target the fallbacks use.
// Tells us how much of the field's render still flows through interpreted per-mode renderers (the next
// ownership target). Pure counting, no state change.
static void pdisp_count(int native, uint32_t mode, uint32_t tgt) {
  static int s_pd = -1; if (s_pd < 0) s_pd = cfg_dbg("pdisp") ? 1 : 0;
  if (!s_pd) return;
  static int last_f = -1, nat = 0, fb = 0; static uint32_t fbmode[32] = {0};
  if (s_frame != last_f) {
    if (last_f >= 0) {
      fprintf(stderr, "[pdisp] f%d native=%d fallback=%d", last_f, nat, fb);
      for (int m = 0; m < 32; m++) if (fbmode[m]) fprintf(stderr, " m%d=%u", m, fbmode[m]);
      fprintf(stderr, "\n");
    }
    last_f = s_frame; nat = fb = 0; for (int m = 0; m < 32; m++) fbmode[m] = 0;
  }
  if (native) nat++; else { fb++; if (mode < 32) fbmode[mode]++; }
}

// The mode dispatcher gen_func_8003F698 indexes a 22-entry table @0x80015268 by *0x800BF870; the entries
// are the dispatcher's OWN internal case-label addresses (0x8003F6xx), NOT renderer function pointers —
// each label then calls the real per-mode renderer. The GENERIC GT3/GT4 path is the label 0x8003F788
// (modes 3 + 9..0x13, and the force/flag/mode≥22 early-outs all branch here → func_800803DC). So we own
// the generic path natively (native_gt3gt4) ONLY when it is provably generic; for any other mode we MUST
// run the real dispatcher gen_func_8003F698 (it owns its internal jump table + the per-mode renderers we
// haven't lifted) — dispatching to the raw table entry ourselves would jump into the middle of 8003F698
// and execute garbage (the f434 margin hang, found via gdb on the live process).
#define DISPATCH_GENERIC 0x8003F788u             // mode-table entry = the generic GT3/GT4 case label
static void native_dispatch(Core* c, uint32_t geomblk, uint32_t otbase, uint32_t flag) {
  if (c->mem_r8(MODE_FORCE) != 0 || (flag & 1u)) { pdisp_count(1, 0, 0); native_gt3gt4(c, geomblk, otbase); return; }
  uint32_t mode = c->mem_r8(MODE_BYTE);
  if (mode >= 22) { pdisp_count(1, mode, 0); native_gt3gt4(c, geomblk, otbase); return; }
  uint32_t tgt = c->mem_r32(MODE_TABLE + mode * 4);
  if (tgt == DISPATCH_GENERIC) { pdisp_count(1, mode, tgt); native_gt3gt4(c, geomblk, otbase); return; }
  pdisp_count(0, mode, tgt);
  c->r[4] = geomblk; c->r[5] = otbase; c->r[6] = flag;   // unowned mode — run the REAL dispatcher
  rec_dispatch(c, 0x8003F698u);
}

static void submit_perobj_flush(Core* c) {
  uint32_t node = c->r[4], flag = c->r[5];
  if (c->mem_r8(node + 8) == 0) return;
  if (c->mem_r8(node + 9) == 0) return;
  uint32_t otbase_ptr = c->mem_r32(OTBASE_PTR);              // *0x800ED8C8
  int i = 0;
  while (i < (int)c->mem_r8(node + 8)) {
    uint32_t cmd = c->mem_r32(node + 0xC0 + i * 4);
    uint32_t geomblk = c->mem_r32(cmd + 0x40);
    if (geomblk == 0) goto next;
    {  // block-scope the compose locals so `goto next` doesn't cross their initialization (C++)
    // HOST-MEMORY compose (never writes guest RAM): all intermediates are C locals; the only writes are
    // to the GTE control/data registers (hardware, not guest RAM). Reads from guest (camera matrix, the
    // object cmd matrix/translation, the camera translation offset) are fine.
    // camera rotation (guest scratch 0x1F8000F8, 5 words, READ) → CR0-4
    gte_write_ctrl(0, c->mem_r32(SCR + 0xF8)); gte_write_ctrl(1, c->mem_r32(SCR + 0xFC));
    gte_write_ctrl(2, c->mem_r32(SCR + 0x100)); gte_write_ctrl(3, c->mem_r32(SCR + 0x104));
    gte_write_ctrl(4, c->mem_r32(SCR + 0x108));
    // compose camera-rotation × object-local matrix, one MVMVA per column (cmd+0x18/+0x1a/+0x1c) → host
    uint16_t hm[9];   // composed rotation, 3 cols interleaved: hm[col]=row0, hm[3+col]=row1, hm[6+col]=row2
    for (int col = 0; col < 3; col++) {
      uint32_t cc = cmd + 0x18 + col;
      gte_write_data(9,  c->mem_r16(cc + 0));
      gte_write_data(10, c->mem_r16(cc + 6));
      gte_write_data(11, c->mem_r16(cc + 12));
      gte_op(c, MVMVA_ROTCOL);
      hm[col]     = (uint16_t)gte_read_data(9);
      hm[3 + col] = (uint16_t)gte_read_data(10);
      hm[6 + col] = (uint16_t)gte_read_data(11);
    }
    // transform the object translation (cmd+0x2c/30/34) by the camera, then add the camera translation
    gte_write_data(0, (uint32_t)c->mem_r16(cmd + 0x2C) | ((uint32_t)c->mem_r16(cmd + 0x30) << 16));  // VXY0
    gte_write_data(1, (uint32_t)c->mem_r16(cmd + 0x34));                                            // VZ0
    gte_op(c, MVMVA_TRANS);
    uint32_t trx = gte_read_data(25) + c->mem_r32(SCR + 0x10C);
    uint32_t try_ = gte_read_data(26) + c->mem_r32(SCR + 0x110);
    uint32_t trz = gte_read_data(27) + c->mem_r32(SCR + 0x114);
    // load the composed transform from host: rotation → CR0-4 (packed halfwords), translation → CR5-7
    gte_write_ctrl(0, (uint32_t)hm[0] | ((uint32_t)hm[1] << 16));
    gte_write_ctrl(1, (uint32_t)hm[2] | ((uint32_t)hm[3] << 16));
    gte_write_ctrl(2, (uint32_t)hm[4] | ((uint32_t)hm[5] << 16));
    gte_write_ctrl(3, (uint32_t)hm[6] | ((uint32_t)hm[7] << 16));
    gte_write_ctrl(4, (uint32_t)hm[8]);
    gte_write_ctrl(5, trx); gte_write_ctrl(6, try_); gte_write_ctrl(7, trz);
    // OT base: node[0xd]&0xf == 4 selects a per-command sub-bucket (cmd[0x3f]*4 offset), else the base
    uint32_t otbase = otbase_ptr;
    if ((c->mem_r8(node + 0xD) & 0xF) == 4)
      otbase = otbase_ptr + (((int32_t)(int8_t)c->mem_r8(cmd + 0x3F)) << 2);
    native_dispatch(c, geomblk, otbase, flag);
    }
  next:
    i++;
    if (i >= (int)c->mem_r8(node + 9)) break;
  }
}

void ov_perobj_flush(Core* c) {
  if (cfg_on("PSXPORT_PEROBJ_RECOMP")) { rec_super_call(c, 0x8003CDD8u); return; }
  submit_perobj_flush(c);
}

// NATIVE per-object render DISPATCH — gen_func_8003CCA4 (later-135). The phase-2 per-object render entry:
// stash the current render object (scratch 0x1F80028C), compute the flush flag (= node[0xb]==0xf, the
// "world" objects), select a case by idx = node[0xd]&0xb (idx>=9 → not rendered), and for the common
// flush-only case (jump-table target 0x8003CD00) run the native per-object flush — NO guest render code.
// The other cases add a secondary effect pass (gen_func_8003D584/8003F344/8003F3F4/8003F4C4/8003F594)
// over the just-emitted packet range; those are not owned yet, so for them we super-call the recomp body
// (which still calls the now-native func_8003CDD8 for the flush, then the secondary pass). At the field
// only idx0 (flush-only) fires (PSXPORT_DEBUG=ccase: 1 call/frame, target 0x8003cd00). A/B: PEROBJ_RECOMP.
static void submit_perobj_render(Core* c) {
  uint32_t node = c->r[4];
  c->mem_w32(SCR + 0x28C, node);                          // current render object (read by downstream code)
  uint32_t idx = c->mem_r8(node + 0xD) & 0xB;
  if (idx >= 9) return;                                // not rendered
  uint32_t flag = (c->mem_r8(node + 0xB) == 0xF) ? 1u : 0u;
  uint32_t tgt = c->mem_r32(0x80014EC8u + idx * 4);
  if (tgt == 0x8003CD00u) {                            // flush-only case → native flush, no guest render
    c->r[4] = node; c->r[5] = flag; submit_perobj_flush(c);
    return;
  }
  rec_super_call(c, 0x8003CCA4u);                      // case w/ a secondary effect pass — not owned yet
}
void ov_perobj_render(Core* c) {
  if (cfg_on("PSXPORT_PEROBJ_RECOMP")) { rec_super_call(c, 0x8003CCA4u); return; }
  submit_perobj_render(c);
}

// NATIVE phase-2 render-list WALK — gen_func_8003C048 (later-135). The master draining of the per-frame
// render list: iterate the linked list (head *0x800F2624, next at node+36), skip non-live nodes
// (node+1==0), and dispatch each live node by its render type (node+0xb, <33) through the 33-entry jump
// table @0x80014DB8 to a per-type renderer. This is the engine's "entity-list iteration → render
// submission" — the native-engine layer named in the project goal.
//
// Faithful-first + own-when-fully-handleable: we PRE-SCAN the live nodes; if EVERY one resolves to a
// case we own natively, run the native walk; otherwise super-call the whole recomp body (so an unfamiliar
// scene is always correct, never a fragile partial). The two cases that fire at the field
// (PSXPORT_DEBUG=rlist):
//   - table target 0x8003C0B4 = the per-object render case → native submit_perobj_render(node).
//   - table target 0x8003C29C = the default case → rec_dispatch(node, *(node+24)) (the node's own render
//     fn ptr; its leaf submit is owned where owned). Identical to the recomp default block.
// Types ≥33 render nothing (the recomp skips them) → treated as handled no-ops. A/B: PEROBJ_RECOMP.
#define RLIST_HEAD   0x800F2624u
#define RLIST_TABLE  0x80014DB8u
#define RCASE_PEROBJ 0x8003C0B4u             // jump-table target = the gen_func_8003CCA4 (per-object) case
#define RCASE_DEFAULT 0x8003C29Cu            // jump-table target = the default case: rec_dispatch(node+24)

static void submit_render_walk(Core* c) {
  uint32_t head = c->mem_r32(RLIST_HEAD);
  if (head == 0) return;
  // pre-scan: bail to the recomp body if any live node uses a case we don't own natively.
  for (uint32_t n = head, g = 0; n && g < 256; n = c->mem_r32(n + 36), g++) {
    if (c->mem_r8(n + 1) == 0) continue;
    uint8_t t = c->mem_r8(n + 0xB);
    if (t >= 33) continue;                            // renders nothing (recomp skips) — handled
    uint32_t tgt = c->mem_r32(RLIST_TABLE + t * 4);
    if (tgt != RCASE_PEROBJ && tgt != RCASE_DEFAULT) { rec_super_call(c, 0x8003C048u); return; }
  }
  // native walk: read `next` before dispatch (the recomp captures node+36 before the case runs).
  for (uint32_t n = head; n; ) {
    uint32_t next = c->mem_r32(n + 36);
    if (c->mem_r8(n + 1) != 0) {
      uint8_t t = c->mem_r8(n + 0xB);
      if (t < 33) {
        uint32_t tgt = c->mem_r32(RLIST_TABLE + t * 4);
        if (tgt == RCASE_PEROBJ) { c->r[4] = n; submit_perobj_render(c); }
        else                     { c->r[4] = n; rec_dispatch(c, c->mem_r32(n + 24)); }  // default case
      }
    }
    n = next;
  }
}
void ov_render_walk(Core* c) {
  if (cfg_on("PSXPORT_PEROBJ_RECOMP")) { rec_super_call(c, 0x8003C048u); return; }
  submit_render_walk(c);
}

// NATIVE field TERRAIN renderer — gen_func_8002AB5C (later-135). The render fn (node+24) of the field's
// t32 render-list node: the bulk map/terrain geometry. Interpreted-only (reached via fn-ptr; seeded into
// the RE set). Decoded from the recomp body — it is structurally the per-object flush specialised for the
// terrain strip: set the depth-cue, build the object matrix (euler + a secondary sway), compose it with
// the camera via the same MVMVA columns as 8003CDD8, then submit the terrain prim records through the
// already-owned byte-packed submitter 0x80027768. The matrix-build leaves (80085480 euler→matrix,
// 80084520 secondary rotate) and the submit stay platform primitives (rec_dispatch / the owned override),
// exactly as the recomp body calls them; we own the orchestration, scratch writes and GTE compose.
//   - FarColor (CR21-23) = 0 (fog toward black); IR0 depth-cue factor (0x1F800090, read by 80027768) =
//     (128 - node[78]) << 5.
//   - two sway angle bytes at 0x800A2014/2016 = (node[64]*node[80])>>11 and (node[66]*node[80])>>11.
//   - terrain geomblk = 0x800A1AE8 (fixed per-frame record buffer); a1=a2=a3=0.
#define A2_PARAM     0x800A2014u             // 3-byte sway-angle param scratch (engine global)
#define IR0_STAGE    0x1F800090u             // IR0 depth-cue factor staged for the 0x80027768 submitter
#define MVMVA_TERRAIN_GEOMBLK 0x800A1AE8u    // terrain prim-record buffer (a0 to 80027768)
static void submit_terrain(Core* c) {
  uint32_t node = c->r[4];
  // depth-cue: FarColor=0, IR0 factor staged for the submitter
  gte_write_ctrl(21, 0); gte_write_ctrl(22, 0); gte_write_ctrl(23, 0);
  uint32_t ir0 = (uint32_t)((128 - (int16_t)c->mem_r16(node + 78)) << 5);
  int32_t a80 = (int16_t)c->mem_r16(node + 80);
  // HOST: the two sway-angle bytes are terrain-internal temps — compute them in host (no 0x800A2014
  // main-RAM writes). The middle byte is set elsewhere → READ it (mem_r8). They scale <<2 into the
  // secondary-rotation args. (IR0 + the SCR matrix-build args still go to guest scratch until the
  // submitter/matrix-build sub-fns are host — see the function note.)
  uint8_t sway0 = (uint8_t)(((int32_t)(int16_t)c->mem_r16(node + 64) * a80) >> 11);
  uint8_t sway2 = (uint8_t)(((int32_t)(int16_t)c->mem_r16(node + 66) * a80) >> 11);
  uint8_t sway1 = c->mem_r8(A2_PARAM + 1);                 // external (read-only here)
  c->mem_w32(IR0_STAGE, ir0);
  // build object rotation matrix at scratch SCR from the node's euler angles (node+84/86/88)
  c->r[4] = node + 84; c->r[5] = SCR; rec_dispatch(c, 0x80085480u);
  // secondary sway rotation by the host-computed angle bytes (scaled <<2)
  c->mem_w32(SCR + 0x1C0, (uint32_t)sway0 << 2);
  c->mem_w32(SCR + 0x1C4, (uint32_t)sway1 << 2);
  c->mem_w32(SCR + 0x1C8, (uint32_t)sway2 << 2);
  c->r[4] = SCR; c->r[5] = SCR + 0x1C0; rec_dispatch(c, 0x80084520u);
  // HOST-MEMORY compose: read the object matrix the build wrote (SCR cols, READ), compose with the camera
  // in host C locals, load only the GTE regs (hardware). No guest-RAM writes here.
  gte_write_ctrl(0, c->mem_r32(SCR + 0xF8)); gte_write_ctrl(1, c->mem_r32(SCR + 0xFC));
  gte_write_ctrl(2, c->mem_r32(SCR + 0x100)); gte_write_ctrl(3, c->mem_r32(SCR + 0x104));
  gte_write_ctrl(4, c->mem_r32(SCR + 0x108));
  uint16_t hm[9];
  for (int col = 0; col < 3; col++) {
    uint32_t cc = SCR + col * 2;
    gte_write_data(9,  c->mem_r16(cc + 0));
    gte_write_data(10, c->mem_r16(cc + 6));
    gte_write_data(11, c->mem_r16(cc + 12));
    gte_op(c, MVMVA_ROTCOL);
    hm[col]     = (uint16_t)gte_read_data(9);
    hm[3 + col] = (uint16_t)gte_read_data(10);
    hm[6 + col] = (uint16_t)gte_read_data(11);
  }
  // transform the object translation (node+72/76) by the camera, add the camera translation offset
  gte_write_data(0, c->mem_r32(node + 72)); gte_write_data(1, c->mem_r32(node + 76));
  gte_op(c, MVMVA_TRANS);
  uint32_t trx = gte_read_data(25) + c->mem_r32(SCR + 0x10C);
  uint32_t try_ = gte_read_data(26) + c->mem_r32(SCR + 0x110);
  uint32_t trz = gte_read_data(27) + c->mem_r32(SCR + 0x114);
  // load composed rotation → CR0-4 (packed host halfwords), translation → CR5-7
  gte_write_ctrl(0, (uint32_t)hm[0] | ((uint32_t)hm[1] << 16));
  gte_write_ctrl(1, (uint32_t)hm[2] | ((uint32_t)hm[3] << 16));
  gte_write_ctrl(2, (uint32_t)hm[4] | ((uint32_t)hm[5] << 16));
  gte_write_ctrl(3, (uint32_t)hm[6] | ((uint32_t)hm[7] << 16));
  gte_write_ctrl(4, (uint32_t)hm[8]);
  gte_write_ctrl(5, trx); gte_write_ctrl(6, try_); gte_write_ctrl(7, trz);
  // submit the terrain prim records via the owned byte-packed GT4 submitter
  c->r[4] = MVMVA_TERRAIN_GEOMBLK; c->r[5] = 0; c->r[6] = 0; c->r[7] = 0;
  rec_dispatch(c, 0x80027768u);
}
void ov_terrain(Core* c) {
  if (cfg_on("PSXPORT_PEROBJ_RECOMP")) { rec_super_call(c, 0x8002AB5Cu); return; }
  submit_terrain(c);
}

// NATIVE per-object TRANSFORM BUILD — gen_func_80051C8C (later-135). Build the object's render matrix
// cache at node+0x98 each frame: seed an identity (0x1000 diagonal), apply the three euler rotations
// (node+0x54/56/58 via the matrix primitives 80084D10/EB0/85050), set the translation node+0xac/b0/b4
// from the gameplay position node+0x2e/32/36, then propagate to the command struct (80051464). The
// per-object flush (8003CDD8) reads this matrix (cmd+0x18); the widescreen margin calls this before its
// render (its last remaining guest call). Interpreted-only (fn-ptr reached) → seeded + decoded. We own
// the orchestration + memory writes; the rotation/propagate leaves stay primitives (rec_dispatch), as
// the recomp body calls them. A/B: PSXPORT_PEROBJ_RECOMP=1.
static void build_xform(Core* c) {
  uint32_t node = c->r[4];
  c->mem_w32(node + 152, 0x1000); c->mem_w32(node + 156, 0); c->mem_w32(node + 160, 0x1000);
  c->mem_w32(node + 164, 0);      c->mem_w32(node + 168, 0x1000); c->mem_w32(node + 172, 0);
  c->mem_w32(node + 176, 0);      c->mem_w32(node + 180, 0);
  c->r[4] = (uint32_t)(int16_t)c->mem_r16(node + 84); c->r[5] = node + 152; rec_dispatch(c, 0x80084D10u);
  c->r[4] = (uint32_t)(int16_t)c->mem_r16(node + 86); c->r[5] = node + 152; rec_dispatch(c, 0x80084EB0u);
  c->r[4] = (uint32_t)(int16_t)c->mem_r16(node + 88); c->r[5] = node + 152; rec_dispatch(c, 0x80085050u);
  c->mem_w32(node + 172, (uint32_t)(int16_t)c->mem_r16(node + 46));
  c->mem_w32(node + 176, (uint32_t)(int16_t)c->mem_r16(node + 50));
  c->mem_w32(node + 180, (uint32_t)(int16_t)c->mem_r16(node + 54));
  c->r[4] = node; rec_dispatch(c, 0x80051464u);
}
void ov_build_xform(Core* c) {
  if (cfg_on("PSXPORT_PEROBJ_RECOMP")) { rec_super_call(c, 0x80051C8Cu); return; }
  build_xform(c);
}

// PSXPORT_DEBUG=subcnt — submitter call-counter. Registered (super-call) on candidate submit fns to see
// which actually fire per scene + how often, so the un-owned variants worth porting are picked by data,
// not guesswork. One slot per registered address; per-present-frame counts flushed on frame change.
static uint32_t s_subcnt[8], s_subaddr[8]; static int s_subn, s_sub_lastf = -1;
static void subcnt_tick(Core* c, uint32_t addr) {
  int slot = -1; for (int i = 0; i < s_subn; i++) if (s_subaddr[i] == addr) { slot = i; break; }
  if (slot < 0 && s_subn < 8) { slot = s_subn; s_subaddr[s_subn++] = addr; }
  if (s_frame != s_sub_lastf) {
    if (s_sub_lastf >= 0) { fprintf(stderr, "[subcnt] f%d", s_sub_lastf);
      for (int i = 0; i < s_subn; i++) { fprintf(stderr, " %08x=%u", s_subaddr[i], s_subcnt[i]); s_subcnt[i] = 0; } }
    s_sub_lastf = s_frame;
  }
  if (slot >= 0) s_subcnt[slot]++;
  rec_super_call(c, addr);
}
void ov_subcnt_b320(Core* c) { subcnt_tick(c, 0x8003B320u); }
void ov_subcnt_c8f4(Core* c) { subcnt_tick(c, 0x8003C8F4u); }

// PSXPORT_DEBUG=rwalk — phase-2 render-walk caller counter. The per-object render dispatch
// gen_func_8003CCA4 is driven by one of several orchestrators (the render-layer/list drainers); count
// which fire per scene so the phase-2 flush walk worth owning next is picked by data. Super-calls.
void ov_rwalk_b588(Core* c) { subcnt_tick(c, 0x8003B588u); }
void ov_rwalk_bb50(Core* c) { subcnt_tick(c, 0x8003BB50u); }
void ov_rwalk_bcf4(Core* c) { subcnt_tick(c, 0x8003BCF4u); }
void ov_rwalk_bf00(Core* c) { subcnt_tick(c, 0x8003BF00u); }
void ov_rwalk_c048(Core* c) { subcnt_tick(c, 0x8003C048u); }
void ov_rwalk_eec0(Core* c) { subcnt_tick(c, 0x8003EEC0u); }

// PSXPORT_DEBUG=rlist — dump the gen_func_8003C048 render-list node TYPES (node+0xb) + jump-table target
// (0x80014DB8[type]) for every node in the list, so we know the full type set the field's phase-2 flush
// walk must handle to be ownable natively. Walk: head *0x800F2624, skip node+1==0, advance node+36. Once
// per frame (first call), then super-calls the original walk.
void ov_rlist_probe(Core* c) {
  static int s_rl_last = -1;
  if (s_frame != s_rl_last) {            // 1/frame gate (own latch)
    s_rl_last = s_frame;
    fprintf(stderr, "[rlist] f%d:", s_frame);
    uint32_t n = c->mem_r32(0x800F2624u); int guard = 0;
    for (; n && guard < 64; n = c->mem_r32(n + 36), guard++) {
      uint8_t live = c->mem_r8(n + 1), t = c->mem_r8(n + 0xB);
      uint32_t tgt = (t < 33) ? c->mem_r32(0x80014DB8u + t * 4) : 0;
      uint32_t rfn = c->mem_r32(n + 24);      // the node's own render fn ptr (used by the default case)
      fprintf(stderr, " [%08x l%u t%u→%08x rfn=%08x]", n, live, t, tgt, rfn);
    }
    fprintf(stderr, "\n");
  }
  rec_super_call(c, 0x8003C048u);
}

// PSXPORT_DEBUG=ccase — gen_func_8003CCA4 case histogram. The per-object render dispatch selects a
// secondary effect pass by idx = node[0xd]&0xb (idx>=9 = no render). Logs the idx + the jump-table
// target (0x80014ec8[idx]) so we know which cases (and which secondary-pass fns) actually fire at the
// field — i.e. whether owning 8003CCA4 natively needs the secondary passes or is flush-only. Super-calls.
static uint32_t s_cc[12], s_cctgt[12]; static int s_cc_lastf = -1;
void ov_ccase_probe(Core* c) {
  uint32_t node = c->r[4];
  uint32_t idx = c->mem_r8(node + 0xD) & 0xB;
  if (s_frame != s_cc_lastf) {
    if (s_cc_lastf >= 0) { fprintf(stderr, "[ccase] f%d", s_cc_lastf);
      for (int i = 0; i < 12; i++) if (s_cc[i]) fprintf(stderr, " idx%d=%u(t=%08x)", i, s_cc[i], s_cctgt[i]);
      fprintf(stderr, "\n"); }
    s_cc_lastf = s_frame; for (int i = 0; i < 12; i++) s_cc[i] = 0;
  }
  if (idx < 12) { s_cc[idx]++; s_cctgt[idx] = (idx < 9) ? c->mem_r32(0x80014EC8u + idx * 4) : 0; }
  rec_super_call(c, 0x8003CCA4u);
}

// --- Auto-ownership of the SAME submit library when it appears in a runtime-loaded overlay --------
// The two resident submit fns above are also present, the same ALGORITHM (verified — only register
// allocation / scheduling / relocated internal calls differ), in per-area render overlays at
// addresses REUSED across scenes (e.g. the field's resident-region GAME overlay). They run
// interpreted and so don't carry native depth. Rather than hardcode per-scene addresses (fragile: a
// later scene can load different code there), the loader calls rec_overlay_loaded(base,size) after
// each overlay copy; we scan the freshly-loaded bytes ONCE for the submit library's signature and
// register the matching native impl at each entry (scan-on-load — no per-call cost). The override is
// cleared on the next overlay load, so an address is only ever owned while the real submit code is
// resident. Signature is POLY_GT3/GT4-specific (packet-pool load + RTPT + the OT tag-length
// immediate 9/12 words), so other primitive submitters (flat/sprite/line) won't match.
// The generic GT3/GT4 CALLER (gen_func_800803DC and its per-scene overlay twins, e.g. the mode-0 render
// 0x80146478): splits a geomblk's packed prim counts and runs the tri-submit then quad-submit. Its
// submitters are already owned (scanned), so this just runs them natively instead of interpreting the
// ~10-insn wrapper. a0 = geomblk, a1 = OT base — exactly native_gt3gt4's args.
void ov_gt3gt4_caller(Core* c) { native_gt3gt4(c, c->r[4], c->r[5]); }

// Detect the generic-caller signature in [addr, jr ra): the distinctive triple is `addiu a0,a0,16`
// (skip the 16-byte geomblk header) + `andi a2,s0,0xffff` (tri count) + `srl a2,s0,16` (quad count).
static int classify_caller(Core* c, uint32_t addr) {
  int hdr = 0, tri = 0, quad = 0;
  for (int i = 0; i < 64; i++) {
    uint32_t w = c->mem_r32(addr + i * 4);
    if (w == 0x03E00008u) break;             // jr $ra
    if (w == 0x24840010u) hdr = 1;           // addiu a0,a0,16
    if (w == 0x3206FFFFu) tri = 1;           // andi a2,s0,0xffff
    if (w == 0x00103402u) quad = 1;          // srl  a2,s0,16
  }
  return hdr && tri && quad;
}

static OverrideFn classify_submit(Core* c, uint32_t addr) {
  int has_pool = 0, has_rtpt = 0, gt3 = 0, gt4 = 0, prev_lui800c = 0;
  for (int i = 0; i < 320; i++) {            // entry .. first jr $ra (single epilogue in these fns)
    uint32_t w = c->mem_r32(addr + i * 4);
    if (w == 0x03E00008u) break;             // jr $ra -> function end
    if ((w >> 26) == 0x0Fu && (w & 0xFFFFu) == 0x800Cu) { prev_lui800c = 1; continue; }   // lui $r,0x800C
    if (prev_lui800c && (w >> 26) == 0x23u && (w & 0xFFFFu) == 0xF544u) has_pool = 1;      // lw $r,-0xABC(.) = &DAT_800bf544
    prev_lui800c = 0;
    if (w == 0x4A280030u) has_rtpt = 1;                                  // RTPT (project the front tri)
    if ((w >> 26) == 0x0Fu && (w & 0xFFFFu) == 0x0900u) gt3 = 1;         // lui $r,0x0900 -> POLY_GT3 tag len 9
    if ((w >> 26) == 0x0Fu && (w & 0xFFFFu) == 0x0C00u) gt4 = 1;         // lui $r,0x0C00 -> POLY_GT4 tag len 12
  }
  if (!has_pool || !has_rtpt) return 0;
  return gt4 ? ov_submit_poly_gt4 : gt3 ? ov_submit_poly_gt3 : 0;
}
// Scan a just-loaded overlay [base,base+size) for submit-library fns. The packet-pool load
// (lui $r,0x800C ; lw $r2,-0xABC(...)) appears only in the submit fns; for each, backtrack to the
// function entry (after the previous fn's `jr $ra`+delay slot) and classify+register there.
static void engine_scan_overlay(Core* c, uint32_t base, uint32_t size) {
  uint32_t lo = base & ~3u, hi = (base + size) & ~3u;
  void ov_gt3gt4_caller(Core*);
  for (uint32_t a = lo; a + 4 <= hi; a += 4) {
    // (1) generic GT3/GT4 CALLER (e.g. the mode-0 render 0x80146478): anchor on `addiu a0,a0,16`,
    //     backtrack to the fn entry, verify the caller signature, own it (runs the native submitters).
    if (c->mem_r32(a) == 0x24840010u && !cfg_on("PSXPORT_NO_CALLER_OWN")) {
      uint32_t entry = lo;
      for (uint32_t b = a; b > lo && b > a - 64; b -= 4)
        if (c->mem_r32(b - 4) == 0x03E00008u) { entry = b + 4; break; }
      if (classify_caller(c, entry)) {
        rec_set_interp_override_auto(entry, ov_gt3gt4_caller);
        if (cfg_dbg("submit"))
          fprintf(stderr, "[submit] own overlay CALLER @ 0x%08X (in load 0x%08X+0x%X)\n", entry, base, size);
      }
      continue;
    }
    if ((c->mem_r32(a) >> 26) != 0x0Fu || (c->mem_r32(a) & 0xFFFFu) != 0x800Cu) continue;   // lui $r,0x800C
    if ((c->mem_r32(a + 4) >> 26) != 0x23u || (c->mem_r32(a + 4) & 0xFFFFu) != 0xF544u) continue; // lw ...,0xF544
    uint32_t entry = lo;                     // backtrack to the fn entry = just past previous `jr $ra`
    for (uint32_t b = a; b > lo && b > a - 64; b -= 4)
      if (c->mem_r32(b - 4) == 0x03E00008u) { entry = b + 4; break; }  // (b-4)=jr ra, (b)=delay slot, entry=b+4
    OverrideFn fn = classify_submit(c, entry);
    if (fn) {
      rec_set_interp_override_auto(entry, fn);
      if (cfg_dbg("submit"))
        fprintf(stderr, "[submit] own overlay %s @ 0x%08X (in load 0x%08X+0x%X)\n",
                fn == ov_submit_poly_gt4 ? "GT4" : "GT3", entry, base, size);
    }
  }
}
void engine_submit_register_autodetect(void) {
  if (submit_recomp()) return;               // A/B: keep all overlay submitters interpreted too
  if (cfg_on("PSXPORT_NO_OVERLAY_OWN")) return;   // A/B: measure overlay-ownership depth contribution
  rec_set_overlay_load_hook(engine_scan_overlay);
}
