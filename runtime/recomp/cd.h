// cd.h — class Cd — the native CD subsystem, owned by Game (`c->game->cd`, back-pointer wired in
// Game()). Implemented in cd_override.cpp: the synchronous native read primitives the PC-native
// loaders call directly, the CD-subsystem PlatformHle registrations (libcd command/sync/read
// leaves), and the deferred ingame-music state (a looping clip suppressed during a dialog is
// remembered here and resumed by MusicCoord::tick).
#pragma once
#include <cstdint>
class Game;

class Cd {
public:
  Game* game = nullptr;
  // deferred ingame-music state (suppressed during dialog, resumed after)
  int      pending_music = 0;   // a looping ingame-music clip is deferred/remembered (was s_pending_music)
  uint8_t  pm_chan  = 0;        // was s_pm_chan
  uint32_t pm_start = 0;        // was s_pm_start
  uint32_t pm_end   = 0;        // was s_pm_end
  int      verbose  = 0;        // [cd] read/loadfile trace (was s_cd_verbose)

  // loadFile(dest, lba, size): direct-call native loadfile (0x8001DB8C semantics) — used by the
  //   PC-native boot/stage path, which owns the START.BIN / stage-overlay load top-down.
  void loadFile(uint32_t dest, uint32_t lba, uint32_t size);
  // asyncRead(): 0x8001D940 FUN_8001d940, the engine's async streaming reader, done natively &
  //   synchronously from the scratchpad read descriptor (0x1f8001f0/f4/f8).
  void asyncRead();
  // dc40Sync(dest, lba, size): direct-call native FUN_8001DC40 — fill the scratchpad read
  //   descriptor and run the synchronous asyncRead; returns size in guest V0.
  void dc40Sync(uint32_t dest, uint32_t lba, uint32_t size);
  // toSpuMix(on): enable/disable CD->SPU mixing (libsnd SpuSetCommonAttr via FUN_8001cf00(1));
  //   needed for the SPU to actually mix the decoded XA (Beetle spu.c gates on SPUControl bit0).
  //   Also called from MusicCoord::tick() on the dialog-end resume path.
  void toSpuMix(int on);
  // audioTrace(tag): diagnostic — trace the game's CD-volume fade state + XA stream lifecycle,
  //   on change only (PSXPORT_XA_DBG).
  void audioTrace(const char* tag);
  // hleInit(): native HLE CdInit — leave RAM in the state FUN_800898a0's SUCCESS path leaves it
  //   (CD-event callback table installed); no controller handshake, no busy-wait.
  void hleInit();
  // overridesInit(): register every CD-subsystem PlatformHle handler with this Game's table.
  void overridesInit();
};
