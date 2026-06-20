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
#include "core.h"
#include "game.h"   // Fps60State::current_object (was g_current_object)
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)

#define COL_MASK     0xFFF0F0F0u   // low-nibble-per-byte clear applied to RGB words (matches the GPU)


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
#define PKT_POOL_PTR  0x800BF544u
void ov_render_cmd(Core* c) {
  if (cfg_dbg("rcmd") && probe_frame_ok(c)) {
    uint8_t mode = c->mem_r8(0x800BF870u);
    fprintf(stderr, "[rcmd] f%d mode=%02x geomblk=%08x ot=%08x flag=%08x ra=%08x M=",
            gpu_frame_no(c), mode, c->r[4], c->r[5], c->r[6], c->r[31]);
    for (int i = 0; i < 8; i++) fprintf(stderr, "%s%08x", i ? "," : "", (uint32_t)gte_read_ctrl(i));
    fprintf(stderr, "\n");
  }
  float ord = proj_obj_center_ord();                 // object depth from the live composed transform
  // Capture the packet-pool address span this command's renderer writes (the pool POINTER doesn't move for
  // the overlay variants, so track the actual stores), and tag it with the object's world-position depth.
  extern int g_pkt_track; extern uint32_t g_pkt_lo, g_pkt_hi;
  g_pkt_track = 1; g_pkt_lo = 0xFFFFFFFFu; g_pkt_hi = 0;
  rec_super_call(c, 0x8003F698u);
  g_pkt_track = 0;
  if (g_pkt_hi > g_pkt_lo) gpu_obj_depth_add(c, g_pkt_lo, g_pkt_hi, ord);
  if (cfg_dbg("objz") && probe_frame_ok(c) && g_pkt_hi > g_pkt_lo)
    fprintf(stderr, "[rcmddep] mode=%02x span %08x->%08x (%dB) ord=%.4f\n",
            c->mem_r8(0x800BF870u), g_pkt_lo, g_pkt_hi, (int)(g_pkt_hi - g_pkt_lo), (double)ord);
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
// PC-NATIVE render path (gte_beetle.c / gpu_native.c): project an explicit model vertex through the GTE's
// composed camera×object transform in FLOAT (no gte_op), and tee a projected quad straight to the VK
// rasterizer with real per-pixel depth (no GP0 packet, no OT). Same primitives native_terrain.cpp uses.
typedef struct { int ir1, ir2, ir3, sz, sx, sy; float px, py, pz, vx, vy, vz; } ProjVtx;
void  proj_native_xform(int vx, int vy, int vz, ProjVtx* out);
float proj_pz_to_ord(float pz);
void  gpu_draw_world_quad(Core* c, const float* px, const float* py, const float* depth,
                          const int* u, const int* v, const uint8_t* r, const uint8_t* g,
                          const uint8_t* b, uint16_t tp, uint16_t clut, int semi);
// gen_func_8007FDB0 — POLY_GT3 (gouraud-textured triangle) submit.
// Record = 36 bytes: {+0 rgb0|code, +4 rgb1 (rgb2 = rgb1<<4), +8 uv0|clut, +12 uv1|tpage,
//   +16 VXY0, +20 VZ0(lo)|VZ1(hi), +24 VXY1, +28 VXY2, +32 VZ2(lo)|uv2(hi)}.
// PC-NATIVE POLY_GT3 submit — project the 3 model verts through the engine's composed transform in FLOAT
// (proj_native_xform, no gte_op) and tee a degenerate quad (v2 repeated) to the VK rasterizer with real
// per-pixel depth. No GP0 packet, no OT, no guest write.
static void submit_poly_gt3_native(Core* c) {
  uint32_t rec = c->r[4], count = c->r[6];
  proj_set_H((uint16_t)gte_read_ctrl(26));
  for (uint32_t i = 0; i < count; i++, rec += 36) {
    uint32_t vz01 = c->mem_r32(rec + 20);
    uint32_t xy0 = c->mem_r32(rec + 16), xy1 = c->mem_r32(rec + 24), xy2 = c->mem_r32(rec + 28);
    ProjVtx p[3];
    proj_native_xform((int16_t)xy0, (int16_t)(xy0 >> 16), (int16_t)vz01,         &p[0]);
    proj_native_xform((int16_t)xy1, (int16_t)(xy1 >> 16), (int16_t)(vz01 >> 16), &p[1]);
    proj_native_xform((int16_t)xy2, (int16_t)(xy2 >> 16), (int16_t)c->mem_r32(rec + 32), &p[2]);
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
    gpu_draw_world_quad(c, px, py, depth, u, v, r, g, b, tp, clut, semi);
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
  uint32_t rec = c->r[4], count = c->r[6];
  proj_set_H((uint16_t)gte_read_ctrl(26));
  for (uint32_t i = 0; i < count; i++, rec += 44) {
    // model verts: V0=rec+20(XY)|rec+24.lo(Z), V1=rec+28|rec+24.hi, V2=rec+32|rec+36.lo, V3=rec+40|rec+36.hi
    uint32_t vz01 = c->mem_r32(rec + 24), vz23 = c->mem_r32(rec + 36);
    uint32_t xy0 = c->mem_r32(rec + 20), xy1 = c->mem_r32(rec + 28),
             xy2 = c->mem_r32(rec + 32), xy3 = c->mem_r32(rec + 40);
    ProjVtx p[4];
    proj_native_xform((int16_t)xy0, (int16_t)(xy0 >> 16), (int16_t)vz01,          &p[0]);
    proj_native_xform((int16_t)xy1, (int16_t)(xy1 >> 16), (int16_t)(vz01 >> 16),  &p[1]);
    proj_native_xform((int16_t)xy2, (int16_t)(xy2 >> 16), (int16_t)vz23,          &p[2]);
    proj_native_xform((int16_t)xy3, (int16_t)(xy3 >> 16), (int16_t)(vz23 >> 16),  &p[3]);
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
    gpu_draw_world_quad(c, px, py, depth, u, v, r, g, b, tp, clut, semi);
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
      gpu_draw_world_quad(c, px, py, depth, u, v, r, g, b, tp, clut, semi);
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
static void pdisp_count(Core* c, int native, uint32_t mode, uint32_t tgt) {
  static int s_pd = -1; if (s_pd < 0) s_pd = cfg_dbg("pdisp") ? 1 : 0;
  if (!s_pd) return;
  static int last_f = -1, nat = 0, fb = 0; static uint32_t fbmode[32] = {0};
  if (gpu_frame_no(c) != last_f) {
    if (last_f >= 0) {
      fprintf(stderr, "[pdisp] f%d native=%d fallback=%d", last_f, nat, fb);
      for (int m = 0; m < 32; m++) if (fbmode[m]) fprintf(stderr, " m%d=%u", m, fbmode[m]);
      fprintf(stderr, "\n");
    }
    last_f = gpu_frame_no(c); nat = fb = 0; for (int m = 0; m < 32; m++) fbmode[m] = 0;
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
  if (c->mem_r8(MODE_FORCE) != 0 || (flag & 1u)) { pdisp_count(c, 1, 0, 0); native_gt3gt4(c, geomblk, otbase); return; }
  uint32_t mode = c->mem_r8(MODE_BYTE);
  if (mode >= 22) { pdisp_count(c, 1, mode, 0); native_gt3gt4(c, geomblk, otbase); return; }
  uint32_t tgt = c->mem_r32(MODE_TABLE + mode * 4);
  if (tgt == DISPATCH_GENERIC) { pdisp_count(c, 1, mode, tgt); native_gt3gt4(c, geomblk, otbase); return; }
  pdisp_count(c, 0, mode, tgt);
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
      // object-local matrix is 9 CONTIGUOUS halfwords at cmd+0x18; each column's 3 elements are at
      // +0,+6,+0xc and the column base advances by a halfword (col*2). The real MIPS (gen_func_8003CDD8)
      // does `addiu v0, a3, 0x18/0x1a/0x1c; lhu (v0), 6(v0), 0xc(v0)` — aligned loads, stride col*2.
      // (A col*1 stride reads unaligned/overlapping halfwords for col=1 → garbage matrix → bad transform.)
      uint32_t cc = cmd + 0x18 + col * 2;
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
  submit_perobj_flush(c);
}

// NATIVE per-object render DISPATCH — gen_func_8003CCA4 (later-135). The phase-2 per-object render entry:
// stash the current render object (scratch 0x1F80028C), compute the flush flag (= node[0xb]==0xf, the
// "world" objects), select a case by idx = node[0xd]&0xb (idx>=9 → not rendered), and for the common
// flush-only case (jump-table target 0x8003CD00) run the native per-object flush — NO guest render code.
// The other cases add a secondary effect pass (gen_func_8003D584/8003F344/8003F3F4/8003F4C4/8003F594)
// over the just-emitted packet range; those are not owned yet, so for them we super-call the recomp body
// (which still calls the now-native func_8003CDD8 for the flush, then the secondary pass). At the field
// only idx0 (flush-only) fires (PSXPORT_DEBUG=ccase: 1 call/frame, target 0x8003cd00).
// Per-object view-space depth from the object's WORLD POSITION + the camera (ov_object_cull dot). camFwd is
// the camera forward (unit×4096); >>12 yields view-Z in world units, matching proj_native's pz so 2D object
// prims share the 3D depth band. The collectable apple is an op-2D billboard quad rendered here with no
// depth — this is the world-position depth it inherits.
static float object_world_view_depth(Core* c, uint32_t node) {
  int32_t ox = (int16_t)c->mem_r16(node + 0x2e), oy = (int16_t)c->mem_r16(node + 0x32), oz = (int16_t)c->mem_r16(node + 0x36);
  int32_t cx = (int16_t)c->mem_r16(0x1F8000D2u), cy = (int16_t)c->mem_r16(0x1F8000D6u), cz = (int16_t)c->mem_r16(0x1F8000DAu);
  int32_t fx = (int16_t)c->mem_r16(0x1F8000E8u), fy = (int16_t)c->mem_r16(0x1F8000EAu), fz = (int16_t)c->mem_r16(0x1F8000ECu);
  int64_t dot = (int64_t)(ox - cx) * fx + (int64_t)(oy - cy) * fy + (int64_t)(oz - cz) * fz;
  return (float)(dot >> 12);
}

static void submit_perobj_render(Core* c) {
  uint32_t node = c->r[4];
  c->mem_w32(SCR + 0x28C, node);                          // current render object (read by downstream code)
  uint32_t idx = c->mem_r8(node + 0xD) & 0xB;
  if (idx >= 9) return;                                // not rendered
  uint32_t flag = (c->mem_r8(node + 0xB) == 0xF) ? 1u : 0u;
  uint32_t tgt = c->mem_r32(0x80014EC8u + idx * 4);
  // Tag the packet-pool span this object renders into with its PC-native WORLD-POSITION depth, so its 2D
  // billboard prims (apple quad, etc.) occlude by real depth at the deferred OT walk. (g_pkt_track records
  // the actual store range — the pool POINTER doesn't move for these renderers.)
  extern int g_pkt_track; extern uint32_t g_pkt_lo, g_pkt_hi;
  g_pkt_track = 1; g_pkt_lo = 0xFFFFFFFFu; g_pkt_hi = 0;
  if (tgt == 0x8003CD00u) { c->r[4] = node; c->r[5] = flag; submit_perobj_flush(c); }  // flush-only (native)
  else                    { rec_super_call(c, 0x8003CCA4u); }                          // secondary-effect case
  g_pkt_track = 0;
  if (g_pkt_hi > g_pkt_lo) gpu_obj_depth_add(c, g_pkt_lo, g_pkt_hi, proj_pz_to_ord(object_world_view_depth(c, node)));
}
void ov_perobj_render(Core* c) {
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
// Types ≥33 render nothing (the recomp skips them) → treated as handled no-ops.
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
        if (tgt == RCASE_PEROBJ) { c->r[4] = n; submit_perobj_render(c); }  // self-tags its world depth
        else {
          // default case: the node's own render fn (node+24) — e.g. a collectable's billboard-quad drawer.
          // Tag the packet span it produces with the object's PC-native world-position depth.
          extern int g_pkt_track; extern uint32_t g_pkt_lo, g_pkt_hi;
          g_pkt_track = 1; g_pkt_lo = 0xFFFFFFFFu; g_pkt_hi = 0;
          c->r[4] = n; rec_dispatch(c, c->mem_r32(n + 24));
          g_pkt_track = 0;
          if (g_pkt_hi > g_pkt_lo) gpu_obj_depth_add(c, g_pkt_lo, g_pkt_hi, proj_pz_to_ord(object_world_view_depth(c, n)));
        }
      }
    }
    n = next;
  }
}
void ov_render_walk(Core* c) {
  submit_render_walk(c);
}

// NATIVE depth for the collectable BILLBOARD-QUAD drawer — gen_func_8003C8F4. This is the single chokepoint
// for the op-2D textured quads the collectables (apple + score pickups) draw as 2D billboards: it GTE-projects
// the quad with the object's composed camera×object transform already live in CR0-7 (RTPT/RTPS @0x8003c98c/9dc).
// The recomp body emits the quad packet into the OT with NO depth, so the pickups fell to the flat 2D band and
// did not occlude. We compute the object's PC-native WORLD-POSITION view-Z from that live transform
// (proj_obj_center_ord = our float proj of the object origin = CR5-7 view translation) and tag the packet span
// the body writes, so the deferred OT walk gives each pickup its real world depth. Reached from multiple render
// walks (owned and un-owned) — owning it HERE covers them all. Super-calls the body (content/packet unchanged).
float proj_obj_center_ord(void);
void ov_collectable_quad(Core* c) {
  float ord = proj_obj_center_ord();                 // object-center depth from the live composed transform
  extern int g_pkt_track; extern uint32_t g_pkt_lo, g_pkt_hi;
  g_pkt_track = 1; g_pkt_lo = 0xFFFFFFFFu; g_pkt_hi = 0;
  rec_super_call(c, 0x8003C8F4u);
  g_pkt_track = 0;
  if (g_pkt_hi > g_pkt_lo) gpu_obj_depth_add(c, g_pkt_lo, g_pkt_hi, ord);   // g_pkt_lo/hi already KSEG
}

// ===================================================================================================
// NATIVE WORLD-BUILDING render walk — gen_func_8003BB50 (the SNAPSHOT-QUEUE object render driver).
//
// This is the engine's per-frame object render phase for the FIELD: it drains the per-object render
// QUEUE (a snapshot of node pointers the entity walk enqueued — scratchpad cursor 0x1F800140 / count
// 0x1F800146), and for each live object dispatches its per-type renderer (by node+0xb through jump
// table 0x80014A70). The recomp body drove this loop but threw away each object's WORLD DEPTH — the 2D
// billboard prims the renderers emit (sprites / flat-textured quads with no projected vertices) then
// fell to a flat enumeration-ordered 2D band, so collectables/decals did NOT occlude by distance.
//
// We OWN the loop natively so the engine drives object rendering; the per-type renderers themselves stay
// guest content (rec_dispatch). Each object's PC-native WORLD-POSITION depth is attached downstream at the
// universal render-command dispatcher (ov_render_cmd on 0x8003F698), where the composed camera×object
// transform is live and the command's packet-pool span is captured — see ov_render_cmd above.
//
// Decoded byte-faithful from the recomp body (subagent RE, this session). Globals (PSX scratchpad):
//   swap_flag 0x1F800136, live list_ptr 0x1F80013C, read cursor 0x1F800140, live count 0x1F800144,
//   snap_count 0x1F800146; list base const 0x800F2410. Queue entries are raw entity-NODE pointers.
#define RQ_SWAP_FLAG  0x1F800136u
#define RQ_LIST_PTR   0x1F80013Cu
#define RQ_CURSOR     0x1F800140u
#define RQ_LIVE_CNT   0x1F800144u
#define RQ_SNAP_CNT   0x1F800146u
#define RQ_LIST_BASE  0x800F2410u
#define RQ_JUMPTABLE  0x80014A70u

// Replicate one jump-table case (node[0xb] → renderer), calling the existing guest renderers via
// rec_dispatch (content stays PSX). The depth for `node` is already published by the caller.
static void rq_dispatch_case(Core* c, uint32_t node, uint32_t tgt) {
  switch (tgt) {
    case 0x8003BC00u:   // per-object render dispatch, then the optional main renderer
    case 0x8003BC24u: { // alt scene/overlay submitter, then the optional main renderer
      c->r[4] = node;
      rec_dispatch(c, tgt == 0x8003BC00u ? 0x8003CCA4u : 0x80122974u);
      uint8_t b = c->mem_r8(node + 0xB);
      if (b & 0x40) { c->r[4] = node; c->r[5] = 80; c->r[6] = 0; rec_dispatch(c, 0x8002AE0Cu); }
      else if (b & 0x80) { c->r[4] = node; c->r[5] = (uint32_t)(int32_t)(int16_t)c->mem_r16(node + 0x80); c->r[6] = 0;
                           rec_dispatch(c, 0x8002AE0Cu); }
      break; }
    case 0x8003BC6Cu: c->r[4] = node; rec_dispatch(c, 0x8003C2D4u); break;
    case 0x8003BC7Cu: c->r[4] = node; rec_dispatch(c, 0x8003C464u); break;
    case 0x8003BC8Cu: c->r[4] = node; rec_dispatch(c, 0x8003C5F8u); break;
    case 0x8003BC9Cu: c->r[4] = node; rec_dispatch(c, 0x8003C788u); break;
    case 0x8003BCACu: { c->r[4] = node; rec_dispatch(c, 0x8003C2D4u);                 // then indirect node[0x7c]
                        uint32_t fn = c->mem_r32(node + 0x7C); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    case 0x8003BCB4u: { uint32_t fn = c->mem_r32(node + 0x7C); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    case 0x8003BCC0u: { uint32_t fn = c->mem_r32(node + 0x18); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    default: break;   // 0x8003BCD0 = the skip/default case: render nothing
  }
}

static void submit_render_walk_snapshot(Core* c) {
  // Prologue: queue double-buffer SWAP (only when swap_flag==0) — capture the live count/cursor as the
  // read snapshot and reset the live write cursor to the list base for next frame's enqueues.
  if (c->mem_r8(RQ_SWAP_FLAG) == 0) {
    uint16_t cnt = c->mem_r16(RQ_LIVE_CNT);
    uint32_t lst = c->mem_r32(RQ_LIST_PTR);
    c->mem_w16(RQ_LIVE_CNT, 0);
    c->mem_w32(RQ_LIST_PTR, RQ_LIST_BASE);
    c->mem_w16(RQ_SNAP_CNT, cnt);
    c->mem_w32(RQ_CURSOR, lst);
  }
  int16_t count = (int16_t)c->mem_r16(RQ_SNAP_CNT);
  uint32_t cursor = c->mem_r32(RQ_CURSOR);
  while (count != 0) {
    uint32_t node = c->mem_r32(cursor);
    cursor += 4;
    count--;
    if (c->mem_r8(node + 1) == 0) continue;                  // not live
    uint8_t t = c->mem_r8(node + 0xB);
    if (t >= 144) continue;                                  // out of jump-table range -> render nothing
    uint32_t tgt = c->mem_r32(RQ_JUMPTABLE + t * 4);
    if (tgt == 0x8003BCD0u) continue;                        // default/skip case
    // Render the object, tagging the packet-pool span it produces with its PC-native world-position depth
    // so its 2D billboard prims (collectable quads, etc.) occlude for real at the deferred OT walk.
    extern int g_pkt_track; extern uint32_t g_pkt_lo, g_pkt_hi;
    g_pkt_track = 1; g_pkt_lo = 0xFFFFFFFFu; g_pkt_hi = 0;
    rq_dispatch_case(c, node, tgt);                          // run the object's per-type renderer (guest content)
    g_pkt_track = 0;
    if (g_pkt_hi > g_pkt_lo) gpu_obj_depth_add(c, g_pkt_lo, g_pkt_hi, proj_pz_to_ord(object_world_view_depth(c, node)));
  }
}

void ov_render_walk_snapshot(Core* c) {
  submit_render_walk_snapshot(c);
}

// ===================================================================================================
// NATIVE AUXILIARY render walks — gen_func_8003BCF4 / 8003BF00 / 8003EEC0 (issue #4: flames/ropes
// drew OVER occluding foliage). These three are the secondary per-object render walks the field runs
// IN ADDITION to the owned snapshot walk 8003BB50 — they drain their own object queues/lists and
// dispatch each live object's per-type renderer through a jump table, exactly like 8003BB50. The
// recomp bodies drove the loop but threw away each object's WORLD DEPTH, so the 2D billboard prims the
// renderers emit (flame sprites, rope decals) landed in NO obj_depth span → fell to the flat 2D band →
// drew in enumeration order, in front of nearer foliage. We OWN the loops PC-native (faithful per-node
// lift of each recomp body), and after running each object's renderer we tag the packet-pool span it
// produced with the object's PC-native WORLD-POSITION depth via gpu_obj_depth_add — exactly as the
// owned snapshot walk does — so the deferred OT walk gives each effect its real world depth. The
// per-type renderers themselves stay guest CONTENT (rec_dispatch), unchanged. Depth tagging is
// PER-NODE (not a whole-walk merged span — a merged span mis-attributes depth in multi-effect scenes).
//
// Decoded byte-faithful from the recomp bodies (tools/disas.py, this session). For each: node is the
// only arg (a0); `next`/queue-advance is captured the same instant the recomp body captures it.

// --- 8003BCF4 -------------------------------------------------------------------------------------
// SNAPSHOT-QUEUE walk, same double-buffer-swap prologue as 8003BB50 but a DIFFERENT queue + table.
//   swap_flag 0x1F800136, live list_ptr 0x1F800148, live count 0x1F800150, snap_count 0x1F800152,
//   read cursor 0x1F80014C; list base const 0x800F26C8; jump table 0x80014CB0 (33 entries, type<33).
//   Liveness node+1!=0; type node+0xb. The s3=0x800C0000 base in two cases reads global 0x800BF870.
// Jump-table case bodies (a0=node), decoded from 0x8003BDAC..0x8003BED8 (0x8003BED8 = skip/continue):
//   idx 0,15  (0x8003BDAC): rec_dispatch 0x8003CCA4 (per-object render dispatch)
//   idx 1     (0x8003BDBC): g=*0x800BF870; ==0 -> 0x801341E8; ==6 -> 0x80123C14; else skip
//   idx 2     (0x8003BDF4): g; ==1->0x80129114, ==7->0x80120D2C, ==10->0x8011AD44, ==15->0x80115338,
//                           else -> 0x80117984
//   idx 3     (0x8003BE74): rec_dispatch 0x80136748
//   idx 16    (0x8003BE84): rec_dispatch 0x8003C2D4
//   idx 17    (0x8003BEA4): rec_dispatch 0x8003C464
//   idx 21    (0x8003BEB4): rec_dispatch 0x8003C2D4, then node[0x7c] indirect (if nonzero)
//   idx 22    (0x8003BEBC): node[0x7c] indirect (if nonzero)
//   idx 23    (0x8003BE94): node[0x7c] indirect (if nonzero), THEN rec_dispatch 0x8003C464
//   idx 32    (0x8003BEC8): node[0x18] indirect (if nonzero)
#define AUX_BCF4_SWAP    0x1F800136u
#define AUX_BCF4_LISTPTR 0x1F800148u
#define AUX_BCF4_CURSOR  0x1F80014Cu
#define AUX_BCF4_LIVECNT 0x1F800150u
#define AUX_BCF4_SNAPCNT 0x1F800152u
#define AUX_BCF4_BASE    0x800F26C8u
#define AUX_BCF4_TABLE   0x80014CB0u
#define AUX_BCF4_SKIP    0x8003BED8u
#define G_RENDER_MODE    0x800BF870u

static void aux_bcf4_case(Core* c, uint32_t node, uint32_t tgt) {
  switch (tgt) {
    case 0x8003BDACu: c->r[4] = node; rec_dispatch(c, 0x8003CCA4u); break;
    case 0x8003BDBCu: { uint8_t g = c->mem_r8(G_RENDER_MODE);
                        if (g == 0)      { c->r[4] = node; rec_dispatch(c, 0x801341E8u); }
                        else if (g == 6) { c->r[4] = node; rec_dispatch(c, 0x80123C14u); }
                        break; }
    case 0x8003BDF4u: { uint8_t g = c->mem_r8(G_RENDER_MODE); uint32_t fn;
                        if      (g == 1)  fn = 0x80129114u;
                        else if (g == 7)  fn = 0x80120D2Cu;
                        else if (g == 10) fn = 0x8011AD44u;
                        else if (g == 15) fn = 0x80115338u;
                        else              fn = 0x80117984u;
                        c->r[4] = node; rec_dispatch(c, fn); break; }
    case 0x8003BE74u: c->r[4] = node; rec_dispatch(c, 0x80136748u); break;
    case 0x8003BE84u: c->r[4] = node; rec_dispatch(c, 0x8003C2D4u); break;
    case 0x8003BEA4u: c->r[4] = node; rec_dispatch(c, 0x8003C464u); break;
    case 0x8003BEB4u: { c->r[4] = node; rec_dispatch(c, 0x8003C2D4u);
                        uint32_t fn = c->mem_r32(node + 0x7C); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    case 0x8003BEBCu: { uint32_t fn = c->mem_r32(node + 0x7C); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    case 0x8003BE94u: { uint32_t fn = c->mem_r32(node + 0x7C); if (fn) { c->r[4] = node; rec_dispatch(c, fn); }
                        c->r[4] = node; rec_dispatch(c, 0x8003C464u); break; }
    case 0x8003BEC8u: { uint32_t fn = c->mem_r32(node + 0x18); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    default: break;   // 0x8003BED8 = skip/default: render nothing
  }
}

void ov_rwalk_aux_bcf4(Core* c) {
  if (c->mem_r8(AUX_BCF4_SWAP) == 0) {              // queue double-buffer swap (only when swap_flag==0)
    uint16_t cnt = c->mem_r16(AUX_BCF4_LIVECNT);
    uint32_t lst = c->mem_r32(AUX_BCF4_LISTPTR);
    c->mem_w16(AUX_BCF4_LIVECNT, 0);
    c->mem_w32(AUX_BCF4_LISTPTR, AUX_BCF4_BASE);
    c->mem_w16(AUX_BCF4_SNAPCNT, cnt);
    c->mem_w32(AUX_BCF4_CURSOR, lst);
  }
  int16_t count = (int16_t)c->mem_r16(AUX_BCF4_SNAPCNT);
  uint32_t cursor = c->mem_r32(AUX_BCF4_CURSOR);
  while (count != 0) {
    uint32_t node = c->mem_r32(cursor);
    cursor += 4;
    count--;
    if (c->mem_r8(node + 1) == 0) continue;                  // not live
    uint8_t t = c->mem_r8(node + 0xB);
    if (t >= 33) continue;                                   // out of jump-table range -> render nothing
    uint32_t tgt = c->mem_r32(AUX_BCF4_TABLE + t * 4);
    if (tgt == AUX_BCF4_SKIP) continue;                      // skip/default
    extern int g_pkt_track; extern uint32_t g_pkt_lo, g_pkt_hi;
    g_pkt_track = 1; g_pkt_lo = 0xFFFFFFFFu; g_pkt_hi = 0;
    aux_bcf4_case(c, node, tgt);                             // per-type renderer (guest content)
    g_pkt_track = 0;
    if (g_pkt_hi > g_pkt_lo) gpu_obj_depth_add(c, g_pkt_lo, g_pkt_hi, proj_pz_to_ord(object_world_view_depth(c, node)));
  }
}

// --- 8003BF00 -------------------------------------------------------------------------------------
// SNAPSHOT-QUEUE walk (own queue), same swap prologue but type range <32.
//   swap_flag 0x1F800136, live list_ptr 0x1F800154, live count 0x1F80015C, snap_count 0x1F80015E,
//   read cursor 0x1F800158; list base const 0x800F2738; jump table 0x80014D38 (32 entries, type<32).
//   Liveness node+1!=0; type node+0xb.
// Jump-table case bodies (a0=node), decoded from 0x8003BFAC..0x8003C028 (0x8003C028 = skip/continue):
//   idx 0,15 (0x8003BFAC): rec_dispatch 0x8003CCA4
//   idx 16   (0x8003BFBC): rec_dispatch 0x8003C2D4
//   idx 17   (0x8003BFCC): rec_dispatch 0x8003C464
//   idx 18   (0x8003BFDC): rec_dispatch 0x8003C5F8
//   idx 19   (0x8003BFEC): rec_dispatch 0x8003C788
//   idx 31   (0x8003BFFC): g=*0x800BF870; ==20 -> 0x8010FC70; else -> 0x8004CC88
#define AUX_BF00_SWAP    0x1F800136u
#define AUX_BF00_LISTPTR 0x1F800154u
#define AUX_BF00_CURSOR  0x1F800158u
#define AUX_BF00_LIVECNT 0x1F80015Cu
#define AUX_BF00_SNAPCNT 0x1F80015Eu
#define AUX_BF00_BASE    0x800F2738u
#define AUX_BF00_TABLE   0x80014D38u
#define AUX_BF00_SKIP    0x8003C028u

static void aux_bf00_case(Core* c, uint32_t node, uint32_t tgt) {
  switch (tgt) {
    case 0x8003BFACu: c->r[4] = node; rec_dispatch(c, 0x8003CCA4u); break;
    case 0x8003BFBCu: c->r[4] = node; rec_dispatch(c, 0x8003C2D4u); break;
    case 0x8003BFCCu: c->r[4] = node; rec_dispatch(c, 0x8003C464u); break;
    case 0x8003BFDCu: c->r[4] = node; rec_dispatch(c, 0x8003C5F8u); break;
    case 0x8003BFECu: c->r[4] = node; rec_dispatch(c, 0x8003C788u); break;
    case 0x8003BFFCu: { uint8_t g = c->mem_r8(G_RENDER_MODE);
                        c->r[4] = node; rec_dispatch(c, g == 20 ? 0x8010FC70u : 0x8004CC88u); break; }
    default: break;   // 0x8003C028 = skip/default: render nothing
  }
}

void ov_rwalk_aux_bf00(Core* c) {
  if (c->mem_r8(AUX_BF00_SWAP) == 0) {             // queue double-buffer swap (only when swap_flag==0)
    uint16_t cnt = c->mem_r16(AUX_BF00_LIVECNT);
    uint32_t lst = c->mem_r32(AUX_BF00_LISTPTR);
    c->mem_w16(AUX_BF00_LIVECNT, 0);
    c->mem_w32(AUX_BF00_LISTPTR, AUX_BF00_BASE);
    c->mem_w16(AUX_BF00_SNAPCNT, cnt);
    c->mem_w32(AUX_BF00_CURSOR, lst);
  }
  int16_t count = (int16_t)c->mem_r16(AUX_BF00_SNAPCNT);
  uint32_t cursor = c->mem_r32(AUX_BF00_CURSOR);
  while (count != 0) {
    uint32_t node = c->mem_r32(cursor);
    cursor += 4;
    count--;
    if (c->mem_r8(node + 1) == 0) continue;                  // not live
    uint8_t t = c->mem_r8(node + 0xB);
    if (t >= 32) continue;                                   // out of jump-table range -> render nothing
    uint32_t tgt = c->mem_r32(AUX_BF00_TABLE + t * 4);
    if (tgt == AUX_BF00_SKIP) continue;                      // skip/default
    extern int g_pkt_track; extern uint32_t g_pkt_lo, g_pkt_hi;
    g_pkt_track = 1; g_pkt_lo = 0xFFFFFFFFu; g_pkt_hi = 0;
    aux_bf00_case(c, node, tgt);                             // per-type renderer (guest content)
    g_pkt_track = 0;
    if (g_pkt_hi > g_pkt_lo) gpu_obj_depth_add(c, g_pkt_lo, g_pkt_hi, proj_pz_to_ord(object_world_view_depth(c, node)));
  }
}

// --- 8003EEC0 -------------------------------------------------------------------------------------
// LINKED-LIST walk (NOT a snapshot queue): head = *0x800F2738 (the same list base 8003BF00 enqueues
// into), next = node+36 (captured BEFORE dispatch), liveness node+1!=0, type node+0xb (<33), jump
// table 0x80015000 (33 entries). Has NO swap prologue.
// Jump-table case bodies (a0=node), decoded from 0x8003EF20..0x8003EF78 (0x8003EF78 = skip/advance):
//   idx 0,15 (0x8003EF20): rec_dispatch 0x8003CCA4
//   idx 1    (0x8003EF30): rec_dispatch 0x8003CCA4, then rec_dispatch 0x8003B704
//   idx 16   (0x8003EF40): rec_dispatch 0x8003C2D4, then IF node[2]==1 rec_dispatch 0x8003B704
//   idx 32   (0x8003EF68): node[0x18] indirect (if nonzero)
#define AUX_EEC0_HEAD    0x800F2738u
#define AUX_EEC0_TABLE   0x80015000u
#define AUX_EEC0_SKIP    0x8003EF78u

static void aux_eec0_case(Core* c, uint32_t node, uint32_t tgt) {
  switch (tgt) {
    case 0x8003EF20u: c->r[4] = node; rec_dispatch(c, 0x8003CCA4u); break;
    case 0x8003EF30u: c->r[4] = node; rec_dispatch(c, 0x8003CCA4u);
                      c->r[4] = node; rec_dispatch(c, 0x8003B704u); break;
    case 0x8003EF40u: { c->r[4] = node; rec_dispatch(c, 0x8003C2D4u);
                        if (c->mem_r8(node + 2) == 1) { c->r[4] = node; rec_dispatch(c, 0x8003B704u); } break; }
    case 0x8003EF68u: { uint32_t fn = c->mem_r32(node + 0x18); if (fn) { c->r[4] = node; rec_dispatch(c, fn); } break; }
    default: break;   // 0x8003EF78 = skip/default: render nothing
  }
}

void ov_rwalk_aux_eec0(Core* c) {
  uint32_t node = c->mem_r32(AUX_EEC0_HEAD);
  while (node) {
    uint32_t next = c->mem_r32(node + 36);                   // captured before dispatch (recomp s1)
    if (c->mem_r8(node + 1) != 0) {
      uint8_t t = c->mem_r8(node + 0xB);
      if (t < 33) {
        uint32_t tgt = c->mem_r32(AUX_EEC0_TABLE + t * 4);
        if (tgt != AUX_EEC0_SKIP) {
          extern int g_pkt_track; extern uint32_t g_pkt_lo, g_pkt_hi;
          g_pkt_track = 1; g_pkt_lo = 0xFFFFFFFFu; g_pkt_hi = 0;
          aux_eec0_case(c, node, tgt);                       // per-type renderer (guest content)
          g_pkt_track = 0;
          if (g_pkt_hi > g_pkt_lo) gpu_obj_depth_add(c, g_pkt_lo, g_pkt_hi, proj_pz_to_ord(object_world_view_depth(c, node)));
        }
      }
    }
    node = next;
  }
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
  c->r[4] = node + 84; c->r[5] = SCR; rec_dispatch(c, 0x80085480u);
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
  c->r[4] = (uint32_t)(int16_t)c->mem_r16(node + 84); c->r[5] = node + 152; rec_dispatch(c, 0x80084D10u);
  c->r[4] = (uint32_t)(int16_t)c->mem_r16(node + 86); c->r[5] = node + 152; rec_dispatch(c, 0x80084EB0u);
  c->r[4] = (uint32_t)(int16_t)c->mem_r16(node + 88); c->r[5] = node + 152; rec_dispatch(c, 0x80085050u);
  c->mem_w32(node + 172, (uint32_t)(int16_t)c->mem_r16(node + 46));
  c->mem_w32(node + 176, (uint32_t)(int16_t)c->mem_r16(node + 50));
  c->mem_w32(node + 180, (uint32_t)(int16_t)c->mem_r16(node + 54));
  c->r[4] = node; rec_dispatch(c, 0x80051464u);
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
    c->r[4] = (uint32_t)(int32_t)(int16_t)c->mem_r16(child + 8);  c->r[5] = 0x1F800000u; rec_dispatch(c, 0x80084D10u); // rot_x
    c->r[4] = (uint32_t)(int32_t)(int16_t)c->mem_r16(child + 10); c->r[5] = 0x1F800000u; rec_dispatch(c, 0x80084EB0u); // rot_y
    c->r[4] = (uint32_t)(int32_t)(int16_t)c->mem_r16(child + 12); c->r[5] = 0x1F800000u; rec_dispatch(c, 0x80085050u); // rot_z
    if (sentinel == -1) {                                        // ROOT: parent = this node
      c->r[4] = node + 152; c->r[5] = 0x1F800000u; c->r[6] = child + 24; rec_dispatch(c, 0x80084110u); // child_mat = node_mat × work
      c->r[4] = child; c->r[5] = child + 44; rec_dispatch(c, 0x80084220u);                              // MVMVA → child+0x2C
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(node + 0xAC));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(node + 0xB0));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(node + 0xB4));
    } else {                                                     // SIBLING: parent = node[0xC0 + 4*sentinel]
      uint32_t p = c->mem_r32(node + 0xC0 + 4u * (uint32_t)(int)sentinel);
      c->r[4] = p + 24; c->r[5] = 0x1F800000u; c->r[6] = child + 24; rec_dispatch(c, 0x80084110u); // child_mat = sibling_mat × work
      c->r[4] = child; c->r[5] = child + 44; rec_dispatch(c, 0x80084220u);
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
  if (gpu_frame_no(c) != s_rl_last) {     // 1/frame gate (own latch)
    s_rl_last = gpu_frame_no(c);
    fprintf(stderr, "[rlist] f%d:", gpu_frame_no(c));
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
  if (gpu_frame_no(c) != s_cc_lastf) {
    if (s_cc_lastf >= 0) { fprintf(stderr, "[ccase] f%d", s_cc_lastf);
      for (int i = 0; i < 12; i++) if (s_cc[i]) fprintf(stderr, " idx%d=%u(t=%08x)", i, s_cc[i], s_cctgt[i]);
      fprintf(stderr, "\n"); }
    s_cc_lastf = gpu_frame_no(c); for (int i = 0; i < 12; i++) s_cc[i] = 0;
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

static void engine_scan_overlay(Core* c, uint32_t base, uint32_t size) {
  stage_scan_overlay(c, base, size);   // own the GAME stage state machine when GAME.BIN loads
  demo_scan_overlay(c, base, size);    // own the DEMO/front-end menu state machine when DEMO loads
  uint32_t lo = base & ~3u, hi = (base + size) & ~3u;
  void ov_gt3gt4_caller(Core*);
  void ov_bg_tilemap(Core*);
  for (uint32_t a = lo; a + 4 <= hi; a += 4) {
    // (0a) the pure 2D table-lookup leaf (0x8013fae0): anchor on the unique pair `lw v1,0x8014c804`
    //      then `lhu v0,0x8014c800` 0x10 apart; backtrack to the fn entry past the previous `jr ra`.
    if (c->mem_r32(a) == 0x8C63C804u && c->mem_r32(a + 0x10) == 0x9442C800u) {
      uint32_t entry = lo;
      for (uint32_t b = a; b > lo && b > a - 0x40; b -= 4)
        if (c->mem_r32(b - 4) == 0x03E00008u) { entry = b + 4; break; }
      s_tile_entry = entry;
      rec_set_interp_override_auto(entry, ov_tile_lookup);
      if (cfg_dbg("submit"))
        fprintf(stderr, "[submit] own tile-lookup leaf @ 0x%08X (in load 0x%08X+0x%X)\n", entry, base, size);
      continue;
    }
    // (0) the screen-space BACKGROUND tilemap drawer (M3 provenance): anchor on the unique tile
    //     command-word build `lui a1,0x7d80 ; ori a1,a1,0x8080`, backtrack to the fn entry, bracket it
    //     so its packet-pool span is tagged RQ_BACKGROUND (gpu_native node_is_bg). later this session.
    if (c->mem_r32(a) == 0x3C057D80u && c->mem_r32(a + 4) == 0x34A58080u) {
      uint32_t entry = lo;
      for (uint32_t b = a; b > lo && b > a - 0x400; b -= 4)
        if (c->mem_r32(b - 4) == 0x03E00008u) { entry = b + 4; break; }
      rec_set_interp_override_auto(entry, ov_bg_tilemap);
      if (cfg_dbg("submit"))
        fprintf(stderr, "[submit] own BACKGROUND tilemap drawer @ 0x%08X (in load 0x%08X+0x%X)\n",
                entry, base, size);
      continue;
    }
    // (1) generic GT3/GT4 CALLER (e.g. the mode-0 render 0x80146478): anchor on `addiu a0,a0,16`,
    //     backtrack to the fn entry, verify the caller signature, own it (runs the native submitters).
    if (c->mem_r32(a) == 0x24840010u) {
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
  rec_set_overlay_load_hook(engine_scan_overlay);
}
