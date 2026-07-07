// Native audio OUTPUT sink — plays the SPU's mixed samples through SDL3. See spu_audio.h.
//
// The SPU lift (spu_beetle.c) mixes 44100 Hz stereo int16 into an internal buffer; spu_update()
// advances that mixer and spu_render() drains the finished frames. This file is the speaker end.
// One `SpuAudio` per Game (embedded on Game as `spu_audio`); the SBS harness ensures only one
// Game's SpuAudio actually drives the host device.
#include "spu_audio.h"
#include "cfg.h"
#include "game.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern "C" {
// SPU lift interface (spu_beetle.c).
int32_t spu_update(int32_t clocks);
int     spu_render(int16_t *out, int max_frames);
// XA/CD streamer (xa_stream.c). When a clip is streaming, GAME LOGIC may BLOCK waiting for it to
// finish; that progress only happens inside spu_update -> CDC_GetCDAudioSample, so the SPU must
// advance for the wait to clear even headless.
struct XaState;
int     xa_stream_is_active(struct XaState* xs);
int     gpu_windowed(void);
}

// The SPU is clocked off the PSX system clock and divides it by 768 to yield 44100 Hz samples
// (Beetle: spu.c clock_divider = 768). One NTSC video frame is 1/60 s, so:
//   33,868,800 Hz / 60 = 564,480 system clocks per frame
//   564,480 / 768       = 735 stereo frames per video frame (== 44100/60). Exact.
#define SPU_CLOCKS_PER_VIDEO_FRAME 564480   // 33868800 / 60
#define SPU_FRAMES_PER_VIDEO_FRAME 735      // 44100 / 60 (one video frame of stereo frames)

// SDL_QueueAudio backlog cap: if the device queue grows past this many bytes we skip a render to
// let it drain, so audio can't accumulate unbounded latency when the producer outruns the consumer.
#define AUDIO_QUEUE_CAP_BYTES (4 * SPU_FRAMES_PER_VIDEO_FRAME * 2 * (int)sizeof(int16_t))
#define WAV_MAX_BYTES (600u * 44100u * 2u * 2u)   // ~10 min of stereo s16

// ---- WAV capture ownership (atexit hook) --------------------------------------------------------
SpuAudio* SpuAudio::sWavOwner = nullptr;
void SpuAudio::wavCloseAtExit() { if (sWavOwner) sWavOwner->wavClose(); }

// ---- WAV capture (PSXPORT_WAV=path) -------------------------------------------------------------
// Dumps the SPU's mixed 44100 Hz stereo int16 output to a WAV file, INDEPENDENT of SDL — works
// headless / under PSXPORT_NOAUDIO. Header sizes patched at exit. Capped so a runaway run can't
// fill the disk.
void SpuAudio::wavClose() {
  if (!mWav) return;
  uint32_t data = mWavBytes, riff = 36 + data;
  fseek(mWav, 4, SEEK_SET);  wavLe32(mWav, riff);      // RIFF chunk size
  fseek(mWav, 40, SEEK_SET); wavLe32(mWav, data);      // data chunk size
  fclose(mWav); mWav = nullptr;
  if (sWavOwner == this) sWavOwner = nullptr;
  fprintf(stderr, "[spu_wav] wrote %u PCM bytes (%.2f s)\n", data, data / (44100.0 * 4.0));
}

void SpuAudio::wavOpen(const char* path) {
  mWav = fopen(path, "wb");
  if (!mWav) { fprintf(stderr, "[spu_wav] cannot open %s\n", path); return; }
  fwrite("RIFF", 1, 4, mWav); wavLe32(mWav, 0);        // size patched at close
  fwrite("WAVE", 1, 4, mWav);
  fwrite("fmt ", 1, 4, mWav); wavLe32(mWav, 16);
  wavLe16(mWav, 1);          // PCM
  wavLe16(mWav, 2);          // stereo
  wavLe32(mWav, 44100);      // sample rate
  wavLe32(mWav, 44100 * 2 * 2); // byte rate
  wavLe16(mWav, 2 * 2);      // block align
  wavLe16(mWav, 16);         // bits/sample
  fwrite("data", 1, 4, mWav); wavLe32(mWav, 0);        // size patched at close
  if (!sWavOwner) { sWavOwner = this; atexit(&SpuAudio::wavCloseAtExit); }
  else            { sWavOwner = this; }   // atexit already registered; hand ownership to us
  fprintf(stderr, "[spu_wav] capturing SPU output -> %s\n", path);
}

// REPL music-dump helper: switch the SPU WAV capture to a new file mid-run.
void SpuAudio::wavReopen(const char* path) {
  wavClose();
  mWavBytes = 0;
  wavOpen(path);
}

// Open the SDL3 audio device (44100 Hz, S16, stereo). Idempotent: subsequent calls are no-ops.
// Honors PSXPORT_NOAUDIO (force-disable) and gracefully disables if SDL can't init/open a device.
void SpuAudio::init() {
  if (mState != 0) return;    // already decided (enabled or disabled)

  // WAV capture is independent of the SDL device: it works even headless / under NOAUDIO.
  { const char* wp = cfg_str("PSXPORT_WAV"); if (wp && !mWav) wavOpen(wp); }

  // Headless implies no audio — there's no point driving the sound device for an automated /
  // offscreen run. Audio opens ONLY for a real on-screen window.
  if (cfg_on("PSXPORT_NOAUDIO") || !gpu_windowed()) { mState = -1; return; }

#ifdef PSXPORT_SDL
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    fprintf(stderr, "[spu_audio] SDL_InitSubSystem(AUDIO) failed: %s — audio disabled\n", SDL_GetError());
    mState = -1; return;
  }

  // SDL3 push-model: open a stream bound to the default playback device (44100 Hz S16 stereo). We
  // feed it via SDL_PutAudioStreamData each frame (no callback). Device buffer is managed by SDL.
  SDL_AudioSpec spec;
  SDL_memset(&spec, 0, sizeof spec);
  spec.freq = 44100; spec.format = SDL_AUDIO_S16; spec.channels = 2;

  mStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if (!mStream) {
    fprintf(stderr, "[spu_audio] SDL_OpenAudioDeviceStream failed: %s — audio disabled\n", SDL_GetError());
    mState = -1; return;
  }
  SDL_ResumeAudioStreamDevice(mStream);   // start playback (streams are paused at open)
  mState = 1;
#else
  mState = -1;
#endif
}

// Called once per video frame: advance the SPU one NTSC frame of clocks (~735 stereo frames),
// drain them, and queue to the device. No-op when audio is disabled/failed. Bounds the device
// queue: if the backlog exceeds AUDIO_QUEUE_CAP_BYTES we still advance the SPU (so its mixer state
// stays correct) but drop the rendered samples instead of queueing, letting the device drain.
void SpuAudio::frameEx(bool output) {
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
  if (!sdl_on && !wav_on && !xa_stream_is_active(&game->xa)) return;

  // Advance the mixer by exactly one video frame of system clocks.
  int16_t* buf = mMixBuf;

  // Diagnostics: `debug spuprof` prints the average spu_update() wall time every 60 frames.
  if (mProfOn < 0) mProfOn = cfg_dbg("spuprof") ? 1 : 0;
  if (mProfOn) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    if (mProfHavePrev) mProfLoopMs += (a.tv_sec - mProfPrev.tv_sec) * 1e3 + (a.tv_nsec - mProfPrev.tv_nsec) / 1e6;
    spu_update(SPU_CLOCKS_PER_VIDEO_FRAME);
    clock_gettime(CLOCK_MONOTONIC, &b);
    mProfAccumMs += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    mProfPrev = a; mProfHavePrev = 1;
    if (++mProfN >= 60) {
      fprintf(stderr, "[spu_prof] spu_update %.4f ms | full frame iter %.4f ms | spu share %.1f%%\n",
              mProfAccumMs / mProfN, mProfLoopMs / mProfN,
              mProfLoopMs > 0 ? 100.0 * mProfAccumMs / mProfLoopMs : 0.0);
      mProfAccumMs = 0; mProfLoopMs = 0; mProfN = 0;
    }
  } else {
    spu_update(SPU_CLOCKS_PER_VIDEO_FRAME);
  }

  int frames = spu_render(buf, SPU_FRAMES_PER_VIDEO_FRAME + 64);
  // Logic-only (SBS): rendered PCM is discarded; XA head has advanced (clip progresses).
  if (!output) return;
  if (frames <= 0) {
    // The SPU produced nothing this frame, but native music may still need output.
    frames = SPU_FRAMES_PER_VIDEO_FRAME;
    memset(buf, 0, (size_t)frames * 2 * sizeof(int16_t));
  }

  // Mix the native music engine on top of the SPU's output. Silent when nothing playing.
  if (game && game->native_music.active()) {
    int16_t* mbuf = mMonoBuf;
    game->native_music.render(mbuf, frames);
    for (int i = 0; i < frames * 2; i++) {
      int v = buf[i] + mbuf[i];
      if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
      buf[i] = (int16_t)v;
    }
  }

  // WAV capture: append the drained PCM. Capped.
  if (wav_on && mWavBytes < WAV_MAX_BYTES) {
    size_t bytes = (size_t)frames * 2 * sizeof(int16_t);
    fwrite(buf, 1, bytes, mWav);
    mWavBytes += (uint32_t)bytes;
  }

#ifdef PSXPORT_SDL
  if (!sdl_on) return;
  // `debug audiorate`: measure effective production rate (samples/wall-sec) + drop count.
  { if (mRateOn < 0) mRateOn = cfg_dbg("audiorate") ? 1 : 0;
    if (mRateOn) {
      struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      double now = ts.tv_sec + ts.tv_nsec/1e9; if (!mRateHave) { mRateT0 = now; mRateHave = 1; }
      mRateSamp += frames; mRateCalls++;
      if (SDL_GetAudioStreamQueued(mStream) > AUDIO_QUEUE_CAP_BYTES) mRateDrops++;
      double dt = now - mRateT0;
      if (dt >= 2.0) { fprintf(stderr, "[audio_rate] %.0f samples/s (want 44100), %ld calls/%.1fs, drops=%ld, backlog=%d\n",
                                mRateSamp/dt, mRateCalls, dt, mRateDrops, SDL_GetAudioStreamQueued(mStream));
                       mRateT0 = now; mRateSamp = 0; mRateCalls = 0; mRateDrops = 0; } } }
  // Drop (don't queue) when the backlog is already too deep — keeps latency bounded.
  if (SDL_GetAudioStreamQueued(mStream) > AUDIO_QUEUE_CAP_BYTES) return;

  SDL_PutAudioStreamData(mStream, buf, (int)(frames * 2 * sizeof(int16_t)));

  if (cfg_dbg("audio"))
    fprintf(stderr, "[spu_audio] rendered %d frames, queued=%d bytes\n",
            frames, SDL_GetAudioStreamQueued(mStream));
#endif
}
