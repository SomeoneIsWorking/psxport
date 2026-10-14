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
#include "audio_queue_policy.h"
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

class SpuAudio {
public:
  Game *game = nullptr; // back-pointer wired by Game(); reaches this Core's SPU/XA/native-music state

  ~SpuAudio();
  void init();
  // Whether THIS session's sound reaches the host's speakers.
  //
  // The host audio device is a per-process resource and a process that runs several sessions at once
  // must not have all of them talking over each other: only the one the player is looking at may be
  // heard. Claiming output is separate from opening the device, because which session is the audible
  // one changes while the sessions are already alive — a title selector that hands the chosen title
  // the speakers. Before the claim, init() opens no device at all (an unclaimed session is silent
  // rather than a second stream competing for the same output); claiming it later opens the device
  // then, if the host has not given it to anyone else.
  //
  // The guest's own audio state keeps advancing either way: this gates OUTPUT, never the SPU.
  void setOutputEnabled(bool enabled);
  bool outputEnabled() const {
    return mOutputEnabled;
  }
  void frame();
  void frameLogic() {
    frameEx(false);
  } // SBS/dual-core: advance XA for game logic only, no output
  void wavReopen(const char *path); // REPL music-dump: finalize current WAV, start a fresh one

  // Queue underruns after playback first started, and fields dropped above the queue limit.
  [[nodiscard]] std::uint64_t reprimes() const {
    return mReprimes;
  }
  [[nodiscard]] std::uint64_t drops() const {
    return mDrops;
  }
  // The two counts, once, at run end.
  void reportQueue() const;

  void clearFieldReports() {
    mFieldReports.clear();
  }
  [[nodiscard]] const std::vector<AudioFieldReport> &fieldReports() const {
    return mFieldReports;
  }

private:
  void openHostDevice();
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
  bool mStreamStarted = false;        // playback begins only after the target level is primed
#endif
  AudioQueueSteer mSteer;
  std::uint64_t mReprimes = 0;
  std::uint64_t mDrops = 0;
  int mState = 0;             // 0 = uninit, 1 = enabled+open, -1 = disabled/failed
  bool mOutputEnabled = true; // this session's sound reaches the host speakers (see setOutputEnabled)
  FILE *mWav = nullptr;       // open WAV file, or NULL
  uint32_t mWavBytes = 0;     // PCM bytes written so far
  uint32_t mWavSynced = 0;    // mWavBytes at the last header patch + flush (see frameEx):
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
