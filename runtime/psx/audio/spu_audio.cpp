// Native audio OUTPUT sink — plays the SPU's mixed samples through SDL3. See spu_audio.h.
//
// The SPU lift (spu_beetle.cpp) mixes 44.1 kHz stereo int16 into an internal buffer; spu::update()
// advances that mixer and spu::render() drains the finished frames. This file is the speaker end.
// One `SpuAudio` per Game (embedded on Game as `spu_audio`); the SBS harness ensures only one
// Game's SpuAudio actually drives the host device.
#include "spu_audio.h"
#include "audio_policy.h" // audio_may_open — ONE definition, shared with native_fmv.cpp
#include "audio_queue_policy.h"
#include "config_vars.h"
#include "diagnostic_config.h"
#include "game.h"
#include "gpu_device.h"
#include "render_device.h"
#include "spu_beetle.h"
#include "xa_state.h"
#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

struct PcmSummary {
  uint32_t hash = 2166136261u;
  uint32_t nonzero = 0;
  uint32_t peak = 0;
};

PcmSummary summarizePcm(const int16_t *samples, int frames) {
  PcmSummary summary;
  const size_t count = static_cast<size_t>(frames > 0 ? frames : 0) * 2u;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t value = static_cast<uint16_t>(samples[i]);
    summary.hash ^= value & 0xffu;
    summary.hash *= 16777619u;
    summary.hash ^= value >> 8;
    summary.hash *= 16777619u;
    if (samples[i] != 0) {
      ++summary.nonzero;
    }
    const int magnitude = samples[i] < 0 ? -static_cast<int>(samples[i]) : samples[i];
    if (magnitude > summary.peak) {
      summary.peak = static_cast<uint32_t>(magnitude);
    }
  }
  return summary;
}

} // namespace

constexpr uint32_t kWavMaxBytes = 600u * SpuFieldCadence::kSampleRateHz * 2u * 2u;

// ---- WAV capture ownership (atexit hook) --------------------------------------------------------
SpuAudio *SpuAudio::sWavOwner = nullptr;
void SpuAudio::wavCloseAtExit() {
  if (sWavOwner) {
    sWavOwner->wavClose();
  }
}

SpuAudio::~SpuAudio() {
  // Game owns this object.  It is destroyed before process atexit handlers run, so the capture
  // must be finalized here rather than leaving a stale Game-owned pointer for wavCloseAtExit().
  wavClose();
}

// ---- WAV capture (PSXPORT_WAV=path) -------------------------------------------------------------
// Dumps the SPU's mixed 44.1 kHz stereo int16 output to a WAV file, INDEPENDENT of SDL — works
// headless / under PSXPORT_NOAUDIO. Header sizes patched at exit. Capped so a runaway run can't
// fill the disk.
void SpuAudio::wavClose() {
  if (!mWav) {
    return;
  }
  uint32_t data = mWavBytes, riff = 36 + data;
  fseek(mWav, 4, SEEK_SET);
  wavLe32(mWav, riff); // RIFF chunk size
  fseek(mWav, 40, SEEK_SET);
  wavLe32(mWav, data); // data chunk size
  fclose(mWav);
  mWav = nullptr;
  if (sWavOwner == this) {
    sWavOwner = nullptr;
  }
  lucent::info("spu_wav",
               "wrote {} PCM bytes ({:.2f} s)",
               data,
               data / (static_cast<double>(SpuFieldCadence::kSampleRateHz) * 4.0));
}

void SpuAudio::wavOpen(const char *path) {
  mWav = fopen(path, "wb");
  if (!mWav) {
    lucent::error("spu_wav", "cannot open {}", path ? path : "(null)");
    return;
  }
  fwrite("RIFF", 1, 4, mWav);
  wavLe32(mWav, 0); // size patched at close
  fwrite("WAVE", 1, 4, mWav);
  fwrite("fmt ", 1, 4, mWav);
  wavLe32(mWav, 16);
  wavLe16(mWav, 1);                                      // PCM
  wavLe16(mWav, 2);                                      // stereo
  wavLe32(mWav, SpuFieldCadence::kSampleRateHz);         // sample rate
  wavLe32(mWav, SpuFieldCadence::kSampleRateHz * 2 * 2); // byte rate
  wavLe16(mWav, 2 * 2);                                  // block align
  wavLe16(mWav, 16);                                     // bits/sample
  fwrite("data", 1, 4, mWav);
  wavLe32(mWav, 0); // size patched at close
  if (!sWavOwner) {
    sWavOwner = this;
    atexit(&SpuAudio::wavCloseAtExit);
  } else {
    sWavOwner = this;
  } // atexit already registered; hand ownership to us
  lucent::info("spu_wav", "capturing SPU output -> {}", path ? path : "(null)");
}

// REPL music-dump helper: switch the SPU WAV capture to a new file mid-run.
void SpuAudio::wavReopen(const char *path) {
  wavClose();
  mWavBytes = 0;
  mWavSynced = 0;
  wavOpen(path);
}

// Open the SDL3 audio device (44.1 kHz, S16, stereo). Idempotent: subsequent calls are no-ops.
// Honors PSXPORT_NOAUDIO (force-disable) and gracefully disables if SDL can't init/open a device.
void SpuAudio::init() {
  if (mState != 0) {
    return; // already decided (enabled or disabled)
  }

  // WAV capture is independent of the SDL device: it works even headless / under NOAUDIO.
  {
    const std::string wavPath = psx::config::wav_capture_path();
    if (!wavPath.empty() && !mWav) {
      wavOpen(wavPath.c_str());
    }
  }

  // This session is not the audible one (a host that runs several sessions claims output for one of
  // them). It is enabled, and it stays enabled: the guest's audio state advances normally and this
  // session becomes audible the moment the host claims output for it. What it does NOT do is open a
  // second stream on the process's one output device.
  if (!mOutputEnabled) {
    mState = 1;
    return;
  }
  openHostDevice();
}

void SpuAudio::setOutputEnabled(bool enabled) {
  if (mOutputEnabled == enabled) {
    return;
  }
  mOutputEnabled = enabled;
  if (!enabled) {
    // The stream, if one is open, is left alone: this gates what is FED to it (frameEx), and a
    // session that loses the claim must not tear down the device it is about to win back.
    lucent::info("spu_audio", "host output released by the host (this session is silent)");
    return;
  }
  if (mState == 0) {
    return; // init() has not run; the claim takes effect when it does
  }
  if (mState != 1 || mStream != nullptr) {
    return; // already open, or audio is off for this run for a reason of its own
  }
  lucent::info("spu_audio", "host output claimed by the host — opening the audio device");
  openHostDevice();
}

// Open the SDL3 audio device (44.1 kHz, S16, stereo). Idempotent: subsequent calls are no-ops.
// Honors PSXPORT_NOAUDIO (force-disable) and gracefully disables if SDL can't init/open a device.
void SpuAudio::openHostDevice() {
  // Headless implies no audio — there's no point driving the sound device for an automated /
  // offscreen run. Audio opens ONLY for a real on-screen window.
  if (!psx::audio::mayOpenOutput(psx::config::no_audio(), game->renderDevice.windowed())) {
    mState = -1;
    return;
  }

#ifdef PSXPORT_SDL
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    {
      const char *err = SDL_GetError();
      lucent::error("spu_audio", "SDL_InitSubSystem(AUDIO) failed: {} — audio disabled", err ? err : "(null)");
    }
    mState = -1;
    return;
  }

  // SDL3 push-model: open a stream bound to the default playback device (44.1 kHz S16 stereo). We
  // feed it via SDL_PutAudioStreamData each frame (no callback). Device buffer is managed by SDL.
  SDL_AudioSpec spec;
  SDL_memset(&spec, 0, sizeof spec);
  spec.freq = static_cast<int>(SpuFieldCadence::kSampleRateHz);
  spec.format = SDL_AUDIO_S16;
  spec.channels = 2;

  mStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if (!mStream) {
    {
      const char *err = SDL_GetError();
      lucent::error("spu_audio", "SDL_OpenAudioDeviceStream failed: {} — audio disabled", err ? err : "(null)");
    }
    mState = -1;
    return;
  }
  {
    SDL_AudioSpec device;
    int deviceFrames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(mStream), &device, &deviceFrames)) {
      lucent::info("spu_audio", "device {} Hz, {} frames per period", device.freq, deviceFrames);
    }
  }
  // Streams are paused at open. frameEx() primes the target level before starting playback.
  mState = 1;
#else
  mState = -1;
#endif
}

// Called once per delivered display field: advance the SPU by that field's fractional share of its
// clock and sample rates, drain the resulting frames, and queue them to the device. No-op when audio
// is disabled/failed. The device queue is steered toward AudioQueuePolicy's target level by trimming
// the device's consumption rate; rendered samples are dropped only above the policy's hard limit.
void SpuAudio::frame() {
  frameEx(true);
}

void SpuAudio::frameEx(bool output) {
  const uint64_t traceField = ++mTraceField;
  // The SPU advances every field, consumed or not: guest logic waits on voice end and on the XA read head,
  // and both live inside spu::update.
  //
  // output == false (logic-only): SBS/dual-core diff path, two Games share the ONE output device so
  // neither may feed it.
#ifdef PSXPORT_SDL
  bool sdl_on = output && mOutputEnabled && (mState == 1 && mStream != nullptr);
#else
  bool sdl_on = false;
#endif
  bool wav_on = output && mWav;

  const SpuFieldAdvance advance =
      mCadence.advance(psx::frame::displayFieldRate(game->core.gpuDevice().displayArea().pal));
  const uint32_t xaWrBefore = game->xa.wr;
  const double xaRdBefore = game->xa.rd;
  const uint32_t xaPullsBefore = game->xa.pulls;
  const uint32_t xaSectorsBefore = game->xa.sectors;

  // Advance the mixer by exactly one display field's share of system clocks.
  int16_t *buf = mMixBuf;

  // Diagnostics: `debug spuprof` prints the average spu::update() wall time every 60 frames.
  if (mProfOn < 0) {
    mProfOn = lucent::channel_on("spuprof") ? 1 : 0;
  }
  if (mProfOn) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    if (mProfHavePrev) {
      mProfLoopMs += static_cast<double>(a.tv_sec - mProfPrev.tv_sec) * 1e3 +
                     static_cast<double>(a.tv_nsec - mProfPrev.tv_nsec) / 1e6;
    }
    psx::audio::spu::update(static_cast<int32_t>(advance.clocks));
    clock_gettime(CLOCK_MONOTONIC, &b);
    mProfAccumMs += static_cast<double>(b.tv_sec - a.tv_sec) * 1e3 + static_cast<double>(b.tv_nsec - a.tv_nsec) / 1e6;
    mProfPrev = a;
    mProfHavePrev = 1;
    if (++mProfN >= 60) {
      lucent::info("spu_prof",
                   "spu::update {:.4f} ms | full frame iter {:.4f} ms | spu share {:.1f}%",
                   mProfAccumMs / mProfN,
                   mProfLoopMs / mProfN,
                   mProfLoopMs > 0 ? 100.0 * mProfAccumMs / mProfLoopMs : 0.0);
      mProfAccumMs = 0;
      mProfLoopMs = 0;
      mProfN = 0;
    }
  } else {
    psx::audio::spu::update(static_cast<int32_t>(advance.clocks));
  }

  const int renderedFrames = psx::audio::spu::render(buf, static_cast<int>(advance.samples + kRenderSlack));
  int frames = renderedFrames;
  if (frames <= 0) {
    frames = static_cast<int>(advance.samples);
    memset(buf, 0, (size_t)frames * 2 * sizeof(int16_t));
  }
  const PcmSummary pcm = summarizePcm(buf, frames);
  mFieldReports.push_back(AudioFieldReport{
      traceField,
      advance.samples,
      advance.clocks,
      static_cast<uint32_t>(renderedFrames),
      static_cast<uint32_t>(frames),
      pcm.hash,
      pcm.nonzero,
      pcm.peak,
      game->xa.wr,
      game->xa.rd,
      game->xa.pulls,
      game->xa.sectors,
      game->xa.wr - xaWrBefore,
      game->xa.rd - xaRdBefore,
      game->xa.pulls - xaPullsBefore,
      game->xa.sectors - xaSectorsBefore,
      game->core.gpuDevice().displayArea().pal,
      std::vector<int16_t>(buf, buf + static_cast<size_t>(frames) * 2u),
  });
  lucent::debug("audiofield",
                "field={} advanced=1 output={} pal={} expected_samples={} expected_clocks={} "
                "rendered_samples={} queued_samples={} pcm_hash={:08X} pcm_nonzero={} "
                "pcm_peak={} xa_wr={} xa_rd={:.3f} xa_pulls={} xa_sectors={} "
                "delta_wr={} delta_rd={:.3f} delta_pulls={} delta_sectors={}",
                traceField,
                output ? 1 : 0,
                game->core.gpuDevice().displayArea().pal ? 1 : 0,
                advance.samples,
                advance.clocks,
                renderedFrames,
                frames,
                pcm.hash,
                pcm.nonzero,
                pcm.peak,
                game->xa.wr,
                game->xa.rd,
                game->xa.pulls,
                game->xa.sectors,
                game->xa.wr - xaWrBefore,
                game->xa.rd - xaRdBefore,
                game->xa.pulls - xaPullsBefore,
                game->xa.sectors - xaSectorsBefore);
  // Logic-only (SBS): rendered PCM is discarded; XA head has advanced (clip progresses).
  if (!output) {
    return;
  }
  // A zero-frame SPU result is represented by the expected-duration silent buffer above so native
  // music and the host sink still receive one complete field.

  if (psx::game::TitleAudio *titleAudio = game->runtime.audio(game->core)) {
    titleAudio->mixFrame(buf, frames);
  }

  // WAV capture: append the drained PCM. Capped.
  if (wav_on && mWavBytes < kWavMaxBytes) {
    size_t bytes = (size_t)frames * 2 * sizeof(int16_t);
    fwrite(buf, 1, bytes, mWav);
    mWavBytes += (uint32_t)bytes;
    // Keep the file VALID AT ALL TIMES rather than only after a clean exit. wavClose() patches the
    // two RIFF size fields from an atexit hook — but atexit does not run on a signal, and the
    // watchdog's SIGINT/SIGTERM handler ends the process with _exit(130). A port that never returns
    // from its frame loop (every one of them, under `timeout`) therefore left a 0-byte file with the
    // PCM still sitting in the stdio buffer: PSXPORT_WAV, the only headless way to hear what the
    // port produced, silently captured NOTHING on exactly the runs it exists for.
    //
    // So flush and patch the sizes about once a second. Cost is one fflush + two seeks per ~172
    // frames; the capture is then at most ~1 s short if the process is killed, instead of empty.
    if (mWavBytes - mWavSynced >= sizeof(int16_t) * 2u * SpuFieldCadence::kSampleRateHz) {
      mWavSynced = mWavBytes;
      const long end = ftell(mWav);
      fseek(mWav, 4, SEEK_SET);
      wavLe32(mWav, 36 + mWavBytes);
      fseek(mWav, 40, SEEK_SET);
      wavLe32(mWav, mWavBytes);
      fseek(mWav, end, SEEK_SET);
      fflush(mWav);
    }
  }

#ifdef PSXPORT_SDL
  if (!sdl_on) {
    return;
  }
  // `debug audiorate`: measure effective production rate (samples/wall-sec) + drop count.
  {
    if (mRateOn < 0) {
      mRateOn = lucent::channel_on("audiorate") ? 1 : 0;
    }
    if (mRateOn) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      double now = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
      if (!mRateHave) {
        mRateT0 = now;
        mRateHave = 1;
      }
      mRateSamp += frames;
      mRateCalls++;
      if (AudioQueuePolicy::shouldDrop(SDL_GetAudioStreamQueued(mStream))) {
        mRateDrops++;
      }
      double dt = now - mRateT0;
      if (dt >= 2.0) {
        lucent::info("audio_rate",
                     "{:.0f} samples/s (want {}), {} calls/{:.1f}s, drops={}, backlog={}, level={:.0f}, ratio={:.5f}, "
                     "run reprimes={} drops={}",
                     static_cast<double>(mRateSamp) / dt,
                     SpuFieldCadence::kSampleRateHz,
                     mRateCalls,
                     dt,
                     mRateDrops,
                     SDL_GetAudioStreamQueued(mStream),
                     mSteer.level(),
                     SDL_GetAudioStreamFrequencyRatio(mStream),
                     mReprimes,
                     mDrops);
        mRateT0 = now;
        mRateSamp = 0;
        mRateCalls = 0;
        mRateDrops = 0;
      }
    }
  }
  int queuedBytes = SDL_GetAudioStreamQueued(mStream);
  if (AudioQueuePolicy::needsReprime(mStreamStarted, queuedBytes)) {
    SDL_PauseAudioStreamDevice(mStream);
    mStreamStarted = false;
    ++mReprimes;
    lucent::debug("audio", "[spu_audio] queue ran empty: re-priming");
  }

  // Drop (don't queue) only above the limit the steer keeps a steady producer under.
  if (AudioQueuePolicy::shouldDrop(queuedBytes)) {
    ++mDrops;
    return;
  }

  // Same drop while the Game fast-forwards (resume replay or host pre-roll): queueing seconds of audio
  // produced in a moment is a shriek or a backlog. The SPU still ran; this only decides what reaches
  // the device, and it stops when the fast-forward ends.
  if (game && game->fastForwarding()) {
    return;
  }

  SDL_PutAudioStreamData(mStream, buf, (int)(sizeof(int16_t) * 2 * frames));

  queuedBytes = SDL_GetAudioStreamQueued(mStream);
  if (mStreamStarted) {
    SDL_SetAudioStreamFrequencyRatio(mStream, static_cast<float>(mSteer.observe(queuedBytes)));
  } else if (AudioQueuePolicy::readyToStart(queuedBytes)) {
    mSteer.reset(queuedBytes);
    SDL_SetAudioStreamFrequencyRatio(mStream, 1.0f);
    SDL_ResumeAudioStreamDevice(mStream);
    mStreamStarted = true;
  }

  lucent::debug("audio", "[spu_audio] rendered {} frames, queued={} bytes", frames, queuedBytes);
#endif
}

void SpuAudio::reportQueue() const {
  lucent::info("playcheck", "audio: reprimes={} drops={}", mReprimes, mDrops);
}
