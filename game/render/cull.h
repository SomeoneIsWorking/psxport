// game/render/cull.h — PC-native visibility CULL / LOD subsystem.
//
// class Cull — instance subsystem owned by Engine. The engine OWNS the per-object visibility decision
// (per CLAUDE.md: render ordering / visibility is the engine's, with its own widescreen-aware margin;
// no PSX ±34° cone inheritance). Holds the per-object cull body (FUN_8007712C), its two camera-
// relative wrappers (FUN_8007778C / FUN_80077ACC), and the standalone view-cone cull (FUN_8002B278).
//
// Currently ORPHANED — the top-down PC path hasn't reached the cull site yet. The class shape is here
// so callers can hook in as `eng(c).cull.method()` once wired; the algorithmic body preserves the
// RE'd cull-decide + widescreen margin re-include logic.
#pragma once
#include <cstdint>
class Core;

class Cull {
public:
  Core* core = nullptr;

  // objectCull (FUN_8007712C): per-object cull body + widescreen margin re-include. Taxi-parameter
  // c->r[4] = object, c->r[5]/[6]/[7] = camera-relative dx/dz/dy (s16 each). Was ov_object_cull.
  void objectCull();

  // performBaseCull — the byte-exact PC-native reimplementation of FUN_8007712C's body ALONE (no
  // widescreen margin re-include). Called from Actor::boundsCull to replace the last rec_dispatch
  // in the bounds-cull chain — same taxi convention as objectCull (r[4]=obj, r[5]/[6]/[7]=dx/dy/dz),
  // same side effects (obj[+1] flag + per-class render-list push), same return in r[2]. Was the
  // file-scope `cull_native_body` helper — now the public entry point Actor::boundsCull dispatches to.
  void performBaseCull();

  // cullWrapper (FUN_8007778C): camera-relative delta + flag reset → cull body. Taxi c->r[4] = object.
  // Was ov_cull_wrapper.
  void cullWrapper();

  // cullWrap77acc (FUN_80077ACC): cull-wrapper variant, caller-supplied position in r5/r6/r7 (flags
  // 1/4 rather than 0/0). Was ov_cull_wrap_77acc. UNFRAMED — this is the public entry point EXISTING
  // native beh_ callers (beh_record_list_scanner.cpp, script_vm.cpp) already use as a plain C++ call,
  // not through the guest ABI; at those call sites c->r[29] is NOT a real guest sp belonging to this
  // call; see cullWrap77accFramed()'s comment for why framing this method itself is a bug.
  void cullWrap77acc();

  // cullWrap77accFramed — GUEST-ABI ENTRY ONLY, used by the shard_set_override trampoline
  // (gov_cullWrap77acc). Mirrors FUN_80077ACC's real `addiu sp,-24; sw ra,16(sp)` frame around
  // cullWrap77acc()'s body, for callers reached through the actual guest call graph (where
  // c->r[29] is a real guest sp). Do NOT call this from native C++ — see cullWrap77acc()'s comment
  // (bug found + fixed 2026-07-08: framing the shared public method broke its existing native
  // callers by pushing/popping a guest-stack frame against an unrelated, arbitrary sp).
  void cullWrap77accFramed();

  // coneCull2b278 (FUN_8002B278): standalone view-cone cull, sets node visible flag on keep. Taxi
  // c->r[4] = node. Was ov_cone_cull_2b278.
  void coneCull2b278();

  // cullWrapperFlag2 (FUN_800777FC): CULL WRAPPER variant of cullWrapper — same taxi shape (obj in
  // c->r[4], camera-relative delta computed from obj+0x2E/0x32/0x36), but writes MODE flag
  // *(u32)0x1F800084 = 2 (vs cullWrapper's 0 and cullWrap77acc's 4) before dispatching to the base
  // cull body. Body from disas 0x800777FC..0x8007786C. 3 callers in beh_id_compare_motion_dispatch.
  // UNFRAMED — see cullWrapperFlag2Framed()'s comment (same class of bug/fix as cullWrap77acc).
  void cullWrapperFlag2();

  // cullWrapperFlag2Framed — GUEST-ABI ENTRY ONLY, used by the shard_set_override trampoline
  // (gov_cullWrapperFlag2). Mirrors FUN_800777FC's real `addiu sp,-24; sw ra,16(sp)` frame around
  // cullWrapperFlag2()'s body. Do NOT call from native C++ — see cullWrapperFlag2()'s comment.
  void cullWrapperFlag2Framed();

  // enqueueQueueC (FUN_80077EFC): MANUAL push of `obj` onto queue C (0x1F800154/0x1F80015C, cap 28) —
  // sibling of enqueueQueueA/enqueueVisibleClass4. Same slti-N gate + push + counter bump shape.
  // Returns 0 on cap-hit, new 1-based count on success (matches recomp).
  uint32_t enqueueQueueC(uint32_t obj);

  // enqueueQueueA (FUN_80077E7C): MANUAL push of `obj` onto queue A (0x1F80013C/0x1F800144, cap 24) —
  // the render queue for object types 2/9 that cull_decide would auto-push. Six callers across
  // game/world/entity.cpp + game/ai/beh_* use this as an unconditional queue insert (early bail if
  // count >= 24; no visibility check). Body from disas 0x80077E7C.
  //
  // Returns v0 as the recomp does — on cap-hit v0=0 (the slti's false result); on success v0 =
  // old_counter + 1 (i.e. the NEW 1-based count). beh_jumptable_release_trigger uses this return.
  uint32_t enqueueQueueA(uint32_t obj);

  // enqueueByClass (FUN_8007703C): CLASS-KEYED queue dispatcher. Sets obj[+1] = 1 (visible marker)
  // unconditionally, then dispatches to the appropriate queue based on obj[+0xC]:
  //   * class 4      → queue B (cap 40, sibling of enqueueVisibleClass4)
  //   * class 2 or 9 → queue A (cap 24, sibling of enqueueQueueA)
  //   * class 5      → queue C (cap 28, sibling of enqueueQueueC)
  //   * other        → no-op (returns 0)
  // Returns v0 = new 1-based count on push, 0 on cap-hit or unknown class (matches recomp).
  uint32_t enqueueByClass(uint32_t obj);

  // enqueueVisibleClass4 (FUN_80077EBC): MANUAL push of `obj` onto render class 4's list — the same
  // list-add tail performBaseCull runs when the base cull KEEPS a class-4 object, but callable
  // directly by beh_ handlers whose scene-specific logic decides an object should render this frame
  // (bypassing the base cull test). Respects the cap-40 limit at *(0x1F800150). Callers set obj[+1]=1
  // themselves; this method only manipulates the queue. Was rec_dispatch(0x80077EBCu) in 5+ handlers.
  uint32_t enqueueVisibleClass4(uint32_t obj);   // returns v0 (new count on push, 0 on cap-hit)

  // cullWrapperFlag1 (FUN_80077870): CULL WRAPPER variant of cullWrapper — same taxi shape (obj in
  // c->r[4], camera-relative delta computed from obj+0x2E/0x32/0x36 vs cam@0x1F8000D2/D6/DA), but
  // writes MODE flag *(u32)0x1F800084 = 1 (vs cullWrapper's 0 and cullWrapperFlag2's 2) before
  // dispatching to the base cull body. FRAMED — shares the family's -24 frame, written INLINE (in the
  // gen store order) rather than via wrapFrame() so the frame+call are visible to port_check; the
  // inner cull still runs through performBaseCullFramed() (FUN_8007712C's nested -40 frame), so the
  // guest stack is byte-identical to cullWrapper. Reached only through the guest-ABI trampoline
  // (eov_cullWrapperFlag1) — the substrate reaches it by direct func_ call and beh_typed_variant_router
  // routes it by address, both through the override thunk, never as a plain native C++ call, so no
  // separate unframed public entry is needed.
  void cullWrapperFlag1();

  // cullWrapperOffset (FUN_800779D0): cull-wrapper variant — caller-supplied 3-component OFFSET
  // (a1/a2/a3) is ADDED to the object's own position (obj+0x2E/0x32/0x36) before the camera-
  // relative subtraction, flags 0/0 (same as cullWrapper). Taxi: c->r[4]=obj, r[5]/[6]/[7]=offset.
  void cullWrapperOffset();

  // cullWrapperOffsetFlag1 (FUN_80077A4C): same offset-add shape as cullWrapperOffset, but flags
  // 1/0 (matches cullWrap77acc's 0x1F800080=1, not its 0x1F800084=4). Taxi identical to
  // cullWrapperOffset.
  void cullWrapperOffsetFlag1();

  // cullWrapperOffsetY (FUN_800778E4): cull-wrapper variant — a SINGLE caller-supplied offset
  // (a1) is added ONLY to the object's Z-axis field (obj+0x32) before the camera-relative
  // subtraction; X (obj+0x2E) and Y (obx+0x36) use the raw object position. Flags 0/0. Taxi:
  // c->r[4]=obj, c->r[5]=offset.
  void cullWrapperOffsetY();

  // NOTE on wiring (RESOLVED 2026-07-08): this whole camera-relative-wrapper family (cullWrapper/
  // cullWrapperFlag2/cullWrap77acc/cullWrapperOffset/cullWrapperOffsetFlag1/cullWrapperOffsetY) is
  // now wired via shard_set_override (see cull.cpp registerOverrides()). Each substrate body
  // (FUN_8007778C etc.) pushes a REAL guest-stack frame (`addiu sp,-24` + `sw ra,16(sp)`); the
  // 2026-07-08 wiring attempt diverged at 0x801FE906 because the native methods didn't replicate
  // that frame. Fixed by mirroring it: wrapFrame() descends the frame, spills the LIVE incoming
  // ra to the RE'd offset, sets ra to the per-site guest jal-return constant, runs the cull body,
  // then restores ra from the stack and ascends — byte-exact to the generated prologue/epilogue.
  void registerOverrides();

private:
  // wrapFrame — shared frame mirror for the whole cullWrapper* family: every one of the 6
  // variants has the IDENTICAL shape (`addiu sp,-24; sw ra,16(sp); ...; jal 0x8007712C; lw
  // ra,16(sp); addiu sp,24`), differing only in the guest ra constant used for the internal jal.
  // RE'd instruction-exact from generated/shard_{0,1,2,4,5,7}.c (gen_func_8007778C/800777FC/
  // 80077ACC/800779D0/80077A4C/800778E4). Mirrors the descent + LIVE-ra spill + ascent around
  // performBaseCull() so the guest stack bytes byte-match the substrate (no SBS exclusion needed).
  void wrapFrame(uint32_t raConst);

  // performBaseCullFramed — mirrors FUN_8007712C's OWN real 40-byte guest-stack frame around
  // performBaseCull(). Used ONLY from wrapFrame() (the guest-ABI path); performBaseCull() itself
  // stays unframed for its other native callers. See cull.cpp for the full RE + bug history.
  void performBaseCullFramed();

  // Pure (read-only) cull decision — reproduces FUN_8007712c's control flow without committing
  // writes. queue: 0=none, 1=A, 2=B, 3=C.
  struct Decision { int kept; int wrote_state2; int queue; };
  Decision decide();

  // coneCull2b278's body; commit!=0 also sets the node visible flag on keep.
  int coneCullBody(int commit);

  // Far-limit multiplier fork (issue #22 / Slip #3-shape, docs/findings/sbs.md). Faithful keeps the
  // stock per-state far limits (×1) so SBS against recomp_path doesn't diverge at 0x800EE489; skip
  // applies the extended multiplier (PSXPORT_CULL_FAR_MULT override, else CULL_FAR_MULT). The two
  // modes deliberately do not converge.
  int cullFarMult();
  int cullFarMultFaithful();
  int cullFarMultSkip();

  // Lazily-initialized cfg caches (-1/-2 = not read yet). Per-instance so the two SBS cores don't
  // share diagnostic state.
  int mObjLog = -1;                              // PSXPORT_DEBUG=obj per-object log
  int mCullEnvRead = -1, mCullFar = 0, mCullFov = 0;   // PSXPORT_CULL_FAR / PSXPORT_CULL_FOV
  int mFarMult = -1;                             // skip-mode far multiplier
  int mOnlyType = -2, mSkipType = -2;            // PSXPORT_CULL_ONLY_TYPE / PSXPORT_CULL_SKIP_TYPE
};
