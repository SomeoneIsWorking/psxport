// class SpuAudio — the native audio OUTPUT sink for one Game.
//
// Drives the shared SPU mix (spu_beetle.c) into a host SDL3 audio stream: init() opens the device
// once per Game; frame() advances the SPU exactly one NTSC frame of clocks, drains the produced
// samples, mixes native music, and queues them to the host device (+ optional WAV capture). No
// PSX SPU hardware is presented — we only consume the already-mixed PCM the SPU produced.
//
// Owned by `class Game` (`c->game->spu_audio.method()`); the back-pointer is wired in Game(). One
// per Game instance. In SBS with two Games only one Game's SDL device init will succeed (the host
// device is a single OS resource) and the other's mState stays at -1 (disabled) — the SBS harness
// otherwise routes so only one core drives audio at a time.
//
// Called from:
//   - boot.cpp                    → spu_audio.init()               (per Game, at boot)
//   - game_tomba2.cpp frame body  → spu_audio.frame()              (per Game, per video frame)
//                                  spu_audio.frameLogic()          (SBS diff: advance XA only, no output)
//   - repl.cpp `wav <path>`       → spu_audio.wavReopen(path)      (mid-run WAV capture handoff)
#pragma once
#include <cstdint>
#include <cstdio>
#ifdef PSXPORT_SDL
#include <SDL3/SDL.h>
#endif

class Game;

class SpuAudio {
public:
  Game* game = nullptr;   // back-pointer wired by Game(); reaches this Core's SPU/XA/native-music state

  void init();
  void frame()      { frameEx(true); }
  void frameLogic() { frameEx(false); }   // SBS/dual-core: advance XA for game logic only, no output
  void wavReopen(const char* path);       // REPL music-dump: finalize current WAV, start a fresh one

private:
  void frameEx(bool output);
  void wavOpen(const char* path);
  void wavClose();
  // atexit hook — finalizes the WAV of whichever SpuAudio has one open (a static pointer set on
  // wavOpen, cleared on wavClose). Only one WAV per process in practice, so this is safe.
  static void wavCloseAtExit();
  static SpuAudio* sWavOwner;
  static void wavLe16(FILE* f, uint16_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
  static void wavLe32(FILE* f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((v >> (8*i)) & 0xFF, f); }

#ifdef PSXPORT_SDL
  SDL_AudioStream* mStream = nullptr;   // NULL = not open / failed / disabled
#endif
  int      mState    = 0;               // 0 = uninit, 1 = enabled+open, -1 = disabled/failed
  FILE*    mWav      = nullptr;         // open WAV file, or NULL
  uint32_t mWavBytes = 0;               // PCM bytes written so far
};
