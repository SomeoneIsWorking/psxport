// class ObjectTable — the PC-native 40-slot FIXED OBJECT TABLE dispatcher.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::objectTable`. Back-pointer wired
// once by Core's constructor. Callers reach the dispatcher through the object graph:
//
//     eng(c).objectTable.dispatch();
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as
// ObjectList / Array8Dispatch / TransitionState3 / SceneTransition.
//
// Owns guest FUN_80026C88: iterates the 40-slot fixed OBJECT TABLE at 0x800EC188 (stride 64);
// for each ACTIVE slot (byte[0] != 0), dispatches its handler via the fn-ptr table at
// 0x800AD52C indexed by byte[1] (a0 = slot). Faithful to the recomp body — the callee handlers
// stay substrate (each honors its own owned override in the super-call path).
//
// pc_skip=true (default ./run.sh): dispatch() runs the shortcut body below, with the
// `disp26c88verify` A/B gate on that channel (every dispatch compared byte-for-byte against
// rec_super_call(0x80026C88), with a scoped exclusion for the dead stack region below entry sp).
// pc_skip=false (SBS core A / pc_faithful): dispatch() forks to dispatchFaithful() via MV_CHECK
// (strict mirror TDD gate, game/core/verify_harness.h), a byte-exact mirror of gen_func_80026C88
// including the guest frame descent + s0/s1/s2/ra stack spill and jal-site r31 — those 16 bytes
// of guest stack are observable under SBS strict compare and the disp26c88verify dead-stack
// exclusion does NOT apply to this path. dispatchFaithful() is yield-free (its sole native
// handler, handler27254, and the LEAF_POOL_RETURN leaf it may call, never scheduler_yield), so
// MV_CHECK is safe to arm here.
#pragma once
#include <cstdint>
class Core;

class ObjectTable {
public:
  Core* core = nullptr;

  static constexpr uint32_t TABLE_BASE     = 0x800EC188u;  // slot 0 base
  static constexpr uint32_t SLOT_STRIDE    = 64u;          // 40 × 64B slots
  static constexpr int      SLOT_COUNT     = 40;
  static constexpr uint32_t HANDLER_TABLE  = 0x8009D52Cu;  // byte[1] -> u32 handler
                                                            // (was 0x800AD52C — off-by-one hex
                                                            // digit; disas: `addiu s2, v0, -10964`
                                                            // with v0=0x800A0000 → 0x8009D52C. In
                                                            // seaside all slots have obj[0]==0 so
                                                            // the bad ptr was never dereferenced,
                                                            // but any cutscene using this pool
                                                            // would have crashed.)

  // dispatch: one call = one sweep of the 40-slot table (was ov_disp_26c88 / FUN_80026C88).
  void dispatch();

private:
  // dispatchFaithful — faithful mirror of gen_func_80026C88 (generated/shard_2.c:1607): guest
  // frame descent (sp-=32), s0/s1/s2/ra spill at [sp+16/20/24/28] with LIVE register values, the
  // jal-site r31=0x80026CE0 set immediately before every active-slot handler dispatch, full
  // epilogue restore. Reference-mirror style per Engine::fieldFrameFaithful (game/core/engine.cpp).
  void dispatchFaithful();
  // FUN_80027254 — the leaf-particle slot handler (the only handler the table holds today).
  void handler27254(uint32_t obj);
};
