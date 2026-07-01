// engine/audio/native_music.h — the in-game REAL-TIME native music player.
//
// Drives the shared synth core (native_audio.c) live: native_music_play() binds a SEP sequence +
// VAB at explicit byte offsets into a caller-owned data buffer, then native_music_render() pulls
// stereo @44100 chunks each audio frame (mixed into the SPU sink in spu_audio.c). Silent when
// nothing is playing. This is the engine side of the SAME audio codebase the offline tool uses.
#ifndef NATIVE_MUSIC_H
#define NATIVE_MUSIC_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start playing the SEP sequence at seqOff using the VAB at vabOff, both byte offsets into `data`.
// `data` must remain valid for the lifetime of playback (the engine does NOT copy it). Stops any
// current track first. Returns 0 on success, -1 if the seq/VAB headers are invalid.
int  native_music_play(const uint8_t* data, long seqOff, long vabOff);

// Stop the active track (subsequent renders are silent).
void native_music_stop(void);

// Is a track currently playing (and not yet drained)?
int  native_music_active(void);

// Render `nframes` stereo frames @44100 into `out` (interleaved L,R int16), advancing the
// sequence. Writes zeros (and returns 0) when nothing is playing. Returns frames actually
// driven by the synth (may be < nframes when the song ends; remaining frames are zeroed).
int  native_music_render(int16_t* out, int nframes);

#ifdef __cplusplus
}
#endif
#endif
