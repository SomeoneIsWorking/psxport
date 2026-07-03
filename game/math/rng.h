// class Rng — the PC-native LFSR pseudo-random number generator (guest FUN_8009A450).
//
// PROPER OOP: one instance per Core (embedded as `Core::rng`), back-pointer to Core wired in
// Core::Core(). Callers use it as `c->rng.next()`. No `extern "C"` shim.
//
// The seed lives in guest memory at 0x80105EE8 (MAIN.EXE .bss, same location the substrate uses)
// so the RNG stream is SHARED with any still-substrate caller — no divergence between the two
// sides. This is deliberate: 0x8009A450 is called from many substrate leaves, and the seed's
// address is a hard ABI point (multiple retained fns literally lw/sw against 0x80105EE8).
//
// Algorithm: classic PSX libc rand() — LCG constants 0x41C64E6D, 12345 (0x3039).
#pragma once
#include <cstdint>
class Core;

class Rng {
public:
  static constexpr uint32_t SEED_ADDR = 0x80105EE8u;
  Core* core = nullptr;

  // Advance the LFSR and return the classic PSX rand() value in [0, 0x7FFF].
  int32_t next();

  // inRange(lo, hi): FUN_80032A44 — signed random integer in [lo, hi). Uses next() then scales
  //   ((next * (hi - lo)) >> 15) + lo — the ~15-bit space fills [0, hi - lo). Matches recomp
  //   verbatim: PSX MULT/MFLO on 32-bit signed operands + `sra 15`. Body from disas 0x80032A44.
  int32_t inRange(int32_t lo, int32_t hi);

  // Slip #5 helper (docs/findings/sbs.md): every native replacement of a code path that on the
  // recomp side ran `rec_dispatch(c, 0x80044BD4u)` must call this to keep the shared PSX rand
  // seed at 0x80105EE8 tick-parity between native (A) and recomp (B) under SBS. FUN_80044BD4
  // (task spawn) calls FUN_8009A450 once per invocation; native replacements (Sop::transition
  // AreaLoad, native_area_load_bd4, Engine::startBinStage, stage0Advance case 1, Demo::stageMain
  // — search for `matchBd4Cadence` callers) all skip the spawn and therefore skip the RNG
  // advance. Live gameplay (SBS off) does nothing here — cadence parity is only a divergence-
  // gate concern.
  void matchBd4Cadence();
};
