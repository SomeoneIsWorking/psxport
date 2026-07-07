// game/audio/native_audio.h — the SHARED PC-native audio synth core.
//
// This is the ONE codebase used by BOTH the offline renderer (tools/snd_render.c) and the
// in-game real-time music engine (engine/audio/native_music.c). It owns: VAB ('pBAV') sound-bank
// parsing, PSX ADPCM (VAG) decode, the per-voice synth (ADSR + pitch resample + vol/pan), and the
// SEP ('pQES') sequencer interpreter. It does NO file I/O and holds NO global state: every routine
// operates on a caller-supplied byte buffer + explicit byte offsets, so the same parser serves a
// whole-file dump (TOMBA2.SND), an area bundle, or guest RAM read live from the running game.
//
// Format references: scratch/native_audio_spec.md (the bible) + scratch/handoff_audio_unify.md
// (the corrected §5b/§6). The SEP stream is libsnd-specific, NOT standard MIDI (see seq_event).
#ifndef NATIVE_AUDIO_H
#define NATIVE_AUDIO_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- VAB ('pBAV') sound bank ---------------------------------------------------------------
// Opened against a byte buffer at an explicit base offset. All sub-table offsets are absolute
// into the same buffer.
typedef struct {
    const uint8_t* buf;
    long base;
    int  ver, id, fsize, ps, ts, vs;
    long progtab, tonetab, vagtab, body;
} NaVab;

// One resolved ToneAttr (32B on disc).
typedef struct {
    int vol, pan, centre, shift, nmin, nmax, adsr1, adsr2, prog, vag;
} NaTone;

// Open a VAB at an explicit byte offset into `buf`. Returns 0 ok, -1 on bad magic.
int  na_vab_open_at(NaVab* v, const uint8_t* buf, long off);
// Read ToneAttr index t (across the concatenated tone array).
void na_tone_read(const NaVab* v, int t, NaTone* o);
// Cumulative tone-start index for program p (tones concatenated in program order).
int  na_prog_tone_start(const NaVab* v, int p);
// Decode VAG index `vagIdx` (1-based) of VAB `v` into `out` (capacity `outcap` samples).
// Returns sample count; loop info via the out params (-1 loop_start = none).
int  na_vab_decode_vag(const NaVab* v, int vagIdx, int16_t* out, int outcap,
                       int* loop_start, int* loops);

// ---- voice synth ---------------------------------------------------------------------------
enum { NA_ADSR_ATTACK, NA_ADSR_DECAY, NA_ADSR_SUSTAIN, NA_ADSR_RELEASE, NA_ADSR_OFF };

// A decoded VAG (PCM cache). The sequencer owns one cache per active VAB.
typedef struct { int16_t* data; int nsamp, loop_start, loops; } NaVagPcm;

typedef struct {
    int      active;
    int16_t* data; int nsamp, loop_start, loops;   // decoded VAG (NOT owned — points at cache)
    uint32_t pos_int, pos_frac;
    uint32_t pitch;                                // 4096 = 1.0x (44100Hz)
    int      phase; uint16_t env; int divider;     // envelope
    int      Ar, Dr, Sr, Rr, Sl, SusLevel;
    int      AttExp, RelExp, SusExp, SusDec;
    int      vol_l, vol_r;                          // 0..0x3fff
    int      chan, note;                            // owning MIDI channel + note
    uint64_t age;                                   // for voice stealing
} NaVoice;

// ---- SEP sequencer ('pQES') ----------------------------------------------------------------
#define NA_NVOICE 24
typedef struct {
    const uint8_t* buf;
    NaVab* vab;
    NaVoice voice[NA_NVOICE];
    NaVagPcm vagcache[256];                          // decoded-VAG cache (per render)
    uint64_t age;
    int    prog[16], vol[16], pan[16];
    int    prog_set;                                 // slot[0x26] program-set selector
    long   p, end;
    int    running_status;
    double tempo_us;
    int    ppqn;
    int    ended;
    double _pending;                                 // samples until next event (carries across renders)
} NaSeq;

// Open a sequence at an explicit byte offset; binds it to the given VAB. Returns 0 ok, -1 bad.
int  na_seq_open(NaSeq* s, const uint8_t* buf, NaVab* vab, long seqOff);
// Free any decoded-VAG caches the sequence allocated (call when done with the sequence).
void na_seq_free(NaSeq* s);
// Render up to `nframes` stereo frames @ `rate` into `out` (interleaved L,R int16). Advances
// the sequencer + voices. Stops naturally at end-of-track once the release tail drains; returns
// the number of frames actually produced (may be < nframes if the song ended).
int  na_seq_render(NaSeq* s, int16_t* out, int nframes, int rate);

#ifdef __cplusplus
}
#endif
#endif
