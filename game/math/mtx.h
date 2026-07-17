// class Mtx — PC-native libgte matrix helpers (tiny leaves the game calls all over).
//
// PROPER OOP: instance subsystem, one per Core (embedded as `Core::mtx`). Back-pointer wired in
// Core::Core(). Callers reach the methods as `mtxOf(c).identity(addr)` — no Core* on the surface;
// the class reads/writes guest memory via `this->core`. Was previously `Mtx::identity(Core*, addr)`
// (static-with-Core); promoted to match the standard subsystem shape (same as Rng / Trig / Math).
//
// Each method here corresponds ONE-TO-ONE to a resident libgte leaf that the game (and the
// substrate) still calls. Owning them native folds the substrate hops those calls used to be.
// Layout is the standard PSX libgte MATRIX: `short m[3][3]` (9 int16 = 18 bytes) + 2 bytes pad
// + `long t[3]` (3 int32 = 12 bytes) = 32 bytes total.
#pragma once
#include <cstdint>
class Core;

class Mtx {
public:
  Core* core = nullptr;

  // MR_init (guest FUN_80051794): write identity (diag 0x1000 in 4.12 fixed, all else zero) at
  //   `addr`. 8 word stores, faithful to the guest.
  void identity(uint32_t addr);

  // NOTE: the diagonal-scale matrix write (guest FUN_800517BC) is owned by
  // `NodeXform::seedBlock` (game/render/node_xform.h) — that is the wired dispatch target for
  // this address. A duplicate `Mtx::diagonal` (dead code, zero callers) lived here until
  // 2026-07-08's dual-ownership dedup; do not re-add it — call `rend(c)->mNodeXform.seedBlock(...)`
  // instead.
};
