// game/scene/mode_state_arm.h — PC-native MODE-STATE ARM primitive pair.
//
// PROPER OOP: one instance per Core, embedded on Engine, reached as
// `eng(c).modeStateArm.method(args)`. Back-pointer wired at Core construction (same pattern
// as SceneEvents / Sfx).
//
// SCOPE: the twin mode-state arm writes — the direct 3-byte payload arm (FUN_8005082C) and the
// per-area-table variant (FUN_800508A8) — over the mirrored triples at 0x800EA0D4.. and
// 0x800EC144.. plus the backup/stamp bytes at 0x800BF8A4..A7.
#pragma once
#include <cstdint>
struct Core;

class ModeStateArm {
public:
  Core* core = nullptr;

  // arm(a, b, c): guest FUN_8005082C. The engine's MODE-STATE ARM primitive — writes a
  //   3-byte payload (a, b, c) into two mirrored triples at 0x800EA0D5..D7 and 0x800EC145..147,
  //   backs the previous payload up at 0x800BF8A4..A6, sets the twin arm flags (0x800EA0D4 = 1,
  //   0x800EC144 = 1), and stamps 0x800BF8A7 = (previous_arm_flag << 7) | 1. Widely called with
  //   (0, 0, 0) to "arm the null mode" (input/pad reset shape used by scene_ui trigger, ScreenFade
  //   sequencer, demo.cpp prologue, bf816_dispatch, pool.finalViewInit's fall-through).
  //   Ghidra decomp scratch/decomp/bf816_leaves.c.
  void arm(uint8_t a = 0, uint8_t b = 0, uint8_t c = 0);

  // armFromAreaTable(): guest FUN_800508A8. The area-table variant of arm —
  //   pulls the 4-byte (class, a, b, c) payload from the per-area lookup table at 0x800A5500,
  //   indexed by `area * 8 + collected_bit * 4` (collected_bit = 0x800BFE56 >> area & 1), then
  //   applies the same arm-flag stamp pattern, with 0x800BF8A7 = 0x81 when class == 1 else 2.
  //   Called by pool.finalViewInit (0x800508A8 hop) and bf816_dispatch. No caller args.
  void armFromAreaTable();
};
