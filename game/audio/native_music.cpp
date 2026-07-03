// game/audio/native_music.cpp — in-game real-time native music player. See native_music.h.
//
// Holds one active NaSeq driving the shared synth core. render() is called from SpuAudio's per-
// video-frame draw path to pull ~735 stereo frames; it advances the sequencer and mixes the 24
// voices. A lightweight lock guards play/stop vs render (play runs on the game/UI thread while
// render runs on the audio-frame path).
#include "native_music.h"
#include "native_audio.h"
#include <cstdlib>
#include <cstring>
#include <pthread.h>

int NativeMusic::play(const uint8_t* data, long seqOff, long vabOff) {
    pthread_mutex_lock(&mLock);
    if (mPlaying) { na_seq_free(&mSeq); mPlaying = false; }
    if (na_vab_open_at(&mVab, data, vabOff)) { pthread_mutex_unlock(&mLock); return -1; }
    if (na_seq_open(&mSeq, data, &mVab, seqOff)) { pthread_mutex_unlock(&mLock); return -1; }
    mPlaying = true;
    pthread_mutex_unlock(&mLock);
    return 0;
}

void NativeMusic::stop() {
    pthread_mutex_lock(&mLock);
    if (mPlaying) { na_seq_free(&mSeq); mPlaying = false; }
    pthread_mutex_unlock(&mLock);
}

bool NativeMusic::active() {
    pthread_mutex_lock(&mLock);
    bool a = mPlaying && !(mSeq.ended);
    pthread_mutex_unlock(&mLock);
    return a;
}

int NativeMusic::render(int16_t* out, int nframes) {
    pthread_mutex_lock(&mLock);
    if (!mPlaying) { pthread_mutex_unlock(&mLock); memset(out, 0, (size_t)nframes*2*sizeof(int16_t)); return 0; }
    int got = na_seq_render(&mSeq, out, nframes, 44100);
    // zero any tail the synth didn't fill (song ended mid-chunk)
    if (got < nframes) memset(out + got*2, 0, (size_t)(nframes-got)*2*sizeof(int16_t));
    // once the song fully drains, mark it stopped so active() reports idle
    if (got < nframes) { na_seq_free(&mSeq); mPlaying = false; }
    pthread_mutex_unlock(&mLock);
    return got;
}
