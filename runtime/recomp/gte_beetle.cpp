// GTE (COP2) coprocessor, lifted from the Beetle GPL-2 fork (mednafen/psx/gte.c, compiled
// as-is). All the game's geometry — RTPS/RTPT projection, NCLIP, matrix ops, color/depth —
// flows through here; our previous stub was a no-op, so any 3D was inert. This adapts the
// recomp GTE interface (r3000.h) to Beetle's GTE_* API and provides faithful-first stubs for
// the few externs gte.c references (PGXP off, widescreen off, savestate unused) so the math
// matches the oracle exactly. The widescreen GTE-scale hack stays OFF here (fps60 tier later).
#include "cfg.h"
#include "core.h"
#include "game.h"             // Core::game->gte (per-instance GTE register file) for gte_bind
#include "pgxp.h"             // class Pgxp — subpixel-cache currently-bound accessor
#include "proj_params.h"      // class ProjParams — camview + per-frame projection constants
#include "proj_vtx.h"         // ProjVtx — proj_native_vertex's POD out-struct (was reached via render.h)
#include "render_substrate.h" // rsub.pgxp / rsub.projParams / rsub.otAttr — per-Core substrate
#include <lucent/log.h>
#include <stdbool.h>
#include <stdint.h>

// Beetle GTE API (mednafen/psx/gte.h), declared locally to avoid pulling Beetle headers.
extern "C" { // mednafen GTE (gte.c, compiled as C)
void GTE_Init(void);
void GTE_Power(void);
int32_t GTE_Instruction(uint32_t instr);
void GTE_WriteCR(unsigned which, uint32_t value);
void GTE_WriteDR(unsigned which, uint32_t value);
uint32_t GTE_ReadCR(unsigned which);
uint32_t GTE_ReadDR(unsigned which);
}

// Externs gte.c references — faithful-first values. SANCTIONED VENDOR INTEROP: the vendored Beetle
// gte.c reads these four knobs by extern; folding them into GteRegs would mean editing the fork for
// no behavioral gain (all are compile-time-constant "faithful" values here).
bool psx_gte_overclock = false;
uint8_t widescreen_hack = 0; // GTE widescreen-scale hack OFF (faithful)
uint8_t widescreen_hack_aspect_ratio_setting = 0;
uint32_t gMode = 0; // PGXP mode 0 = off

// --- PGXP subpixel cache (vertex-smoothing / PSX-wobble fix) -------------------------------------
// PSX projected vertices to INTEGER screen coords, so 3D geometry jitters ("wobbles") as the
// camera/object moves — the classic PS1 look. Beetle's GTE already computes the SUBPIXEL-precise
// projected x/y/z (gte.c TransformXY: precise_x/precise_y/precise_z) and hands them to this hook,
// keyed `v` = the packed integer SXY the game then copies verbatim into its GP0 vertex packets.
// We cache (precise_x,y,z) keyed by that packed int; the GPU tee (gpu_native.c) looks the precise
// coords back up by each vertex's integer (sx,sy) and rasterizes with FLOAT positions instead of
// the integer-snapped ones. This is value-keyed "PGXP-lite": on a key collision we simply fall
// back to the integer coords (a miss), so a wrong match can only ever cost smoothing, not correctness.
// The cache is reset each presented frame (pgxp_frame_reset) so a stale precise value from a prior
// frame can't be re-applied to a freshly-placed integer vertex (kills cross-frame wobble artifacts).
// PGXP-lite subpixel cache moved to `class Pgxp` on Render (game/render/pgxp.h). The `PGXP_pushSXYZ2f`
// Beetle callback + the `PGXP_NCLIP*` / `MDFNSS_StateAction` vestigial stubs live in pgxp.cpp now.

// Recomp GTE interface (r3000.h) -> Beetle GTE. mfc2/cfc2/mtc2/ctc2/lwc2/swc2 map to the
// data/control register ports; the COP2 ops map to GTE_Instruction.
uint32_t gte_read_data(uint32_t reg) {
  return GTE_ReadDR(reg);
}
void gte_write_data(uint32_t reg, uint32_t v) {
  GTE_WriteDR(reg, v);
}
uint32_t gte_read_ctrl(uint32_t reg) {
  return GTE_ReadCR(reg);
}
void gte_write_ctrl(uint32_t reg, uint32_t v) {
  GTE_WriteCR(reg, v);
}
// --- Widescreen RE tool (journal later-55): histogram the projected screen-X the GTE produces, to
// learn whether world geometry is projected beyond the 320 display window. Result: ~14% of verts
// land outside [0,320) (near bands [-64,0)≈3%, [320,384)≈11%) — the GTE DOES project wide geometry,
// but VRAM packing (textures abut the FB) blocks drawing it. Gated on PSXPORT_WS_SXHIST; accumulates
// and gpu_present dumps it every 500 frames. Reads SXY-FIFO (DR12/13/14) after RTPS/RTPT.
#include <stdio.h>
#include <stdlib.h>
// (histogram state lives on GteRegs::dbg — the bound core's instance, gte_state.h)
static void ws_sx_record(void) {
  GteDebug &d = GTE_CurState()->dbg;
  if (d.sxhist_on < 0) {
    d.sxhist_on = lucent::channel_on("sxhist") ? 1 : 0;
  }
  if (!d.sxhist_on) {
    return;
  }
  for (unsigned r = 12; r <= 14; r++) {
    int16_t sx = (int16_t)(GTE_ReadDR(r) & 0xFFFF);
    d.sx_n++;
    if (sx < 0) {
      d.sx_oob_lo++;
    }
    if (sx >= 320) {
      d.sx_oob_hi++;
    }
    int b = (sx + 256) / 64;
    if (b < 0) {
      b = 0;
    }
    if (b > 15) {
      b = 15;
    }
    d.sx_hist[b]++;
  }
}
void ws_sx_dump(const char *tag) {
  GteDebug &d = GTE_CurState()->dbg;
  if (d.sxhist_on != 1 || d.sx_n == 0) {
    return;
  }
  lucent::info("ws_sxhist",
               "{} n={}  below0={}({:.1f}%)  atOrAbove320={}({:.1f}%)",
               tag ? tag : "(null)",
               d.sx_n,
               d.sx_oob_lo,
               100.0 * d.sx_oob_lo / d.sx_n,
               d.sx_oob_hi,
               100.0 * d.sx_oob_hi / d.sx_n);
  for (int b = 0; b < 16; b++) {
    lucent::info("ws_sxhist", "  [{:5}..{:5}) {}", b * 64 - 256, b * 64 - 256 + 64, d.sx_hist[b]);
  }
  for (int b = 0; b < 16; b++) {
    d.sx_hist[b] = 0;
  }
  d.sx_n = d.sx_oob_lo = d.sx_oob_hi = 0;
}

#include "mods.h" // g_mods.fps60 gates the capture tap (was g_fps60_on)

// --- GTE/lighting RE probe (PSXPORT_GTEPROBE=N) -------------------------------------------------
// Logs which GTE commands ACTUALLY execute (static call-site counts in generated/ aren't run-weighted)
// and snapshots the lighting/fog control registers, to pin down Tomba2's shading model. Finding so far:
// NO NC*/CC/CDP ops => no GTE dynamic per-vertex lighting; colors are baked + DPCS/DPCT depth-cue FOG.
static const char *gte_name(unsigned fn) {
  switch (fn) {
  case 0x01:
    return "RTPS";
  case 0x06:
    return "NCLIP";
  case 0x0C:
    return "OP";
  case 0x10:
    return "DPCS";
  case 0x11:
    return "INTPL";
  case 0x12:
    return "MVMVA";
  case 0x13:
    return "NCDS";
  case 0x14:
    return "CDP";
  case 0x16:
    return "NCDT";
  case 0x1B:
    return "NCCS";
  case 0x1C:
    return "CC";
  case 0x1E:
    return "NCS";
  case 0x20:
    return "NCT";
  case 0x28:
    return "SQR";
  case 0x29:
    return "DCPL";
  case 0x2A:
    return "DPCT";
  case 0x2D:
    return "AVSZ3";
  case 0x2E:
    return "AVSZ4";
  case 0x30:
    return "RTPT";
  case 0x3D:
    return "GPF";
  case 0x3E:
    return "GPL";
  case 0x3F:
    return "NCCT";
  default:
    return "?";
  }
}
void gte_probe_dump(const char *tag) {
  GteDebug &d = GTE_CurState()->dbg;
  if (d.gteprobe <= 0) {
    return;
  }
  lucent::info("gteprobe", "{} executed GTE ops:", tag ? tag : "(null)");
  for (unsigned fn = 0; fn < 64; fn++) {
    if (d.gte_hist[fn]) {
      lucent::info("gteprobe", "    {:<6}(0x{:02X}) = {}", gte_name(fn), fn, d.gte_hist[fn]);
    }
  }
  // lighting/fog control-register snapshot (cop2 CR numbering)
  lucent::info("gteprobe",
               "  LightMatrix(LLM cr8-12): {:08X} {:08X} {:08X} {:08X} {:08X}",
               GTE_ReadCR(8),
               GTE_ReadCR(9),
               GTE_ReadCR(10),
               GTE_ReadCR(11),
               GTE_ReadCR(12));
  lucent::info("gteprobe",
               "  BackColor(RBK/GBK/BBK cr13-15): {} {} {}",
               (int32_t)GTE_ReadCR(13),
               (int32_t)GTE_ReadCR(14),
               (int32_t)GTE_ReadCR(15));
  lucent::info("gteprobe",
               "  LightColorMtx(LCM cr16-20): {:08X} {:08X} {:08X} {:08X} {:08X}",
               GTE_ReadCR(16),
               GTE_ReadCR(17),
               GTE_ReadCR(18),
               GTE_ReadCR(19),
               GTE_ReadCR(20));
  lucent::info("gteprobe",
               "  FarColor(RFC/GFC/BFC cr21-23): {} {} {}   [fog target]",
               (int32_t)GTE_ReadCR(21),
               (int32_t)GTE_ReadCR(22),
               (int32_t)GTE_ReadCR(23));
  lucent::info("gteprobe",
               "  DepthCue DQA(cr27)={} DQB(cr28)={}   [IR0 = DQB + DQA*h/sz -> fog factor]",
               (int16_t)GTE_ReadCR(27),
               (int32_t)GTE_ReadCR(28));
}
// --- Native projection (Phase 1): reimplement RTPS/RTPT in native C, oracle-gate it ----------------
// The engine projects every vertex through the GTE (RTPS/RTPT) and then copies the integer screen
// coords into GP0 packets. To OWN the projection (plan atomic-riding-sparkle, Phase 1) we recompute it
// here in native C from the loaded matrix + input vertex, producing both the integer outputs (to prove
// 0-diff vs Beetle's GTE — i.e. our math IS the engine's projection, so gameplay stays untouched) and
// the FLOAT view-space pos / screen / depth that the renderer needs and that the GP0 packet drops.
// This replaces the value-keyed PGXP-lite cache as the source of subpixel/depth (which gets attached at
// submission in a later step); here it is a read-only verifier. The integer half exactly mirrors
// mednafen/psx/gte.c (MultiplyMatrixByVector_PT + Divide/UNR + TransformXY), so a 0-diff result proves
// the reimplementation is faithful. Gated on PSXPORT_PROJPROBE (read-only, no effect on output).
#include "native_projection.h"
#include "native_projection_internal.h"

// ProjVtx typedef comes from projection.h (included via render.h above); dropped the local decl.

// per-frame projection constants moved to `class ProjParams` on Render (per-Core, SBS-safe) —
// reach via `c->rsub.projParams.projH()` etc. Callers below without a Core* in scope go through
// `ProjParams::current()` — the currently-bound instance, set by `bind()` at native_step_frame.

// proj_pz_to_ord + the camview + projection-plane accessors moved to game/render/proj_params.cpp — see
// the free-function bridges at the bottom of that file. They forward to ProjParams::current() (bound
// per-frame by gte_bind), so callers with no Core* in scope keep working.
// Project an EXPLICIT model vertex V (int16 x/y/z) through the GTE's composed camera×object transform
// (rotation CR0-4 + translation CR5-7 + screen offset/H CR24-26) to float screen coords + view-Z, with
// NO gte_op — the full RTPT math in C. `insn` carries the sf/lm flags an RTPT would use (RTPT = 0x280030,
// sf=1 lm=0). This is what a PC engine does: read the transform the engine built, transform vertices in
// float. proj_native_vertex (below) is the same math reading V from the GTE DR regs (for PSXPORT_PROJPROBE).
void proj_native_xform(int vx, int vy, int vz, ProjVtx *out) {
  using namespace psxport::native_projection;
  const uint32_t c0 = gte_read_ctrl(0), c1 = gte_read_ctrl(1), c2 = gte_read_ctrl(2), c3 = gte_read_ctrl(3),
                 c4 = gte_read_ctrl(4);
  const int32_t OFX = (int32_t)gte_read_ctrl(24), OFY = (int32_t)gte_read_ctrl(25);
  const uint16_t H = (uint16_t)gte_read_ctrl(26);
  FixedAffine affine{};
  affine.m = {{{(int16_t)c0, (int16_t)(c0 >> 16), (int16_t)c1},
               {(int16_t)(c1 >> 16), (int16_t)c2, (int16_t)(c2 >> 16)},
               {(int16_t)c3, (int16_t)(c3 >> 16), (int16_t)c4}}};
  affine.t = {{(int32_t)gte_read_ctrl(5), (int32_t)gte_read_ctrl(6), (int32_t)gte_read_ctrl(7)}};
  const NativeProjectedVertex p = project(affine, {OFX, OFY, H}, {(int16_t)vx, (int16_t)vy, (int16_t)vz});
  out->ir1 = p.ir[0];
  out->ir2 = p.ir[1];
  out->ir3 = p.ir[2];
  out->sz = p.sz;
  out->sx = p.sx;
  out->sy = p.sy;
  // Preserve the historical adapter contract: ProjVtx exposes saturated IR
  // coordinates. Producers that need the pre-saturation endpoint use the pure
  // native_projection result directly.
  out->vx = (float)p.ir[0];
  out->vy = (float)p.ir[1];
  out->vz = (float)p.ir[2];
  out->px = p.px;
  out->py = p.py;
  out->pz = p.pz;
  if (auto *pp = ProjParams::current()) {
    pp->setProjH(H);
  }
  float fofx = (float)OFX / 65536.0f, fofy = (float)OFY / 65536.0f;
  if (auto *pp = ProjParams::current()) {
    pp->setProjCenter(fofx, fofy);
  }
}

// Object-CENTER depth from the LIVE composed camera×object transform in the GTE control regs (CR0-7).
// At the per-object render-command dispatch the engine has composed camera×model into the GTE; the object
// origin's view-Z is proj_native_xform(0,0,0).pz — our own float pipeline, in the SAME band as per-vertex
// world depth. This is the object's WORLD POSITION projected to view depth (CR5-7 = world pos in view
// space). 2D billboard prims the object then emits occlude by this real depth instead of sprite order.
float proj_obj_center_ord(void) {
  ProjVtx p;
  proj_native_xform(0, 0, 0, &p);
  return proj_pz_to_ord(p.pz);
}

// MAIN scene camera view matrix, published once per frame by native_terrain at terrain-draw time (when the
// scratchpad holds the real scene camera, before the per-object compose overwrites it). Used to project an
// object's WORLD POSITION to a STABLE view-Z (deterministic, render-order-independent), so a 2D billboard
// occludes by where the object really is — consistent with the terrain it stands on. PC-owned, not PSX OT.
// camview + world→screen projection moved to `class ProjParams` on Render (per-Core). The thin
// free-function bridges (camview_publish/valid, proj_camview_world_ord/screen) live in proj_params.cpp;
// this side keeps only the one that has to consult the GTE control regs (which are per-instance too but
// reached via gte_read_ctrl below, not through ProjParams).

// Recompute one vertex's projection from a snapshot of the GTE control/data regs (read post-instruction;
// neither matrix CR0-7 nor the input V regs DR0-5 are touched by RTP, so reading after is exact).
static void proj_native_vertex(unsigned vidx, uint32_t insn, ProjVtx *out) {
  using namespace psxport::native_projection;
  const uint32_t c0 = gte_read_ctrl(0), c1 = gte_read_ctrl(1), c2 = gte_read_ctrl(2), c3 = gte_read_ctrl(3),
                 c4 = gte_read_ctrl(4);
  const uint32_t dlo = gte_read_data(2 * vidx), dhi = gte_read_data(2 * vidx + 1);
  const int32_t OFX = (int32_t)gte_read_ctrl(24), OFY = (int32_t)gte_read_ctrl(25);
  const uint16_t H = (uint16_t)gte_read_ctrl(26);
  FixedAffine affine{};
  affine.m = {{{(int16_t)c0, (int16_t)(c0 >> 16), (int16_t)c1},
               {(int16_t)(c1 >> 16), (int16_t)c2, (int16_t)(c2 >> 16)},
               {(int16_t)c3, (int16_t)(c3 >> 16), (int16_t)c4}}};
  affine.t = {{(int32_t)gte_read_ctrl(5), (int32_t)gte_read_ctrl(6), (int32_t)gte_read_ctrl(7)}};
  const NativeProjectedVertex p = detail::project_gte_mode(affine,
                                                           {OFX, OFY, H},
                                                           {(int16_t)dlo, (int16_t)(dlo >> 16), (int16_t)dhi},
                                                           (insn & (1u << 19)) ? 12u : 0u,
                                                           ((insn >> 10) & 1u) != 0);
  out->ir1 = p.ir[0];
  out->ir2 = p.ir[1];
  out->ir3 = p.ir[2];
  out->sz = p.sz;
  out->sx = p.sx;
  out->sy = p.sy;
  out->vx = (float)p.ir[0];
  out->vy = (float)p.ir[1];
  out->vz = (float)p.ir[2];
  out->px = p.px;
  out->py = p.py;
  out->pz = p.pz;
  if (auto *pp = ProjParams::current()) {
    pp->setProjH(H); // remember the projection plane for depth-normalize
  }
  float fofx = (float)OFX / 65536.0f, fofy = (float)OFY / 65536.0f;
  if (auto *pp = ProjParams::current()) {
    pp->setProjCenter(fofx, fofy);
  }
}

// Verification accumulators (PSXPORT_PROJPROBE) live on GteRegs::dbg (bound core's instance).
static void
proj_probe_one(unsigned vidx, uint32_t insn, int b_ir1, int b_ir2, int b_ir3, int b_sz, int b_sx, int b_sy) {
  GteDebug &g = GTE_CurState()->dbg;
  ProjVtx p;
  proj_native_vertex(vidx, insn, &p);
  g.pp_verts++;
  int dir = 0;
  if (p.ir1 != b_ir1) {
    dir = abs(p.ir1 - b_ir1) > dir ? abs(p.ir1 - b_ir1) : dir;
  }
  if (p.ir2 != b_ir2) {
    int d = abs(p.ir2 - b_ir2);
    if (d > dir) {
      dir = d;
    }
  }
  if (p.ir3 != b_ir3) {
    int d = abs(p.ir3 - b_ir3);
    if (d > dir) {
      dir = d;
    }
  }
  if (dir) {
    g.pp_bad_ir++;
    if (dir > g.pp_maxd_ir) {
      g.pp_maxd_ir = dir;
    }
  }
  int dsz = abs(p.sz - b_sz);
  if (dsz) {
    g.pp_bad_sz++;
    if (dsz > g.pp_maxd_sz) {
      g.pp_maxd_sz = dsz;
    }
  }
  int dsx = abs(p.sx - b_sx);
  if (dsx) {
    g.pp_bad_sx++;
    if (dsx > g.pp_maxd_sx) {
      g.pp_maxd_sx = dsx;
    }
  }
  int dsy = abs(p.sy - b_sy);
  if (dsy) {
    g.pp_bad_sy++;
    if (dsy > g.pp_maxd_sy) {
      g.pp_maxd_sy = dsy;
    }
  }
  // sanity: our subpixel float, rounded, should land within 1px of the integer projection
  int rx = (int)(p.px < 0 ? p.px - 0.5f : p.px + 0.5f), ry = (int)(p.py < 0 ? p.py - 0.5f : p.py + 0.5f);
  if (abs(rx - b_sx) <= 1 && abs(ry - b_sy) <= 1) {
    g.pp_subpix_le1++;
  }
}
void proj_probe_dump(const char *tag) {
  GteDebug &g = GTE_CurState()->dbg;
  if (g.projprobe <= 0 || g.pp_verts == 0) {
    return;
  }
  lucent::info(
      "projprobe",
      "{} verts={}  IR diff={}(max{}) SZ diff={}(max{}) SX diff={}(max{}) SY diff={}(max{})  subpix<=1px={}({:.2f}%)",
      tag ? tag : "(null)",
      g.pp_verts,
      g.pp_bad_ir,
      g.pp_maxd_ir,
      g.pp_bad_sz,
      g.pp_maxd_sz,
      g.pp_bad_sx,
      g.pp_maxd_sx,
      g.pp_bad_sy,
      g.pp_maxd_sy,
      g.pp_subpix_le1,
      100.0 * g.pp_subpix_le1 / g.pp_verts);
  g.pp_verts = g.pp_bad_ir = g.pp_bad_sz = g.pp_bad_sx = g.pp_bad_sy = g.pp_subpix_le1 = 0;
  g.pp_maxd_ir = g.pp_maxd_sz = g.pp_maxd_sx = g.pp_maxd_sy = 0;
}

// PSXPORT_RTPCALLER: histogram the return address (RA=r[31]) at each RTPS/RTPT — pins the submit/handler
// call sites that build POLY packets from the projection, the targets for attach-at-submission.
static void rtpcaller_record(uint32_t ra) {
  GteDebug &d = GTE_CurState()->dbg;
  if (d.rtpcaller_on < 0) {
    d.rtpcaller_on = lucent::channel_on("rtpcaller") ? 1 : 0;
  }
  if (!d.rtpcaller_on) {
    return;
  }
  for (int i = 0; i < 64; i++) {
    if (d.rtpcaller[i].n == 0) {
      d.rtpcaller[i].ra = ra;
      d.rtpcaller[i].n = 1;
      return;
    }
    if (d.rtpcaller[i].ra == ra) {
      d.rtpcaller[i].n++;
      return;
    }
  }
}
void rtpcaller_dump(Core *c, const char *tag) {
  GteDebug &d = c->game->gte.dbg;
  if (d.rtpcaller_on != 1) {
    return;
  }
  lucent::info(
      "rtpcaller", "{} RA histogram (caller return site -> jal target = projection fn):", tag ? tag : "(null)");
  for (int i = 0; i < 64 && d.rtpcaller[i].n; i++) {
    // the jal that set RA is at RA-8 (RA = jal_addr + 8, MIPS branch-delay). Decode its target.
    uint32_t jal = c->mem_r32(d.rtpcaller[i].ra - 8);
    uint32_t tgt = ((jal >> 26) == 3) ? (((d.rtpcaller[i].ra - 8) & 0xF0000000u) | ((jal & 0x03FFFFFFu) << 2))
                                      : 0; // 0 = not a jal (jalr / inlined)
    lucent::info(
        "rtpcaller", "    RA=0x{:08X}  {:8}   jal[{:08X}]->fn=0x{:08X}", d.rtpcaller[i].ra, d.rtpcaller[i].n, jal, tgt);
  }
}
void rtpcaller_reset(void) {
  GteDebug &d = GTE_CurState()->dbg;
  for (int i = 0; i < 64; i++) {
    d.rtpcaller[i].ra = 0;
    d.rtpcaller[i].n = 0;
  }
}

// Native per-vertex depth cache: now `class ProjPrim` on Render — reach as `c->rsub.projprim`.
// See game/render/proj_prim.h. All previously free `projprim_*` functions retired 2026-07-03.

// PC-native per-pixel depth is THE render behavior — this is a PC GAME, not an emulator. The OT-order
// painter's algorithm is the PSX limitation we transcend; genuine widescreen needs real per-pixel
// occlusion (the widened FOV breaks painter order). One behavior, no toggle.
int native_depth_on(void) {
  return 1;
}
// The native-depth path gates the engine's depth recording + the per-frame reset. Always active.
int attach_enabled(void) {
  return 1;
}
// (Projection-plane setters/getters moved to game/render/proj_params.cpp — see the free-function
// bridges at the bottom of that file. They forward to ProjParams::current(); no state here anymore.)

// The GTE coprocessor is RETAINED for now ONLY as the PSX-content math service (collision/physics still
// run as recompiled PSX and call gte_op). The GTE/Beetle dependency goes away by PORTING ITS CALLERS to
// PC-native math — NOT by building a general native GTE.
//   FORBIDDEN (later-171 dead-end): a `gte_op_native` that ALL gte_op calls route through, byte-identical
//   to gte.c. That just swaps one PSX-hardware emulator for our own — it advances nothing and is the PSX
//   mimicry CLAUDE.md forbids. (Reverted the native NCLIP/AVSZ replica then.)
//   CORRECT (and being done — later-186): port individual gte_op CALLERS to plain C so they no longer
//   invoke the GTE at all (e.g. gte_math.cpp ov_mat_mul = FUN_80084110's 3x3 matmul). For a RENDER
//   caller the engine already owns it PC-native (proj_native_vertex — float matrices, real depth; no
//   gte_op for render) → bypass, don't replicate. For a caller whose result feeds retained PSX CONTENT
//   (e.g. object position), the C math MUST produce the game's fixed-point values so the content reads
//   correct data — that is the content-interface CORRECTNESS gate, not GTE-hardware emulation, and it is
//   fine. The fixed-point interface itself disappears once that content consumer is ported too.
// Every gte_op caller ported this way removes work from GTE_Instruction; it vanishes when none remain.
static void gte_op_impl(Core *c, uint32_t insn, uint32_t guest_pc) {
  c->rsub.gtePreOp.observeAround(c, guest_pc, insn, [insn] {
    GTE_Instruction(insn);
  });
  unsigned op = insn & 0x3F;
  GteDebug &gd = c->game->gte.dbg;
  if (gd.gteprobe < 0) {
    const char *e = cfg_str("PSXPORT_GTEPROBE");
    gd.gteprobe = e ? atoi(e) : 0;
  }
  if (gd.gteprobe > 0) {
    gd.gte_hist[op]++;
  }
  if (op == 0x01 || op == 0x30) {
    // PROJECTION CONSTANTS, captured generically. pzToOrd needs the
    // projection plane H (CR26) to place the near plane; without it every
    // depth normalizes against a default and the whole scene flattens.
    // The reference consumer sets these from its own native transform, but
    // a game that has no native transform yet still projects through the
    // GTE — so read them from the control regs that RTPS/RTPT just used.
    // OFX/OFY (CR24/25) are 16.16 fixed point.
    c->rsub.projParams.setProjH((uint16_t)gte_read_ctrl(26));
    c->rsub.projParams.setProjCenter((float)(int32_t)gte_read_ctrl(24) / 65536.0f,
                                     (float)(int32_t)gte_read_ctrl(25) / 65536.0f);
    ws_sx_record();             // self-gated (PSXPORT_WS_SXHIST)
    rtpcaller_record(c->r[31]); // self-gated (PSXPORT_RTPCALLER)
    c->rsub.otAttr.trackGte(c); // self-gated (`debug otattr`)
    // fps60 (docs/fps60-rework.md) does not tap the GTE op
    // stream for interpolation — the interp present re-runs
    // the native render under lerped input chokes (camera /
    // object transform / backdrop scroll).
    if (gd.projprobe < 0) {
      gd.projprobe = cfg_on("PSXPORT_PROJPROBE") ? 1 : 0;
    }
    if (gd.projprobe > 0) {
      // Compare native projection to Beetle's outputs. After RTPS the single vertex
      // is in XY_FIFO(3)=DR15 / Z_FIFO(3)=DR19 / IR=DR9-11. After RTPT the 3 verts
      // land in XY DR12,DR13,DR14 (DR15==DR14, see push quirk below) and Z DR17-19;
      // IR holds only the last vertex.
      if (op == 0x01) {
        uint32_t xy = gte_read_data(15), z = gte_read_data(19);
        proj_probe_one(0,
                       insn,
                       (int16_t)gte_read_data(9),
                       (int16_t)gte_read_data(10),
                       (int16_t)gte_read_data(11),
                       (uint16_t)z,
                       (int16_t)xy,
                       (int16_t)(xy >> 16));
      } else {
        // XY_FIFO push duplicates the new value into slots 2 & 3 (gte.c TransformXY:
        // XY_FIFO(2)=XY_FIFO(3) runs after (3)=new), so after 3 pushes v0,v1,v2 land in
        // DR12,DR13,DR14 (DR15==DR14). Z_FIFO shifts cleanly: v0,v1,v2 -> DR17,DR18,DR19.
        for (unsigned i = 0; i < 3; i++) {
          uint32_t xy = gte_read_data(12 + i), z = gte_read_data(17 + i);
          // IR1-3 only valid for the last vertex; pass native's own for the others so
          // the IR check is meaningful only on vertex 2 (others compare native vs native).
          int bir1, bir2, bir3;
          if (i == 2) {
            bir1 = (int16_t)gte_read_data(9);
            bir2 = (int16_t)gte_read_data(10);
            bir3 = (int16_t)gte_read_data(11);
          } else {
            ProjVtx t;
            proj_native_vertex(i, insn, &t);
            bir1 = t.ir1;
            bir2 = t.ir2;
            bir3 = t.ir3;
          }
          proj_probe_one(i, insn, bir1, bir2, bir3, (uint16_t)z, (int16_t)xy, (int16_t)(xy >> 16));
        }
      }
    }
  }
}
void gte_op(Core *c, uint32_t insn) {
  gte_op_impl(c, insn, c ? c->pc : 0);
}
void gte_op_at(Core *c, uint32_t insn, uint32_t guest_pc) {
  gte_op_impl(c, insn, guest_pc);
}

void gte_preop_observer_arm(Core *c, GtePreOpFn fn, void *user) {
  if (!c) {
    return;
  }
  c->rsub.gtePreOp.arm(fn, user);
}
void gte_op_observer_arm(Core *c, GtePreOpFn preFn, GtePostOpFn postFn, void *user) {
  if (!c) {
    return;
  }
  c->rsub.gtePreOp.arm(preFn, postFn, user);
}
uint64_t gte_preop_observer_disarm(Core *c) {
  return c ? c->rsub.gtePreOp.disarm() : 0;
}
uint64_t gte_preop_observer_seen(const Core *c) {
  return c ? c->rsub.gtePreOp.seen() : 0;
}
// swc2 of a PROJECTED SCREEN-XY register (DR12/13/14, or DR15 for RTPS's single-vertex slot). The
// recompiler routes only those registers here (emit.py GTE_SCREEN_XY_REGS); every other cop2 store
// keeps the plain inline form, so this costs nothing on them.
//
// WHY THIS IS THE NATIVE-DEPTH SEAM. The renderer needs each drawn vertex's view-space Z, and it can
// only ask for it by the ADDRESS the guest wrote the projected X/Y to (gp0_exec -> projprim.lookupPz).
// `gte_stsxy*` compiles to exactly this instruction, so the address and the depth are both in hand
// here — for ANY game, with no reverse engineering of its submit path. That is the whole point of the
// GTE choke point the porting guide describes.
//
// THE PAIRING IS FIXED BY THE FIFOs, not guessed: an RTPT pushes v0,v1,v2 into XY DR12,DR13,DR14 and
// Z into DR17,DR18,DR19; an RTPS writes the single vertex to DR15 and its Z to DR19.
//
// The Z is the GTE's SZ, which is view-space Z in the same units as the projection plane H — exactly
// what ProjParams::pzToOrd expects (it clamps at H/2 and normalizes 1/z). Reading it UNSIGNED matters:
// SZ is a 16-bit unsigned FIFO value and sign-extending it would fold the far half of the scene in
// front of the camera.
void gte_store_xy(Core *c, uint32_t addr, int rt) {
  c->mem_w32(addr, gte_read_data((uint32_t)rt));
  if (!attach_enabled()) {
    return;
  }
  const int zreg = (rt == 15) ? 19 : (17 + (rt - 12));
  c->rsub.projprim.setPz(c, addr, (float)(uint16_t)gte_read_data((uint32_t)zreg));
}

// Native depth, mfc2 form. The recompiler pairs `mfc2 rX, DR12..15` with the `sw rX, off(base)` that
// writes it into the primitive packet (emit.py vertex_pz_stores) and emits this call right after the
// store. `addr` is what was written; `zreg` is the Z_FIFO register the analysis proved pairs with it.
//
// The pairing is only emitted when nothing between the mfc2 and the store could invalidate it — no
// redefinition of the GPR, no COP2 op moving the Z FIFO on, no block boundary — so reading the Z here
// is reading the same vertex's depth, not a later one.
//
// Same unsigned read as gte_store_xy: SZ is a 16-bit unsigned FIFO value.
// HELD DEPTHS, one slot per GPR. gte_hold_pz snapshots the Z that belongs to the vertex just read out
// of the GTE; gte_record_pz consumes it when that GPR is stored into the packet.
//
// WHY HOLD AT ALL, instead of reading Z at the store: PSX submit loops are SOFTWARE PIPELINED. They
// read vertex N's screen XY, issue the transform for vertex N+1, then store N — so at the store the Z
// FIFO already holds N+1's depth. Reading it there attaches the NEXT vertex's distance to this one,
// which is a wrong depth rather than a missing one. The mfc2 is the last moment the value is still
// this vertex's. In Spyro this shape is 29 of 70 vertex sites.
//
// Keyed by GPR because the recompiler proved the pairing: between the hold and the consume the GPR is
// not redefined and control does not branch, so the slot cannot be claimed by another vertex in
// between. No staleness handling is needed — an unconsumed slot is simply overwritten by the next
// hold on that register.
static float s_held_pz[32];

void gte_hold_pz(Core *c, int gpr, int zreg) {
  (void)c;
  s_held_pz[gpr & 31] = (float)(uint16_t)gte_read_data((uint32_t)zreg);
}

// A word the guest COPIED between buffers. If the source carries a projected vertex's depth, the
// destination inherits it; if it does not, nothing happens.
//
// THAT ASYMMETRY IS THE WHOLE SAFETY PROPERTY. This runs on every `lw`->`sw` pair the recompiler
// found, which is most word copies in the game — the overwhelming majority have nothing to do with
// geometry. Fabricating a depth for those would give 2D elements a world Z and sort them into the 3D
// scene, which is a visibly wrong picture; leaving them alone costs nothing, because absent depth
// falls back to draw order, exactly as today.
//
// Uses peekPz, not lookupPz: these probes must not move the render's hit/miss counters, which are the
// numbers that say whether native depth is working.
// Where a loaded word came FROM, one slot per seed GPR, captured at the load.
//
// IT MUST BE CAPTURED AT THE LOAD, not rebuilt at the store. Spyro's terrain renderer indexes its
// scratchpad vertex cache with `add t6,t6,s4` then `lw t6,0(t6)` — the load CLOBBERS ITS OWN BASE, so
// evaluating that address expression at the store site reads the loaded VALUE as a pointer and would
// carry some unrelated word's depth. A wrong depth is worse than none.
static uint32_t s_held_src[32];

void gte_hold_src(Core *c, int gpr, uint32_t src) {
  (void)c;
  s_held_src[gpr & 31] = src;
}

// COUNT THE BRIDGE. Records and lookups were both counted; the copy BETWEEN them was not, and that
// is the step that decides whether a staged vertex's depth ever reaches the packet the renderer
// draws from. Without these, "15M records, 6% of lookups hit" has two indistinguishable readings —
// the copies are not running, or they are running and landing on addresses nobody reads — and no
// amount of staring at either end separates them.
static long long s_copy_try = 0, s_copy_carried = 0;
void gte_copy_pz_counts(long long *tried, long long *carried) {
  if (tried) {
    *tried = s_copy_try;
  }
  if (carried) {
    *carried = s_copy_carried;
  }
}

void gte_copy_pz(Core *c, int gpr, uint32_t dst) {
  if (!attach_enabled()) {
    return;
  }
  float pz;
  s_copy_try++;
  if (c->rsub.projprim.peekPz(c, s_held_src[gpr & 31], &pz)) {
    s_copy_carried++;
    c->rsub.projprim.setPz(c, dst, pz);
  }
}

// Carry a hold from one GPR to another, for a value the guest DERIVED rather than copied verbatim.
//
// Both renderers here pack a projected vertex as `(screenXY << 5) | clipcode` into a cache and unshift
// it when they assemble the packet, so the word that reaches the primitive is several ALU ops away
// from the one that was loaded. The provenance is unchanged by that arithmetic — the value still
// derives from that vertex, or from that source address — so the hold moves with it. Copies the SLOT,
// not the register file: re-deriving a held Z later would read whatever SZ has become by then.
void gte_hold_move(int dst, int src) {
  s_held_pz[dst & 31] = s_held_pz[src & 31];
  s_held_src[dst & 31] = s_held_src[src & 31];
}

void gte_record_pz(Core *c, uint32_t addr, int gpr) {
  if (!attach_enabled()) {
    return;
  }
  c->rsub.projprim.setPz(c, addr, s_held_pz[gpr & 31]);
}

// Bind the GTE math to THIS core's register file (game.h GteRegs) so two cores keep separate GTE state.
// Called per core frame-step (native_step_frame) + at boot, from the explicit Core — no shared regs.
void gte_bind(Core *c) {
  GTE_BindState(&c->game->gte);
  // Also bind THIS core's per-Core PGXP cache + projection-constant/camview state (both were file-scope
  // process-wide before deglobalize-2026-07-03 → SBS's two cores would clobber each other's per-frame
  // subpixel cache and projection center).
  c->rsub.pgxp.bind(c);
  c->rsub.projParams.bind(c);
}
void gte_init(void) {
  GTE_Init();
  GTE_Power();
  // PSXPORT_WIDE is PC-native widescreen now: the GTE keeps its NATIVE projection (NO squish) and the
  // renderer (gpu_vk) re-centers the geometry into a wider scratch framebuffer at a true wider FOV.
  // The old emulator squish-X + display-stretch hack (widescreen_hack=1) is intentionally NOT used —
  // it loses horizontal resolution and stretches the 2D HUD (user rejected it). Keep the hack OFF. }
  widescreen_hack = 0;
}
