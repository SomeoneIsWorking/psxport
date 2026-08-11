// dualcore.h — class DualCore — the NATIVE-render vs PSX-render guest-RAM divergence harness
// (PSXPORT_DUALCORE=1). Implemented in dualcore.cpp; one-shot diagnostic run from boot.cpp. Runs
// the SAME native-gameplay game twice (native render vs PSX render) and diffs guest RAM per frame
// to pin the first render-side corruption of gameplay-read state.
#pragma once
#include <cstdint>
#include "render_noise.h"   // RenderNoiseMask — the GameConfig-derived render-noise windows
struct Core;

class DualCore {
public:
  void run(const char* exe_path);

private:
  // 3-phase nav machine, IDENTICAL to native_boot AUTO_SKIP: tap Cross to the GAME stage, wait
  // for the intro cutscene, pulse Start until the cutscene-end fade settles.
  enum Phase { REACH_GAME, AWAIT_CUT, SKIP_CUT, DONE };
  struct Nav { Phase phase = REACH_GAME; int idle = 0; };

  int show_all = 0;   // PSXPORT_DC_ALL: include render-only regions in the report

  // Nav predicate, from GameConfig (was Tomba!2's 0x801fe00c / 0x8010637C hardcoded in dualcore.cpp).
  // run() refuses to start unless both are known: with 0 the REACH_GAME test reads
  // `mem_r32(0xc) == 0`, which is TRUE on frame 0, so the harness would record the BIOS boot as
  // gameplay and then report a clean bill of health over it.
  uint32_t mStageEntryAddr = 0;   // cfg->taskTableBase + the slot's stage-entry offset
  uint32_t mStageGame      = 0;   // cfg->stageGame
  RenderNoiseMask mMask;          // render-path noise excluded from the diff (empty when un-RE'd)

  bool navStep(Core* c, Nav& nv, uint32_t f, const char* tag);
  void scriptedInput(Core* c, int k);
  int  runAndRecord(const char* exe, int render_psx, const char* tag,
                    int n, uint32_t lo, uint32_t hi, uint8_t** snaps, uint8_t** spads);
  bool isRenderRegion(uint32_t a) const;
  void diffFrameRegion(const char* name, const uint8_t* a, const uint8_t* b, uint32_t n, uint32_t gbase);
};
