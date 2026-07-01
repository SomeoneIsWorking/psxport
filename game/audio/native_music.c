// engine/audio/native_music.c — in-game real-time native music player. See native_music.h.
//
// Holds one active NaSeq driving the shared synth core. native_music_render() is called from the
// audio sink (spu_audio.c) each video frame to pull ~735 stereo frames; it advances the sequencer
// and mixes the 24 voices. A lightweight lock guards play/stop vs render since play is triggered
// from the game/UI thread while render runs on the audio-frame path.
#include "native_music.h"
#include "native_audio.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int    s_playing;           // 1 = a track is bound and not yet drained
static NaVab  s_vab;
static NaSeq  s_seq;

int native_music_play(const uint8_t* data, long seqOff, long vabOff) {
    pthread_mutex_lock(&s_lock);
    if (s_playing) { na_seq_free(&s_seq); s_playing = 0; }
    if (na_vab_open_at(&s_vab, data, vabOff)) { pthread_mutex_unlock(&s_lock); return -1; }
    if (na_seq_open(&s_seq, data, &s_vab, seqOff)) { pthread_mutex_unlock(&s_lock); return -1; }
    s_playing = 1;
    pthread_mutex_unlock(&s_lock);
    return 0;
}

void native_music_stop(void) {
    pthread_mutex_lock(&s_lock);
    if (s_playing) { na_seq_free(&s_seq); s_playing = 0; }
    pthread_mutex_unlock(&s_lock);
}

int native_music_active(void) {
    pthread_mutex_lock(&s_lock);
    int a = s_playing && !(s_seq.ended);
    pthread_mutex_unlock(&s_lock);
    return a;
}

int native_music_render(int16_t* out, int nframes) {
    pthread_mutex_lock(&s_lock);
    if (!s_playing) { pthread_mutex_unlock(&s_lock); memset(out, 0, (size_t)nframes*2*sizeof(int16_t)); return 0; }
    int got = na_seq_render(&s_seq, out, nframes, 44100);
    // zero any tail the synth didn't fill (song ended mid-chunk)
    if (got < nframes) memset(out + got*2, 0, (size_t)(nframes-got)*2*sizeof(int16_t));
    // once the song fully drains, mark it stopped so native_music_active() reports idle
    if (got < nframes) { na_seq_free(&s_seq); s_playing = 0; }
    pthread_mutex_unlock(&s_lock);
    return got;
}
