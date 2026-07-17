// game/render/widescreen_margin_quad.cpp — native port of FUN_8013CDD4, the widescreen-margin
// OT.GT4 quad emitter (a00 overlay leaf, override slot 451).
//
// RE: generated/ov_a00_shard_1.c:25528-25899 (ov_a00_gen_8013CDD4) is the ground truth — the
// recompiler's per-instruction transcription, more precise than Ghidra's COP2/branch-delay
// decompilation for this function. Cross-referenced with `tools/abi_extract.py 0x8013CDD4
// --contract` for the frame/spill table. Full field-layout derivation + the load-bearing
// record+30 dual read (V0.y at RTPT time, then re-read sign-extended as the shared fog delta
// input) is recorded in docs/findings/render.md "0x8013CDD4 port — ambiguity SETTLED".
//
// Same faithful-substrate-mirror carve-out as OverlayGt3Gt4 (see overlay_gt3gt4.cpp): this is
// the SUBSTRATE's own GTE + OT + packet-pool writer, not pc_render. Every guest write below is
// part of the byte-exact state SBS compares — no write here is optional or "residual".
#include "core.h"
#include "game_ctx.h"
#include "game.h"
#include "guest_abi.h"
#include "widescreen_margin_quad.h"
#include "render.h"            // Render::WqRec — display-pass capture (#67/#66)
#include "render_internal.h"   // wq_factor_world
#include "cfg.h"
#include <cstdint>

namespace {

// -------------------------------------------------------------------------------------------
// Guest addresses / constants (named, not inline hex — CLAUDE.md "no magic constant offsets").
constexpr uint32_t kPktPoolBase  = 0x800C0000u;   // gen's base for the pool-cursor address (v0 residue)
constexpr uint32_t kPktPoolPtr   = 0x800BF544u;   // packet-pool bump-allocator cursor (== base-2748;
                                                   // same pool OverlayGt3Gt4's gt3/gt4 write into).
constexpr uint32_t kPktTag       = 0x0C000000u;   // POLY_GT4 tag high word (len=12 words << 24); v1 residue
constexpr uint32_t kOtBasePtrPtr = 0x800ED8C8u;   // guest word holding the live OT base pointer.
constexpr uint32_t kGteFlagScratch = 0x1F800080u; // scratchpad temp: FLAG readback / OTZ combine
                                                   // (gen uses two aliased registers, r20 and r22,
                                                   // for the exact same address; both mirrored).
constexpr uint32_t kComposeObjectTransform = 0x800318A0u; // "compose object transform into GTE
                                                   // CR0-8" (docs/engine_re.md cluster 3) — still
                                                   // substrate; called via rec_dispatch exactly as
                                                   // gen does, same as any un-owned leaf callee.
constexpr uint32_t kComposeObjectTransformRa = 0x8013CE74u; // jal-site return address (gen: r31
                                                   // is set to this literal before the call).
constexpr uint32_t kColorCodeTag = 0x3E000000u;   // fixed rgb0 code-byte tag OR'd in after masking
constexpr uint32_t kPlanePayloadMask = 0x007FFFFFu; // low 23 bits of the dual-purpose plane word

// Pack a signed screen-delta byte into a GTE-input halfword: the byte lands as the HIGH byte of a
// 16-bit value (value*256 — an 8.8-style fixed-point scale). gen stages each vertex coordinate this
// way into guest stack scratch, then loads 32-bit words back into the GTE VXY/VZ data registers.
uint16_t packDelta(int8_t b) { return (uint16_t)((uint8_t)b << 8); }

// Stack-scratch layout for the per-record vertex staging block (sp-relative byte offsets). gen
// writes each coordinate as a halfword here, leaving sp+38/46/54/62 as prior stack content, then
// reads 32-bit words spanning those gaps into the GTE (the stale upper halves are GTE-ignored VZ
// bits). These are GUEST-VISIBLE stack bytes SBS byte-compares, so the port must write them too —
// it does NOT feed the GTE from host registers (that skipped the guest stack and diverged).
enum : uint32_t {
  kVtxScratch_X0 = 32, kVtxScratch_Y0 = 34, kVtxScratch_Z0 = 36,  // -> GTE VXY0 (sp+32), VZ0 (sp+36)
  kVtxScratch_X1 = 40, kVtxScratch_Y1 = 42, kVtxScratch_Z1 = 44,  // -> GTE VXY1 (sp+40), VZ1 (sp+44)
  kVtxScratch_X2 = 48, kVtxScratch_Y2 = 50, kVtxScratch_Z2 = 52,  // -> GTE VXY2 (sp+48), VZ2 (sp+52)
  kVtxScratch_X3 = 56, kVtxScratch_Y3 = 58, kVtxScratch_Z3 = 60,  // -> GTE VXY3 (sp+56), VZ3 (sp+60)
};

// -------------------------------------------------------------------------------------------
// Record lens: 36-byte-stride quad record. Field roles below are exactly what generated/
// ov_a00_shard_1.c reads/writes at each offset (traced instruction-by-instruction); offsets not
// independently named by docs/findings/render.md are named by their PACKET destination role.
struct MarginQuadRecord {
  Core* c;
  uint32_t addr;

  uint32_t uv0ClutBase()   const { return c->mem_r32(addr + 0); }  // -> pool uv0|clut (+ node clut bias)
  uint32_t planeWord()     const { return c->mem_r32(addr + 4); }  // dual-purpose: continuation flag
                                                                    // (tested as signed >0) AND, masked
                                                                    // to kPlanePayloadMask, the uv1|tpage payload.
  uint32_t uv2Word()       const { return c->mem_r32(addr + 8); }  // -> pool uv2 raw; its high 16 bits
                                                                    // (sign-extended) also become pool uv3.
  uint32_t colorCode0Src() const { return c->mem_r32(addr + 12); } // -> pool rgb0|code (masked+re-tagged)
  uint32_t rgb1Raw()       const { return c->mem_r32(addr + 16); } // -> pool rgb1, raw copy
  uint32_t rgb2Raw()       const { return c->mem_r32(addr + 20); } // -> pool rgb2, raw copy
  uint32_t rgb3Raw()       const { return c->mem_r32(addr + 24); } // -> pool rgb3, raw copy

  int8_t z0() const { return (int8_t)c->mem_r8(addr + 15); }
  int8_t z1() const { return (int8_t)c->mem_r8(addr + 19); }
  int8_t z2() const { return (int8_t)c->mem_r8(addr + 23); }
  int8_t z3() const { return (int8_t)c->mem_r8(addr + 27); }

  int8_t x0() const { return (int8_t)c->mem_r8(addr + 28); }
  int8_t x1() const { return (int8_t)c->mem_r8(addr + 29); }
  // addr+30: shared byte — V0.y (packed into VXY0's high half) AND, read again with NO shift and
  // sign-extended, the fog-delta input. Extract ONCE (below), use twice — the settled ambiguity.
  int8_t y0AndFogByte() const { return (int8_t)c->mem_r8(addr + 30); }
  int8_t y1() const { return (int8_t)c->mem_r8(addr + 31); }
  int8_t x2() const { return (int8_t)c->mem_r8(addr + 32); }
  int8_t x3() const { return (int8_t)c->mem_r8(addr + 33); }
  int8_t y2() const { return (int8_t)c->mem_r8(addr + 34); }
  int8_t y3() const { return (int8_t)c->mem_r8(addr + 35); }

  bool continues() const { return (int32_t)planeWord() > 0; }
};

// clamp0_255(base - delta): the fog-shade helper shared by all four vertex colours. Traced
// instruction-by-instruction against generated/ov_a00_shard_1.c:25715-25858 — R, G and B each
// get an UNCONDITIONAL `-= delta` (B's subtraction happens to live in R's branch-delay slot, a
// pure instruction-scheduling artifact) followed by an independent clamp-to-[0,255]. The net
// effect for all three channels is the same ordinary clamp; no real per-channel asymmetry
// survives the trace (per the finding: "replicate as-is" — this literal transcription does).
uint8_t fogShade(uint8_t base, int32_t delta) {
  int32_t v = (int32_t)base - delta;
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

// OTZ index for this leaf's z-combine: exponent-nibble shift-and-recombine, exactly the bit
// pattern generated/ov_a00_shard_1.c:25674-25682 performs. Distinct bounds from
// overlay_gt_otz_index in overlay_gt3gt4.cpp (this leaf's valid range is [4, 2047]) — its own
// small helper rather than a shared one, matching the ground truth exactly.
int32_t marginOtzIndex(int32_t zPlusBias) {
  int32_t shift = zPlusBias >> 10;
  int32_t idx = (zPlusBias >> (shift & 31)) + shift * 512;
  bool inRange = (uint32_t)(idx - 4) < 2044u;   // idx in [4, 2047]
  return inRange ? idx : -1;
}

} // namespace

void WidescreenMarginQuad::emit(Core* c) {
  // Frame: sp -= 104, spill s0..s6/ra at their RE'd offsets (tools/abi_extract.py --contract).
  // Mirrored per CLAUDE.md "MIRROR THE GUEST STACK" — the compose-transform call below is a real
  // guest call into still-substrate code that may spill/restore these same registers.
  static constexpr GuestFrameSpill kSpills[] = {
    {18, 80}, {17, 76}, {20, 88}, {22, 96}, {19, 84}, {31, 100}, {21, 92}, {16, 72},
  };
  GuestFrame<104, 8> frame(c, kSpills);

  // Callee-saved registers, held live in c->r[N] (not C++ locals) for the duration they mirror
  // gen's own usage — see guest_abi.h's GuestReg rationale.
  GuestReg<18> obj(c);          obj = c->r[4];
  GuestReg<17> pool(c);         pool = c->mem_r32(kPktPoolPtr);
  GuestReg<20> zScratch(c);     zScratch = kGteFlagScratch;
  GuestReg<22> flagScratch(c);  flagScratch = kGteFlagScratch;
  // r19 = &obj->records (used only to derive the fogBase field at r19+6 == obj+86); the record
  // ARRAY POINTER ITSELF is a separate dereference, mem32(obj+80) — NOT obj+80 (r19's own value).
  GuestReg<19> recArrayField(c); recArrayField = (uint32_t)obj + 80;
  GuestReg<21> clutBiasLow(c);  clutBiasLow = (uint32_t)c->mem_r8((uint32_t)obj + 7) << 5;
  GuestReg<16> node(c);         node = c->mem_r32((uint32_t)obj + 60);

  // Empty-node early out. gen reads r2 = mem8(obj+7) just before the node test (it's the source of
  // clutBiasLow's <<5); on the node==0 path r2 is never clobbered again, so it is this leaf's v0
  // residue. Reproduce it for register-exact fidelity (v1/r3 is untouched on this path, as in gen).
  if ((uint32_t)node == 0) { c->r[2] = c->mem_r8((uint32_t)obj + 7); return; }

  // Compose the node's rotation angles (each byte scaled x10, a PSX-angle-unit conversion) via
  // the still-unowned "compose object transform into GTE CR0-8" leaf (docs/engine_re.md cluster
  // 3, 0x800318A0) — it writes the composed camera-relative R/T straight into scratchpad
  // 0x1F8000F8../0x1F800014.. for the RTPT/RTPS calls below to consume. Args mirror gen exactly.
  c->mem_w8(c->r[29] + 64, (uint8_t)(c->mem_r8((uint32_t)node + 0) * 10));
  c->mem_w8(c->r[29] + 65, (uint8_t)(c->mem_r8((uint32_t)node + 1) * 10));
  c->mem_w8(c->r[29] + 66, (uint8_t)(c->mem_r8((uint32_t)node + 2) * 10));
  guest_fn(c, kComposeObjectTransform, kComposeObjectTransformRa,
           (uint32_t)obj + 44, c->r[29] + 64, (uint32_t)obj + 72);

  const uint8_t nodeFlags = c->mem_r8((uint32_t)node + 3);
  const uint32_t clutBiasHigh = (uint32_t)(nodeFlags & 0x0Fu) << 22;
  // obj+64 "next" field: node.flags bit7 clear -> node+4, set -> 0 (traced literally; this
  // function never reads obj+64 back itself, but it's an observable guest write SBS compares).
  c->mem_w32((uint32_t)obj + 64, (nodeFlags & 0x80u) ? 0u : ((uint32_t)node + 4));

  const uint8_t  objRenderKind    = c->mem_r8((uint32_t)obj + 3);
  const uint8_t  objRenderSubKind = c->mem_r8((uint32_t)obj + 5);
  const int32_t  objZBias  = c->mem_r16s((uint32_t)obj + 50);
  const int32_t  fogBase   = c->mem_r16s((uint32_t)recArrayField + 6); // = mem16(obj+86)

  uint32_t poolBase = (uint32_t)pool + 48; // tracks (current-packet-base + 48); advances on commit
  uint32_t recAddr = c->mem_r32((uint32_t)recArrayField); // dereference: the actual record array ptr

  cfg_logf("wmq", "obj=%08X pool=%08X recArrayPtr=%08X node=%08X",
           (uint32_t)obj, (uint32_t)pool, recAddr, (uint32_t)node);

  for (;;) {
    MarginQuadRecord rec{c, recAddr};
    const bool more = rec.continues();

    // --- GTE transform: V0..V2 via RTPT, V3 via a separate RTPS (the GTE only triples). ---
    // Stage all 12 vertex coordinates into guest stack scratch first (byte-exact guest state), then
    // load 32-bit words into the GTE. sharedByte30 = record+30, read once, used twice (see below).
    const uint32_t sp = c->r[29];
    const int8_t sharedByte30 = rec.y0AndFogByte();
    c->mem_w16(sp + kVtxScratch_X0, packDelta(rec.x0()));
    c->mem_w16(sp + kVtxScratch_Y0, packDelta(sharedByte30));
    c->mem_w16(sp + kVtxScratch_Z0, packDelta(rec.z0()));
    c->mem_w16(sp + kVtxScratch_X1, packDelta(rec.x1()));
    c->mem_w16(sp + kVtxScratch_Y1, packDelta(rec.y1()));
    c->mem_w16(sp + kVtxScratch_Z1, packDelta(rec.z1()));
    c->mem_w16(sp + kVtxScratch_X2, packDelta(rec.x2()));
    c->mem_w16(sp + kVtxScratch_Y2, packDelta(rec.y2()));
    c->mem_w16(sp + kVtxScratch_Z2, packDelta(rec.z2()));
    c->mem_w16(sp + kVtxScratch_X3, packDelta(rec.x3()));
    c->mem_w16(sp + kVtxScratch_Y3, packDelta(rec.y3()));
    c->mem_w16(sp + kVtxScratch_Z3, packDelta(rec.z3()));

    gte_write_data(0, c->mem_r32(sp + kVtxScratch_X0)); // VXY0 = {x0, y0}
    gte_write_data(1, c->mem_r32(sp + kVtxScratch_Z0)); // VZ0  (low half z0; upper = stale gap, GTE-ignored)
    gte_write_data(2, c->mem_r32(sp + kVtxScratch_X1)); // VXY1 = {x1, y1}
    gte_write_data(3, c->mem_r32(sp + kVtxScratch_Z1)); // VZ1
    gte_write_data(4, c->mem_r32(sp + kVtxScratch_X2)); // VXY2 = {x2, y2}
    gte_write_data(5, c->mem_r32(sp + kVtxScratch_Z2)); // VZ2
    gte_op(c, 0x4A280030u); // RTPT

    // rgb0|code: colorCode0Src masked to 24 bits, re-tagged with the fixed code byte.
    c->mem_w32(poolBase - 44, (rec.colorCode0Src() & 0x00FFFFFFu) | kColorCodeTag);
    // uv0|clut: record's base word plus the node's clut bias.
    c->mem_w32(poolBase - 36, rec.uv0ClutBase() + clutBiasHigh);

    c->mem_w32(zScratch, gte_read_ctrl(31));
    if ((int32_t)c->mem_r32(zScratch) < 0) goto skipRecord;

    c->mem_w32(pool + 8,  gte_read_data(12)); // SXY0
    c->mem_w32(pool + 20, gte_read_data(13)); // SXY1
    c->mem_w32(pool + 32, gte_read_data(14)); // SXY2

    // uv1|tpage: low 23 bits of the same plane word that also gates the loop's continuation.
    c->mem_w32(poolBase - 24, rec.planeWord() & kPlanePayloadMask);
    // uv2 raw copy, plus its high half (sign-extended) staged into uv3.
    c->mem_w32(poolBase - 12, rec.uv2Word());
    c->mem_w32(poolBase + 0,  (uint32_t)((int32_t)rec.uv2Word() >> 16));

    // V3 via RTPS (the GTE's RTPT only transforms 3 points) — loaded from the same stack scratch.
    gte_write_data(0, c->mem_r32(sp + kVtxScratch_X3)); // VXY3 = {x3, y3}
    gte_write_data(1, c->mem_r32(sp + kVtxScratch_Z3)); // VZ3
    c->mem_w32(poolBase - 32, rec.rgb1Raw()); // rgb1 raw copy
    gte_op(c, 0x4A180001u); // RTPS

    c->mem_w32(flagScratch, gte_read_ctrl(31));
    if ((int32_t)c->mem_r32(flagScratch) < 0) goto skipRecord;
    c->mem_w32(pool + 44, gte_read_data(14)); // SXY3
    c->mem_w32(poolBase - 20, rec.rgb2Raw());  // rgb2 raw copy

    gte_op(c, 0x4B68002Eu); // AVSZ4
    c->mem_w32(poolBase - 8, rec.rgb3Raw());   // rgb3 raw copy

    {
      int32_t idx = marginOtzIndex((int32_t)gte_read_data(7) + objZBias);
      c->mem_w32(zScratch, (uint32_t)idx);
      if (idx < 0) goto skipRecord;
    }

    // Kind-gated tpage override: unless the object's render kind is 1 or 3, records whose
    // sub-kind falls in [3,6) or [9,11) get their uv1|tpage high half force-set to 46.
    if (objRenderKind != 1 && objRenderKind != 3) {
      if ((objRenderSubKind >= 3 && objRenderSubKind < 6) ||
          (objRenderSubKind >= 9 && objRenderSubKind < 11)) {
        c->mem_w16(poolBase - 22, 46);
      }
    }

    // Fog shade: ONE shared delta (record+30's byte, sign-extended, minus the object's fog
    // base, clamped >=0) applied to R/G/B of all four vertex colour words; code byte untouched.
    {
      int32_t delta = (int32_t)sharedByte30 - fogBase;
      if (delta < 0) delta = 0;
      static constexpr uint32_t kRgbWordOff[4] = {4, 16, 28, 40}; // pool-relative rgb0..rgb3
      for (uint32_t off : kRgbWordOff) {
        uint32_t addr = (uint32_t)pool + off;
        uint8_t r = c->mem_r8(addr + 0), g = c->mem_r8(addr + 1), b = c->mem_r8(addr + 2);
        c->mem_w8(addr + 0, fogShade(r, delta));
        c->mem_w8(addr + 1, fogShade(g, delta));
        c->mem_w8(addr + 2, fogShade(b, delta));
      }
    }

    // Per-node clut bias added to the low byte of every uv word (u0, u1, u2, u3).
    {
      static constexpr uint32_t kUvWordOff[4] = {12, 24, 36, 48};
      for (uint32_t off : kUvWordOff) {
        uint32_t addr = (uint32_t)pool + off;
        c->mem_w8(addr, (uint8_t)(c->mem_r8(addr) + (uint32_t)clutBiasLow));
      }
    }

    // Commit: link the packet into its OT bucket, advance the pool cursor. gen holds the packet-tag
    // constant (len=12 data words = 0x0C<<24) live in r3/v1; it survives as the leaf's v1 return
    // residue, so mirror it in c->r[3] rather than an inline literal.
    {
      uint32_t otBase = c->mem_r32(kOtBasePtrPtr);
      uint32_t idx = c->mem_r32(zScratch);
      uint32_t slotAddr = otBase + idx * 4;
      uint32_t oldHead = c->mem_r32(slotAddr);
      c->r[3] = kPktTag;                                 // v1 = tag constant (gen leaves this live)
      c->mem_w32((uint32_t)pool + 0, oldHead | c->r[3]); // tag | old head
      c->mem_w32(slotAddr, (uint32_t)pool);
      // #67/#66 display-pass capture: this GT4's picture from state — model corners (the staged
      // stack scratch, already delta×256 ints), the composed GTE transform factored against the
      // camera (render.h WqRec banner), and the FINAL packet material (post fog-shade + clut-bias:
      // per-vertex colors +4/16/28/40, uv words +12/24/36/48). These prop quads (drum/windmill caps
      // etc.) previously had NO pc_render picture at all — the guest packets were their only output.
      if (!c->game->oracle) {
        Render::WqRec w;
        w.node = (uint32_t)node;
        w.seq = 0;
        for (const Render::WqRec& p : rend(c)->mWqRecs) if (p.node == w.node) w.seq++;
        static constexpr uint32_t kVX[4] = { kVtxScratch_X0, kVtxScratch_X1, kVtxScratch_X2, kVtxScratch_X3 };
        static constexpr uint32_t kVY[4] = { kVtxScratch_Y0, kVtxScratch_Y1, kVtxScratch_Y2, kVtxScratch_Y3 };
        static constexpr uint32_t kVZ[4] = { kVtxScratch_Z0, kVtxScratch_Z1, kVtxScratch_Z2, kVtxScratch_Z3 };
        for (int i = 0; i < 4; i++) {
          w.vx[i] = c->mem_r16s(sp + kVX[i]);
          w.vy[i] = c->mem_r16s(sp + kVY[i]);
          w.vz[i] = c->mem_r16s(sp + kVZ[i]);
        }
        { constexpr float FX = 1.0f / 4096.0f;
          float crF[3][3], tr[3];
          uint32_t g0 = gte_read_ctrl(0), g1 = gte_read_ctrl(1), g2 = gte_read_ctrl(2),
                   g3 = gte_read_ctrl(3), g4 = gte_read_ctrl(4);
          crF[0][0] = (int16_t)g0 * FX;         crF[0][1] = (int16_t)(g0 >> 16) * FX; crF[0][2] = (int16_t)g1 * FX;
          crF[1][0] = (int16_t)(g1 >> 16) * FX; crF[1][1] = (int16_t)g2 * FX;         crF[1][2] = (int16_t)(g2 >> 16) * FX;
          crF[2][0] = (int16_t)g3 * FX;         crF[2][1] = (int16_t)(g3 >> 16) * FX; crF[2][2] = (int16_t)g4 * FX;
          for (int i = 0; i < 3; i++) tr[i] = (float)(int32_t)gte_read_ctrl(5u + (unsigned)i);
          wq_factor_world(c, crF, tr, w.objR, w.objT);
        }
        static constexpr uint32_t kColOff[4] = { 4, 16, 28, 40 };
        for (int i = 0; i < 4; i++) w.wCol[i] = c->mem_r32((uint32_t)pool + kColOff[i]);
        w.wUv0 = c->mem_r32((uint32_t)pool + 12); w.wUv1 = c->mem_r32((uint32_t)pool + 24);
        w.wUv2 = c->mem_r32((uint32_t)pool + 36); w.wUv3 = c->mem_r32((uint32_t)pool + 48);
        rend(c)->mWqRecs.push_back(w);
      }
      pool = (uint32_t)pool + 52;
      poolBase += 52;
    }

  skipRecord:
    recAddr += 36;
    if (!more) break;
  }

  // Final pool-cursor store. gen computes the store address as (kPktPoolBase - 2748) and leaves
  // kPktPoolBase live in r2/v0 — reproduce that so the leaf's v0 return residue byte-matches.
  c->r[2] = kPktPoolBase;
  c->mem_w32((uint32_t)c->r[2] - 2748u, (uint32_t)pool);   // == kPktPoolPtr (0x800BF544)
}

void WidescreenMarginQuad::registerOverrides(Game*) {
  extern void ov_a00_gen_8013CDD4(Core*);
  extern void engine_set_override_a00(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_a00(0x8013CDD4u, &WidescreenMarginQuad::emit, ov_a00_gen_8013CDD4);
}
