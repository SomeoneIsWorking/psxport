// class Animation — PC-native per-object ANIMATION-VM subsystem owned by Engine.
//
// PROPER OOP: one instance per Core, reached as `eng(c).animation.step(node)`. Back-pointer
// `core` wired once at Core construction time (same pattern as GraphicsBind / Font).
//
// SCOPE: the per-object animation-sequence VM stepper (guest FUN_80076D68). Given a node
// address, advances its animation state one frame (evaluates the opcode stream at
// node+anim_ptr, applies the current key-frame via FUN_80075F0C, loads the next frame via
// FUN_80076904, etc.). Called every frame from several per-object behavior handlers
// (beh_actor_move_sm, beh_flagbit_timer_machine, beh_id_compare_motion_dispatch).
//
// Was the free function `ov_anim_vm_76d68` in animation.cpp, taking the node via MIPS taxi
// parameter c->r[4]. Now a real instance method with an explicit uint32_t node argument.
#pragma once
#include <stdint.h>
class Core;

class Animation {
public:
  Core* core = nullptr;

  // step(node): advance node's animation-VM one frame. Returns v0 via c->r[2] (some callers
  // read the returned "keep-going" flag). Retains the `animvm` A/B verify gate against the
  // recomp super-call at 0x80076D68 for regression checking.
  void step(uint32_t node);

  // stepFramed(node): step()'s guest-frame-mirrored body — descends FUN_80076D68's real 40-byte
  // stack frame (spilling the LIVE incoming r16/r17/r18/r19/ra), runs the VM, then ascends. The
  // frame mirror itself is correct and RE'd from generated/shard_7.c, but NOT currently wired to
  // anything (see registerOverrides()'s comment / docs/findings/animation.md): wiring it exposed a
  // separate, PRE-EXISTING fidelity gap in anim_vm_76d68 for one pool-adjacent "node" address
  // (0x800E7E80), unrelated to the frame. Kept available (public) for whoever picks that up.
  void stepFramed(uint32_t node);

  // loadFrame(node): FUN_80076904 — POSE-TABLE FRAME LOADER. Given node's cursor (node+0x38,
  // an index-record pointer) and pose table base (node+0x3C), resolves the CURRENT keyframe
  // record (indexed by the u16 at cursor[0]) and unpacks its packed rotation/scale payload into
  // the node's per-limb sub-structures (array of struct* at node+192, stride 4). Called every
  // time the anim-VM (step()/FUN_80076D68) or attach() (FUN_80077C40) loads a new frame. Was
  // rec_dispatch(c, 0x80076904u); now native (see loadFrame's own header comment in
  // animation.cpp for the full field-packing RE).
  void loadFrame(uint32_t node);

  // advanceLinkChain(node): FUN_80077B5C. Decrements node's countdown (node+0xE); while running,
  // returns 0. On expiry, walks ONE step of a small 4-byte-stride tag/jump-pointer chain rooted
  // at node's cursor (node+0x38) — the same cursor field as loadFrame/step's keyframe stream, but
  // a distinct, coarser chain format reused by many non-animation behavior handlers as a generic
  // "tick this countdown, advance this event chain" primitive. Returns 0 (ADVANCE/FOLLOW, tag
  // 0/0x4000) or 1 (HOLD/FOLLOW-terminal, tag 0x8000/0xc000). Was rec_dispatch(c, 0x80077B5Cu) /
  // the `leaf1(c, nd, 0x80077B5Cu)` call-site idiom in ~10 beh_ handlers.
  uint32_t advanceLinkChain(uint32_t node);

  // attach(node, table, id): FUN_80077C40. Installs animation-table entry `id` (an array of
  // struct* at `table`, stride 4) onto `node`: sets the cursor (node+0x38) to the entry pointer,
  // seeds the countdown (node+0xE) from the entry's descriptor low-12 bits, calls loadFrame(node),
  // then — if the descriptor's 0x2000 "execute" bit is set — dispatches the frame executor
  // (FUN_80075ff8, kept reachable by address; same call shape as step()'s exec_tail) exactly like
  // the anim-VM would for this entry's tag. Was rec_dispatch(c, 0x80077C40u) / the `call3(c, obj,
  // table, id, 0x80077C40u)` idiom.
  void attach(uint32_t node, uint32_t table, uint32_t id);

  // applyFrame(node, snapCursor): FUN_80075F0C — per-frame KEYFRAME APPLIER. Integrates node's
  // base position (node+0x88/0x8a/0x8c) by its per-frame delta (node+0x90/0x92/0x94), then — if
  // node+9 (child count) is nonzero — walks the child-limb pointer array at node+0xC0 (up to
  // node+8 entries) integrating each child's position (child+8/0xa/0xc) by its own delta
  // (child+0x10/0x12/0x14). If `snapCursor` == 1, ORs the KSEG1 tag bit (0x80000000) into node's
  // cursor word (node+0x38) — the anim-VM's "just-applied-the-first-frame" marker (RE'd verbatim
  // from Ghidra: FUN_80075f0c). Called from step()'s DELAY branch with snapCursor = the
  // post-decrement counter (only ever compared against 1 by the guest).
  void applyFrame(uint32_t node, int32_t snapCursor);

  // registerOverrides(): wires FUN_80076904 / FUN_80077B5C / FUN_80077C40 / FUN_80075F0C into
  // the override registry (overrides::install) at their guest addresses, so every existing
  // rec_dispatch call site (native
  // beh_ handlers AND any substrate-internal caller) reaches these native methods uniformly.
  // FUN_80075F0C (applyFrame) is ALSO dual-wired via shard_set_override (see .cpp) because the
  // substrate reaches it through direct `func_<addr>(c)` call sites, not just rec_dispatch.
  // FUN_80076D68 (step) is DELIBERATELY NOT wired here (investigated + reverted 2026-07-08 — see
  // the .cpp comment above gov_animApplyFrame / docs/findings/animation.md): the frame mirror
  // (stepFramed()) is correct, but wiring surfaces an unrelated, pre-existing anim_vm_76d68
  // fidelity gap for node address 0x800E7E80 (pool-adjacent, not a normal animation node) that
  // needs its own RE session. Called once from boot.cpp.
  void registerOverrides();
};
