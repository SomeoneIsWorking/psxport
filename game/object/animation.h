// class Animation — PC-native per-object ANIMATION-VM subsystem owned by Engine.
//
// PROPER OOP: one instance per Core, reached as `c->engine.animation.step(node)`. Back-pointer
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
};
