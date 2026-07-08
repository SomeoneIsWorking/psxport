// game/render/quad_rtpt_submit.h — PC-native DRAFT bodies for the two shared "GTE quad submit"
// leaves in the 0x8003xxxx render-submit band: FUN_8003B054 (quad-corner UV/vertex-index ROTATE)
// and FUN_8003B320 (RTPT+RTPS project-and-OT-link the quad the rotate helper built).
//
// STATUS: RE'd from generated/shard_3.c (FUN_8003B054, gen_func_8003B054) and generated/shard_6.c
// (FUN_8003B320, gen_func_8003B320) — the recompiler's per-instruction translation, which is
// ground truth for GTE ops per CLAUDE.md (Ghidra's COP2 decompile of these two garbles the GTE
// register indices into synthetic setCopReg/copFunction "bus" pseudo-calls; the shard C does not).
// UNWIRED / UNVERIFIED: no EngineOverrides registration, no shard_set_override, no SBS run. This
// is fleet-agent RE-ahead-of-frontier work (see the dispatching prompt) — compile-only draft.
//
// See quad_rtpt_submit.cpp for the full per-field trace. Callers of both leaves are OUTSIDE this
// 0x80030000-0x8003BFFF band (the >=0x8003C000 per-object render family, already owned elsewhere —
// see game/render/perobj_dispatch.cpp / engine_submit.cpp), so neither leaf is drafted with a
// caller-side wiring plan yet; that is future work once the caller side is RE'd too.
#pragma once
struct Core;

class QuadRtptSubmit {
public:
  // FUN_8003B054(dst=a0, src=a1, cornerIndex=a2): rotates the 4 (u16 or s8-pair) corner fields of
  // a quad-UV/vertex-index record from `src` into `dst`, choosing which physical corner of `src`
  // maps to which logical slot of `dst` based on cornerIndex (0..3 — the quad's "which edge is up"
  // orientation, e.g. a rope/flame segment whose facing rotates as it sways). Pure data movement +
  // a few "-1" bias adjustments on two of the four rotations; NO GTE, NO memory outside dst/src.
  static void rotateQuadCorners(Core* c);   // FUN_8003B054: a0=dst, a1=src, a2=cornerIndex(0..3)

  // FUN_8003B320(out=a0, composedXform=a1, otzBias=a2): the shared "project a quad through an
  // already-composed transform and link it into the OT" leaf used by the rope/flame per-quad
  // renderer (see game/render/submit.cpp's "0x8003B320 into 0x800C0xxx" comment). `composedXform`
  // is 6 packed words = the GTE VXY0/VZ0/VXY1/VZ1/VXY2/VZ2 inputs for the first 3 corners (already
  // MTC2-ready — the caller, not this leaf, is what composes the object's R/T into a projectable
  // vertex set; that caller is un-RE'd, outside this band). `out` is a PRE-BUILT 10-word packet
  // record (colour/uv already filled in by the caller at +4/+12/+20/+28/+36; this leaf fills the
  // 4 SXY slots at +8/+16/+24/+32 via RTPT+RTPS) that gets bump-copied whole into the packet pool.
  // Returns nothing (v0 unused by every known caller); drops the quad silently (packet-pool bytes
  // written but pointer never advanced) on: RTPT/RTPS GTE FLAG error, off-screen (all 4 corners
  // outside [0,320)x[0,240) — an UNSIGNED compare, so this is a faithful port of the recomp's own
  // 4:3-only frustum test, NOT gpu_gpu_wide_engine-aware; a future wire-up should decide whether to
  // widen it the way submit.cpp's submit_xmax() does), or an out-of-range OT bucket.
  static void submitQuad(Core* c);          // FUN_8003B320: a0=out, a1=composedXform(6 words), a2=otzBias
};
