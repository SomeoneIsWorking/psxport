// class SpuAudio: one Game's audio output. frame() finishes the SPU's run through one display field's exact share
// of its clock (register accesses inside the field run it to the guest's time first), drains the mixed 44.1 kHz
// PCM, lets the title mix its music, and feeds the host stream and WAV capture.
#pragma once
#include "audio_queue_policy.h"
#include "host_audio_output.h"
#include "spu_field_cadence.h"
#include "wav_writer.h"

#include <cstdint>
#include <optional>

class Game;
class Core;

// One field of mixed SPU output: its samples, how many were nonzero and the largest magnitude.
struct SpuOutputSummary {
  uint32_t samples = 0;
  uint32_t hash = 2166136261u;
  uint32_t nonzero = 0;
  uint32_t peak = 0;
};

class SpuAudio {
public:
  Game *game = nullptr;

  void init();
  void frame();
  // Finish the current capture and start one at `path`.
  void wavReopen(const char *path);
  // The underruns after playback started and the fields dropped above the queue limit, once at run end.
  void reportQueue() const;
  psx::audio::HostAudioOutput &output() {
    return output_;
  }
  // The last field's output as the player hears it: the SPU mix with the title's music mixed in.
  const SpuOutputSummary &lastField() const {
    return mLastField;
  }

private:
  void openHostDevice();
  void wavOpen(const char *path);
  void feedHost(const int16_t *buf, int frames);

  psx::audio::HostAudioOutput output_;
  psx::audio::HostAudioOutput::Stream mStream;
  bool mStreamStarted = false; // playback begins only after the target level is primed
  AudioQueueSteer mSteer;
  std::uint64_t mReprimes = 0;
  std::uint64_t mDrops = 0;
  int mState = 0; // 0 = uninit, 1 = enabled, -1 = disabled/failed
  std::optional<psx::audio::WavWriter> mWav;
  uint32_t mWavSynced = 0; // data bytes at the last header patch

  SpuFieldCadence mCadence;
  // The field in progress, planned at the previous frame() so register accesses inside it are timed.
  std::optional<SpuFieldAdvance> mField;
  SpuOutputSummary mLastField;

  // One PAL field plus slack; a title's music mixes into it through TitleAudio::mixFrame.
  static constexpr uint32_t kRenderSlack = 64;
  int16_t mMixBuf[2 * (SpuFieldCadence::kMaximumStandardSamplesPerField + kRenderSlack)] = {};

  // `debug spuprof`: average spu::finishField() wall time every 60 fields.
  double mProfAccumMs = 0;
  double mProfLoopMs = 0;
  int mProfN = 0;
  std::optional<std::uint64_t> mProfPrevNs;

  // `debug audiorate`: effective production rate and drops.
  std::optional<std::uint64_t> mRateT0Ns;
  long mRateSamp = 0;
  long mRateDrops = 0;
  long mRateCalls = 0;

  uint64_t mTraceField = 0;
};
