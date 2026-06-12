// Tomba! 2 (SCUS-94454) game module. RE notes live in patches/tomba2/.

#include "tomba2.h"

#include "../psxport_hooks.h"

#include <cstring>

void Tomba2_Install()
{
  // overrides registered here as they are RE'd
}

uint16_t Tomba2_FrameTick(uint8_t* ram)
{
  (void)ram;
  return 0;
}

bool Tomba2_WantTurbo(const uint8_t* ram, unsigned frame)
{
  // The license text and Whoopee Camp logo are LOAD MASKS, not skippable
  // segments (verified: X has no effect; the segment-end code is absent from
  // RAM until the loader finishes, and the logo then runs out its jingle).
  // Turbo instead, while either holds:
  //  - the main game overlay is not yet resident (0x8005082C empty during
  //    BIOS/license/early logo, game code from ~frame 1989), or
  //  - the logo stream clock at 0x8011824C is still ticking (it advances
  //    every frame during the logo segment and freezes at its end; verified
  //    frozen during the in-engine intro cutscene).
  // Frame cap as a safety net against later overlay swaps / streams.
  if (frame > 6000)
    return false;
  uint32_t overlay, clock;
  memcpy(&overlay, ram + 0x5082C, 4);
  memcpy(&clock, ram + 0x11824C, 4);
  static uint32_t last_clock = 0;
  const bool ticking = (clock != last_clock);
  last_clock = clock;
  return overlay == 0 || ticking;
}
