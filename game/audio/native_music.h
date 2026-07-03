// game/audio/native_music.h — the in-game REAL-TIME native music player.
//
// Drives the shared synth core (native_audio.c) live: NativeMusic::instance().play() binds a SEP
// sequence + VAB at explicit byte offsets into a caller-owned data buffer, then
// NativeMusic::instance().render() pulls stereo @44100 chunks each audio frame (mixed into the SPU
// sink in spu_audio.c). Silent when nothing is playing. This is the engine side of the SAME audio
// codebase the offline tool uses.
//
// One singleton per process (one host audio device, one active track at a time). A pthread mutex
// guards play/stop from the game/UI thread vs render() on the audio thread. The legacy
// `native_music_*` C entries are one-liner bridges over `NativeMusic::instance()` so spu_audio and
// music_list link unchanged.
#ifndef NATIVE_MUSIC_H
#define NATIVE_MUSIC_H
#include <stdint.h>

#ifdef __cplusplus
#include "native_audio.h"
#include <pthread.h>

class NativeMusic {
public:
  static NativeMusic& instance();

  // Bind the SEP sequence at seqOff using the VAB at vabOff, both byte offsets into `data`. `data`
  // must remain valid for the lifetime of playback (no copy). Stops any current track first.
  // Returns 0 on success, -1 if the seq/VAB headers are invalid.
  int  play(const uint8_t* data, long seqOff, long vabOff);
  // Stop the active track (subsequent renders are silent).
  void stop();
  // Is a track currently playing (and not yet drained)?
  bool active();
  // Render `nframes` stereo frames @44100 into `out` (interleaved L,R int16), advancing the
  // sequence. Writes zeros (and returns 0) when nothing is playing. May return < nframes when the
  // song ends; remaining frames are zeroed.
  int  render(int16_t* out, int nframes);

private:
  NativeMusic() = default;
  pthread_mutex_t mLock = PTHREAD_MUTEX_INITIALIZER;
  bool  mPlaying = false;     // a track is bound and not yet drained
  NaVab mVab;
  NaSeq mSeq;
};
#endif // __cplusplus

// Legacy free-function API — thin bridges to `NativeMusic::instance()`.
#ifdef __cplusplus
extern "C" {
#endif
int  native_music_play(const uint8_t* data, long seqOff, long vabOff);
void native_music_stop(void);
int  native_music_active(void);
int  native_music_render(int16_t* out, int nframes);
#ifdef __cplusplus
}
#endif
#endif
