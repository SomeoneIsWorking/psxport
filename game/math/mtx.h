// class Mtx — PC-native libgte matrix helpers (tiny leaves the game calls all over).
//
// PROPER OOP: instance subsystem, one per Core (embedded as `Core::mtx`). Back-pointer wired in
// Core::Core(). Callers reach the methods as `c->mtx.identity(addr)` — no Core* on the surface;
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

  // Diagonal-scale matrix (guest FUN_800517BC): write a 3x3 with m[0][0]=x, m[1][1]=y, m[2][2]=z
  //   (sign-extended s16), all off-diagonals and the translation zeroed. Same 32-byte layout as
  //   identity(). Faithful to the guest.
  void diagonal(uint32_t addr, int32_t x, int32_t y, int32_t z);
};
