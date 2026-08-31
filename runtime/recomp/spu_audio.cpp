// Native audio OUTPUT sink — plays the SPU's mixed samples through SDL3. See spu_audio.h.
//
// The SPU lift (spu_beetle.c) mixes 44.1 kHz stereo int16 into an internal buffer; spu_update()
// advances that mixer and spu_render() drains the finished frames. This file is the speaker end.
// One `SpuAudio` per Game (embedded on Game as `spu_audio`); the SBS harness ensures only one
// Game's SpuAudio actually drives the host device.
#include "spu_audio.h"
#include "audio_policy.h" // audio_may_open — ONE definition, shared with native_fmv.cpp
#include "cfg.h"
#include "config_vars.h"
#include "game.h"
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

extern "C" {
// SPU lift interface (spu_beetle.c).
int32_t spu_update(int32_t clocks);
int spu_render(int16_t *out, int max_frames);
// XA/CD streamer (xa_stream.c). When a clip is streaming, GAME LOGIC may BLOCK waiting for it to
// finish; that progress only happens inside spu_update -> CDC_GetCDAudioSample, so the SPU must
// advance for the wait to clear even headless.
struct XaState;
int xa_stream_is_active(struct XaState *xs);
int gpu_windowed(void);
}

// SDL_QueueAudio backlog cap: if the device queue grows past this many bytes we skip a render to
// let it drain, so audio can't accumulate unbounded latency when the producer outruns the consumer.
constexpr int kAudioQueueCapBytes = 4 * (SpuFieldCadence::kSampleRateHz / 60) * 2 * static_cast<int>(sizeof(int16_t));
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
    const char *wp = cfg_str("PSXPORT_WAV");
    if (wp && !mWav) {
      wavOpen(wp);
    }
  }

  // Headless implies no audio — there's no point driving the sound device for an automated /
  // offscreen run. Audio opens ONLY for a real on-screen window.
  if (!audio_may_open(psx::config::cv_noaudio.get(), gpu_windowed() != 0)) {
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
  SDL_ResumeAudioStreamDevice(mStream); // start playback (streams are paused at open)
  mState = 1;
#else
  mState = -1;
#endif
}

// See spu_audio.h for why this is a free function rather than part of frameEx.
//
// `audioMixFrame` is an OPTIONAL hook. A port with no native music engine of its own never binds it,
// and psxport's convention (bind by name, unlisted fields value-initialise to null) makes that the
// normal state rather than an error — so there is nothing to report here: no game music is not a
// fault, it is a port that has none. Gated by tests/test_spu_mix_optional_hook.cpp, which also
// asserts the hook IS called when a port does supply one.
void spu_mix_game_audio(Core *c, const GameHooks *hooks, int16_t *buf, int frames) {
  if (!hooks || !hooks->audioMixFrame) {
    return;
  }
  hooks->audioMixFrame(c, buf, frames);
}

// Called once per delivered display field: advance the SPU by that field's fractional share of its
// clock and sample rates, drain the resulting frames, and queue them to the device. No-op when audio
// is disabled/failed. Bounds the device queue: if the backlog exceeds kAudioQueueCapBytes we still
// advance the SPU (so its mixer state stays correct) but drop the rendered samples instead of
// queueing, letting the device drain.
void SpuAudio::frame() {
  // SBS/dual-core sets diff_mode before stepping each Game. Those cores must advance their private
  // SPU mixers even though neither is allowed to open or feed the host device; otherwise ordinary
  // voice state is never evolved and the per-Core SPU write log cannot prove audio parity. The
  // frameEx(false) path is deliberately logic-only and still renders into the discarded mix buffer.
  frameEx(!(game && game->diff_mode));
}

void SpuAudio::frameEx(bool output) {
  const uint64_t traceField = ++mTraceField;
  // We advance + drain the SPU when SOMETHING consumes it: the SDL device (playback) OR a WAV
  // capture (PSXPORT_WAV, works headless). We ALSO advance it (output discarded) when an XA clip
  // is streaming, because game LOGIC blocks until the clip's read head passes its end LBA and that
  // progress lives inside spu_update -> CDC_GetCDAudioSample.
  //
  // output == false (logic-only): SBS/dual-core diff path — two Games share the ONE output device
  // so neither may feed it. Still advance THIS core's XA stream so its game logic progresses.
#ifdef PSXPORT_SDL
  bool sdl_on = output && (mState == 1 && mStream != nullptr);
#else
  bool sdl_on = false;
#endif
  bool wav_on = output && mWav;
  if (!sdl_on && !wav_on && !xa_stream_is_active(&game->xa) && output) {
    lucent::debug("audiofield",
                  "field={} advanced=0 output={} reason=no-consumer xa_active={} xa_wr={} "
                  "xa_rd={:.3f} xa_pulls={} xa_sectors={}",
                  traceField,
                  output ? 1 : 0,
                  game->xa.active,
                  game->xa.wr,
                  game->xa.rd,
                  game->xa.pulls,
                  game->xa.sectors);
    return;
  }

  const SpuFieldAdvance advance = mCadence.advance(display_field_rate(game->gpu.s_disp_pal != 0));
  const uint32_t xaWrBefore = game->xa.wr;
  const double xaRdBefore = game->xa.rd;
  const uint32_t xaPullsBefore = game->xa.pulls;
  const uint32_t xaSectorsBefore = game->xa.sectors;

  // Advance the mixer by exactly one display field's share of system clocks.
  int16_t *buf = mMixBuf;

  // Diagnostics: `debug spuprof` prints the average spu_update() wall time every 60 frames.
  if (mProfOn < 0) {
    mProfOn = lucent::channel_on("spuprof") ? 1 : 0;
  }
  if (mProfOn) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    if (mProfHavePrev) {
      mProfLoopMs += (a.tv_sec - mProfPrev.tv_sec) * 1e3 + (a.tv_nsec - mProfPrev.tv_nsec) / 1e6;
    }
    spu_update(static_cast<int32_t>(advance.clocks));
    clock_gettime(CLOCK_MONOTONIC, &b);
    mProfAccumMs += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    mProfPrev = a;
    mProfHavePrev = 1;
    if (++mProfN >= 60) {
      lucent::info("spu_prof",
                   "spu_update {:.4f} ms | full frame iter {:.4f} ms | spu share {:.1f}%",
                   mProfAccumMs / mProfN,
                   mProfLoopMs / mProfN,
                   mProfLoopMs > 0 ? 100.0 * mProfAccumMs / mProfLoopMs : 0.0);
      mProfAccumMs = 0;
      mProfLoopMs = 0;
      mProfN = 0;
    }
  } else {
    spu_update(static_cast<int32_t>(advance.clocks));
  }

  const int renderedFrames = spu_render(buf, static_cast<int>(advance.samples + kRenderSlack));
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
      game->gpu.s_disp_pal != 0,
      std::vector<int16_t>(buf, buf + static_cast<size_t>(frames) * 2u),
  });
  lucent::debug("audiofield",
                "field={} advanced=1 output={} pal={} expected_samples={} expected_clocks={} "
                "rendered_samples={} queued_samples={} pcm_hash={:08X} pcm_nonzero={} "
                "pcm_peak={} xa_wr={} xa_rd={:.3f} xa_pulls={} xa_sectors={} "
                "delta_wr={} delta_rd={:.3f} delta_pulls={} delta_sectors={}",
                traceField,
                output ? 1 : 0,
                game->gpu.s_disp_pal ? 1 : 0,
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

  // Mix the game's native music engine on top of the SPU's output (game-owned; the framework names
  // no game audio type). The hook renders the game's NativeMusic into its own scratch and saturating-
  // adds it into `buf`; it's a no-op when nothing is playing. Silent when no game is bound.
  if (game) {
    spu_mix_game_audio(&game->core, game->core.hooks, buf, frames);
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
    if (mWavBytes - mWavSynced >= SpuFieldCadence::kSampleRateHz * 2u * sizeof(int16_t)) {
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
      double now = ts.tv_sec + ts.tv_nsec / 1e9;
      if (!mRateHave) {
        mRateT0 = now;
        mRateHave = 1;
      }
      mRateSamp += frames;
      mRateCalls++;
      if (SDL_GetAudioStreamQueued(mStream) > kAudioQueueCapBytes) {
        mRateDrops++;
      }
      double dt = now - mRateT0;
      if (dt >= 2.0) {
        lucent::info("audio_rate",
                     "{:.0f} samples/s (want {}), {} calls/{:.1f}s, drops={}, backlog={}",
                     mRateSamp / dt,
                     SpuFieldCadence::kSampleRateHz,
                     mRateCalls,
                     dt,
                     mRateDrops,
                     SDL_GetAudioStreamQueued(mStream));
        mRateT0 = now;
        mRateSamp = 0;
        mRateCalls = 0;
        mRateDrops = 0;
      }
    }
  }
  // Drop (don't queue) when the backlog is already too deep — keeps latency bounded.
  if (SDL_GetAudioStreamQueued(mStream) > kAudioQueueCapBytes) {
    return;
  }

  // Same drop, for the same reason, while a RESUME run fast-forwards (PSXPORT_PAD_RESUME): the guest
  // is producing minutes of audio in seconds, so queueing it is either a shriek or an ever-growing
  // backlog that then plays the past over the present once control is handed back. The SPU still ran
  // and its mixer state is intact — this only decides what reaches the device, and it stops the
  // instant the recording is spent, which is when the player starts listening.
  if (game && game->pad.fastForwarding()) {
    return;
  }

  SDL_PutAudioStreamData(mStream, buf, (int)(frames * 2 * sizeof(int16_t)));

  lucent::debug("audio", "[spu_audio] rendered {} frames, queued={} bytes", frames, SDL_GetAudioStreamQueued(mStream));
#endif
}
