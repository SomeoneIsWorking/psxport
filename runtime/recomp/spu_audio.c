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
#include <stdio.h>
#include <stdlib.h>
#ifdef PSXPORT_SDL
#include <SDL.h>
#endif

// SPU lift interface (spu_beetle.c). Declared here rather than via a shared header so
// this output sink is self-contained (matches the runtime's per-module convention).
int32_t spu_update(int32_t clocks);
int     spu_render(int16_t *out, int max_frames);

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

// Open the SDL2 audio device (44100 Hz, AUDIO_S16SYS, stereo). Idempotent: subsequent
// calls are no-ops. Honors PSXPORT_NOAUDIO (force-disable) and gracefully disables if
// SDL can't init/open a device.
void spu_audio_init(void)
{
   if (s_state != 0)
      return;                     // already decided (enabled or disabled)

   if (getenv("PSXPORT_NOAUDIO"))
   {
      s_state = -1;
      return;
   }

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
#ifdef PSXPORT_SDL
   if (s_state != 1 || s_dev == 0)
      return;

   // Advance the mixer by exactly one video frame of system clocks. spu_render drains
   // whatever the SPU finished; sized to a full frame's worth plus headroom.
   static int16_t buf[2 * (SPU_FRAMES_PER_VIDEO_FRAME + 64)];
   spu_update(SPU_CLOCKS_PER_VIDEO_FRAME);
   int frames = spu_render(buf, SPU_FRAMES_PER_VIDEO_FRAME + 64);
   if (frames <= 0)
      return;

   // Drop (don't queue) when the backlog is already too deep — keeps latency bounded.
   if ((int)SDL_GetQueuedAudioSize(s_dev) > AUDIO_QUEUE_CAP_BYTES)
      return;

   SDL_QueueAudio(s_dev, buf, (Uint32)frames * 2 * sizeof(int16_t));

   // Diagnostics: PSXPORT_AUDIO_LOG=1 reports the device backlog each frame so the queue
   // can be observed to grow / stay bounded (used by the smoke test; off by default).
   if (getenv("PSXPORT_AUDIO_LOG"))
      fprintf(stderr, "[spu_audio] rendered %d frames, queued=%u bytes\n",
              frames, SDL_GetQueuedAudioSize(s_dev));
#else
   // No SDL: nothing to play.
#endif
}
