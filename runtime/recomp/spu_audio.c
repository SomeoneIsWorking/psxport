// Native audio OUTPUT sink — plays the SPU's mixed samples through SDL2.
//
// The SPU lift (spu_beetle.c) mixes 44100 Hz stereo int16 into an internal buffer;
// spu_update(clocks) advances that mixer and spu_render() drains the finished frames.
// This file is the speaker end: it opens an SDL2 audio device and, once per video
// frame, advances the SPU exactly one NTSC frame's worth of clocks, drains the
// produced samples, and queues them to the device. No PSX SPU hardware is presented
// here — we only consume the already-mixed PCM the SPU produced.
//
// Disabled (silent no-op) if PSXPORT_NOAUDIO is set or the SDL device fails to open
// (e.g. headless host with no audio backend). Init is lazy + idempotent.
#include <stdint.h>
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef PSXPORT_SDL
#include <SDL.h>
#endif

// SPU lift interface (spu_beetle.c). Declared here rather than via a shared header so
// this output sink is self-contained (matches the runtime's per-module convention).
int32_t spu_update(int32_t clocks);
int     spu_render(int16_t *out, int max_frames);

// Native music engine (engine/audio/native_music.c) — the PC-native SEP/VAB synth that
// replaces the broken libsnd sequenced-music path. Its stereo @44100 output is MIXED into the
// SPU sink below so the sound-test (and, eventually, all sequenced BGM) is audible. Silent when
// nothing is playing. Declared here to keep this sink self-contained.
int     native_music_render(int16_t *out, int nframes);
int     native_music_active(void);

// Public interface (this module).
void spu_audio_init(void);
void spu_audio_frame(void);

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

#ifdef PSXPORT_SDL
static SDL_AudioDeviceID s_dev;   // 0 = not open / failed / disabled
#endif
static int s_state = 0;           // 0 = uninit, 1 = enabled+open, -1 = disabled/failed

// ---- WAV capture (PSXPORT_WAV=path) -------------------------------------------------
// Dumps the SPU's mixed 44100 Hz stereo int16 output to a WAV file, INDEPENDENT of SDL —
// works headless / under PSXPORT_NOAUDIO (the SPU is still advanced + drained, the samples
// are just written to disk instead of, or as well as, played). This is the audio analog of
// the GPU VRAM dump: it lets the produced audio be inspected/compared offline (the SPU lift
// is Beetle's own spu.c, so a faithful run should match the oracle's mix). Header sizes are
// patched at exit. Capped so a runaway run can't fill the disk.
static FILE*    s_wav;            // open WAV file, or NULL
static uint32_t s_wav_bytes;      // PCM bytes written so far
#define WAV_MAX_BYTES (600u * 44100u * 2u * 2u)   // ~10 min of stereo s16

static void wav_le16(FILE* f, uint16_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void wav_le32(FILE* f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((v >> (8*i)) & 0xFF, f); }

static void wav_close(void) {
   if (!s_wav) return;
   uint32_t data = s_wav_bytes, riff = 36 + data;
   fseek(s_wav, 4, SEEK_SET);  wav_le32(s_wav, riff);      // RIFF chunk size
   fseek(s_wav, 40, SEEK_SET); wav_le32(s_wav, data);      // data chunk size
   fclose(s_wav); s_wav = NULL;
   fprintf(stderr, "[spu_wav] wrote %u PCM bytes (%.2f s)\n", data, data / (44100.0 * 4.0));
}

static void wav_open(const char* path) {
   s_wav = fopen(path, "wb");
   if (!s_wav) { fprintf(stderr, "[spu_wav] cannot open %s\n", path); return; }
   fwrite("RIFF", 1, 4, s_wav); wav_le32(s_wav, 0);        // size patched at close
   fwrite("WAVE", 1, 4, s_wav);
   fwrite("fmt ", 1, 4, s_wav); wav_le32(s_wav, 16);
   wav_le16(s_wav, 1);          // PCM
   wav_le16(s_wav, 2);          // stereo
   wav_le32(s_wav, 44100);      // sample rate
   wav_le32(s_wav, 44100 * 2 * 2); // byte rate
   wav_le16(s_wav, 2 * 2);      // block align
   wav_le16(s_wav, 16);         // bits/sample
   fwrite("data", 1, 4, s_wav); wav_le32(s_wav, 0);        // size patched at close
   atexit(wav_close);
   fprintf(stderr, "[spu_wav] capturing SPU output -> %s\n", path);
}

// REPL music-dump helper: switch the SPU WAV capture to a new file mid-run (finalize the
// current one, start a fresh one). Lets each BGM track be rendered to its own WAV in one
// session. Enabling capture here also makes spu_audio_frame advance+drain the SPU every
// frame even headless (the `!s_wav` early-out no longer trips), so the song actually renders.
void spu_wav_reopen(const char* path) {
   wav_close();
   s_wav_bytes = 0;
   wav_open(path);
}

// Open the SDL2 audio device (44100 Hz, AUDIO_S16SYS, stereo). Idempotent: subsequent
// calls are no-ops. Honors PSXPORT_NOAUDIO (force-disable) and gracefully disables if
// SDL can't init/open a device.
void spu_audio_init(void)
{
   if (s_state != 0)
      return;                     // already decided (enabled or disabled)

   // WAV capture is independent of the SDL device: it works even headless / under NOAUDIO.
   { const char* wp = cfg_str("PSXPORT_WAV"); if (wp && !s_wav) wav_open(wp); }

   // Headless implies no audio — there's no point driving the sound device for an automated/offscreen
   // run (it just makes noise). Audio opens ONLY for a real on-screen window. `PSXPORT_NOAUDIO` stays as
   // an explicit mute for windowed runs. (WAV capture above is independent and still works headless.)
   { int gpu_windowed(void);
     if (cfg_on("PSXPORT_NOAUDIO") || !gpu_windowed())   // audio opens only for a real on-screen window
     {
        s_state = -1;
        return;
     } }

#ifdef PSXPORT_SDL
   if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
   {
      fprintf(stderr, "[spu_audio] SDL_InitSubSystem(AUDIO) failed: %s — audio disabled\n",
              SDL_GetError());
      s_state = -1;
      return;
   }

   SDL_AudioSpec want, have;
   SDL_memset(&want, 0, sizeof want);
   want.freq     = 44100;
   want.format   = AUDIO_S16SYS;
   want.channels = 2;
   want.samples  = 1024;         // device buffer; we feed it via SDL_QueueAudio
   want.callback = NULL;         // push model (queue), not pull (callback)

   s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
   if (s_dev == 0)
   {
      fprintf(stderr, "[spu_audio] SDL_OpenAudioDevice failed: %s — audio disabled\n",
              SDL_GetError());
      s_state = -1;
      return;
   }

   SDL_PauseAudioDevice(s_dev, 0);   // start playback
   s_state = 1;
#else
   // No SDL compiled in: cleanly disabled.
   s_state = -1;
#endif
}

// Called once per video frame: advance the SPU one NTSC frame of clocks (~735 stereo
// frames), drain them, and queue to the device. No-op when audio is disabled/failed.
// Bounds the device queue: if the backlog exceeds AUDIO_QUEUE_CAP_BYTES we still advance
// the SPU (so its mixer state stays correct) but drop the rendered samples instead of
// queueing, letting the device drain and resync latency.
void spu_audio_frame(void)
{
   // We advance + drain the SPU when SOMETHING consumes it: the SDL device (playback) OR a
   // WAV capture (PSXPORT_WAV, works headless). If neither is active, leave the SPU idle.
#ifdef PSXPORT_SDL
   int sdl_on = (s_state == 1 && s_dev != 0);
#else
   int sdl_on = 0;
#endif
   if (!sdl_on && !s_wav)
      return;

   // Advance the mixer by exactly one video frame of system clocks. spu_render drains
   // whatever the SPU finished; sized to a full frame's worth plus headroom.
   static int16_t buf[2 * (SPU_FRAMES_PER_VIDEO_FRAME + 64)];

   // Diagnostics: PSXPORT_SPU_PROF=1 prints the average spu_update() wall time every
   // 60 frames so the mixer's true per-frame cost can be observed in context.
   static int s_prof = -1;
   if (s_prof < 0) s_prof = cfg_dbg("spuprof") ? 1 : 0;
   if (s_prof)
   {
      static double accum_ms, loop_ms; static int n;
      static struct timespec prev; static int have_prev;
      struct timespec a, b;
      clock_gettime(CLOCK_MONOTONIC, &a);
      if (have_prev)
         loop_ms += (a.tv_sec - prev.tv_sec) * 1e3 + (a.tv_nsec - prev.tv_nsec) / 1e6;
      spu_update(SPU_CLOCKS_PER_VIDEO_FRAME);
      clock_gettime(CLOCK_MONOTONIC, &b);
      accum_ms += (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
      prev = a; have_prev = 1;
      if (++n >= 60) {
         fprintf(stderr, "[spu_prof] spu_update %.4f ms | full frame iter %.4f ms | spu share %.1f%%\n",
                 accum_ms / n, loop_ms / n, loop_ms > 0 ? 100.0 * accum_ms / loop_ms : 0.0);
         accum_ms = 0; loop_ms = 0; n = 0;
      }
   }
   else
      spu_update(SPU_CLOCKS_PER_VIDEO_FRAME);

   int frames = spu_render(buf, SPU_FRAMES_PER_VIDEO_FRAME + 64);
   if (frames <= 0)
   {
      // The SPU produced nothing this frame, but native music may still need output. Emit a full
      // frame of silence as the base and let the native mix fill it.
      frames = SPU_FRAMES_PER_VIDEO_FRAME;
      memset(buf, 0, (size_t)frames * 2 * sizeof(int16_t));
   }

   // Mix the native music engine on top of the SPU's output (sound-test / native sequenced BGM).
   // It renders into a scratch buffer and we add it sample-by-sample with saturation. When nothing
   // is playing native_music_render writes silence, so this is a no-op for normal play.
   if (native_music_active())
   {
      static int16_t mbuf[2 * (SPU_FRAMES_PER_VIDEO_FRAME + 64)];
      native_music_render(mbuf, frames);
      for (int i = 0; i < frames * 2; i++)
      {
         int v = buf[i] + mbuf[i];
         if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
         buf[i] = (int16_t)v;
      }
   }

   // WAV capture: append the drained PCM (every frame, regardless of SDL). Capped.
   if (s_wav && s_wav_bytes < WAV_MAX_BYTES)
   {
      size_t bytes = (size_t)frames * 2 * sizeof(int16_t);
      fwrite(buf, 1, bytes, s_wav);
      s_wav_bytes += (uint32_t)bytes;
   }

#ifdef PSXPORT_SDL
   if (!sdl_on)
      return;
   // PSXPORT_AUDIO_RATE=1: measure effective production rate (samples/wall-sec) + drop count. If
   // production > 44100/s the SPU (and the XA stream) is advancing faster than realtime -> backlog
   // overflows, frames get dropped, and the music skips ahead = loops early.
   { static int on = -1; if (on < 0) on = cfg_dbg("audiorate") ? 1 : 0;
     if (on) { static double t0; static long samp, drops, calls; static int have;
       struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
       double now = ts.tv_sec + ts.tv_nsec/1e9; if (!have) { t0 = now; have = 1; }
       samp += frames; calls++;
       if ((int)SDL_GetQueuedAudioSize(s_dev) > AUDIO_QUEUE_CAP_BYTES) drops++;
       double dt = now - t0;
       if (dt >= 2.0) { fprintf(stderr, "[audio_rate] %.0f samples/s (want 44100), %ld calls/%.1fs, drops=%ld, backlog=%u\n",
                                samp/dt, calls, dt, drops, SDL_GetQueuedAudioSize(s_dev));
                        t0 = now; samp = 0; calls = 0; drops = 0; } } }
   // Drop (don't queue) when the backlog is already too deep — keeps latency bounded.
   if ((int)SDL_GetQueuedAudioSize(s_dev) > AUDIO_QUEUE_CAP_BYTES)
      return;

   SDL_QueueAudio(s_dev, buf, (Uint32)frames * 2 * sizeof(int16_t));

   // Diagnostics: PSXPORT_AUDIO_LOG=1 reports the device backlog each frame so the queue
   // can be observed to grow / stay bounded (used by the smoke test; off by default).
   if (cfg_dbg("audio"))
      fprintf(stderr, "[spu_audio] rendered %d frames, queued=%u bytes\n",
              frames, SDL_GetQueuedAudioSize(s_dev));
#endif
}
