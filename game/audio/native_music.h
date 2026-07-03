// class NativeMusic — in-game REAL-TIME native music player, owned by `class Game`.
//
// Drives the shared synth core (native_audio.c) live: play() binds a SEP sequence + VAB at explicit
// byte offsets into a caller-owned data buffer; render() pulls stereo @44100 chunks each audio frame
// (mixed into the SPU sink from SpuAudio::frameEx). Silent when nothing is playing.
//
// One per Game (`c->game->native_music.method()`). In SBS with two Games each has its own — only
// the Game that drives audio (SpuAudio's mState==1) actually mixes anything audible.
#ifndef NATIVE_MUSIC_H
#define NATIVE_MUSIC_H
#include <stdint.h>

#ifdef __cplusplus
#include "native_audio.h"
#include <pthread.h>

class Game;

class NativeMusic {
public:
  Game* game = nullptr;   // back-pointer wired by Game()

  // Bind the SEP sequence at seqOff using the VAB at vabOff, both byte offsets into `data`. `data`
  // must remain valid for the lifetime of playback (no copy). Stops any current track first.
  // Returns 0 on success, -1 if the seq/VAB headers are invalid.
  int  play(const uint8_t* data, long seqOff, long vabOff);
  // Stop the active track (subsequent renders are silent).
  void stop();
  // Is a track currently playing (and not yet drained)?
  bool active();
  // Render `nframes` stereo frames @44100 into `out` (interleaved L,R int16), advancing the
  // sequence. Writes zeros (and returns 0) when nothing is playing.
  int  render(int16_t* out, int nframes);

private:
  pthread_mutex_t mLock = PTHREAD_MUTEX_INITIALIZER;
  bool  mPlaying = false;     // a track is bound and not yet drained
  NaVab mVab;
  NaSeq mSeq;
};
#endif // __cplusplus
#endif
