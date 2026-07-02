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
// field gameplay. The GTE math itself stays a
// platform primitive (gte_op → the Beetle GTE), exactly as the recomp body called it, so projection
// results are bit-identical; we own the control flow, record decode, packet assembly and OT insertion.
//
// RE (recomp bodies gen_func_8007FDB0 / gen_func_8008007C, decoded into clean form — docs/engine_re.md):
//   args: a0 = primitive-record array, a1 = OT base, a2 = record count;  returns a0 advanced past the array.
//   global packet-pool write pointer at 0x800BF544 (advanced past each committed packet).
//
// NOTE (2026-07 restructure): game/render/render_native.cpp (+ render/scene/scene_build.cpp +
// render/mesh/mesh_draw.cpp) is the CLAUDE.md-mandated eventual replacement for this file.
#include "core.h"
#include "game.h"   // Fps60State::current_object (was g_current_object)
#include "cfg.h"
#include "mods.h"   // g_mods — live PC-native lighting params (engine-native shading, not a deferred pass)
#include "lighting.h" // PER-AREA light registry (sun / lava+torch); selected per frame in engine_shade_select
#include "render_queue.h" // RQ_BACKGROUND + rq_push_2d_quad — native backdrop tilemap path
#include "render_internal.h" // shared render internals (PktSpanSession, obj_world_ord, native_gt3gt4)
#include "engine_math.h"     // ov_mat_mul/ov_apply_matlv/ov_rot_x/y/z — the GTE-transform cluster
#include "mtx.h"              // class Mtx — libgte helpers (identity, diagonal, ...)
#include "trig.h"             // class Trig — libgte rsin/rcos
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern uint32_t g_dbg_cur_geomblk;   // sil_bbox_log_node diag: geomblk chunk currently in native_gt3gt4
#include <math.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)

// ---------------------------------------------------------------------------------------------------
// NESTING-SAFE packet-pool span tracking (issue #4 — ropes/flames drew over terrain).
//
// Per-object depth tagging captures the address span an object's renderer writes into the packet pool
// (g_pkt_track/g_pkt_lo/g_pkt_hi in mem.cpp), then stamps it with the object's world depth. The three
// globals are ONE shared session, so the sessions did NOT nest. The rope/flame objects are rendered by
// an aux walk (BCF4) whose per-type renderer (overlay fn 0x801341E8 / 0x80136748) INTERNALLY dispatches
// the universal per-object render command 0x8003F698 (ov_render_cmd) for the SAME node. ov_render_cmd
// opened its own session and, on exit, reset g_pkt_track=0 — so the rope's own quads, emitted by the
// renderer AFTER that inner dispatch (via the per-quad submitter 0x8003B320 into 0x800C0xxx), were
// written with tracking OFF. They got NO span, missed obj_depth_lookup at the OT walk, and fell to the
// flat 2D OVERLAY band — drawing over the terrain/foliage. (Diagnosed live at tp 11647 -1597 2352:
// idx-1 PRE track=1 -> inner ov_render_cmd -> POST track=0; 63/73 2D prims MISS.)
//
// Fix: a session SAVES the outer session's state on open and, on close, RESTORES it while MERGING its own
// [lo,hi) into the outer's. The inner ov_render_cmd still publishes its own span, but tracking resumes
// for the OUTER (rope-walk) session, so the outer's final gpu_obj_depth_add covers ALL of the object's
// packets — the flush quads AND the rope segment quads — with the object's world depth. Nesting now works
// for every span-tracking site (this is the general fix, not a rope special-case).
// PktSpanSession + the nesting-safe packet-span rationale now live in render_internal.h (shared with
// render_walk.cpp).

#define COL_MASK     0xFFF0F0F0u   // low-nibble-per-byte clear applied to RGB words (matches the GPU)


// Right-edge frustum-cull threshold. The submit drops a prim only if ALL its verts are off the right of
// the screen. In 4:3 that's SX>=320 (faithful). Genuine engine-wide (gpu_gpu_wide_engine) extends the
// screen to the wide width (428@16:9), so geometry projected into the [320,wide) right band is ON-screen
// and MUST NOT be dropped — widen the threshold to the wide width. THIS is why the right-side terrain was
// missing in wide: the engine's own submit culled it to 4:3. (Vertical 240 cull unchanged.) later-119.
int gpu_gpu_wide_engine(void), gpu_gpu_wide_engine_w(void);
static int submit_xmax(void) { return gpu_gpu_wide_engine() ? gpu_gpu_wide_engine_w() : 320; }

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
extern uint32_t g_render_object;
int gpu_frame_no(Core*);
// Optional single-frame gate (PSXPORT_GEOMBLK_FRAME=<frame>) shared by the geomblk + rcmd probes: bound the
// firehose to one present frame so a 2900-frame headless run to the field doesn't emit gigabytes. Unset = every.
static int s_probe_frame = -2;
static inline int probe_frame_ok(Core* c) {
  if (s_probe_frame == -2) { const char* f = cfg_str("PSXPORT_GEOMBLK_FRAME"); s_probe_frame = f ? atoi(f) : -1; }
  return s_probe_frame < 0 || gpu_frame_no(c) == s_probe_frame;
}
static int s_geomblk = -1;
static inline int geomblk_on(Core* c) {
  if (s_geomblk < 0) s_geomblk = cfg_dbg("geomblk") ? 1 : 0;
  return s_geomblk && probe_frame_ok(c);
}
static void geomblk_dump(Core* c, const char* kind, uint32_t rec, uint32_t count, uint32_t stride) {
  if (!geomblk_on(c)) return;
  uint32_t o = g_render_object;                    // weak hint: last-culled object (see header — not the source)
  uint32_t handler = o ? c->mem_r32(o + 0x1c) : 0;
  uint8_t  type    = o ? c->mem_r8(o + 0x0c) : 0xff;
  fprintf(stderr, "[geomblk] f%d cur=%08x lastcull=%08x type=%02x handler=%08x %s n=%u\n",
          gpu_frame_no(c), c->game->fps60.current_object, o, type, handler, kind, count);
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
// NATIVE per-object DEPTH at the render-command dispatcher (gen_func_8003F698) — the UNIVERSAL chokepoint.
// EVERY queued render command passes through here with the engine's composed camera×object transform live
// in the GTE (CR0-7), regardless of which render walk drove it. We (a) compute the object's PC-native
// world-position view-depth from that transform (proj_obj_center_ord), and (b) capture the packet-pool span
// the command's renderer produces, recording span->depth. The geometry rasterizes LATER (deferred OT walk)
// where the per-object context is gone; the span lets a 2D billboard prim recover its object's real depth
// there (GpuState::obj_depth_lookup). Owned modes (generic GT3/GT4) carry per-VERTEX depth already and are
// untouched; this gives the UNowned overlay-mode prims (the 2D billboards) their true world depth.
void gpu_obj_depth_add(Core*, uint32_t lo, uint32_t hi, float ord);
float proj_obj_center_ord(void);
// PC-NATIVE object depth from real 3D placement. proj_camview_world_ord projects a WORLD point through the
// STABLE scene camera (published once per frame at terrain draw, camview_publish); camview_valid says it's
// known. This is the engine owning object depth from the object's spawned world position — NOT the volatile
// live-GTE origin projection (proj_obj_center_ord reads whatever camera×object transform was composed LAST,
// so render ORDER leaks into the depth and billboards get a wrong/too-far view-Z, losing the depth test to
// terrain and vanishing). See obj_world_ord below.
float proj_camview_world_ord(float wx, float wy, float wz);
int   camview_valid(void);
extern int g_fps60_on;                                    // fps60 mode (overlay toggle) — gate the billboard recorder
// The entity node the native render walk is currently rendering (set around each per-object dispatch,
// below). The PER-INSTANCE identity for every prim an object emits — including a 2D billboard whose quad
// rasterizes later at the OT walk. Used both for the objid overlay (gpu_native rq_emit_or_queue) and as the
// billboard span identity (so collectables/flames are identified individually, not merged under a shared id).
uint32_t g_dbg_render_node = 0;
// cur_render_node + obj_world_ord (PC-native per-object depth) now live in render_internal.h (shared).
// fps60: record a 3D-positioned 2D quad's reproject inputs, keyed by the object's packet-pool SPAN (the OT
// walk later matches each billboard item's source node against this span, identical to obj_depth_lookup).
void  fps60_record_billboard_span(Core* c, uint32_t lo, uint32_t hi, uint32_t ident);
#define PKT_POOL_PTR  0x800BF544u
// Native replacement for the resident render-command DISPATCHER FUN_8003F698 (the interpreted body was
// ~4% of field time). It reads a command byte at 0x800BF870, bounds-checks (<22), indexes the jump table
// at 0x80015268, and `jr`s into a per-command thunk = `jal <handler>; j <epilogue>`. The dispatcher does
// NO work after the handler and passes a0..a3 untouched, so a native tail-call to the handler is exactly
// equivalent (same args, same return target, ra/sp net-zero) — render output unchanged, only the dispatch
// GLUE goes native. Two guards force the DEFAULT handler 0x800803DC: busy flag 0x1F800234, and a2&1. Each
// thunk's first insn is the `jal`, so the handler is decoded from it (the table's default entries already
// point at the 0x800803DC thunk → cmd<22 needs no special case). RE: disas.py 0x8003F698 / table
// 0x80015268 / thunks 0x8003F6E8..0x8003F790.
static void render_cmd_dispatch(Core* c) {
  uint32_t handler;
  if (c->mem_r8(0x1F800234u) != 0 || (c->r[6] & 1u) != 0) {
    handler = 0x800803DCu;
  } else {
    uint32_t cmd = c->mem_r8(0x800BF870u);
    if (cmd >= 22u) handler = 0x800803DCu;
    else {
      uint32_t thunk = c->mem_r32(0x80015268u + cmd * 4u);
      uint32_t jal   = c->mem_r32(thunk);
      handler = ((jal & 0x03FFFFFFu) << 2) | 0x80000000u;
    }
  }
  rec_dispatch(c, handler);
}
void ov_render_cmd(Core* c) {
  if (cfg_dbg("rcmd") && probe_frame_ok(c)) {
    uint8_t mode = c->mem_r8(0x800BF870u);
    fprintf(stderr, "[rcmd] f%d mode=%02x geomblk=%08x ot=%08x flag=%08x ra=%08x M=",
            gpu_frame_no(c), mode, c->r[4], c->r[5], c->r[6], c->r[31]);
    for (int i = 0; i < 8; i++) fprintf(stderr, "%s%08x", i ? "," : "", (uint32_t)gte_read_ctrl(i));
    fprintf(stderr, "\n");
  }
  float ord = obj_world_ord(c, cur_render_node(c));  // PC-native depth from the object's real world position
  // Capture the packet-pool address span this command's renderer writes (the pool POINTER doesn't move for
  // the overlay variants, so track the actual stores), and tag it with the object's world-position depth.
  // NESTING-SAFE: this command is often dispatched from WITHIN an outer object-render session (e.g. the
  // rope/flame renderer calls back here for the same node). The session restores + merges into the outer
  // on close, so the outer walk keeps tracking the object's remaining quads (issue #4).
  uint32_t slo, shi; PktSpanSession sess;
  render_cmd_dispatch(c);                             // native dispatch (was rec_super_call(0x8003F698) — the ~4%)
  if (sess.close(&slo, &shi)) {
    gpu_obj_depth_add(c, slo, shi, ord);
    // fps60: the unowned-mode (overlay/billboard) prims this command emits land in THIS inner span. Record an
    // fps60 billboard entry keyed by that SPAN (the OT walk matches each item's source node against it) +
    // the current render object (scratch 0x1F80028C) as the stable cross-frame identity. The composed
    // camera×object transform is live in CR0-7 here (proj_obj_center_ord just read it).
    if (g_fps60_on || g_mods.debug_ids || cfg_dbg("objid")) fps60_record_billboard_span(c, slo, shi, cur_render_node(c));
    if (cfg_dbg("objz") && probe_frame_ok(c))
      fprintf(stderr, "[rcmddep] mode=%02x span %08x->%08x (%dB) ord=%.4f\n",
              c->mem_r8(0x800BF870u), slo, shi, (int)(shi - slo), (double)ord);
  }
}

// PSXPORT_DEBUG=enq — ENQUEUE tap (later-131 NEXT). The render-command PUSH gen_func_80077EBC appends its a0
// to the per-frame render list (scratchpad write-ptr 0x1F800148, count 0x1F800150, cap 40). Called by the
// per-object handlers during phase 1 (entity walk), so g_current_object names the SOURCE object — the
// attribution the rcmd/geomblk oracle can't get downstream. We dump that object + the pushed pointer a0 and
// its candidate command fields (a0+0x40 geomblk, a0+0x18 transform word) to confirm a0 IS the command struct
// and to build the object→command→geomblk map needed to enqueue margin commands natively. Super-calls original.
void ov_enqueue_probe(Core* c) {
  if (cfg_dbg("enq") && probe_frame_ok(c)) {
    uint32_t a0 = c->r[4], o = c->game->fps60.current_object;
    fprintf(stderr, "[enq] f%d obj=%08x type=%02x handler=%08x a0=%08x a0+40=%08x a0+18=%08x\n",
            gpu_frame_no(c), o, o ? c->mem_r8(o + 0x0c) : 0xff, o ? c->mem_r32(o + 0x1c) : 0,
            a0, c->mem_r32(a0 + 0x40), c->mem_r32(a0 + 0x18));
  }
  rec_super_call(c, 0x80077EBCu);
}

// PSXPORT_DEBUG=flush — render-command FLUSH tap (later-131 NEXT). Taps gen_func_8003F174: a0 = a command list
// whose header has the command count at +8 and a command-pointer array at +0xc0. Dumps each command's ADDRESS
// (list+0xc0[i]) + its geomblk (cmd+0x40) so the still-open render-command ENQUEUE can be traced (the writer
// of those cmd structs). The cmd address is the thing the dispatcher/rcmd tap can't see. Super-calls original.
void ov_flush_probe(Core* c) {
  if (cfg_dbg("flush") && probe_frame_ok(c)) {
    uint32_t list = c->r[4];
    uint32_t count = c->mem_r8(list + 8);
    fprintf(stderr, "[flush] f%d list=%08x count=%u\n", gpu_frame_no(c), list, count);
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
  if (cfg_dbg("flush2") && probe_frame_ok(c)) {
    uint32_t list = c->r[4];
    uint32_t count = c->mem_r8(list + 8);
    fprintf(stderr, "[flush2] f%d list=%08x count=%u ot=%08x\n", gpu_frame_no(c), list, count, c->r[5]);
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
  if (cfg_dbg("cmdenq") && probe_frame_ok(c)) {
    uint32_t cmd = c->r[4], group = c->r[5], sub = c->r[6], o = c->game->fps60.current_object;
    uint32_t T = c->mem_r32(0x800ECF58u + group*4);
    uint32_t geomblk = T + c->mem_r32(T + sub*4 + 4);
    fprintf(stderr, "[cmdenq] f%d obj=%08x type=%02x group=%u sub=%u T=%08x geomblk=%08x cmd=%08x\n",
            gpu_frame_no(c), o, o ? c->mem_r8(o + 0x0c) : 0xff, group, sub, T, geomblk, cmd);
  }
  rec_super_call(c, 0x80051B04u);
}

// PC-native per-vertex depth (Phase 2): because we OWN the projection, we know each vertex's real
// view-space Z (the SZ the GTE just produced) — record it keyed by the packet vertex word's address so
// the renderer's D32 depth buffer does true per-pixel occlusion (PSXPORT_NATIVE_DEPTH / the SBS A/B
// view) instead of OT-submission order. No correlation, no value-matching: the engine that emits the
// vertex writes the depth for the exact address it stored the SXY to. Off (faithful) by default.
void proj_set_H(uint16_t h);                     // tell proj_pz_to_ord the projection-plane H (CR26)
// PC-NATIVE render path. ProjVtx + the per-object world-coord projection live in engine/engine_project.*.
// proj_native_xform (gte_beetle) is the GTE-composed-transform projection still used by the resident
// byte-packed GT4 emitter (submit_poly_gt4_bp), whose upstream compose is the still-PSX field code.
#include "engine_project.h"
void  proj_native_xform(int vx, int vy, int vz, ProjVtx* out);
float proj_pz_to_ord(float pz);
void  gpu_draw_world_quad(Core* c, const float* px, const float* py, const float* depth,
                          const int* u, const int* v, const uint8_t* r, const uint8_t* g,
                          const uint8_t* b, uint16_t tp, uint16_t clut, int semi,
                          const float (*sv)[3]);
int  gpu_gpu_shadows_active(void);
// Fill `vv` with the prim's 4 view-space verts (x=ir1=vx, y=ir2=vy, z=pz) — the shadow VBO input — and
// return a pointer to it (NULL when this prim doesn't cast: semi, or shadows off). The shadow geometry is
// then carried ON the queued RqItem (gpu_draw_world_quad's sv arg) so it is rebuilt per present pass from
// the queue, NOT pushed here into a side stream — that is what removes the keep_shadow strobe hack.
static inline const float (*shadow_verts(const ProjVtx* p, int nv, int semi, float vv[4][3]))[3] {
  if (semi || !gpu_gpu_shadows_active()) return nullptr;          // only opaque world casts shadows
  for (int k = 0; k < 4; k++) { int s = k < nv ? k : nv - 1;
    vv[k][0] = p[s].vx; vv[k][1] = p[s].vy; vv[k][2] = p[s].pz; }   // view space (x=ir1, y=ir2, z=pz)
  return vv;
}
// fps60: after a GTE-composed world quad is pushed to the render queue, capture its model verts + the
// composed transform (CR0-7) + the current actor key so the 60fps tier can reproject it at the A/B
// midpoint (engine/fps60.cpp). No-op unless g_fps60_on. mv[k] = per-vertex model coords (4th = v2 for tris).
// (g_fps60_on + fps60_record_billboard_span are declared above, near gpu_obj_depth_add.)
void  fps60_stamp_world(Core* c, const int16_t mv[4][3], int nv, uint32_t key);
void  fps60_stamp_world_cr(Core* c, const int16_t mv[4][3], int nv, uint32_t key, const uint32_t cr[11]);

// fps60: 3D-POSITIONED 2D QUAD (billboard) capture. The collectable/flame/decal billboards are guest GP0
// quads/sprites the per-object renderers emit; they reach the render queue LATER, at the deferred OT walk
// (gpu_native.cpp, off the engine-submit path), where they inherit the object's WORLD-POSITION depth via
// obj_depth_lookup — so they carry fps_world=0 and the 60fps tier snapped them to camera-B (they juddered
// while Tomba moved smoothly). We tag them at QUEUE TIME from the OT walk now: we record an fps60 BILLBOARD
// entry (fps60_record_billboard_span, above) at the SAME instant we publish the object's depth span — keyed
// by that SPAN [lo,hi) (the OT walk matches each billboard item's source node against it, identical to
// obj_depth_lookup) + the object's stable cross-frame identity (`ident`, the node/cmd ptr) + the live
// composed camera×object transform. The OT walk (gpu_native.cpp) then stamps each billboard item directly
// as an anchor-reproject billboard, and build_lerp reprojects its WORLD ANCHOR at the midpoint camera — the
// same anchor-translate the mesh path uses, keyed on identity (not depth ord). Host-only; no guest write.
// fps60_bb_node now lives in render_internal.h (shared with render_walk.cpp).
static inline void fps60_stamp(Core* c, const ProjVtx* p, int nv) {
  if (!g_fps60_on) return;
  int16_t mv[4][3];
  for (int k = 0; k < 4; k++) { int s = k < nv ? k : nv - 1;
    mv[k][0] = (int16_t)p[s].mx; mv[k][1] = (int16_t)p[s].my; mv[k][2] = (int16_t)p[s].mz; }
  // World-coord native path: capture the composed transform from the active float xform; GTE path (the
  // resident byte-packed emitter) falls back to reading the live control registers.
  if (eproj_active()) { uint32_t cr[11]; eproj_active_cr(cr); fps60_stamp_world_cr(c, mv, nv, c->game->fps60.fps_cur_key, cr); }
  else                  fps60_stamp_world(c, mv, nv, c->game->fps60.fps_cur_key);
}

// ENGINE-NATIVE directional lighting (user directive 2026-06-21: lighting must be engine-native, NOT a
// screen-space deferred pass). Compute a real per-FACE normal from the prim's own view-space geometry
// (cross of two edges of ProjVtx.vx/vy/vz = the GTE-rotated vertex = view-space position) and modulate the
// vertex colours by ambient + diffuse*max(0,N·L). This shades ONLY the opaque world geometry it is called
// on — it never touches semi-transparent surfaces (water etc.), so translucency is unaffected by
// construction (that was the deferred pass's bug: it re-shaded/clobbered pixels behind translucent water).
// Light dir is the to-light vector in view space (g_mods.light_dir), same convention as the retired pass.
// PER-AREA lighting (engine/lighting.cpp): the directional light is now COLOURED and AREA-SELECTED (open
// areas get a warm SUN, mines get a dim cave ambient), plus optional POINT lights (lava up-glow / torches)
// attenuated by the face's view-space position. Config picked once per frame in engine_shade_select() (the
// renderer caches it so this hot per-face routine doesn't re-read guest RAM). lit is now per-CHANNEL: each
// vertex colour is modulated by (ambient_col*ambient + dir_col*diffuse*N·L + Σ point_col*att*N·L).
static const LightConfig* s_shade_cfg = 0;   // selected per frame; falls back to the SUN default if unset
void engine_shade_select(Core* c) {          // called once per world frame before the submitters run
  unsigned key = lighting_area_key_from(
      [](void* ctx, unsigned a) -> unsigned { return ((Core*)ctx)->mem_r32(a); }, c);
  s_shade_cfg = lighting_select(key);
}
static inline void engine_shade_face(const ProjVtx* p, int nv, uint8_t r[4], uint8_t g[4], uint8_t b[4]) {
  if (!g_mods.light) return;
  const LightConfig* cfg = s_shade_cfg ? s_shade_cfg : lighting_default();
  float e1x = p[1].vx - p[0].vx, e1y = p[1].vy - p[0].vy, e1z = p[1].vz - p[0].vz;
  float e2x = p[2].vx - p[0].vx, e2y = p[2].vy - p[0].vy, e2z = p[2].vz - p[0].vz;
  float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
  float ln = sqrtf(nx*nx + ny*ny + nz*nz); if (ln < 1e-6f) return;
  nx /= ln; ny /= ln; nz /= ln;
  if (nz > 0.0f) { nx = -nx; ny = -ny; nz = -nz; }            // face the camera (view -Z)
  // directional key light (coloured): ambient_col*ambient + dir_col*diffuse*max(0,N·L).
  float lx = cfg->dir[0], ly = cfg->dir[1], lz = cfg->dir[2];
  float ll = sqrtf(lx*lx + ly*ly + lz*lz); if (ll > 1e-6f) { lx /= ll; ly /= ll; lz /= ll; }
  float ndl = nx*lx + ny*ly + nz*lz; if (ndl < 0.0f) ndl = 0.0f;
  float litR = cfg->ambient_color[0]*cfg->ambient + cfg->dir_color[0]*cfg->dir_intensity*ndl;
  float litG = cfg->ambient_color[1]*cfg->ambient + cfg->dir_color[1]*cfg->dir_intensity*ndl;
  float litB = cfg->ambient_color[2]*cfg->ambient + cfg->dir_color[2]*cfg->dir_intensity*ndl;
  // point lights (lava up-glow / torches): face-centre view-space pos, soft falloff to radius, N·L diffuse.
  if (cfg->num_points > 0) {
    float cxp = (p[0].vx + p[1].vx + p[2].vx) * (1.0f/3.0f);
    float cyp = (p[0].vy + p[1].vy + p[2].vy) * (1.0f/3.0f);
    float czp = (p[0].vz + p[1].vz + p[2].vz) * (1.0f/3.0f);
    for (int i = 0; i < cfg->num_points; i++) {
      const PointLight* pl = &cfg->points[i];
      float dx = pl->pos[0]-cxp, dy = pl->pos[1]-cyp, dz = pl->pos[2]-czp;
      float d = sqrtf(dx*dx+dy*dy+dz*dz);
      float rad = pl->radius > 1.0f ? pl->radius : 1.0f;
      float att = 1.0f - d/rad; if (att < 0.0f) att = 0.0f; att *= att;   // soft quadratic falloff
      float pdl = (d > 1e-3f) ? (nx*dx + ny*dy + nz*dz)/d : 0.0f; if (pdl < 0.0f) pdl = 0.0f;
      float w = pl->intensity * att * pdl;
      litR += pl->color[0]*w; litG += pl->color[1]*w; litB += pl->color[2]*w;
    }
  }
  for (int k = 0; k < nv; k++) {
    int rr = (int)(r[k] * litR + 0.5f), gg = (int)(g[k] * litG + 0.5f), bb = (int)(b[k] * litB + 0.5f);
    r[k] = (uint8_t)(rr > 255 ? 255 : rr); g[k] = (uint8_t)(gg > 255 ? 255 : gg); b[k] = (uint8_t)(bb > 255 ? 255 : bb);
  }
}
// gen_func_8007FDB0 — POLY_GT3 (gouraud-textured triangle) submit.
// Record = 36 bytes: {+0 rgb0|code, +4 rgb1 (rgb2 = rgb1<<4), +8 uv0|clut, +12 uv1|tpage,
//   +16 VXY0, +20 VZ0(lo)|VZ1(hi), +24 VXY1, +28 VXY2, +32 VZ2(lo)|uv2(hi)}.
// PC-NATIVE POLY_GT3 submit — project the 3 model verts through the engine's composed transform in FLOAT
// (proj_native_xform, no gte_op) and tee a degenerate quad (v2 repeated) to the VK rasterizer with real
// per-pixel depth. No GP0 packet, no OT, no guest write.
static void submit_poly_gt3_native(Core* c) {
  if (cfg_dbg("subc")) { static long n=0; if(n++%240==0) fprintf(stderr,"[subc] gt3_native %ld\n", n); }
  uint32_t rec = c->r[4], count = c->r[6];
  proj_set_H((uint16_t)gte_read_ctrl(26));
  for (uint32_t i = 0; i < count; i++, rec += 36) {
    uint32_t vz01 = c->mem_r32(rec + 20);
    uint32_t xy0 = c->mem_r32(rec + 16), xy1 = c->mem_r32(rec + 24), xy2 = c->mem_r32(rec + 28);
    ProjVtx p[3];
    eproj_vertex_active((int16_t)xy0, (int16_t)(xy0 >> 16), (int16_t)vz01,         &p[0]);
    eproj_vertex_active((int16_t)xy1, (int16_t)(xy1 >> 16), (int16_t)(vz01 >> 16), &p[1]);
    eproj_vertex_active((int16_t)xy2, (int16_t)(xy2 >> 16), (int16_t)c->mem_r32(rec + 32), &p[2]);
    float area = (p[1].px - p[0].px) * (p[2].py - p[0].py) - (p[2].px - p[0].px) * (p[1].py - p[0].py);
    if (area <= 0) continue;                                  // backface
    int xmax = submit_xmax();
    if (p[0].sx >= xmax && p[1].sx >= xmax && p[2].sx >= xmax) continue;
    if (p[0].sy >= 240 && p[1].sy >= 240 && p[2].sy >= 240) continue;
    uint32_t code = c->mem_r32(rec + 0);                      // rgb0|op ; rgb1 @rec+4, rgb2 = rgb1<<4
    uint32_t rgb[3] = { code & COL_MASK, c->mem_r32(rec + 4) & COL_MASK, (c->mem_r32(rec + 4) << 4) & COL_MASK };
    uint32_t uv0 = c->mem_r32(rec + 8), uv1 = c->mem_r32(rec + 12);
    uint16_t clut = (uint16_t)(uv0 >> 16), tp = (uint16_t)(uv1 >> 16);
    int u[4], v[4]; uint8_t r[4], g[4], b[4]; float px[4], py[4], depth[4];
    u[0] = uv0 & 0xFF;  v[0] = (uv0 >> 8) & 0xFF;
    u[1] = uv1 & 0xFF;  v[1] = (uv1 >> 8) & 0xFF;
    u[2] = c->mem_r16(rec + 34) & 0xFF; v[2] = (c->mem_r16(rec + 34) >> 8) & 0xFF;   // uv2 (high half of rec+32)
    for (int k = 0; k < 3; k++) {
      px[k] = p[k].px; py[k] = p[k].py; depth[k] = proj_pz_to_ord(p[k].pz);
      r[k] = rgb[k] & 0xFF; g[k] = (rgb[k] >> 8) & 0xFF; b[k] = (rgb[k] >> 16) & 0xFF;
    }
    px[3] = px[2]; py[3] = py[2]; depth[3] = depth[2];        // 4th vert = v2 (degenerate -> a triangle)
    u[3] = u[2]; v[3] = v[2]; r[3] = r[2]; g[3] = g[2]; b[3] = b[2];
    int semi = (code & 0x02000000) ? 1 : 0;
    if (!semi) engine_shade_face(p, 3, r, g, b);             // engine-native lighting (opaque only)
    { char tag[32]; snprintf(tag, sizeof tag, "gt3_native@%08X", g_dbg_cur_geomblk); sil_bbox_log_verts(tag, px, py, depth, 3, cur_render_node(c), rec, r, g, b); }
    { float vv[4][3]; const float (*sv)[3] = shadow_verts(p, 3, semi, vv);   // dynamic shadow verts (carried on the item)
      gpu_draw_world_quad(c, px, py, depth, u, v, r, g, b, tp, clut, semi, sv); }
    fps60_stamp(c, p, 3);                                    // fps60: capture for midpoint reprojection
  }
  c->r[2] = rec;
}

void ov_submit_poly_gt3(Core* c) { submit_poly_gt3_native(c); }

// gen_func_8008007C — POLY_GT4 (gouraud-textured quad) submit, PC-NATIVE.
// Record = 44 bytes: {+0 rgb0(rgb1=<<4), +4 rgb2(rgb3=<<4), +8 uv0|clut, +12 uv1|tpage,
//   +16 uv2(lo)|uv3(hi), +20 VXY0, +24 VZ0(lo)|VZ1(hi), +28 VXY1, +32 VXY2, +36 VZ2(lo)|VZ3(hi), +40 VXY3}.
// Project the 4 model verts through the engine's composed transform in FLOAT (proj_native_xform, no
// gte_op) and tee the quad straight to the VK rasterizer (gpu_draw_world_quad) with real per-pixel depth.
// NO GP0 packet, NO OT, NO guest write — the renderer a PC game has. Cull rules (backface/frustum) are
// reproduced on the native projection so we drop the same prims the engine would. Returns the advanced
// record pointer (the engine reads it back).
static void submit_poly_gt4_native(Core* c) {
  if (cfg_dbg("subc")) { static long n=0; if(n++%240==0) fprintf(stderr,"[subc] gt4_native %ld\n", n); }
  uint32_t rec = c->r[4], count = c->r[6];
  proj_set_H((uint16_t)gte_read_ctrl(26));
  for (uint32_t i = 0; i < count; i++, rec += 44) {
    // model verts: V0=rec+20(XY)|rec+24.lo(Z), V1=rec+28|rec+24.hi, V2=rec+32|rec+36.lo, V3=rec+40|rec+36.hi
    uint32_t vz01 = c->mem_r32(rec + 24), vz23 = c->mem_r32(rec + 36);
    uint32_t xy0 = c->mem_r32(rec + 20), xy1 = c->mem_r32(rec + 28),
             xy2 = c->mem_r32(rec + 32), xy3 = c->mem_r32(rec + 40);
    ProjVtx p[4];
    eproj_vertex_active((int16_t)xy0, (int16_t)(xy0 >> 16), (int16_t)vz01,          &p[0]);
    eproj_vertex_active((int16_t)xy1, (int16_t)(xy1 >> 16), (int16_t)(vz01 >> 16),  &p[1]);
    eproj_vertex_active((int16_t)xy2, (int16_t)(xy2 >> 16), (int16_t)vz23,          &p[2]);
    eproj_vertex_active((int16_t)xy3, (int16_t)(xy3 >> 16), (int16_t)(vz23 >> 16),  &p[3]);
    // backface cull on the FRONT triangle's signed screen area (NCLIP: (SX1-SX0)*(SY2-SY0)-(SX2-SX0)*(SY1-SY0)).
    float area = (p[1].px - p[0].px) * (p[2].py - p[0].py) - (p[2].px - p[0].px) * (p[1].py - p[0].py);
    if (area <= 0) continue;                                  // backface (matches MAC0<=0 drop)
    // frustum cull (right/bottom edges only, as the original) over all 4 verts.
    int xmax = submit_xmax();
    if (p[0].sx >= xmax && p[1].sx >= xmax && p[2].sx >= xmax && p[3].sx >= xmax) continue;
    if (p[0].sy >= 240 && p[1].sy >= 240 && p[2].sy >= 240 && p[3].sy >= 240) continue;
    // decode RGB (rgb0 @rec+0, rgb1=rgb0<<4; rgb2 @rec+4, rgb3=rgb2<<4) and UV/CLUT/texpage.
    uint32_t code0 = c->mem_r32(rec + 0), code2 = c->mem_r32(rec + 4);
    uint32_t rgb[4] = { code0 & COL_MASK, (code0 << 4) & COL_MASK, code2 & COL_MASK, (code2 << 4) & COL_MASK };
    uint32_t uv0 = c->mem_r32(rec + 8), uv1 = c->mem_r32(rec + 12), uv23 = c->mem_r32(rec + 16);
    uint16_t clut = (uint16_t)(uv0 >> 16);
    uint16_t tp   = (uint16_t)(uv1 >> 16);
    int u[4], v[4]; uint8_t r[4], g[4], b[4]; float px[4], py[4], depth[4];
    u[0] = uv0 & 0xFF;        v[0] = (uv0 >> 8) & 0xFF;
    u[1] = uv1 & 0xFF;        v[1] = (uv1 >> 8) & 0xFF;
    u[2] = uv23 & 0xFF;       v[2] = (uv23 >> 8) & 0xFF;
    u[3] = (uv23 >> 16) & 0xFF; v[3] = (uv23 >> 24) & 0xFF;
    for (int k = 0; k < 4; k++) {
      px[k] = p[k].px; py[k] = p[k].py; depth[k] = proj_pz_to_ord(p[k].pz);
      r[k] = rgb[k] & 0xFF; g[k] = (rgb[k] >> 8) & 0xFF; b[k] = (rgb[k] >> 16) & 0xFF;
    }
    int semi = (code0 & 0x02000000) ? 1 : 0;                  // GP0 op byte (code0>>24) bit1 = semi-transparency
    if (!semi) engine_shade_face(p, 4, r, g, b);             // engine-native lighting (opaque only)
    { char tag[32]; snprintf(tag, sizeof tag, "gt4_native@%08X", g_dbg_cur_geomblk); sil_bbox_log_verts(tag, px, py, depth, 4, cur_render_node(c), rec, r, g, b); }
    { float vv[4][3]; const float (*sv)[3] = shadow_verts(p, 4, semi, vv);   // dynamic shadow verts (carried on the item)
      gpu_draw_world_quad(c, px, py, depth, u, v, r, g, b, tp, clut, semi, sv); }
    fps60_stamp(c, p, 4);                                    // fps60: capture for midpoint reprojection
  }
  c->r[2] = rec;                                              // return: record pointer advanced past the array
}

void ov_submit_poly_gt4(Core* c) { submit_poly_gt4_native(c); }

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

// PC-NATIVE byte-packed GT4 submit. Verts are signed bytes <<8; the top byte of each RGB word is that
// vertex's Z (<<8). Project natively (proj_native_xform) → float screen+depth, tee the quad to the VK
// rasterizer with real per-pixel depth. The DPCT/DPCS depth-cue fog the PSX body applied to the colors is
// dropped here — fog is the renderer's job (PSXPORT_FOG shader), not a per-vertex color bake. The OT-Z
// bias (a2) is moot with a real depth buffer. CLUT bank (a1) and U offset (a3) ARE applied (texture
// addressing). Same decode native_terrain.cpp uses; this is its general (any-geomblk) form.
static void submit_poly_gt4_bp(Core* c) {
  if (cfg_dbg("subc")) { static long n=0; if(n++%240==0) fprintf(stderr,"[subc] gt4_bp #%ld\n", n); }
  uint32_t rec = c->r[4];
  uint32_t clut_bank = c->r[5];                        // a1: CLUT-Y bank (added <<22 to the uv0|clut word)
  uint32_t uoff = c->r[7] & 0xFF;                      // a3: U-texture offset (mod 256 per U byte)
  proj_set_H((uint16_t)gte_read_ctrl(26));
  static const uint32_t XO[4] = {0x1C,0x1D,0x20,0x21}, YO[4] = {0x1E,0x1F,0x22,0x23},
                        ZO[4] = {0x0F,0x13,0x17,0x1B};
  for (;;) {
    uint32_t ctl = c->mem_r32(rec + 4);                 // control word (sign = last record; bit30 = semi)
    ProjVtx p[4]; float px[4], py[4], depth[4]; int u[4], v[4]; uint8_t r[4], g[4], b[4];
    for (int k = 0; k < 4; k++) {
      int vx = (int)(int8_t)c->mem_r8(rec + XO[k]) << 8;
      int vy = (int)(int8_t)c->mem_r8(rec + YO[k]) << 8;
      int vz = (int)(int8_t)c->mem_r8(rec + ZO[k]) << 8;
      proj_native_xform(vx, vy, vz, &p[k]);
      px[k] = p[k].px; py[k] = p[k].py; depth[k] = proj_pz_to_ord(p[k].pz);
    }
    float area = (p[1].px - p[0].px) * (p[2].py - p[0].py) - (p[2].px - p[0].px) * (p[1].py - p[0].py);
    int cull = (area <= 0);
    int xmax = submit_xmax();
    if (p[0].sx >= xmax && p[1].sx >= xmax && p[2].sx >= xmax && p[3].sx >= xmax) cull = 1;
    if (p[0].sy >= 240 && p[1].sy >= 240 && p[2].sy >= 240 && p[3].sy >= 240) cull = 1;
    if (!cull) {
      uint32_t uv0 = c->mem_r32(rec + 0) + (clut_bank << 22);  // uv0 | (clut + bank)
      uint16_t clut = (uint16_t)(uv0 >> 16);
      uint16_t tp   = (uint16_t)(((uint32_t)ctl & 0x7FFFFFu) >> 16);  // = packet uv1|tpage high half
      uint32_t uv2  = c->mem_r32(rec + 8);
      u[0] = (uv0 & 0xFF);            v[0] = (uv0 >> 8) & 0xFF;
      u[1] = ((uint32_t)ctl & 0xFF); v[1] = ((uint32_t)ctl >> 8) & 0xFF;
      u[2] = (uv2 & 0xFF);           v[2] = (uv2 >> 8) & 0xFF;
      u[3] = ((uv2 >> 16) & 0xFF);   v[3] = (uv2 >> 24) & 0xFF;
      for (int k = 0; k < 4; k++) {
        u[k] = (u[k] + (int)uoff) & 0xFF;
        uint32_t col = c->mem_r32(rec + 0x0C + 4 * k);  // raw RGB (top byte = Z, ignored); no DPCT/DPCS bake
        r[k] = col & 0xFF; g[k] = (col >> 8) & 0xFF; b[k] = (col >> 16) & 0xFF;
      }
      int semi = (ctl & 0x40000000) ? 1 : 0;
      if (!semi) engine_shade_face(p, 4, r, g, b);      // engine-native lighting (opaque only)
      sil_bbox_log_verts("gt4_bp", px, py, depth, 4, cur_render_node(c), rec, r, g, b);
      { float vv[4][3]; const float (*sv)[3] = shadow_verts(p, 4, semi, vv);   // dynamic shadow verts (carried on the item)
        gpu_draw_world_quad(c, px, py, depth, u, v, r, g, b, tp, clut, semi, sv); }
      fps60_stamp(c, p, 4);                             // fps60: capture for midpoint reprojection
    }
    if ((int32_t)ctl <= 0) break;                       // control sign marks the last record
    rec += 36;
  }
  c->r[2] = 0x800C0000u;                                // return value the recomp body leaves in r2
}

void ov_submit_poly_gt4_bp(Core* c) {
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
// next RE target (engine_re "OPEN — full field depth coverage").
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
uint32_t g_dbg_cur_geomblk = 0;   // sil_bbox_log_node diag: which geomblk chunk is currently submitting
void native_gt3gt4(Core* c, uint32_t geomblk, uint32_t otbase) {   // decl in render_internal.h (used by render_walk.cpp)
  g_dbg_cur_geomblk = geomblk;
  uint32_t counts = c->mem_r32(geomblk + 0);
  c->r[4] = geomblk + 16; c->r[5] = otbase; c->r[6] = counts & 0xFFFFu;
  ov_submit_poly_gt3(c);
  c->r[4] = c->r[2];      c->r[5] = otbase; c->r[6] = counts >> 16;
  ov_submit_poly_gt4(c);
}

// FIELD ENTITY RENDER LOOP — PC-native ownership of the SOP field-overlay entity render 0x80109fe0
// (sop.cpp:203, "entity render loop"). The field loads the scene CAMERA into the GTE once, then submits
// each entity's GT3/GT4 geometry whose vertices are already in WORLD space. We own it natively: build the
// float camera-view transform from world coordinates (eproj_compose_camera) and route every entity through
// the native GT3/GT4 submitters — projecting in float through that world-coord transform, with real depth.
// NO gte_op, NO PSX submit library (0x801099b4 / 0x80109c80). This is the path that actually draws the
// visible field (Tomba, props, terrain props); before this it ran 100% interpreted.
//
// Faithful transcription of the loop's addressing (decoded from the overlay disasm):
//   a0 = entity-list struct: [6] = u8 count, [0xc] = packed-geometry base, [0x10..] = u16 entry offsets.
//   per entry: cmd = base + idx*4; s0 = *cmd (packed counts); records follow at cmd+4.
//     GT3 submit(rec = cmd+4, ot, count = s0 & 0xff) -> returns the advanced record ptr = the GT4 base.
//     GT4 submit(rec = <ret>,  ot, count = (s0 >> 16) & 0xff).
void ov_field_entity_render(Core* c) {
  uint32_t es = c->r[4];
  uint8_t count = c->mem_r8(es + 6);
  if (count == 0) return;
  uint32_t otbase = c->mem_r32(OTBASE_PTR);
  uint32_t base   = c->mem_r32(es + 0xC);
  EObjXform w; eproj_compose_camera(c, &w); eproj_set_active(&w);
  // DIAG groundproj: log the camera xform + first GT4 record's model verts and their eproj projection, so we
  // can see whether the world-space scene-table geometry projects on-screen with sane depth. (later-231b)
  if (cfg_dbg("groundproj")) { static int n=0; if (n++ < 3) {
    fprintf(stderr, "[groundproj] es=%08x count=%u base=%08x T=(%.0f,%.0f,%.0f) H=%.0f R0=(%.3f,%.3f,%.3f)\n",
      es, count, base, (double)w.T[0],(double)w.T[1],(double)w.T[2],(double)w.H,
      (double)w.R[0][0]/4096,(double)w.R[0][1]/4096,(double)w.R[0][2]/4096);
    uint32_t cmd0 = base + (uint32_t)c->mem_r16(es+0x10)*4; uint32_t s0d=c->mem_r32(cmd0);
    uint32_t rec = cmd0+4 + ((s0d&0xFF)*36);   // skip GT3s to first GT4 record (44B)
    for (int k=0;k<2;k++){ uint32_t r2=rec+k*44;
      int16_t vx=(int16_t)c->mem_r16(r2+20), vy=(int16_t)(c->mem_r32(r2+20)>>16), vz=(int16_t)c->mem_r16(r2+24);
      ProjVtx pv; eproj_vertex_active(vx,vy,vz,&pv);
      fprintf(stderr,"   gt4[%d] model=(%d,%d,%d) -> px=%.1f py=%.1f pz=%.1f sx=%d sy=%d\n",
        k, vx,vy,vz, (double)pv.px,(double)pv.py,(double)pv.pz, pv.sx, pv.sy); } } }
  uint32_t p = es + 0x10, end = es + 0x10 + (uint32_t)count * 2;
  for (; p < end; p += 2) {
    uint32_t cmd = base + (uint32_t)c->mem_r16(p) * 4;
    uint32_t s0  = c->mem_r32(cmd);
    c->game->fps60.fps_cur_key = cmd;                                  // fps60: per-entity reproject key
    g_dbg_cur_geomblk = cmd;    // sil_bbox_log diag: tag this entity's cmd record (native_gt3gt4 is NOT the caller here)
    c->r[4] = cmd + 4;  c->r[5] = otbase; c->r[6] = s0 & 0xFF;          submit_poly_gt3_native(c);
    c->r[4] = c->r[2];  c->r[5] = otbase; c->r[6] = (s0 >> 16) & 0xFF;  submit_poly_gt4_native(c);
    c->game->fps60.fps_cur_key = 0;
  }
  eproj_clear_active();
}

// ov_ground_probe (diagnostic, `debug groundprobe`) moved to render_debug_probes.cpp (2026-07 restructure).

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
//   - terrain geomblk = 0x8009FAE8 (fixed per-frame record buffer); a1=a2=a3=0.
#define A2_PARAM     0x800A2014u             // 3-byte sway-angle param scratch (engine global)
#define IR0_STAGE    0x1F800090u             // IR0 depth-cue factor staged for the 0x80027768 submitter
// Terrain prim-record buffer (a0 to 80027768). The recomp body 0x8002AB5C loads `lui 0x800A; addiu -1304`
// = 0x8009FAE8 (confirmed: all three real callers of 0x80027768 pass -1304; 0x800A1AE8 is referenced by
// NO function as a geomblk — it was a fabricated address in the prior native port that read the WRONG
// buffer → garbage/water terrain geometry instead of the actual field strip).
#define MVMVA_TERRAIN_GEOMBLK 0x8009FAE8u
// Shared terrain scene-data prep (the faithful gameplay half): write the depth-cue regs + the two sway
// gameplay bytes, then build the object rotation matrix at scratch SCR (euler 0x80085480 + secondary
// sway 0x80084520). Used by the PC-native terrain_render_pc (engine/native_terrain.cpp); the verified
// sway-byte writes (later-157, A/B RAM-0-diff) have a single source of truth. Leaves the object matrix at SCR; camera matrix is at
// SCR+0xF8 (set earlier). The matrix-build leaves stay platform primitives (rec_dispatch), as the
// recomp body calls them.
void terrain_prep_object_matrix(Core* c, uint32_t node) {
  // depth-cue: FarColor=0, IR0 factor staged for the submitter
  gte_write_ctrl(21, 0); gte_write_ctrl(22, 0); gte_write_ctrl(23, 0);
  uint32_t ir0 = (uint32_t)((128 - (int16_t)c->mem_r16(node + 78)) << 5);
  int32_t a80 = (int16_t)c->mem_r16(node + 80);
  // The two sway-angle bytes (0x800A2014/2016) are written by the recomp terrain body and read back
  // by it (scaled <<2) into the secondary-rotation args; the middle byte 0x800A2015 is set elsewhere.
  // We write them to guest exactly as the recomp does (the no-guest-write rule was discarded — these
  // are part of the function's faithful behavior, and leaving them stale was the only true-gameplay
  // divergence vs the recomp body, root-caused via the A/B RAM diff). Compute, store, use.
  uint8_t sway0 = (uint8_t)(((int32_t)(int16_t)c->mem_r16(node + 64) * a80) >> 11);
  uint8_t sway2 = (uint8_t)(((int32_t)(int16_t)c->mem_r16(node + 66) * a80) >> 11);
  c->mem_w8(A2_PARAM + 0, sway0);
  c->mem_w8(A2_PARAM + 2, sway2);
  uint8_t sway1 = c->mem_r8(A2_PARAM + 1);                 // external (set elsewhere)
  c->mem_w32(IR0_STAGE, ir0);
  // build object rotation matrix at scratch SCR from the node's euler angles (node+84/86/88)
  c->r[4] = node + 84; c->r[5] = SCR; ov_rotmat(c);
  // Secondary sway rotation by the host-computed angle bytes (scaled <<2). The recomp body 0x8002AB5C
  // stages these three angle words on its OWN STACK FRAME (r29 -= 56; words at r29+16/20/24), NOT in
  // scratchpad — and passes that stack pointer as 0x80084520's arg. The prior native code wrote them to
  // scratchpad 0x1F8001C0 instead, a guest write the recomp NEVER makes; that clobbered whatever live
  // engine state occupied 0x1F8001C0, corrupting gameplay (terrain collision → Tomba fell through). It
  // was invisible to the later-157 A/B gate, which diffs only the 2 MB main RAM, not the scratchpad.
  // Mirror the recomp exactly: take a guest stack frame, write the angles there, restore on the way out.
  uint32_t saved_sp = c->r[29];
  c->r[29] = saved_sp - 56;                               // recomp's stack frame (private scratch, not 0x1F800xxx)
  c->mem_w32(c->r[29] + 16, (uint32_t)sway0 << 2);
  c->mem_w32(c->r[29] + 20, (uint32_t)sway1 << 2);
  c->mem_w32(c->r[29] + 24, (uint32_t)sway2 << 2);
  c->r[4] = SCR; c->r[5] = c->r[29] + 16; rec_dispatch(c, 0x80084520u);
  c->r[29] = saved_sp;                                    // pop the frame
}

void terrain_render_pc(Core* c);             // engine/native_terrain.cpp — PC-native float terrain render
void ov_terrain(Core* c) {
  if (cfg_dbg("terrgte")) fprintf(stderr, "[ov_terrain] node(a0=r4)=%08X\n", c->r[4]);
  // Pick this area's light config ONCE per world frame (terrain renders first); the per-face shader reads
  // the cached pointer. Cheap guest-RAM fingerprint read; unknown area -> village SUN default.
  if (g_mods.light) engine_shade_select(c);
  // Dual-core diff: the `b` core neutralizes terrain to the recomp body via a per-Game flag (the override
  // table is shared; the per-core choice is this flag, not a divergent table). `a` keeps the native path.
  if (c->game->neutralize_terrain) { rec_super_call(c, 0x8002AB5Cu); return; }
  // RENDER PC-NATIVE (USER DIRECTIVE: behave like a PC game, do NOT simulate PSX). The terrain is rendered
  // by terrain_render_pc — float transform + real per-pixel depth, drawn straight to the rasterizer, NO GTE
  // compose / NO gte_op / NO byte-packed PSX packet. (The old GTE-compose + 0x80027768-submit transcription
  // oracle was removed — no gating.) terrain_prep_object_matrix does the gameplay sway writes + object-matrix
  // scene data; the render method is PC-native float.
  terrain_render_pc(c);
}

void ov_xform_propagate(Core* c);   // fwd (defined below)

// NATIVE per-object TRANSFORM BUILD — gen_func_80051C8C (later-135). Build the object's render matrix
// cache at node+0x98 each frame: seed an identity (0x1000 diagonal), apply the three euler rotations
// (node+0x54/56/58 via the matrix primitives 80084D10/EB0/85050), set the translation node+0xac/b0/b4
// from the gameplay position node+0x2e/32/36, then propagate to the command struct (80051464). The
// per-object flush (8003CDD8) reads this matrix (cmd+0x18); the widescreen margin calls this before its
// render (its last remaining guest call). Interpreted-only (fn-ptr reached) → seeded + decoded. We own
// the orchestration + memory writes; the rotation/propagate leaves stay primitives (rec_dispatch), as
// the recomp body calls them.
static void build_xform(Core* c) {
  uint32_t node = c->r[4];
  c->mem_w32(node + 152, 0x1000); c->mem_w32(node + 156, 0); c->mem_w32(node + 160, 0x1000);
  c->mem_w32(node + 164, 0);      c->mem_w32(node + 168, 0x1000); c->mem_w32(node + 172, 0);
  c->mem_w32(node + 176, 0);      c->mem_w32(node + 180, 0);
  c->r[4] = (uint32_t)(int16_t)c->mem_r16(node + 84); c->r[5] = node + 152; ov_rot_x(c);
  c->r[4] = (uint32_t)(int16_t)c->mem_r16(node + 86); c->r[5] = node + 152; ov_rot_y(c);
  c->r[4] = (uint32_t)(int16_t)c->mem_r16(node + 88); c->r[5] = node + 152; ov_rot_z(c);
  c->mem_w32(node + 172, (uint32_t)(int16_t)c->mem_r16(node + 46));
  c->mem_w32(node + 176, (uint32_t)(int16_t)c->mem_r16(node + 50));
  c->mem_w32(node + 180, (uint32_t)(int16_t)c->mem_r16(node + 54));
  c->r[4] = node; ov_xform_propagate(c);
}
void ov_build_xform(Core* c) {
  build_xform(c);
}

// FUN_80051464 — child-node transform PROPAGATION. For each of the node's child sub-nodes (count =
// node[8], pointer array @node+0xC0), seed an identity work matrix in scratchpad @0x1F800000, compose
// the child's three euler angles onto it (rot_x/y/z = 80084D10/EB0/85050), then matmul it against a
// PARENT matrix and MVMVA-rotate the child's translation, and finally accumulate the parent's world
// translation onto the child's. The parent source depends on the child's sentinel angle c+6: -1 = the
// node itself (matrix node+0x98, trans node+0xAC); else = sibling child node[0xC0 + 4*c[6]] (matrix
// p+0x18, trans p+0x2C). All five callees are already-owned native overrides — this is pure orchestration
// + scratchpad seeding + integer translation adds, so we rec_dispatch the primitives exactly as the
// recomp body's jal sequence does (preserving the matmul→MVMVA GTE-CR coupling identically). The matmul
// out-pointer a2 = c+24 is set in the recomp's branch-delay slot for BOTH branches, so it is constant.
// The loop bound is re-read each iteration (top: s3 < node[8]; bottom: s3 < node[9]) — mirrored exactly.
static void xform_propagate_body(Core* c) {
  uint32_t node = c->r[4];
  if (c->mem_r8(node + 9) == 0) return;
  int i = 0;
  while (i < (int)(uint8_t)c->mem_r8(node + 8)) {                 // top check (node[8])
    uint32_t child = c->mem_r32(node + 0xC0 + 4u * (uint32_t)i);
    // seed identity 3x3 work matrix (diagonal 0x1000) into scratchpad @0x1F800000
    c->mem_w32(0x1F800000u, 4096); c->mem_w32(0x1F800004u, 0);
    c->mem_w32(0x1F800008u, 4096); c->mem_w32(0x1F80000Cu, 0);
    c->mem_w32(0x1F800010u, 4096); c->mem_w32(0x1F800014u, 0);
    c->mem_w32(0x1F800018u, 0);    c->mem_w32(0x1F80001Cu, 0);
    int16_t sentinel = (int16_t)c->mem_r16(child + 6);
    c->r[4] = (uint32_t)(int32_t)(int16_t)c->mem_r16(child + 8);  c->r[5] = 0x1F800000u; ov_rot_x(c); // rot_x
    c->r[4] = (uint32_t)(int32_t)(int16_t)c->mem_r16(child + 10); c->r[5] = 0x1F800000u; ov_rot_y(c); // rot_y
    c->r[4] = (uint32_t)(int32_t)(int16_t)c->mem_r16(child + 12); c->r[5] = 0x1F800000u; ov_rot_z(c); // rot_z
    if (sentinel == -1) {                                        // ROOT: parent = this node
      c->r[4] = node + 152; c->r[5] = 0x1F800000u; c->r[6] = child + 24; ov_mat_mul(c); // child_mat = node_mat × work
      c->r[4] = child; c->r[5] = child + 44; ov_apply_matlv(c);                              // MVMVA → child+0x2C
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(node + 0xAC));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(node + 0xB0));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(node + 0xB4));
    } else {                                                     // SIBLING: parent = node[0xC0 + 4*sentinel]
      uint32_t p = c->mem_r32(node + 0xC0 + 4u * (uint32_t)(int)sentinel);
      c->r[4] = p + 24; c->r[5] = 0x1F800000u; c->r[6] = child + 24; ov_mat_mul(c); // child_mat = sibling_mat × work
      c->r[4] = child; c->r[5] = child + 44; ov_apply_matlv(c);
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(p + 0x2C));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(p + 0x30));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(p + 0x34));
    }
    i++;
    if (!(i < (int)(uint8_t)c->mem_r8(node + 9))) break;         // bottom check (node[9])
  }
}
// PSXPORT_DEBUG=xformverify — per-call gate. Both paths run the identical owned inner primitives, so this
// verifies the orchestration (loop bounds, branch select, address math, add order, scratchpad seeding).
// Snapshot every touched child sub-struct (+0x18..+0x38) + the scratchpad work matrix + GTE data regs,
// run native, capture, restore, run the recomp body, diff.
static void ov_xform_propagate_verify(Core* c) {
  uint32_t rs[32]; memcpy(rs, c->r, sizeof rs);
  uint32_t node = c->r[4];
  int n8 = (uint8_t)c->mem_r8(node + 8), n9 = (uint8_t)c->mem_r8(node + 9);
  int N = n8 > n9 ? n8 : n9; if (N > 64) N = 64;
  uint32_t ch[64]; uint32_t snap[64][8];     // child+0x18..+0x37
  for (int i = 0; i < N; i++) { ch[i] = c->mem_r32(node + 0xC0 + 4u * (uint32_t)i);
    for (int w = 0; w < 8; w++) snap[i][w] = c->mem_r32(ch[i] + 0x18 + 4u * (uint32_t)w); }
  uint32_t spad_b[8]; for (int w = 0; w < 8; w++) spad_b[w] = c->mem_r32(0x1F800000u + 4u * (uint32_t)w);
  uint32_t gd_b[32]; for (int i = 0; i < 32; i++) gd_b[i] = gte_read_data(i);
  // NATIVE
  xform_propagate_body(c);
  uint32_t nat[64][8], spad_n[8], gd_n[32];
  for (int i = 0; i < N; i++) for (int w = 0; w < 8; w++) nat[i][w] = c->mem_r32(ch[i] + 0x18 + 4u * (uint32_t)w);
  for (int w = 0; w < 8; w++) spad_n[w] = c->mem_r32(0x1F800000u + 4u * (uint32_t)w);
  for (int i = 0; i < 32; i++) gd_n[i] = gte_read_data(i);
  // RESTORE
  memcpy(c->r, rs, sizeof rs);
  for (int i = 0; i < N; i++) for (int w = 0; w < 8; w++) c->mem_w32(ch[i] + 0x18 + 4u * (uint32_t)w, snap[i][w]);
  for (int w = 0; w < 8; w++) c->mem_w32(0x1F800000u + 4u * (uint32_t)w, spad_b[w]);
  for (int i = 0; i < 32; i++) gte_write_data(i, gd_b[i]);
  // RECOMP
  rec_super_call(c, 0x80051464u);
  static long ngood = 0, nbad = 0; int bad = 0;
  for (int i = 0; i < N && !bad; i++) for (int w = 0; w < 8; w++)
    if (nat[i][w] != c->mem_r32(ch[i] + 0x18 + 4u * (uint32_t)w)) {
      if (nbad < 40) fprintf(stderr, "[xformverify] MISMATCH node=%08x child[%d]=%08x +0x%x mine=%08x oracle=%08x\n",
                             node, i, ch[i], 0x18 + w * 4, nat[i][w], c->mem_r32(ch[i] + 0x18 + 4u * (uint32_t)w));
      bad = 1; break; }
  for (int w = 0; w < 8 && !bad; w++) if (spad_n[w] != c->mem_r32(0x1F800000u + 4u * (uint32_t)w)) {
    if (nbad < 40) fprintf(stderr, "[xformverify] MISMATCH node=%08x spad+0x%x mine=%08x oracle=%08x\n",
                           node, w * 4, spad_n[w], c->mem_r32(0x1F800000u + 4u * (uint32_t)w)); bad = 1; }
  for (int i = 0; i < 32 && !bad; i++) { if (i >= 12 && i <= 15) continue; if (i == 31) continue;
    if (gd_n[i] != gte_read_data(i)) { if (nbad < 40) fprintf(stderr, "[xformverify] MISMATCH node=%08x GTE-DR%d mine=%08x oracle=%08x\n",
                                                              node, i, gd_n[i], gte_read_data(i)); bad = 1; } }
  if (bad) nbad++; else if (++ngood == 1 || ngood % 20000 == 0) fprintf(stderr, "[xformverify] %ld matches (last node=%08x N=%d)\n", ngood, node, N);
}
void ov_xform_propagate(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("xformverify") ? 1 : 0;
  if (s_v) { ov_xform_propagate_verify(c); return; }
  xform_propagate_body(c);
}

// FUN_80051128 — per-object CHILD-NODE TRANSFORM loop (~3.7% field hot; later-205). A SIBLING of
// xform_propagate (0x80051464): for each child node it builds a per-child rotation from the child's stored
// rotation triple + euler angles, multiplies it onto the parent's world matrix, MVMVA-transforms the child's
// local translation, and accumulates the parent's world translation. Every callee is an already-owned native
// transform PRIMITIVE (ov_rotmat 0x80085480, ov_mat_mul 0x80084110, ov_apply_matlv 0x80084220), so this is
// pure orchestration + scratchpad seeding + integer translation adds — we rec_dispatch the primitives in the
// recomp's EXACT jal order to preserve the matmul→MVMVA GTE-CR coupling (ov_mat_mul CTC2's R→CR0-4 so the
// following ov_apply_matlv reads the right matrix). NO GTE op in this body, NO render packets.
//   Scratchpad work areas (exact addrs from the disas): 0x1F800000 SetVector work (s4/s7) ·
//   0x1F800020 RotMatrix out (s6) · 0x1F800040 composed matrix (s5).
//   GUARD: if node[9]==0 -> return (@512d4). Loop s2 in [0, node[8]) (TOP bound node[8]; the CONTINUE check
//   at the bottom is node[9], same dual-bound idiom as xform_propagate):
//     child = node[0xC0 + 4*s2].
//     Seed work @0x1F800000: zero +4/+0C/+14/+18/+1C; +0=(s16)child[56]; +8=(s16)child[58]; +10=(s16)child[60].
//     sentinel = (s16)child[6].
//     ov_rotmat(a0=child+8 euler, a1=0x1F800020).
//     ov_mat_mul(a0=0x1F800020, a1=0x1F800000, a2=0x1F800040).            // 0x40 = rot × work
//     sentinel == -1 (ROOT, parent = this node):
//       ov_mat_mul(a0=node+152, a1=0x1F800040, a2=child+24);              // child+0x18 = node_mat × 0x40
//       ov_apply_matlv(a0=child, a1=child+44);                           // MVMVA child local-trans -> child+0x2C
//       child[0x2C]+=node[0xAC]; child[0x30]+=node[0xB0]; child[0x34]+=node[0xB4].
//     else (SIBLING, parent = node[0xC0 + 4*sentinel]):
//       p = node[0xC0 + 4*sentinel];
//       ov_mat_mul(a0=p+24, a1=0x1F800040, a2=child+24);
//       ov_apply_matlv(a0=child, a1=child+44);
//       child[0x2C]+=p[0x2C]; child[0x30]+=p[0x30]; child[0x34]+=p[0x34].
// GOTCHAs: (1) +56/58/60 are sign-extended (lhu then sll16/sra16). (2) the sentinel is sll'd by 2 BEFORE the
//   branch (delay slot at 0x511fc) so in the sibling path it is already a byte offset; the parent ptr is at
//   node[0xC0 + sentinel*4]. (3) the loop is a dual-bound: enter on node[8], continue on node[9] (matches the
//   recomp; identical to xform_propagate). `xform51128` gate = same scheme as xformverify (snapshot touched
//   child sub-structs +0x18..+0x37 + the scratchpad work matrix + GTE data regs; both paths run the identical
//   owned primitives, so it verifies the orchestration: bounds, branch select, address math, add order).
static void xform51128_body(Core* c) {
  uint32_t node = c->r[4];
  if (c->mem_r8(node + 9) == 0) return;                           // @512d4 guard
  int i = 0;
  while (i < (int)(uint8_t)c->mem_r8(node + 8)) {                 // TOP bound node[8]
    uint32_t child = c->mem_r32(node + 0xC0 + 4u * (uint32_t)i);
    // seed work matrix @0x1F800000: diagonal from child[56/58/60], the rest zero
    c->mem_w32(0x1F800004u, 0); c->mem_w32(0x1F80000Cu, 0); c->mem_w32(0x1F800014u, 0);
    c->mem_w32(0x1F800018u, 0); c->mem_w32(0x1F80001Cu, 0);
    c->mem_w32(0x1F800000u, (uint32_t)(int32_t)(int16_t)c->mem_r16(child + 56));
    c->mem_w32(0x1F800008u, (uint32_t)(int32_t)(int16_t)c->mem_r16(child + 58));
    c->mem_w32(0x1F800010u, (uint32_t)(int32_t)(int16_t)c->mem_r16(child + 60));
    int16_t sentinel = (int16_t)c->mem_r16(child + 6);
    c->r[4] = child + 8; c->r[5] = 0x1F800020u; ov_rotmat(c);                       // ov_rotmat
    c->r[4] = 0x1F800020u; c->r[5] = 0x1F800000u; c->r[6] = 0x1F800040u; ov_mat_mul(c); // ov_mat_mul
    if (sentinel == -1) {                                        // ROOT: parent = this node
      c->r[4] = node + 152; c->r[5] = 0x1F800040u; c->r[6] = child + 24; ov_mat_mul(c);
      c->r[4] = child; c->r[5] = child + 44; ov_apply_matlv(c);
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(node + 0xAC));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(node + 0xB0));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(node + 0xB4));
    } else {                                                     // SIBLING: parent = node[0xC0 + 4*sentinel]
      uint32_t p = c->mem_r32(node + 0xC0 + 4u * (uint32_t)(int)sentinel);
      c->r[4] = p + 24; c->r[5] = 0x1F800040u; c->r[6] = child + 24; ov_mat_mul(c);
      c->r[4] = child; c->r[5] = child + 44; ov_apply_matlv(c);
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(p + 0x2C));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(p + 0x30));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(p + 0x34));
    }
    i++;
    if (!(i < (int)(uint8_t)c->mem_r8(node + 9))) break;         // CONTINUE bound node[9]
  }
}
static void ov_xform51128_verify(Core* c) {
  uint32_t rs[32]; memcpy(rs, c->r, sizeof rs);
  uint32_t node = c->r[4];
  int n8 = (uint8_t)c->mem_r8(node + 8), n9 = (uint8_t)c->mem_r8(node + 9);
  int N = n8 > n9 ? n8 : n9; if (N > 64) N = 64;
  uint32_t ch[64]; uint32_t snap[64][8];     // child+0x18..+0x37
  for (int i = 0; i < N; i++) { ch[i] = c->mem_r32(node + 0xC0 + 4u * (uint32_t)i);
    for (int w = 0; w < 8; w++) snap[i][w] = c->mem_r32(ch[i] + 0x18 + 4u * (uint32_t)w); }
  uint32_t spad_b[8]; for (int w = 0; w < 8; w++) spad_b[w] = c->mem_r32(0x1F800000u + 4u * (uint32_t)w);
  uint32_t gd_b[32]; for (int i = 0; i < 32; i++) gd_b[i] = gte_read_data(i);
  xform51128_body(c);
  uint32_t nat[64][8], spad_n[8], gd_n[32];
  for (int i = 0; i < N; i++) for (int w = 0; w < 8; w++) nat[i][w] = c->mem_r32(ch[i] + 0x18 + 4u * (uint32_t)w);
  for (int w = 0; w < 8; w++) spad_n[w] = c->mem_r32(0x1F800000u + 4u * (uint32_t)w);
  for (int i = 0; i < 32; i++) gd_n[i] = gte_read_data(i);
  memcpy(c->r, rs, sizeof rs);
  for (int i = 0; i < N; i++) for (int w = 0; w < 8; w++) c->mem_w32(ch[i] + 0x18 + 4u * (uint32_t)w, snap[i][w]);
  for (int w = 0; w < 8; w++) c->mem_w32(0x1F800000u + 4u * (uint32_t)w, spad_b[w]);
  for (int i = 0; i < 32; i++) gte_write_data(i, gd_b[i]);
  rec_super_call(c, 0x80051128u);
  static long ngood = 0, nbad = 0; int bad = 0;
  for (int i = 0; i < N && !bad; i++) for (int w = 0; w < 8; w++)
    if (nat[i][w] != c->mem_r32(ch[i] + 0x18 + 4u * (uint32_t)w)) {
      if (nbad < 40) fprintf(stderr, "[xform51128] MISMATCH node=%08x child[%d]=%08x +0x%x mine=%08x oracle=%08x\n",
                             node, i, ch[i], 0x18 + w * 4, nat[i][w], c->mem_r32(ch[i] + 0x18 + 4u * (uint32_t)w));
      bad = 1; break; }
  for (int w = 0; w < 8 && !bad; w++) if (spad_n[w] != c->mem_r32(0x1F800000u + 4u * (uint32_t)w)) {
    if (nbad < 40) fprintf(stderr, "[xform51128] MISMATCH node=%08x spad+0x%x mine=%08x oracle=%08x\n",
                           node, w * 4, spad_n[w], c->mem_r32(0x1F800000u + 4u * (uint32_t)w)); bad = 1; }
  for (int i = 0; i < 32 && !bad; i++) { if (i >= 12 && i <= 15) continue; if (i == 31) continue;
    if (gd_n[i] != gte_read_data(i)) { if (nbad < 40) fprintf(stderr, "[xform51128] MISMATCH node=%08x GTE-DR%d mine=%08x oracle=%08x\n",
                                                              node, i, gd_n[i], gte_read_data(i)); bad = 1; } }
  if (bad) nbad++; else if (++ngood == 1 || ngood % 2000 == 0) fprintf(stderr, "[xform51128] %ld matches (last node=%08x N=%d)\n", ngood, node, N);
}
void ov_xform51128(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("xform51128") ? 1 : 0;
  if (s_v) { ov_xform51128_verify(c); return; }
  xform51128_body(c);
}

// FUN_800597AC — per-object WORLD-TRANSFORM orchestrator (3.8% field hot). A bigger sibling of
// build_xform/xform_propagate: builds the node's render matrix at node+0x98 from its euler angles +
// translation, optionally a SECONDARY transform variant (node[0x145]/0x146 gated), then propagates to
// all child sub-nodes. Every callee is an already-owned native primitive, so this is pure orchestration
// + scratchpad seeding + integer translation adds — we rec_dispatch the primitives in the recomp's EXACT
// jal order (preserving the matmul→MVMVA/CompMatrixLV GTE-CR coupling; ov_mat_mul/ov_compmatlv now CTC2
// R→CR0-4 so the following ov_apply_matlv reads the right matrix). Scratchpad work areas (exact addrs):
//   0x1F800000 SetVector work · 0x1F800020 CPU-rot mat · 0x1F800040 RotMatrix mat · 0x1F800060 secondary
//   mat (+0x74/78/7C its translation) · 0x1F8000C0 angle SVECTOR. Primitives: 800517BC SetVector,
//   80085480 RotMatrix, 800851F0 CPU-RotMatrix, 80084110 matmul, 80084360 CompMatrixLV, 80084470/80084250
//   apply-matrix, 80084220 MVMVA. node[8] is temporarily forced to node[9] (gated) for the child loop and
//   restored on exit. Child parent select: child[6]<0 → this node (s6/s7 pick node vs the secondary mat);
//   child[6]>=0 → sibling node[0xC0+4*child[6]].
static void orch597AC_body(Core* c) {
  uint32_t node = c->r[4];
  #define R16(a)  ((uint32_t)(int32_t)(int16_t)c->mem_r16(a))   // lh: sign-extended
  #define HU(a)   ((uint32_t)c->mem_r16(a))                     // lhu: zero-extended
  uint8_t saved8 = c->mem_r8(node + 8);
  if ((c->mem_r16(node + 0x17E) & 0x20) && c->mem_r8(node + 0x179) != 0)
    c->mem_w8(node + 8, c->mem_r8(node + 9));
  // SetVector(0x1F800000, node->h[0xB8/BA/BC])
  Mtx::diagonal(c, 0x1F800000u, (int32_t)R16(node+0xB8), (int32_t)R16(node+0xBA), (int32_t)R16(node+0xBC));   // was 0x800517BCu
  // RotMatrix(angles=node->hu[0x54/56/58] → 0x1F8000C0, out 0x1F800040)
  c->mem_w16(0x1F8000C0u, (uint16_t)HU(node+0x54));
  c->mem_w16(0x1F8000C2u, (uint16_t)HU(node+0x56));
  c->mem_w16(0x1F8000C4u, (uint16_t)HU(node+0x58));
  c->r[4]=0x1F8000C0u; c->r[5]=0x1F800040u; ov_rotmat(c);
  // CPU RotMatrix: angle = (node->byte[0x177]&1) ? node->hu[0x14E] : 0, around Y → 0x1F800020
  uint32_t a1ang = (c->mem_r8(node+0x177) & 1) ? HU(node+0x14E) : 0;
  c->mem_w16(0x1F8000C0u, 0);
  c->mem_w16(0x1F8000C2u, (uint16_t)a1ang);
  c->mem_w16(0x1F8000C4u, 0);
  c->r[4]=0x1F8000C0u; c->r[5]=0x1F800020u; rec_dispatch(c, 0x800851F0u);
  // node+0x98 = (0x1F800020 × 0x1F800000) then ×= 0x1F800040 (CompMatrixLV), then 80084470(node+0x98,node+0x88,node+0xAC)
  c->r[4]=0x1F800020u; c->r[5]=0x1F800000u; c->r[6]=node+0x98; ov_mat_mul(c);
  c->r[4]=0x1F800040u; c->r[5]=node+0x98;                       rec_dispatch(c, 0x80084360u);
  c->r[4]=node+0x98;   c->r[5]=node+0x88;  c->r[6]=node+0xAC;   rec_dispatch(c, 0x80084470u);
  // translation accumulate: node+0xAC/B0/B4 += node->h[0x2E/32/36]
  c->mem_w32(node+0xAC, c->mem_r32(node+0xAC) + R16(node+0x2E));
  c->mem_w32(node+0xB0, c->mem_r32(node+0xB0) + R16(node+0x32));
  c->mem_w32(node+0xB4, c->mem_r32(node+0xB4) + R16(node+0x36));
  if (c->mem_r8(node+0x164) == 5) {                              // ov_80084250(node+0x98, node->w[0x10]+24)
    c->r[4]=node+0x98; c->r[5]=c->mem_r32(node+0x10)+24; rec_dispatch(c, 0x80084250u);
  }
  // SECONDARY transform (gated): builds a second matrix at 0x1F800060 + translation at 0x1F800074..7C.
  int s6 = 0;
  if (c->mem_r8(node+0x145) == 0 && (c->mem_r8(node+0x146) & 3) != 0) {
    c->mem_w16(0x1F8000C0u, (uint16_t)HU(node+0x54));
    c->mem_w16(0x1F8000C4u, 0);
    c->mem_w16(0x1F8000C2u, (uint16_t)HU(node+0x56));
    c->r[4]=0x1F8000C0u; c->r[5]=0x1F800040u; ov_rotmat(c);
    c->r[4]=0x1F800020u; c->r[5]=0x1F800000u; c->r[6]=0x1F800060u; ov_mat_mul(c);
    c->r[4]=0x1F800040u; c->r[5]=0x1F800060u;                      rec_dispatch(c, 0x80084360u);
    c->r[4]=0x1F800060u; c->r[5]=node+0x88; c->r[6]=0x1F800074u;   rec_dispatch(c, 0x80084470u);
    c->mem_w32(0x1F800074u, c->mem_r32(0x1F800074u) + R16(node+0x2E));
    c->mem_w32(0x1F800078u, c->mem_r32(0x1F800078u) + R16(node+0x32));
    c->mem_w32(0x1F80007Cu, c->mem_r32(0x1F80007Cu) + R16(node+0x36));
    s6 = 1;
  }
  // CHILD PROPAGATION LOOP
  if (c->mem_r8(node+9) != 0) {
    int s7 = 0, i = 0;
    while (i < (int)(uint8_t)c->mem_r8(node+8)) {                 // top check (node[8], possibly forced to node[9])
      uint32_t child = c->mem_r32(node + 0xC0 + 4u*(uint32_t)i);
      int psel = (int)(int16_t)c->mem_r16(child + 6);             // parent select (signed)
      // SetVector(0x1F800000, child->h[0x38/3A/3C]); RotMatrix(child+8 → 0x1F800020); mat 0x1F800040 = 0x1F800020 × 0x1F800000
      Mtx::diagonal(c, 0x1F800000u, (int32_t)R16(child+0x38), (int32_t)R16(child+0x3A), (int32_t)R16(child+0x3C));   // was 0x800517BCu
      c->r[4]=child+8; c->r[5]=0x1F800020u; ov_rotmat(c);
      c->r[4]=0x1F800020u; c->r[5]=0x1F800000u; c->r[6]=0x1F800040u; ov_mat_mul(c);
      if (psel >= 0) {                                            // SIBLING-by-index: parent = node[0xC0 + 4*psel]
        uint32_t p = c->mem_r32(node + 0xC0 + 4u*(uint32_t)psel);
        c->r[4]=p+24; c->r[5]=0x1F800040u; c->r[6]=child+24; ov_mat_mul(c);
        c->r[4]=child; c->r[5]=child+44; ov_apply_matlv(c);
        c->mem_w32(child+0x2C, c->mem_r32(child+0x2C) + c->mem_r32(p+0x2C));
        c->mem_w32(child+0x30, c->mem_r32(child+0x30) + c->mem_r32(p+0x30));
        c->mem_w32(child+0x34, c->mem_r32(child+0x34) + c->mem_r32(p+0x34));
      } else if (s6 == 0) {                                       // parent = this node (matrix node+0x98, trans node+0xAC)
        c->r[4]=node+0x98; c->r[5]=0x1F800040u; c->r[6]=child+24; ov_mat_mul(c);
        c->r[4]=child; c->r[5]=child+44; ov_apply_matlv(c);
        c->mem_w32(child+0x2C, c->mem_r32(child+0x2C) + c->mem_r32(node+0xAC));
        c->mem_w32(child+0x30, c->mem_r32(child+0x30) + c->mem_r32(node+0xB0));
        c->mem_w32(child+0x34, c->mem_r32(child+0x34) + c->mem_r32(node+0xB4));
      } else if (s7 == 0) {                                       // first secondary child: matrix 0x1F800060, trans 0x1F800074
        c->r[4]=0x1F800060u; c->r[5]=0x1F800040u; c->r[6]=child+24; ov_mat_mul(c);
        c->r[4]=child; c->r[5]=child+44; ov_apply_matlv(c);
        c->mem_w32(child+0x2C, c->mem_r32(child+0x2C) + c->mem_r32(0x1F800074u));
        c->mem_w32(child+0x30, c->mem_r32(child+0x30) + c->mem_r32(0x1F800078u));
        c->mem_w32(child+0x34, c->mem_r32(child+0x34) + c->mem_r32(0x1F80007Cu));
        s7 = 1;
      } else {                                                    // subsequent secondary children: parent = this node again
        c->r[4]=node+0x98; c->r[5]=0x1F800040u; c->r[6]=child+24; ov_mat_mul(c);
        c->r[4]=child; c->r[5]=child+44; ov_apply_matlv(c);
        c->mem_w32(child+0x2C, c->mem_r32(child+0x2C) + c->mem_r32(node+0xAC));
        c->mem_w32(child+0x30, c->mem_r32(child+0x30) + c->mem_r32(node+0xB0));
        c->mem_w32(child+0x34, c->mem_r32(child+0x34) + c->mem_r32(node+0xB4));
      }
      i++;
      if (!(i < (int)(uint8_t)c->mem_r8(node+9))) break;          // bottom check (node[9])
    }
  }
  c->mem_w8(node + 8, saved8);                                    // restore node[8]
  c->r[2] = node;                                                 // (no explicit return; harmless)
  #undef R16
  #undef HU
}
// PSXPORT_DEBUG=orchverify — per-call A/B gate. Snapshot node+0x98..0xB7 (matrix+trans) + node[8], every
// child sub-struct +0x18..0x37, the scratchpad work areas (0x1F800000..0x80 + ANG 0x1F8000C0..C5) and the
// GTE data regs; run native, capture, restore everything, run the recomp body, diff.
static void ov_orch597AC_verify(Core* c) {
  uint32_t rs[32]; memcpy(rs, c->r, sizeof rs);
  uint32_t node = c->r[4];
  int n8 = (uint8_t)c->mem_r8(node+8), n9 = (uint8_t)c->mem_r8(node+9);
  int N = n8 > n9 ? n8 : n9; if (N > 64) N = 64;
  uint32_t ch[64]; uint32_t csnap[64][8];
  for (int i=0;i<N;i++){ ch[i]=c->mem_r32(node+0xC0+4u*(uint32_t)i);
    for (int w=0;w<8;w++) csnap[i][w]=c->mem_r32(ch[i]+0x18+4u*(uint32_t)w); }
  uint32_t nsnap[8]; for (int w=0;w<8;w++) nsnap[w]=c->mem_r32(node+0x98+4u*(uint32_t)w);  // matrix(5)+trans(3)
  uint8_t node8 = c->mem_r8(node+8);
  uint32_t sp_b[34]; for (int w=0;w<32;w++) sp_b[w]=c->mem_r32(0x1F800000u+4u*(uint32_t)w);
  sp_b[32]=c->mem_r32(0x1F8000C0u); sp_b[33]=c->mem_r32(0x1F8000C4u);
  uint32_t gd_b[32]; for (int i=0;i<32;i++) gd_b[i]=gte_read_data(i);
  // NATIVE
  orch597AC_body(c);
  uint32_t cn[64][8], nn[8], sp_n[34], gd_n[32]; uint8_t node8_n=c->mem_r8(node+8);
  for (int i=0;i<N;i++) for (int w=0;w<8;w++) cn[i][w]=c->mem_r32(ch[i]+0x18+4u*(uint32_t)w);
  for (int w=0;w<8;w++) nn[w]=c->mem_r32(node+0x98+4u*(uint32_t)w);
  for (int w=0;w<32;w++) sp_n[w]=c->mem_r32(0x1F800000u+4u*(uint32_t)w);
  sp_n[32]=c->mem_r32(0x1F8000C0u); sp_n[33]=c->mem_r32(0x1F8000C4u);
  for (int i=0;i<32;i++) gd_n[i]=gte_read_data(i);
  // RESTORE
  memcpy(c->r, rs, sizeof rs);
  for (int i=0;i<N;i++) for (int w=0;w<8;w++) c->mem_w32(ch[i]+0x18+4u*(uint32_t)w, csnap[i][w]);
  for (int w=0;w<8;w++) c->mem_w32(node+0x98+4u*(uint32_t)w, nsnap[w]);
  c->mem_w8(node+8, node8);
  for (int w=0;w<32;w++) c->mem_w32(0x1F800000u+4u*(uint32_t)w, sp_b[w]);
  c->mem_w32(0x1F8000C0u, sp_b[32]); c->mem_w32(0x1F8000C4u, sp_b[33]);
  for (int i=0;i<32;i++) gte_write_data(i, gd_b[i]);
  // RECOMP
  rec_super_call(c, 0x800597ACu);
  static long ngood=0, nbad=0; int bad=0;
  for (int w=0;w<8 && !bad;w++) if (nn[w]!=c->mem_r32(node+0x98+4u*(uint32_t)w)) {
    if (nbad<40) fprintf(stderr,"[orchverify] MISMATCH node=%08x +0x%x mine=%08x oracle=%08x\n", node, 0x98+w*4, nn[w], c->mem_r32(node+0x98+4u*(uint32_t)w)); bad=1; }
  if (!bad && node8_n != c->mem_r8(node+8)) { if (nbad<40) fprintf(stderr,"[orchverify] MISMATCH node=%08x node[8] mine=%02x oracle=%02x\n", node, node8_n, c->mem_r8(node+8)); bad=1; }
  for (int i=0;i<N && !bad;i++) for (int w=0;w<8;w++) if (cn[i][w]!=c->mem_r32(ch[i]+0x18+4u*(uint32_t)w)) {
    if (nbad<40) fprintf(stderr,"[orchverify] MISMATCH node=%08x child[%d]=%08x +0x%x mine=%08x oracle=%08x\n", node, i, ch[i], 0x18+w*4, cn[i][w], c->mem_r32(ch[i]+0x18+4u*(uint32_t)w)); bad=1; break; }
  for (int w=0;w<32 && !bad;w++) if (sp_n[w]!=c->mem_r32(0x1F800000u+4u*(uint32_t)w)) {
    if (nbad<40) fprintf(stderr,"[orchverify] MISMATCH node=%08x spad+0x%x mine=%08x oracle=%08x\n", node, w*4, sp_n[w], c->mem_r32(0x1F800000u+4u*(uint32_t)w)); bad=1; }
  if (!bad && sp_n[32]!=c->mem_r32(0x1F8000C0u)) { if(nbad<40) fprintf(stderr,"[orchverify] MISMATCH node=%08x spad+0xC0 mine=%08x oracle=%08x\n", node, sp_n[32], c->mem_r32(0x1F8000C0u)); bad=1; }
  if (!bad && sp_n[33]!=c->mem_r32(0x1F8000C4u)) { if(nbad<40) fprintf(stderr,"[orchverify] MISMATCH node=%08x spad+0xC4 mine=%08x oracle=%08x\n", node, sp_n[33], c->mem_r32(0x1F8000C4u)); bad=1; }
  for (int i=0;i<32 && !bad;i++){ if (i>=12&&i<=15) continue; if (i==31) continue;
    if (gd_n[i]!=gte_read_data(i)) { if (nbad<40) fprintf(stderr,"[orchverify] MISMATCH node=%08x GTE-DR%d mine=%08x oracle=%08x\n", node, i, gd_n[i], gte_read_data(i)); bad=1; } }
  if (bad) nbad++; else if (++ngood==1 || ngood%20000==0) fprintf(stderr,"[orchverify] %ld matches (last node=%08x N=%d)\n", ngood, node, N);
  // keep native results live
  memcpy(c->r, rs, sizeof rs);
  for (int i=0;i<N;i++) for (int w=0;w<8;w++) c->mem_w32(ch[i]+0x18+4u*(uint32_t)w, cn[i][w]);
  for (int w=0;w<8;w++) c->mem_w32(node+0x98+4u*(uint32_t)w, nn[w]);
  c->mem_w8(node+8, node8_n);
  for (int w=0;w<32;w++) c->mem_w32(0x1F800000u+4u*(uint32_t)w, sp_n[w]);
  c->mem_w32(0x1F8000C0u, sp_n[32]); c->mem_w32(0x1F8000C4u, sp_n[33]);
  for (int i=0;i<32;i++) gte_write_data(i, gd_n[i]);
  c->r[2] = node;
}
void ov_orch597AC(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("orchverify") ? 1 : 0;
  if (s_v) { ov_orch597AC_verify(c); return; }
  orch597AC_body(c);
}

// PSXPORT_DEBUG=subcnt — submitter call-counter. Registered (super-call) on candidate submit fns to see
// which actually fire per scene + how often, so the un-owned variants worth porting are picked by data,
// not guesswork. One slot per registered address; per-present-frame counts flushed on frame change.
static uint32_t s_subcnt[8], s_subaddr[8]; static int s_subn, s_sub_lastf = -1;
static void subcnt_tick(Core* c, uint32_t addr) {
  int slot = -1; for (int i = 0; i < s_subn; i++) if (s_subaddr[i] == addr) { slot = i; break; }
  if (slot < 0 && s_subn < 8) { slot = s_subn; s_subaddr[s_subn++] = addr; }
  if (gpu_frame_no(c) != s_sub_lastf) {
    if (s_sub_lastf >= 0) { fprintf(stderr, "[subcnt] f%d", s_sub_lastf);
      for (int i = 0; i < s_subn; i++) { fprintf(stderr, " %08x=%u", s_subaddr[i], s_subcnt[i]); s_subcnt[i] = 0; } }
    s_sub_lastf = gpu_frame_no(c);
  }
  if (slot >= 0) s_subcnt[slot]++;
  rec_super_call(c, addr);
}
void ov_subcnt_b320(Core* c) { subcnt_tick(c, 0x8003B320u); }
void ov_subcnt_c8f4(Core* c) { subcnt_tick(c, 0x8003C8F4u); }

// PSXPORT_DEBUG=rwalk — phase-2 render-walk caller counter. The per-object render dispatch
// gen_func_8003CCA4 is driven by one of several orchestrators (the render-layer/list drainers); count
// which fire per scene so the phase-2 flush walk worth owning next is picked by data. Super-calls.
// 0x8003B588 (later-231 "Pass A") — the field WATER render pass, OWNED native real-depth.
// Node 0x800E7E80 (cmd-ptr array @+0xC0). Structure (disas.py 0x8003b588): node-byte bookkeeping
// (anim/state @0x8003b5a0..0x8003b698, ported 1:1 below), then the PSX per-object transform SETUP leaf
// 0x800597AC (rec_dispatch — still PSX, does NO render), then the per-object RENDER. Live, node+0xD=0 →
// (node+0xD)&0xB=0 → render-case table 0x80014EC8[0]=0x8003CD00 = the native eproj FLUSH case, so routing
// the render through the native submit_perobj_render gives the water world-coord FLOAT projection with REAL
// per-vertex depth. Previously the whole pass ran as pure PSX (engine_render.cpp d0(0x8003b588)), so the
// inner jal 0x8003CCA4 emitted GTE packets the native renderer couldn't project (is3d=0) → the water drew
// as a flat 2D FOREGROUND fill OVER the world (the "sea on top" bug). NB: own this TOGETHER with the native
// ground (later-231 caveat) so both sort by real depth — a still-2D-FG ground would occlude the real-depth water.
void ov_rwalk_b588(Core* c) {
  const uint32_t node = 0x800E7E80u;
  uint32_t v1 = c->mem_r8(node + 0x0D);
  if ((v1 & 0xD0) == 0) {
    c->mem_w8(node + 0x0D, 0);                                   // @698
  } else {
    v1 |= 0x02; c->mem_w8(node + 0x0D, v1);                      // @5b0/@5bc
    if (!(v1 & 0x20)) {                                          // @5b8: (v1&0x20)==0 → byte setup
      uint32_t g = c->mem_r8(0x1F800247u);                       // @5cc/@608/@658 (same addr)
      if (v1 & 0x10) {                                           // @5c0 → @5cc
        int to5f4 = ((g & 0x30) != 0) || ((g & 0x03) < 2);       // @5d4 / @5e0+@5e4
        if (!to5f4) {                                            // @5e8 → @68c (v0=208)
          c->mem_w8(node + 0x18, 208); c->mem_w8(node + 0x19, 208); c->mem_w8(node + 0x1A, 208);
        } else {                                                 // @5f4
          uint32_t v1b = c->mem_r8(node + 0x0D);
          if (v1b & 0x80) {                                      // @5fc≠0 → @604/@684 (v0 computed, then 32/32)
            int32_t r = ((int32_t)((uint32_t)Trig::rsin(c, (int32_t)((g & 0x0F) << 7)) << 16)) >> 22;   // FUN_80083E80 -> native Trig::rsin
            c->mem_w8(node + 0x18, (uint8_t)(r + 48)); c->mem_w8(node + 0x19, 32); c->mem_w8(node + 0x1A, 32);
          } else if (v1b & 0x40) {                               // @62c → @634 (v0=32)
            c->mem_w8(node + 0x18, 32); c->mem_w8(node + 0x19, 32); c->mem_w8(node + 0x1A, 32);
          } else {                                               // @640 (v0=128)
            c->mem_w8(node + 0x18, 128); c->mem_w8(node + 0x19, 128); c->mem_w8(node + 0x1A, 128);
          }
        }
      } else {                                                   // @64c: (v1&0x10)==0
        if (v1 & 0x80) {                                         // @650≠0 → @654/@680/@684 (v0=r+16+32, then 32/32)
          int32_t r = ((int32_t)((uint32_t)Trig::rsin(c, (int32_t)((g & 0x0F) << 7)) << 16)) >> 22;   // FUN_80083E80 -> native Trig::rsin
          c->mem_w8(node + 0x18, (uint8_t)(r + 48)); c->mem_w8(node + 0x19, 32); c->mem_w8(node + 0x1A, 32);
        } else {                                                 // @67c → @680/@684 (v0=0+32)
          c->mem_w8(node + 0x18, 32); c->mem_w8(node + 0x19, 32); c->mem_w8(node + 0x1A, 32);
        }
      }
    }
  }
  c->r[4] = node; rec_dispatch(c, 0x800597ACu);                  // @69c: PSX transform setup (no render)
  if (c->mem_r8(node + 1) != 0) {                                // @6a4: per-object RENDER (native real-depth)
    uint8_t s1 = c->mem_r8(node + 8);
    if ((c->mem_r16(node + 0x17E) & 0x20) && c->mem_r8(node + 0x179)) c->mem_w8(node + 8, c->mem_r8(node + 9));
    void ov_perobj_render(Core*);                                // engine_render_walk.cpp (native per-object render)
    c->r[4] = node; ov_perobj_render(c);                         // was inner jal 0x8003CCA4 (PSX GTE)
    c->mem_w8(node + 8, s1);
  }
}
// ov_rlist_probe (`debug rlist`) and ov_ccase_probe (`debug ccase`/`rwalk`) moved to
// render_debug_probes.cpp (2026-07 restructure) — no callers in this file (reachable only via their own
// PSXPORT_DEBUG channel, no actual call site).

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

// =====================================================================================================
// M3 PROVENANCE — own the 2D BACKGROUND layer by WHO drew it, not by per-prim screen coverage.
//
// The field's screen-space backdrop is a scrolling tilemap: a grid of 352 16×16 textured tiles drawn by
// FUN_80115598 (a per-area render overlay, loaded interpreted). Each tile is far below the bg_2d coverage
// threshold, so the old heuristic mis-classified the WHOLE backdrop as HUD and painted it over the world.
// Provenance fixes this at the source: we bracket the drawer, capture the packet-pool span it produces,
// and tag every OT node in that span RQ_BACKGROUND (gpu_native.cpp node_is_bg) — regardless of tile size.
// The drawer still builds its packets and links them into the OT (this owns the LAYER decision, not yet
// the geometry); retiring the OT walk + a native background renderer is the remaining M3/M4 step.
//
// Registered via the same scan-on-load path as the submit library (the overlay reloads at reused
// addresses across areas, so a fixed address is fragile). Signature: the tile command-word build
// `lui a1,0x7d80; ori a1,a1,0x8080` (the 0x7D/0x7C textured-rect opcode + neutral 0x808080 modulate) —
// unique in RAM. A field can carry SEVERAL backdrop layers (parallax), so this one override is registered
// at every matching entry; it super-calls the EXACT body it intercepted via g_override_tgt (set by the
// interp on override entry), not a stored address.
extern uint32_t g_override_tgt;
void ov_bg_tilemap(Core* c) {
  uint32_t entry = g_override_tgt;                       // the backdrop drawer this call intercepted
  uint32_t p0 = c->mem_r32(0x800BF544u);                 // packet-pool write ptr before the backdrop build
  rec_super_call(c, entry);                              // run the real tilemap drawer (builds + links tiles)
  uint32_t p1 = c->mem_r32(0x800BF544u);                 // ptr after = the span of THIS frame's backdrop tiles
  gpu_bg_range_add(c, p0 | 0x80000000u, p1 | 0x80000000u);
}

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
void stage_scan_overlay(Core* c, uint32_t base, uint32_t size);   // engine_stage.cpp: own GAME stage SM
void demo_scan_overlay(Core* c, uint32_t base, uint32_t size);    // engine_demo.cpp: own DEMO menu SM
// Gameplay-overlay pure LEAF: a 2D table lookup `tab[52*a1 + a0] & (mask16 << 4)` where
// tab = *0x8014c804 and mask16 = *0x8014c800 (overlay data). RE'd from the disasm (0x8013fae0,
// later-188 — the single hottest overlay piece, 4.25% / 39.9k calls/300fr, mis-bucketed under
// FUN_8013F0DC until the prof_report overlay-resolution fix). It's a pure tile/attribute lookup;
// owning it native is a free, exact win (same family as isqrt/sin/cos). Registered by signature in
// engine_scan_overlay (anchor on the unique pair lw 0x8014c804 / lhu 0x8014c800), backtracked to
// the fn entry. PSXPORT_DEBUG=tileverify gates a per-call predict-vs-recomp compare.
static uint32_t s_tile_entry = 0;
static inline uint32_t tile_lookup_calc(Core* c, uint32_t a0, uint32_t a1) {
  uint32_t idx  = 52u * a1 + a0;
  uint8_t  b    = c->mem_r8(c->mem_r32(0x8014c804u) + idx);
  uint16_t mask = c->mem_r16(0x8014c800u);
  return (uint32_t)(b & (uint32_t)(mask << 4));
}
void ov_tile_lookup(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  uint32_t mine = tile_lookup_calc(c, a0, a1);
  static int s_tv = -1; if (s_tv < 0) s_tv = cfg_dbg("tileverify") ? 1 : 0;
  if (s_tv && s_tile_entry) {
    rec_super_call(c, s_tile_entry);                  // oracle: interpret the original body
    static long ng = 0, nb = 0;
    if ((uint32_t)c->r[2] != mine) { if (nb++ < 40)
        fprintf(stderr, "[tileverify] MISMATCH a0=%x a1=%x mine=%x oracle=%x\n", a0, a1, mine, (uint32_t)c->r[2]); }
    else if (++ng % 20000 == 0) fprintf(stderr, "[tileverify] %ld matches\n", ng);
  }
  c->r[2] = mine;
}

// ============================================================================================
// FUN_8013F4DC — per-object triangle-scan SOLID-TILE gatherer (0x8013f4dc..0x8013fadc). Hot field fn
// (~11% — the profiler mislabeled it under FUN_8013F0DC; the anim/scale SM at 0x8013efa8 the handoff
// named is actually cold, 3 calls/run). a0=object; a1/a2/a3 each point to a 2-word {x,y} corner. It
// y-sorts the 3 corners, rasterizes the triangle scanline-by-scanline, and for every covered tile cell
// whose attribute (the owned ov_tile_lookup at 0x8013fae0) is nonzero APPENDS the cell's tile id into
// the object's list (obj[0x10 + 2*count], count byte at obj+6, capped 254) — the per-object solid-tile
// broad-phase that the collision/interaction code reads. CLEAN INTEGER DATA feeding retained PSX
// content → faithful exact-match port (same family as the owned ov_tile_lookup leaf), gated on the
// CONTENT-INTERFACE bar: the gathered list must be bit-identical to the recomp body. NO render here.
//
// Structure (all 4 asm loop variants share one scanline body): s6 is ALWAYS the left-x 16.16
// accumulator (s1 = s6>>16), s5 ALWAYS the right-x accumulator (s3 = s5>>16). The cross-product sign
// (which side the middle vertex is on) only selects WHICH edge-gradient steps each accumulator:
//   path A (cross>0): right(s5) follows the long edge A→C; left(s6) follows short A→B then B→C.
//   path B (cross<=0): swapped — left(s6) follows A→C; right(s5) follows A→B then B→C.
// The right accumulator s5 also gets a constant +0x10000 nudge before scanning (asm 0x8013f610 delay).
static uint32_t s_tilescan_entry = 0;
// One scanline at row s2, right end = s5>>16, left start = s6>>16 (mirrors asm 0x8013f6c8..0x8013f798).
// The cell array (obj+0xC == *0x800ecf78, +4 past its {xbound,width} header) is row-major: the element
// for column s1, row s2 lives at base+4 + 2*(s1*width + s2). The asm reaches it because its FIRST s0
// advance (the `j 0x8013f788` entry) lands with v0 = the linear index s1*width+s2 (left in v0 by the
// delay slot), so s0 += 2*(s1*width+s2); every later advance is the plain +2*width row step — leaving
// s0 = base+4 + 2*(s1*width+s2) at each iteration. An EMPTY cell (0xFFFF/-1) is skipped — the dedup
// `beq v1,v0` runs with v0 = -1, left in v0 by the loop-branch delay slot `addiu v0,zero,-1`
// (0x8013f798) — as is a non-solid cell (ov_tile_lookup low16 == 0). Non-empty solid cells are
// appended (capped 254).
static inline void tilescan_scanline(Core* c, uint32_t obj, int s2, int s5, int s6) {
  if (s2 < 0) return;                              // bltz s2 → skip (advance handled by caller)
  int s3 = s5 >> 16;                               // right end x
  int s1 = s6 >> 16;                               // left start x
  int width = (int16_t)c->mem_r16(obj + 10);       // obj[10] (row stride + y-bound)
  if (!(s2 < width)) return;                       // slt s2,width ; beq → skip scanline
  int xb = (int16_t)c->mem_r16(obj + 8);           // obj[8] = x-bound
  if (!(s3 < xb)) s3 = xb - 1;                     // clamp right to xb-1
  if (s1 < 0) s1 = 0;                              // clamp left to 0
  uint32_t base = c->mem_r32(0x800ecf78u) + 4;     // cell-array base past the 2-halfword header
  for (; s1 <= s3; s1++) {
    uint32_t s0 = base + 2u * (uint32_t)(s1 * width + s2);   // row-major (s1,s2) cell address
    int v1 = (int16_t)c->mem_r16(s0);
    if (v1 != -1) {                                // skip empty cell: dedup beq v1,v0 where v0=-1 (delay slot 0x8013f798)
      uint32_t look = tile_lookup_calc(c, (uint32_t)s1, (uint32_t)s2);  // jal 0x8013fae0 (owned leaf)
      if ((uint16_t)look != 0) {                   // sll v0,16 ; beq zero → low16 nonzero
        uint8_t cnt = c->mem_r8(obj + 6) & 0xFF;
        if (cnt < 254) {                           // sltiu count,254
          c->mem_w8(obj + 6, (uint8_t)(cnt + 1));
          c->mem_w16(obj + 16 + 2u * cnt, c->mem_r16(s0));  // lhu cell → sh obj[16+2*count]
        }
      }
    }
  }
}
static void tilescan_body(Core* c) {
  uint32_t obj = c->r[4], a1p = c->r[5], a2p = c->r[6], a3p = c->r[7];
  int xA = (int)c->mem_r32(a1p), yA = (int)c->mem_r32(a1p + 4);
  int xB = (int)c->mem_r32(a2p), yB = (int)c->mem_r32(a2p + 4);
  int xC = (int)c->mem_r32(a3p), yC = (int)c->mem_r32(a3p + 4);
  int slt_BA = (yB < yA);                          // slt v0,s7,a1 (delay-slot, always evaluated)
  if (yB == yA && yB == yC) return;                // 0x8013f52c: all-y-equal degenerate → no scan
  if (slt_BA)      { int tx=xA; xA=xB; xB=tx; int ty=yA; yA=yB; yB=ty; }   // sort y: A<=B
  if (yC < yA)     { int tx=xA; xA=xC; xC=tx; int ty=yA; yA=yC; yC=ty; }   //         A<=C
  if (yC < yB)     { int tx=xB; xB=xC; xC=tx; int ty=yB; yB=yC; yC=ty; }   //         B<=C
  int t1 = xB - xA, t2 = xC - xA, a2d = yB - yA, a3d = yC - yA;
  int gradAB, gradBC, gradAC, s5, s6;
  if (yB == yA) {                                  // flat top
    if (xA < xB) { s6 = xA << 16; s5 = xB << 16; } else { s6 = xB << 16; s5 = xA << 16; }
    gradAB = 0;
  } else {
    s5 = xA << 16; s6 = xA << 16;
    gradAB = (int32_t)((uint32_t)t1 << 16) / a2d;  // (xB-xA)<<16 / (yB-yA), trunc toward 0
  }
  s5 += 0x10000;                                   // 0x8013f610 (always)
  gradBC = (yB == yC) ? 0 : (int32_t)((uint32_t)(xC - xB) << 16) / (yC - yB);
  int32_t v1cross = (int32_t)((uint32_t)(-a3d) * (uint32_t)t1);   // mflo(-a3d * t1)
  int32_t t0cross = (int32_t)((uint32_t)t2 * (uint32_t)a2d);      // mflo(t2 * a2d)
  gradAC = (a3d != 0) ? (int32_t)((uint32_t)t2 << 16) / a3d : 0;  // (xC-xA)<<16 / (yC-yA)
  int32_t crossv = v1cross + t0cross;
  int s2 = yA;
  if (crossv > 0) {                                // 0x8013f6b8 (blez not taken): s6 steps the short edges
    for (; s2 < yB; s2++) { tilescan_scanline(c, obj, s2, s5, s6); s6 += gradAB; s5 += gradAC; }
    for (; s2 <= yC; s2++) { tilescan_scanline(c, obj, s2, s5, s6); s6 += gradBC; s5 += gradAC; }
  } else {                                         // 0x8013f8b8 (blez taken, crossv<=0): s5 steps the short edges
    for (; s2 < yB; s2++) { tilescan_scanline(c, obj, s2, s5, s6); s5 += gradAB; s6 += gradAC; }
    for (; s2 <= yC; s2++) { tilescan_scanline(c, obj, s2, s5, s6); s5 += gradBC; s6 += gradAC; }
  }
}
void ov_tilescan(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("tilescanverify") ? 1 : 0;
  if (!s_v) { tilescan_body(c); return; }
  // A/B gate: snapshot the only mutated state (count byte + tile list), run native, capture, restore,
  // run the recomp body, compare. Registers don't matter (callee-saved, restored by the body's epilogue).
  uint32_t obj = c->r[4]; uint32_t rs[35]; memcpy(rs, c->r, sizeof rs);
  uint8_t  cnt_b = c->mem_r8(obj + 6);
  uint16_t list_b[254]; for (int i = 0; i < 254; i++) list_b[i] = c->mem_r16(obj + 16 + 2u * i);
  // NATIVE
  tilescan_body(c);
  uint8_t cnt_n = c->mem_r8(obj + 6);
  uint16_t list_n[254]; for (int i = 0; i < 254; i++) list_n[i] = c->mem_r16(obj + 16 + 2u * i);
  // RESTORE
  memcpy(c->r, rs, sizeof rs);
  c->mem_w8(obj + 6, cnt_b);
  for (int i = 0; i < 254; i++) c->mem_w16(obj + 16 + 2u * i, list_b[i]);
  // RECOMP
  rec_super_call(c, s_tilescan_entry);
  static long ngood = 0, nbad = 0; int bad = 0;
  if (cnt_n != c->mem_r8(obj + 6)) {
    if (nbad < 8) fprintf(stderr, "[tilescanverify] MISMATCH obj=%08x count mine=%u oracle=%u\n",
                          obj, cnt_n, c->mem_r8(obj + 6));
    bad = 1; }
  int olim = c->mem_r8(obj + 6); if (olim > 254) olim = 254;
  for (int i = 0; i < olim && !bad; i++) if (list_n[i] != c->mem_r16(obj + 16 + 2u * i)) {
    if (nbad < 40) fprintf(stderr, "[tilescanverify] MISMATCH obj=%08x list[%d] mine=%04x oracle=%04x\n",
                           obj, i, list_n[i], c->mem_r16(obj + 16 + 2u * i)); bad = 1; }
  if (bad) nbad++; else if (++ngood == 1 || ngood % 200 == 0)
    fprintf(stderr, "[tilescanverify] %ld matches (last obj=%08x count=%u)\n", ngood, obj, cnt_n);
  // keep native results live
  c->mem_w8(obj + 6, cnt_n);
  for (int i = 0; i < 254; i++) c->mem_w16(obj + 16 + 2u * i, list_n[i]);
}

// OVERRIDE SYSTEM REMOVED (2026-06-22): the overlay-load scan existed only to register AUTO overrides
// (rec_set_interp_override_auto) on freshly-loaded overlay library fns (tile-lookup, tile-scan gatherer,
// background tilemap, GT3/GT4 callers/submitters) plus the GAME/DEMO state-machine entries. With the
// override table gone, there is nothing to register; native ownership is wired by PC calling the native
// fn directly (top-down). The classify_*/ov_* defs above are kept as future direct-call targets.
void engine_submit_register_autodetect(void) {
  // No-op: no overlay-load hook to install (override table removed).
}
