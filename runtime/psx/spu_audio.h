// class SpuAudio — the native audio OUTPUT sink for one Game.
//
// Drives the shared SPU mix (spu_beetle.c) into a host SDL3 audio stream: init() opens the device
// once per Game; frame() advances the SPU by the exact rational duration of one display field,
// drains the produced samples, mixes native music, and queues them to the host device (+ optional
// WAV capture). No
// PSX SPU hardware is presented — we only consume the already-mixed PCM the SPU produced.
//
// Owned by `class Game` (`c->game->spu_audio.method()`); the back-pointer is wired in Game(). One
// per Game instance. In SBS with two Games only one Game's SDL device init will succeed (the host
// device is a single OS resource) and the other's mState stays at -1 (disabled) — the SBS harness
// otherwise routes so only one core drives audio at a time.
//
// Called from:
//   - boot.cpp                    → spu_audio.init()               (per Game, at boot)
//   - game_tomba2.cpp frame body  → spu_audio.frame()              (per Game, per display field)
//                                  spu_audio.frameLogic()          (SBS diff: advance XA only, no output)
//   - repl.cpp `wav <path>`       → spu_audio.wavReopen(path)      (mid-run WAV capture handoff)
#pragma once
#include "audio_field_report.h"
#include "spu_field_cadence.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <vector>
#ifdef PSXPORT_SDL
#include <SDL3/SDL.h>
#endif

class Game;
class Core;
struct GameHooks;

// Mix the GAME's own music engine on top of the SPU's rendered PCM.
//
// A free function, not a private method, because `audioMixFrame` is an OPTIONAL hook: a port with no
// native music engine of its own leaves it null (spider1 binds its hooks by name, so every field it
// has not stood up is value-initialised to nullptr). The framework's idiom for an optional hook is to
// test the pointer before calling it (native_boot.cpp does this for devWarp) — this used to
// be an unguarded call inside frameEx that checked only `game`, so the FIRST consumer without a music
// engine segfaulted the moment anything drove the mixer.
//
// Split out here so that contract is reachable by a hermetic test: no SPU, no device, no Game.
// Null `hooks` or null `hooks->audioMixFrame` is the normal case for such a port and must be a
// silent no-op that leaves `buf` exactly as the SPU rendered it.
void spu_mix_game_audio(Core *c, const GameHooks *hooks, int16_t *buf, int frames);

class SpuAudio {
public:
  Game *game = nullptr; // back-pointer wired by Game(); reaches this Core's SPU/XA/native-music state

  ~SpuAudio();
  void init();
  void frame();
  void frameLogic() {
    frameEx(false);
  } // SBS/dual-core: advance XA for game logic only, no output
  void wavReopen(const char *path); // REPL music-dump: finalize current WAV, start a fresh one

  void clearFieldReports() {
    mFieldReports.clear();
  }
  [[nodiscard]] const std::vector<AudioFieldReport> &fieldReports() const {
    return mFieldReports;
  }

private:
  void frameEx(bool output);
  void wavOpen(const char *path);
  void wavClose();
  // atexit hook — finalizes the WAV of whichever SpuAudio has one open (a static pointer set on
  // wavOpen, cleared on wavClose). Only one WAV per process in practice, so this is safe.
  static void wavCloseAtExit();
  static SpuAudio *sWavOwner;
  static void wavLe16(FILE *f, uint16_t v) {
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
  }
  static void wavLe32(FILE *f, uint32_t v) {
    for (int i = 0; i < 4; i++) {
      fputc((v >> (8 * i)) & 0xFF, f);
    }
  }

#ifdef PSXPORT_SDL
  SDL_AudioStream *mStream = nullptr; // NULL = not open / failed / disabled
  bool mStreamStarted = false;        // playback begins only after the bounded cushion is primed
#endif
  int mState = 0;          // 0 = uninit, 1 = enabled+open, -1 = disabled/failed
  FILE *mWav = nullptr;    // open WAV file, or NULL
  uint32_t mWavBytes = 0;  // PCM bytes written so far
  uint32_t mWavSynced = 0; // mWavBytes at the last header patch + flush (see frameEx):
                           // the capture stays a valid WAV even if the process is
                           // killed, which is how every headless run actually ends

  SpuFieldCadence mCadence;

  // Per-field mix buffer (up to one PAL field + slack). Per-instance so two SBS Games never mix
  // through the same scratch. The native-music render scratch moved game-side into the audioMixFrame
  // GameHooks impl (game_hooks.cpp), which owns its own per-call buffer.
  static constexpr uint32_t kRenderSlack = 64;
  int16_t mMixBuf[2 * (SpuFieldCadence::kMaximumStandardSamplesPerField + kRenderSlack)] = {};

  // `debug spuprof` diagnostics (average spu_update() wall time every 60 frames).
  int mProfOn = -1; // -1 = unknown, 0/1 cached
  double mProfAccumMs = 0, mProfLoopMs = 0;
  int mProfN = 0, mProfHavePrev = 0;
  struct timespec mProfPrev = {};

  // `debug audiorate` diagnostics (effective production rate + drop count).
  int mRateOn = -1; // -1 = unknown, 0/1 cached
  double mRateT0 = 0;
  long mRateSamp = 0, mRateDrops = 0, mRateCalls = 0;
  int mRateHave = 0;

  // Monotonic field ordinal for the optional audio-field trace. This is per SpuAudio so SBS can
  // distinguish the two cores without adding shared state to the comparator.
  uint64_t mTraceField = 0;
  std::vector<AudioFieldReport> mFieldReports;
};
