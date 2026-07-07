// dualcore.h — class DualCore — the NATIVE-render vs PSX-render guest-RAM divergence harness
// (PSXPORT_DUALCORE=1). Implemented in dualcore.cpp; one-shot diagnostic run from boot.cpp. Runs
// the SAME native-gameplay game twice (native render vs PSX render) and diffs guest RAM per frame
// to pin the first render-side corruption of gameplay-read state.
#pragma once
#include <cstdint>
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

  bool navStep(Core* c, Nav& nv, uint32_t f, const char* tag);
  void scriptedInput(Core* c, int k);
  int  runAndRecord(const char* exe, int render_psx, const char* tag,
                    int n, uint32_t lo, uint32_t hi, uint8_t** snaps, uint8_t** spads);
  static bool isRenderRegion(uint32_t a);
  void diffFrameRegion(const char* name, const uint8_t* a, const uint8_t* b, uint32_t n, uint32_t gbase);
};
