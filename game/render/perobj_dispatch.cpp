// perobj_dispatch.cpp — SUBSTRATE MIRROR for the per-object cmd-list dispatch chain
// FUN_8003CDD8 (Render::cmdListDispatch) -> FUN_8003F698 (Render::perModeDispatch).
//
// Ownership: address band 0x8003xxxx (this agent's exclusive band per the frontier task). Both
// addresses confirmed unowned via tools/codemap.py before porting.
//
// WHY these two and not the whole 0x8003CCA4 family: 0x8003CCA4 (submit_perobj_render) already has a
// TRANSPARENT observer wrapper installed (render_observer.cpp, g_override slot 845) that runs the
// LITERAL gen body then tags host-side depth (obj_world_ord/gpu_obj_depth_add) for the
// billboard-occlusion fix (issue #4 class). Replacing CCA4 wholesale would have to reproduce its 5
// special-effect sub-cases (FUN_8003F4C4/F3F4/D584/F594/F344, none of which fire at seaside) AND fold
// in the observer's host bookkeeping to avoid regressing that landed fix — out of scope for a focused
// 2-address cluster. CDD8/F698 are NOT currently wrapped by anything (RenderObserver only wraps CCA4,
// C2D4, C464, C5F8, C788, 80039F4C), so owning them here is a clean, additive, non-conflicting slice
// of the SAME dispatch chain: CCA4's cases 0/4 (the only ones seaside objects hit) call `FUN_8003cdd8`
// unconditionally as their entire body.
//
// CALL-SITE MECHANISM: unlike the walk-cluster addresses the override registry's rec_dispatch
// interception targets (rec_dispatch-only, nullptr setter), CDD8 and F698 are reached from CCA4's
// generated body as PLAIN INTRA-SHARD C CALLS (`func_8003CDD8(c)` / `func_8003F698(c)` — see
// generated/shard_5.c gen_func_8003CCA4 and generated/shard_6.c gen_func_8003CDD8).
// overrides::dispatch only intercepts inside rec_dispatch, so it cannot see these calls; the ONLY
// interception point is the g_override[] slot each func_XXXX wrapper
// in shard_disp.c already checks (`if (g_override[N]) { g_override[N](c); return; }`) — the exact
// mechanism render_observer.cpp uses. shard_set_override() is that installer.
//
// RE (Ghidra headless decompile of a live free-roam RAM dump, scratch/bin/field_ram.bin, cross-checked
// against the ACTUAL recompiled body in generated/shard_6.c + generated/shard_4.c — the recompiler's
// gte_write_ctrl/gte_write_data/gte_op/gte_read_data calls are ground truth, not the raw Ghidra pseudo-C
// which mislabels COP2 moves as fictitious setCopReg/copFunction helpers). Independently corroborated by
// game/render/submit.cpp's pre-existing "later-133" comment block (a RETIRED, issue-#32-superseded
// native lift of this exact pair) — same scratch addresses, same MVMVA opcodes; used here as a second
// source cross-check for the GTE compose shape.
//
// FUN_8003CDD8 (a0=node r4, a1=flag r5): for each active cmd on node's persistent render-command list
// (count @node+8, capacity @node+9, cmd ptr array @node+0xC0+4i):
//   - geomblk = cmd+0x40; skip (continue) if 0.
//   - stash the object's WORLD POSITION triple (cmd+0x2C/0x30/0x34, 3 s16) into scratch WORLD_POS
//     (0x1F8000C0/C2/C4).
//   - load the CAMERA rotation (scratch CAM_ROT 0x1F8000F8, 5-word CR-packed 3x3) into GTE CR0-4.
//   - for each of 3 columns of the object's LOCAL rotation (cmd+0x18, row-stride 6 col-stride 2):
//     write IR1-3, run MVMVA_ROTCOL (mx=ROT using CR0-4=camera, v=IR, cv=none, sf=1), store the
//     resulting IR1-3 into the composed matrix OBJ_ROT (scratch 0x1F800000, CR-packed).
//   - transform the stashed WORLD_POS by the camera (CR0-4 still camera; MVMVA_TRANS, v=V0, cv=none),
//     then ADD the camera translation offset (scratch CAM_TRANS 0x1F80010C/110/114) -> the composed
//     WORLD translation, stored at OBJ_ROT+20/24/28.
//   - reload CR0-4 from OBJ_ROT (the composed WORLD rotation) and CR5-7 from OBJ_ROT+20/24/28 (the
//     composed WORLD translation) — this is the transform the per-mode renderer submits vertices
//     through.
//   - OT base = *0x800ED8C8, or that value + cmd[0x3F](s8)*4 when node[0xD]&0xF == 4 (identical rule
//     to Render::perObjFlush's otbase select).
//   - dispatch (a0=geomblk, a1=otbase, a2=flag) to FUN_8003F698.
//
// FUN_8003F698 (a0=geomblk r4, a1=otbase r5, a2=flag r6): if the generic-force scratch flag
// (MODE_FORCE 0x1F800234) is clear AND (a2&1)==0, index the 22-entry mode table (MODE_TABLE
// 0x80015268) by the area's render-mode byte (MODE_BYTE 0x800BF870, bound-checked <22) and
// rec_dispatch the resolved handler address (owned per-area leaves like 0x80146478 live outside this
// band — untouched, reached transparently via rec_dispatch exactly as the recomp body reaches them).
// Otherwise (or mode>=22) fall back to func_800803DC (the substrate's generic GT3/GT4 packet emitter).
#include "core.h"
#include "game_ctx.h"
#include "game.h"
#include "render.h"
#include "cfg.h"
#include "guest_abi.h"   // GuestFrame/guest_dispatch — perModeDispatch's demo migration (docs/port-framework.md)
#include "render_internal.h"   // render_field_native_active / gpu_native_cover_add (REDIRECT below)
#include "pkt_span.h"           // PktSpanSession (REDIRECT's own inner span capture)

void rec_dispatch(Core*, uint32_t);          // overlay_router.cpp — the shared choke point for owned/substrate leaves
void func_800803DC(Core*);                    // generated/shard_disp.c — generic GT3/GT4 packet emitter (still substrate)
void shard_set_override(uint32_t addr, OverrideFn fn);   // generated/shard_disp.c (C++ linkage)

namespace {
// Guest-stack frame RAII, mirroring gen_func_8003CDD8's real `addiu sp,-56` prologue (spills
// r16..r23/ra at +16..+48) and gen_func_8003F698's real `addiu sp,-24` prologue (spills ra only at
// +16) — see CLAUDE.md "MIRROR THE GUEST STACK". Neither function's own C++ body needs r16..r23 as
// meaningful cross-call state (register-faithfulness concern only applies to nested TAIL-CALLS that
// themselves spill a caller's live callee-saved regs — cmdListDispatch/perModeDispatch never set
// r16..r23 to a value a callee depends on), so this is the simple spill-live/restore-live idiom
// (same as game/render/cull.cpp's wrapFrame / perobj_billboard.cpp's GuestFrame), NOT the
// value-injecting variant node_xform.cpp needed.
struct CmdListFrame {
  Core* c; uint32_t s16,s17,s18,s19,s20,s21,s22,s23,sra;
  explicit CmdListFrame(Core* c_)
    : c(c_), s16(c_->r[16]), s17(c_->r[17]), s18(c_->r[18]), s19(c_->r[19]),
      s20(c_->r[20]), s21(c_->r[21]), s22(c_->r[22]), s23(c_->r[23]), sra(c_->r[31]) {
    c->r[29] -= 56;
    c->mem_w32(c->r[29] + 16, s16); c->mem_w32(c->r[29] + 20, s17);
    c->mem_w32(c->r[29] + 24, s18); c->mem_w32(c->r[29] + 28, s19);
    c->mem_w32(c->r[29] + 32, s20); c->mem_w32(c->r[29] + 36, s21);
    c->mem_w32(c->r[29] + 40, s22); c->mem_w32(c->r[29] + 44, s23);
    c->mem_w32(c->r[29] + 48, sra);
  }
  ~CmdListFrame() {
    c->r[31] = c->mem_r32(c->r[29] + 48); c->r[23] = c->mem_r32(c->r[29] + 44);
    c->r[22] = c->mem_r32(c->r[29] + 40); c->r[21] = c->mem_r32(c->r[29] + 36);
    c->r[20] = c->mem_r32(c->r[29] + 32); c->r[19] = c->mem_r32(c->r[29] + 28);
    c->r[18] = c->mem_r32(c->r[29] + 24); c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] += 56;
  }
};
}

// DEMO MIGRATION (docs/port-framework.md validation #4): PerModeFrame's hand-rolled RAII replaced
// by runtime/recomp/guest_abi.h's GuestFrame<FrameSize, NumSpills> — the contract-driven form
// tools/abi_extract.py <addr> --scaffold --guestabi emits straight from gen_func_8003F698's real
// `addiu sp,-24` prologue (ra spill only, at sp+16). Behavior identical; this is purely the OPT-IN
// style swap the framework's deliverable 2 exists to validate (SBS-full 0-diff gate covers it).
static constexpr GuestFrameSpill kSpills_8003F698[1] = { { 31 /*ra*/, 16 } };

namespace {
constexpr uint32_t SCR        = 0x1F800000u;   // PSX scratchpad base (the engine's GTE-compose temp area)
constexpr uint32_t WORLD_POS  = SCR + 0xC0u;   // object world position stash (0x1F8000C0/C2/C4)
constexpr uint32_t CAM_ROT    = 0x1F8000F8u;   // scene camera view rotation (CR-packed 3x3, 5 words)
constexpr uint32_t CAM_TRANS  = 0x1F80010Cu;   // scene camera view translation (3 s32)
constexpr uint32_t OBJ_ROT    = SCR;           // composed WORLD object rotation (CR-packed) + [+20/24/28] translation
constexpr uint32_t OTBASE_PTR = 0x800ED8C8u;   // *this = the active ordering-table base
constexpr uint32_t MODE_BYTE  = 0x800BF870u;   // *this = render-mode select (0..0x15)
constexpr uint32_t MODE_FORCE = 0x1F800234u;   // *this != 0 forces the generic GT3/GT4 path
constexpr uint32_t MODE_TABLE = 0x80015268u;   // 22-entry jump table: mode -> per-mode renderer addr
constexpr uint32_t MVMVA_ROTCOL = 0x4A49E012u; // MVMVA: camera-rot(CR0-4) x IR vector -> composed col
constexpr uint32_t MVMVA_TRANS  = 0x4A486012u; // MVMVA: camera-rot(CR0-4) x V0 (object world position)
}

// Forward decl: perModeCaseTarget's real definition sits with perModeDispatch below (it decodes
// MODE_TABLE's per-mode jump-table LABEL values into the actual FUN_ target address); cmdListDispatch's
// REDIRECT (below) needs it to recognize the generic-overlay case BEFORE calling perModeDispatch.
static uint32_t perModeCaseTarget(uint32_t caseLabel);

// FUN_8003CDD8 — per-object cmd-list dispatch: composes the WORLD object transform (camera-rot x
// object-local, via MVMVA) into GTE CR0-7 for each active render command, then calls FUN_8003F698.
// ORACLE: gen_func_8003CDD8 (tools/port_check.py equivalence-gate marker; see docs/port-framework.md)
void Render::cmdListDispatch() {
  Core* c = mCore;
  CmdListFrame frame(c);   // real -56 guest frame (RE: gen_func_8003CDD8 prologue) — descended even
                            // on the immediate-return path below, exactly like gen.
  const uint32_t node = c->r[4];
  const uint32_t flag = c->r[5];
  if (c->mem_r8(node + 8) == 0 || c->mem_r8(node + 9) == 0) return;
  // Exact control flow of the recomp body: count (node+8) is re-checked at the TOP of every
  // iteration (bails mid-list if the active count ever shrinks); capacity (node+9) gates only the
  // BACK-edge (post-increment), so it is never consulted before running i==0's body. Re-read both
  // fields fresh each time rather than caching (nothing in this body writes them, but this loop must
  // reproduce the recomp's exact reads, not an equivalent-in-practice shortcut).
  int i = 0;
  for (;;) {
    if (i >= (int)c->mem_r8(node + 8)) return;
    const uint32_t cmd     = c->mem_r32(node + 0xC0u + (uint32_t)i * 4);
    const uint32_t geomblk = c->mem_r32(cmd + 0x40u);
    if (geomblk != 0) {

    // Stash the object's world position (cmd+0x2C/0x30/0x34) for the translate MVMVA below.
    c->mem_w16(WORLD_POS + 0, c->mem_r16(cmd + 0x2Cu));
    c->mem_w16(WORLD_POS + 2, c->mem_r16(cmd + 0x30u));
    c->mem_w16(WORLD_POS + 4, c->mem_r16(cmd + 0x34u));

    // Load the CAMERA rotation into CR0-4.
    gte_write_ctrl(0, c->mem_r32(CAM_ROT + 0));
    gte_write_ctrl(1, c->mem_r32(CAM_ROT + 4));
    gte_write_ctrl(2, c->mem_r32(CAM_ROT + 8));
    gte_write_ctrl(3, c->mem_r32(CAM_ROT + 12));
    gte_write_ctrl(4, c->mem_r32(CAM_ROT + 16));

    // Compose the object's LOCAL rotation (cmd+0x18, 3 columns, row-stride 6 / col-stride 2) through
    // the camera rotation, one MVMVA per column, into OBJ_ROT (CR-packed 3x3).
    for (int col = 0; col < 3; col++) {
      const uint32_t base = cmd + 0x18u + (uint32_t)col * 2;
      gte_write_data(9,  (uint32_t)c->mem_r16(base + 0));
      gte_write_data(10, (uint32_t)c->mem_r16(base + 6));
      gte_write_data(11, (uint32_t)c->mem_r16(base + 12));
      gte_op(c, MVMVA_ROTCOL);
      const uint32_t dst = OBJ_ROT + (uint32_t)col * 2;
      c->mem_w16(dst + 0,  (uint16_t)gte_read_data(9));
      c->mem_w16(dst + 6,  (uint16_t)gte_read_data(10));
      c->mem_w16(dst + 12, (uint16_t)gte_read_data(11));
    }

    // Transform the stashed world position by the (still-resident) camera rotation, add the camera
    // translation, and store the composed WORLD translation right after the rotation in OBJ_ROT.
    gte_write_data(0, c->mem_r32(WORLD_POS + 0));   // VXY0 = (posX, posY)
    gte_write_data(1, c->mem_r32(WORLD_POS + 4));   // VZ0  = posZ (low half)
    gte_op(c, MVMVA_TRANS);
    const int32_t tx = (int32_t)gte_read_data(25) + (int32_t)c->mem_r32(CAM_TRANS + 0);
    const int32_t ty = (int32_t)gte_read_data(26) + (int32_t)c->mem_r32(CAM_TRANS + 4);
    const int32_t tz = (int32_t)gte_read_data(27) + (int32_t)c->mem_r32(CAM_TRANS + 8);
    c->mem_w32(OBJ_ROT + 20, (uint32_t)tx);
    c->mem_w32(OBJ_ROT + 24, (uint32_t)ty);
    c->mem_w32(OBJ_ROT + 28, (uint32_t)tz);

    // Reload CR0-7 from the composed WORLD object transform for the per-mode renderer to consume.
    gte_write_ctrl(0, c->mem_r32(OBJ_ROT + 0));
    gte_write_ctrl(1, c->mem_r32(OBJ_ROT + 4));
    gte_write_ctrl(2, c->mem_r32(OBJ_ROT + 8));
    gte_write_ctrl(3, c->mem_r32(OBJ_ROT + 12));
    gte_write_ctrl(4, c->mem_r32(OBJ_ROT + 16));
    gte_write_ctrl(5, (uint32_t)tx);
    gte_write_ctrl(6, (uint32_t)ty);
    gte_write_ctrl(7, (uint32_t)tz);

    // OT base select: same sub-bucket rule as Render::perObjFlush.
    const uint32_t otbase_val = c->mem_r32(OTBASE_PTR);
    uint32_t otbase = otbase_val;
    if ((c->mem_r8(node + 0xDu) & 0xFu) == 4)
      otbase = otbase_val + ((uint32_t)(int32_t)c->mem_r8s(cmd + 0x3Fu) << 2);

    // REDIRECT (docs/fps60-rework.md, RE+PORT not stamping): give this cmd's PICTURE real engine
    // identity when perModeDispatch is about to route it to the generic-overlay leaf FUN_80146478
    // (OverlayGt3Gt4::gt3/gt4, overlay_gt3gt4.cpp) — a byte-exact GTE mirror with no object identity
    // (no dbg_node, coarse flat per-object depth via the obj_depth billboard fallback, screen position
    // from the GTE not the engine). FUN_80146478's own record format — geomblk+0 = {GT3 count lo16,
    // GT4 count hi16}, GT3 records at geomblk+16 (36B stride), GT4 records after (44B stride) — is
    // IDENTICAL to what Render::gt3gt4/submitPolyGt3/4Native already parse for every native-owned
    // object (submit.cpp Render::gt3gt4: `counts=mem32(geomblk+0); gt3(geomblk+16,...)`). And this
    // cmd's own `cmd+0x18` rotation / `cmd+0x2C` world position — the SAME fields cmdListDispatch just
    // read to compose the GTE CR0-7 transform above — are the SAME fields Render::projComposeObject
    // reads for perObjFlush's world objects. So the geometry FITS the existing native path exactly;
    // this is a real REDIRECT (drawing the SAME data through the owned path), not a stamp/fake.
    //
    // Gated to render_field_native_active(c) (pc_render's native field-pass window — see
    // render_internal.h) so this NEVER runs under psx_render/oracle/menus/narration, where the guest
    // OT's full walk is the sole picture source and an extra native draw would double-draw.
    //
    // Invariant (a): the substrate GTE math below (perModeDispatch -> FUN_80146478 -> gt3/gt4) is
    // UNTOUCHED and still runs unconditionally — SBS byte-exactness is unaffected, only the PICTURE
    // decision changes.
    // Invariant (b) NO DOUBLE-DRAW: register this cmd's own packet-pool span (captured via a nested
    // PktSpanSession bracketing ONLY the perModeDispatch call below) into the native-cover registry,
    // so gpu_native.cpp's field OT walk drops the substrate's now-redundant guest-OT copy instead of
    // billboard-promoting it via obj_depth (see gpu_native_internal.h's NATIVE-COVER banner). The
    // nested session's span still MERGES into perObjRenderDispatch's own outer withDepthTag session
    // (pkt_span.h: nesting always merges) — harmless, since native-cover is checked BEFORE the
    // obj_depth billboard promotion in gp0_exec.
    // Invariant (c): dbg_node = the real owning node (perObjRenderDispatch's `node` — beginObject
    // brackets ONLY this native draw, mirroring withDepthTag's own discipline) so matchAndLerp covers
    // these prims with no matcher changes.
    // COVERAGE (bug #48, docs/findings/render.md "Z-fight sweep 2026-07-14"; render.h banner):
    // independent of which per-mode target this cmd resolves to, if Render::perObjFlush (render_walk.
    // cpp) has ALREADY drawn this SAME node's cmd list natively this frame, the perModeDispatch call
    // below is a redundant guest-OT copy for EVERY target (func_800803DC's generic emitter and every
    // owned per-mode leaf alike all draw the identical geomblk this cmd already carries) — not just
    // the generic-overlay REDIRECT case. PROVENANCE via Render::nativeObjDrawn (perObjFlush is the
    // only writer): nodes on OTHER walk lists perObjFlush never visits (e.g. the Bcf4 aux list the
    // REDIRECT below exists for) are simply absent, so they stay uncovered unless redirectGeneric's
    // own inline native draw covers them.
    bool nodeNativeCovered = render_field_native_active(c) && rend(c)->nativeObjDrawn(c, node);
    bool redirectGeneric = false;
    if (render_field_native_active(c)) {
      const uint32_t modeByte = c->mem_r8(MODE_BYTE);
      redirectGeneric = c->mem_r8(MODE_FORCE) == 0 && (flag & 1u) == 0 && modeByte < 22 &&
                        perModeCaseTarget(c->mem_r32(MODE_TABLE + modeByte * 4u)) == 0x80146478u;
      if (redirectGeneric && !nodeNativeCovered) {
        if (cfg_dbg("redirect")) { static long n=0; if (n++%256==0)
          cfg_logf("redirect", "cmdListDispatch node=%08X cmd=%08X geomblk=%08X otbase=%08X", node, cmd, geomblk, otbase); }
        // FAIL-FAST (CLAUDE.md pc_render READ-ONLY OVERLAY invariant): this native draw is a
        // display-pass addition living INSIDE cmdListDispatch, whose surrounding substrate body
        // legitimately writes guest RAM (GTE CR/OT/packet-pool) — so the guard is scoped tightly to
        // JUST this block, not the whole function, the same discipline game_tomba2.cpp's
        // Engine::drawOTag uses around sceneNative().
        DisplayPassGuard displayPass(c->rsub.mode);
        c->rsub.diag.beginObject(node);           // real dbg_node identity for this cmd's RqItems
        EObjXform w; rend(c)->projComposeObject(cmd, &w); rend(c)->projSetActive(&w);
        rend(c)->gt3gt4(geomblk, otbase);           // the real picture: native float, real per-vertex depth
        rend(c)->projClearActive();
        c->rsub.diag.endObject();
      }
    }

    c->r[4] = geomblk; c->r[5] = otbase; c->r[6] = flag;
    // Register-faithfulness (f62 residual root cause, 2026-07-09): gen_func_8003CDD8 keeps r16..r23
    // LIVE as loop-invariant/loop-index scratch for its ENTIRE loop body (r16=i the loop counter,
    // r17=r23=SCR scratchpad base 0x1F800000, r18=node, r19=SCR+0xD0, r20=OTBASE_PTR, r21=WORLD_POS,
    // r22=flag — verified against generated/shard_6.c gen_func_8003CDD8 lines 5119-5285). These
    // survive the nested func_8003F698/func_800803DC call chain via plain MIPS callee-save (never
    // explicitly reloaded before each per-iteration call). The still-substrate `func_800803DC`
    // (unowned generic GT3/GT4 emitter) SPILLS the incoming r16/r17 to its own guest stack frame
    // (sp+16/sp+20) before reusing them as locals, then restores them on return — i.e. r16/r17's
    // CALLER value is genuine guest-stack-visible state, not dead scratch. Native cmdListDispatch used
    // C++ locals for `i`/`node`/`flag` and never wrote c->r[16..23], so func_800803DC's prologue was
    // spilling STALE leftover register content instead of gen's real loop state — the exact SBS diff
    // at 0x801FE870..0x801FE878 (verified: gen's r16=i, r17=SCR match the two divergent words byte-
    // for-byte). Set the full live set here (not just r16/r17) since perModeDispatch's mode-table path
    // can reach OTHER still-substrate per-mode renderers that may equally depend on this callee-save
    // state.
    c->r[16] = (uint32_t)i;
    c->r[17] = SCR;
    c->r[18] = node;
    c->r[19] = SCR + 0xD0u;
    c->r[20] = OTBASE_PTR;
    c->r[21] = WORLD_POS;
    c->r[22] = flag;
    c->r[23] = SCR;
    c->r[31] = 0x8003D07Cu;   // RE'd return-address constant (gen_func_8003CDD8, right before func_8003F698)
    if (redirectGeneric || nodeNativeCovered) {
      // NO DOUBLE-DRAW (invariant b, extended to the broader nodeNativeCovered case above): capture
      // the substrate's own packet-pool span (bracketing ONLY this call, so terrain/other cmds on the
      // SAME node aren't mis-attributed) and register it as native-covered — gpu_native.cpp's field OT
      // walk drops it unconditionally instead of billboard-promoting it via obj_depth (checked first
      // in gp0_exec; see the NATIVE-COVER banner in gpu_native_internal.h). This nested session's
      // range still merges into perObjRenderDispatch's outer withDepthTag session (pkt_span.h: nesting
      // always merges) — harmless, since native-cover wins over obj_depth regardless of which registry
      // also holds the span.
      PktSpanSession nativeCoverSess(c);
      perModeDispatch();
      uint32_t nlo, nhi;
      if (nativeCoverSess.close(&nlo, &nhi)) gpu_native_cover_add(c, nlo, nhi);
    } else {
      perModeDispatch();
    }
    } // if (geomblk != 0)
    i++;
    if (!(i < (int)c->mem_r8(node + 9))) return;
  }
}

// MODE_TABLE's 22 entries are NOT the final FUN_ target addresses — they are addresses of F698's OWN
// internal case labels (jump-table entries the recompiler statically resolved when it built the
// switch below), confirmed by reading the live table out of a free-roam RAM dump
// (scratch/bin/field_ram.bin @0x80015268, all 22 words are one of these 11 literals). Each label's
// body immediately rec_dispatch'es a fixed real target; 0x8003F788 is the "generic" label that falls
// through to func_800803DC instead. This mapping is fixed game DATA (identical to the switch in
// generated/shard_4.c gen_func_8003F698), not something that varies at runtime.
static uint32_t perModeCaseTarget(uint32_t caseLabel) {
  switch (caseLabel) {
    case 0x8003F6E8u: return 0x80146478u;
    case 0x8003F6F8u: return 0x80132DC0u;
    case 0x8003F708u: return 0x8012555Cu;
    case 0x8003F718u: return 0x8013DAFCu;
    case 0x8003F728u: return 0x801362CCu;
    case 0x8003F738u: return 0x8013D568u;
    case 0x8003F748u: return 0x8012E1A0u;
    case 0x8003F758u: return 0x8012A9DCu;
    case 0x8003F768u: return 0x80116B14u;
    case 0x8003F778u: return 0x8010B1B8u;
    default:          return 0;   // 0x8003F788 (generic) or anything unrecognized -> fallback
  }
}

// RE'd return-address constant gen sets in r31 immediately before each case's dispatch call (see
// generated/shard_4.c gen_func_8003F698, labels L_8003F6E8.. — each is `caseLabel + 8`). Register-
// faithfulness (2026-07-09, the f118 residual root cause): a prior draft called rec_dispatch without
// ever touching c->r[31], leaving whatever STALE value the caller (perObjRenderDispatch/cmdListDispatch)
// left there instead — a real, reproducible SBS diff at FUN_80146478's own ra spill slot
// (0x801FE8D0..). Mirrored below per CLAUDE.md ("MIRROR THE GUEST STACK... register-faithfulness").
static uint32_t perModeCaseReturnAddr(uint32_t caseLabel) { return caseLabel + 8u; }

// FUN_8003F698 — per-mode render dispatcher: routes to the area's per-mode renderer (mode-select byte
// + jump table) or the generic GT3/GT4 packet emitter (func_800803DC).
void Render::perModeDispatch() {
  Core* c = mCore;
  GuestFrame<24, 1> frame(c, kSpills_8003F698);   // real -24 guest frame (RE: gen_func_8003F698 prologue, ra spill only)
  const uint32_t flag = c->r[6];
  if (c->mem_r8(MODE_FORCE) == 0 && (flag & 1u) == 0) {
    const uint32_t mode = c->mem_r8(MODE_BYTE);
    if (mode < 22) {
      const uint32_t caseLabel = c->mem_r32(MODE_TABLE + mode * 4);
      const uint32_t target = perModeCaseTarget(caseLabel);
      if (target != 0) { guest_dispatch(c, perModeCaseReturnAddr(caseLabel), target); return; }
      // caseLabel == 0x8003F788 (or an unrecognized label) -> the recomp's own `default:
      // rec_dispatch(c, c->r[2])` would dispatch the RAW label address here; since 0x8003F788 IS the
      // generic-fallback label (whose body is just `func_800803DC(c)`, no rec_dispatch), reproduce
      // that directly rather than rec_dispatch-ing a label address that has no recompiled entry.
      if (caseLabel != 0x8003F788u) { guest_dispatch(c, perModeCaseReturnAddr(caseLabel), caseLabel); return; }
    }
  }
  c->r[31] = 0x8003F790u;   // RE'd: L_8003F788's own r31 set before func_800803DC (the generic label)
  func_800803DC(c);
}

namespace {
void ov_cmdListDispatch(Core* c) { rend(c)->cmdListDispatch(); }
void ov_perModeDispatch(Core* c) { rend(c)->perModeDispatch(); }
}

// ORACLE-PURITY FIX (2026-07-09, the f118 residual root cause): these two were installed via the RAW
// shard_set_override — the process-global g_override[] table shard_disp.c's func_8003CDD8/func_8003F698
// wrappers consult on BOTH cores, with no oracle gate. That means SBS core B (supposed to be the pure
// gen_func_* substrate) was ALSO running this native code whenever gen_func_8003CCA4 (correctly running
// pure on B via its own engine_set_override_main registration) called func_8003CDD8(c) — exactly the
// failure mode override_registry.h's own banner documents ("a trampoline that omitted its
// psx_fallback guard silently ran native on core B and turned SBS into a native-vs-native fake
// 0-diff"). Concretely: native Render::perObjRenderDispatch never mirrors gen_func_8003CCA4's
// `c->r[18] = node` prologue assignment (r18 is plain scratch to the native C++ body), so when B's PURE
// gen_func_8003CCA4 called into this SAME native cmdListDispatch, the CmdListFrame RAII spilled A's
// stale r18 instead of B's real node pointer — a genuine cross-core state leak, not just a byte diff.
// Fixed by routing through the shared override registry (engine_set_override_main) like every other
// engine/game native in this call chain (perobj_billboard.cpp, overlay_gt3gt4.cpp,
// overlay_ground_gt3gt4.cpp, quad_rtpt_submit.cpp) — B now always runs the real gen_func_8003CDD8/
// gen_func_8003F698 bodies, closing the leak at its source.
void perobj_dispatch_install() {
  static bool done = false;
  if (done) return;
  done = true;
  extern void gen_func_8003CDD8(Core*);
  extern void gen_func_8003F698(Core*);
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x8003CDD8u, ov_cmdListDispatch, gen_func_8003CDD8);
  engine_set_override_main(0x8003F698u, ov_perModeDispatch, gen_func_8003F698);
}
