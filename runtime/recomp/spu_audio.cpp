// Native audio OUTPUT sink — plays the SPU's mixed samples through SDL3.
//
// The SPU lift (spu_beetle.c) mixes 44100 Hz stereo int16 into an internal buffer;
// spu_update(clocks) advances that mixer and spu_render() drains the finished frames.
// This file is the speaker end: it opens an SDL3 audio stream and, once per video
// frame, advances the SPU exactly one NTSC frame's worth of clocks, drains the
// produced samples, and queues them to the device. No PSX SPU hardware is presented
// here — we only consume the already-mixed PCM the SPU produced.
//
// One `class SpuAudio` singleton per process (one host output device, one WAV capture).
// Disabled (silent no-op) if PSXPORT_NOAUDIO is set or the SDL device fails to open
// (e.g. headless host with no audio backend). Init is lazy + idempotent. The legacy
// `spu_audio_*` / `spu_wav_reopen` C entries are one-liner bridges over the singleton
// so boot / game_tomba2 / repl link unchanged.

#include <cstdint>
#include "cfg.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#ifdef PSXPORT_SDL
#include <SDL3/SDL.h>
#endif

extern "C" {
// SPU lift interface (spu_beetle.c). Declared here rather than via a shared header so
// this output sink is self-contained (matches the runtime's per-module convention).
int32_t spu_update(int32_t clocks);
int     spu_render(int16_t *out, int max_frames);

// Native music engine (game/audio/native_music) — the PC-native SEP/VAB synth that
// replaces the broken libsnd sequenced-music path. Its stereo @44100 output is MIXED into the
// SPU sink below so the sound-test (and, eventually, all sequenced BGM) is audible. Silent when
// nothing is playing. Declared here to keep this sink self-contained.
int     native_music_render(int16_t *out, int nframes);
int     native_music_active(void);

// XA/CD streamer (xa_stream.c). When a clip is streaming, GAME LOGIC may BLOCK waiting for it to
// finish (the field/cutscene `while(*(0x801fe0e0)!=0)` wait, driven by the read head passing the
// clip's end LBA). That progress only happens inside spu_update -> CDC_GetCDAudioSample, so the SPU
// must be advanced for the wait to clear EVEN when there is no audio output consumer (headless /
// PSXPORT_NOAUDIO). Declared here to keep this sink self-contained.
int     xa_stream_is_active(void);

int     gpu_windowed(void);
}

// The SPU is clocked off the PSX system clock and divides it by 768 to yield 44100 Hz
// samples (Beetle: spu.c clock_divider = 768). One NTSC video frame is 1/60 s, so:
//   33,868,800 Hz / 60 = 564,480 system clocks per frame
//   564,480 / 768       = 735 stereo frames per video frame (== 44100/60). Exact.
// We pass these clocks to spu_update once per video frame; the SPU then has ~735
// frames ready for spu_render. (One frame's clocks comfortably fit the SPU's 4096-
// frame intermediate buffer, so no sub-stepping is needed.)
#define SPU_CLOCKS_PER_VIDEO_FRAME 564480   // 33868800 / 60
#define SPU_FRAMES_PER_VIDEO_FRAME 735      // 44100 / 60 (one video frame of stereo frames)

// SDL_QueueAudio backlog cap: if the device queue grows past this many bytes we skip a
// render to let it drain, so audio can't accumulate unbounded latency when the producer
// outruns the consumer (or a host stall lets frames pile up). 4 video frames of stereo
// int16 = 4 * 735 * 2 ch * 2 bytes.
#define AUDIO_QUEUE_CAP_BYTES (4 * SPU_FRAMES_PER_VIDEO_FRAME * 2 * (int)sizeof(int16_t))
#define WAV_MAX_BYTES (600u * 44100u * 2u * 2u)   // ~10 min of stereo s16

// ---- SpuAudio singleton -------------------------------------------------------------------------
class SpuAudio {
public:
  static SpuAudio& instance();

  void init();
  void frame()      { frameEx(true); }
  void frameLogic() { frameEx(false); }   // SBS/dual-core: advance XA for game logic only, no output
  void wavReopen(const char* path);       // REPL music-dump helper: switch WAV capture mid-run

private:
  SpuAudio() = default;
  void frameEx(bool output);
  void wavOpen(const char* path);
  void wavClose();
  static void wavCloseAtExit();
  static void wavLe16(FILE* f, uint16_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
  static void wavLe32(FILE* f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((v >> (8*i)) & 0xFF, f); }

#ifdef PSXPORT_SDL
  SDL_AudioStream* mStream = nullptr;   // NULL = not open / failed / disabled
#endif
  int      mState    = 0;               // 0 = uninit, 1 = enabled+open, -1 = disabled/failed
  FILE*    mWav      = nullptr;         // open WAV file, or NULL
  uint32_t mWavBytes = 0;               // PCM bytes written so far
};

SpuAudio& SpuAudio::instance() {
  static SpuAudio inst;
  return inst;
}

// ---- WAV capture (PSXPORT_WAV=path) -------------------------------------------------
// Dumps the SPU's mixed 44100 Hz stereo int16 output to a WAV file, INDEPENDENT of SDL —
// works headless / under PSXPORT_NOAUDIO (the SPU is still advanced + drained, the samples
// are just written to disk instead of, or as well as, played). This is the audio analog of
// the GPU VRAM dump: it lets the produced audio be inspected/compared offline (the SPU lift
// is Beetle's own spu.c, so a faithful run should match the oracle's mix). Header sizes are
// patched at exit. Capped so a runaway run can't fill the disk.
void SpuAudio::wavClose() {
  if (!mWav) return;
  uint32_t data = mWavBytes, riff = 36 + data;
  fseek(mWav, 4, SEEK_SET);  wavLe32(mWav, riff);      // RIFF chunk size
  fseek(mWav, 40, SEEK_SET); wavLe32(mWav, data);      // data chunk size
  fclose(mWav); mWav = nullptr;
  fprintf(stderr, "[spu_wav] wrote %u PCM bytes (%.2f s)\n", data, data / (44100.0 * 4.0));
}
void SpuAudio::wavCloseAtExit() { instance().wavClose(); }

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
  atexit(&SpuAudio::wavCloseAtExit);
  fprintf(stderr, "[spu_wav] capturing SPU output -> %s\n", path);
}

// REPL music-dump helper: switch the SPU WAV capture to a new file mid-run (finalize the
// current one, start a fresh one). Lets each BGM track be rendered to its own WAV in one
// session. Enabling capture here also makes frame() advance+drain the SPU every frame even
// headless (the `!mWav` early-out no longer trips), so the song actually renders.
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

  // Headless implies no audio — there's no point driving the sound device for an automated/offscreen
  // run. Audio opens ONLY for a real on-screen window. `PSXPORT_NOAUDIO` stays as an explicit mute
  // for windowed runs. (WAV capture above is independent and still works headless.)
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
  mState = -1;   // No SDL compiled in: cleanly disabled.
#endif
}

// Called once per video frame: advance the SPU one NTSC frame of clocks (~735 stereo frames),
// drain them, and queue to the device. No-op when audio is disabled/failed. Bounds the device
// queue: if the backlog exceeds AUDIO_QUEUE_CAP_BYTES we still advance the SPU (so its mixer state
// stays correct) but drop the rendered samples instead of queueing, letting the device drain.
void SpuAudio::frameEx(bool output) {
  // We advance + drain the SPU when SOMETHING consumes it: the SDL device (playback) OR a WAV
  // capture (PSXPORT_WAV, works headless). We ALSO advance it (output discarded) when an XA clip is
  // streaming, because game LOGIC blocks until the clip's read head passes its end LBA and that
  // progress lives inside spu_update -> CDC_GetCDAudioSample — so a headless run (no device, no
  // WAV) must still tick the SPU or an audio-gated cutscene/voice wait NEVER clears (the field
  // freeze: "frozen, music would keep playing"). One spu_update == one video frame == correct
  // realtime-equivalent pacing, so the clip can't over-advance. If nothing consumes AND nothing
  // streams, the SPU is genuinely idle — leave it.
  //
  // output == false (logic-only): used by the SBS/dual-core diff path, where two cores share the
  // ONE output device singleton so neither may feed it (that would double the audio). We still must
  // advance THIS core's per-instance XA stream so its game LOGIC progresses past the clip wait —
  // so advance + drain (driving CDC_GetCDAudioSample) but skip native-music mix / WAV / device feed.
#ifdef PSXPORT_SDL
  bool sdl_on = output && (mState == 1 && mStream != nullptr);
#else
  bool sdl_on = false;
#endif
  bool wav_on = output && mWav;
  if (!sdl_on && !wav_on && !xa_stream_is_active()) return;

  // Advance the mixer by exactly one video frame of system clocks. spu_render drains whatever the
  // SPU finished; sized to a full frame's worth plus headroom.
  static int16_t buf[2 * (SPU_FRAMES_PER_VIDEO_FRAME + 64)];

  // Diagnostics: `debug spuprof` prints the average spu_update() wall time every 60 frames so the
  // mixer's true per-frame cost can be observed in context.
  static int s_prof = -1;
  if (s_prof < 0) s_prof = cfg_dbg("spuprof") ? 1 : 0;
  if (s_prof) {
    static double accum_ms, loop_ms; static int n;
    static struct timespec prev; static int have_prev;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    if (have_prev) loop_ms += (a.tv_sec - prev.tv_sec) * 1e3 + (a.tv_nsec - prev.tv_nsec) / 1e6;
    spu_update(SPU_CLOCKS_PER_VIDEO_FRAME);
    clock_gettime(CLOCK_MONOTONIC, &b);
    accum_ms += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    prev = a; have_prev = 1;
    if (++n >= 60) {
      fprintf(stderr, "[spu_prof] spu_update %.4f ms | full frame iter %.4f ms | spu share %.1f%%\n",
              accum_ms / n, loop_ms / n, loop_ms > 0 ? 100.0 * accum_ms / loop_ms : 0.0);
      accum_ms = 0; loop_ms = 0; n = 0;
    }
  } else {
    spu_update(SPU_CLOCKS_PER_VIDEO_FRAME);
  }

  int frames = spu_render(buf, SPU_FRAMES_PER_VIDEO_FRAME + 64);
  // Logic-only (SBS): the XA read-head has now advanced (clip progresses toward its end LBA); the
  // rendered PCM is discarded. No native-music mix, no WAV, no device feed.
  if (!output) return;
  if (frames <= 0) {
    // The SPU produced nothing this frame, but native music may still need output. Emit a full
    // frame of silence as the base and let the native mix fill it.
    frames = SPU_FRAMES_PER_VIDEO_FRAME;
    memset(buf, 0, (size_t)frames * 2 * sizeof(int16_t));
  }

  // Mix the native music engine on top of the SPU's output (sound-test / native sequenced BGM).
  // It renders into a scratch buffer and we add it sample-by-sample with saturation. When nothing
  // is playing native_music_render writes silence, so this is a no-op for normal play.
  if (native_music_active()) {
    static int16_t mbuf[2 * (SPU_FRAMES_PER_VIDEO_FRAME + 64)];
    native_music_render(mbuf, frames);
    for (int i = 0; i < frames * 2; i++) {
      int v = buf[i] + mbuf[i];
      if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
      buf[i] = (int16_t)v;
    }
  }

  // WAV capture: append the drained PCM (every frame, regardless of SDL). Capped.
  if (wav_on && mWavBytes < WAV_MAX_BYTES) {
    size_t bytes = (size_t)frames * 2 * sizeof(int16_t);
    fwrite(buf, 1, bytes, mWav);
    mWavBytes += (uint32_t)bytes;
  }

#ifdef PSXPORT_SDL
  if (!sdl_on) return;
  // `debug audiorate`: measure effective production rate (samples/wall-sec) + drop count. If
  // production > 44100/s the SPU (and the XA stream) is advancing faster than realtime -> backlog
  // overflows, frames get dropped, and the music skips ahead = loops early.
  { static int on = -1; if (on < 0) on = cfg_dbg("audiorate") ? 1 : 0;
    if (on) { static double t0; static long samp, drops, calls; static int have;
      struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      double now = ts.tv_sec + ts.tv_nsec/1e9; if (!have) { t0 = now; have = 1; }
      samp += frames; calls++;
      if (SDL_GetAudioStreamQueued(mStream) > AUDIO_QUEUE_CAP_BYTES) drops++;
      double dt = now - t0;
      if (dt >= 2.0) { fprintf(stderr, "[audio_rate] %.0f samples/s (want 44100), %ld calls/%.1fs, drops=%ld, backlog=%d\n",
                                samp/dt, calls, dt, drops, SDL_GetAudioStreamQueued(mStream));
                       t0 = now; samp = 0; calls = 0; drops = 0; } } }
  // Drop (don't queue) when the backlog is already too deep — keeps latency bounded.
  if (SDL_GetAudioStreamQueued(mStream) > AUDIO_QUEUE_CAP_BYTES) return;

  SDL_PutAudioStreamData(mStream, buf, (int)(frames * 2 * sizeof(int16_t)));

  // `debug audio` reports the device backlog each frame so the queue can be observed to grow /
  // stay bounded (used by the smoke test; off by default).
  if (cfg_dbg("audio"))
    fprintf(stderr, "[spu_audio] rendered %d frames, queued=%d bytes\n",
            frames, SDL_GetAudioStreamQueued(mStream));
#endif
}

// ---- Legacy free-function bridges — one-liners over the singleton -------------------------------
extern "C" {
void spu_audio_init(void)             { SpuAudio::instance().init(); }
void spu_audio_frame(void)            { SpuAudio::instance().frame(); }
void spu_audio_frame_logic(void)      { SpuAudio::instance().frameLogic(); }
void spu_wav_reopen(const char* path) { SpuAudio::instance().wavReopen(path); }
}
