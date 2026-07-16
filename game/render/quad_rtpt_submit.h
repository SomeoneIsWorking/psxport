// game/render/quad_rtpt_submit.h — PC-native bodies for the two shared "GTE quad submit" leaves
// in the 0x8003xxxx render-submit band: FUN_8003B054 (quad-corner UV/vertex-index ROTATE) and
// FUN_8003B320 (RTPT+RTPS project-and-OT-link the quad the rotate helper built).
//
// RE'd from generated/shard_3.c (FUN_8003B054, gen_func_8003B054) and generated/shard_6.c
// (FUN_8003B320, gen_func_8003B320) — the recompiler's per-instruction translation, which is
// ground truth for GTE ops per CLAUDE.md (Ghidra's COP2 decompile of these two garbles the GTE
// register indices into synthetic setCopReg/copFunction "bus" pseudo-calls; the shard C does not).
//
// WIRED (frontier, 2026-07-08): registerOverrides() below installs both leaves into the
// recompiler's OWN process-global override table via `shard_set_override` (g_override[] —
// generated/shard_disp.c). Callers of both leaves are direct C calls the recompiler generates
// (`func_8003B054(c)`/`func_8003B320(c)`, always routed through that table), so this ONE
// registration covers every call site across shards uniformly — same discipline as
// game/render/overlay_gt3gt4.cpp's ov_a00_set_override wiring, and (like that cluster) this is
// the "faithful substrate mirror" carve-out: the process-global table is shared by BOTH SBS
// cores, so both run the SAME native translation (byte/op-exact transcription verified against
// generated/ ground truth, not a port DECISION differentially validated core-vs-core).
#pragma once
struct Core;
class  Game;

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
  // written but pointer never advanced) on: RTPT/RTPS GTE FLAG error, off-screen (NONE of the 4
  // corners' SX is inside [0,320), or NONE is inside [0,240) — unsigned 16-bit ANY-corner-in-range
  // tests, same OR convention as the sibling OverlayGt3Gt4/OverlayGroundGt3Gt4 leaves; a prior
  // draft of this comment + the .cpp both mis-stated this as an ALL-4-corners AND test — fixed
  // 2026-07-08 against generated/shard_6.c gen_func_8003B320, which jumps to "keep" the instant
  // any one corner is in range), or an out-of-range OT bucket. Also calls NCLIP between RTPT and
  // the SXY3 project (see .cpp) — a REAL executed GTE op whose only output is provably clobbered
  // before ever being read, reproduced anyway for full op-exact fidelity (a prior draft comment
  // claimed "no NCLIP/backface test here"; the call exists, it's just a no-op here). Real 16-byte
  // guest stack frame (RE: `addiu sp,-16`, pure scratch, no saved registers) MIRRORED per
  // CLAUDE.md — see .cpp.
  static void submitQuad(Core* c);          // FUN_8003B320: a0=out, a1=composedXform(6 words), a2=otzBias

  // Wire both leaf addresses into the recompiler's OWN process-global override table
  // (shard_set_override -> g_override[], generated/shard_disp.c) via overrides::install
  // (engine_set_override_main, runtime/recomp/override_registry.h). Both leaves are reached by a
  // direct C call the recompiler generates (`func_8003B054(c)`/`func_8003B320(c)`), never
  // rec_dispatch, so this ONE registration (called once per Game) covers every call site,
  // including both SBS cores.
  static void registerOverrides(Game* game);
};
