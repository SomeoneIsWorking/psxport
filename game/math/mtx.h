// class Mtx — PC-native libgte matrix helpers (tiny leaves the game calls all over).
//
// PROPER OOP: stateless static methods (a helper namespace as a class). No Core member — the
// operations are pure functions over guest memory, given a Core* explicitly.
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
  // MR_init (guest FUN_80051794): write identity (diag 0x1000 in 4.12 fixed, all else zero) at
  //   `addr`. 8 word stores, faithful to the guest.
  static void identity(Core* c, uint32_t addr);
};
