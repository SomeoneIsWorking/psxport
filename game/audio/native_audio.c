// game/audio/native_audio.c — the SHARED PC-native audio synth core.
//
// See native_audio.h. This is tools/snd_render.c's proven synth (VAB parse + PSX ADPCM decode +
// ADSR/pitch/pan voice synth + SEP sequencer), lifted verbatim but de-globalized: every routine
// takes its byte buffer + offsets explicitly (no file-scope g_buf), and the decoded-VAG cache
// lives on the NaSeq, so the offline tool and the in-game engine share ONE implementation and
// multiple sequences can render concurrently.
#include "native_audio.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// little-endian / big-endian readers over an explicit buffer.
static uint16_t rd16(const uint8_t* b, long o){ return b[o] | (b[o+1]<<8); }
static uint32_t rd32(const uint8_t* b, long o){ return b[o] | (b[o+1]<<8) | (b[o+2]<<16) | ((uint32_t)b[o+3]<<24); }
static uint16_t rd16be(const uint8_t* b, long o){ return (b[o]<<8) | b[o+1]; }
static uint32_t rd24be(const uint8_t* b, long o){ return (b[o]<<16) | (b[o+1]<<8) | b[o+2]; }

// ---- PSX ADPCM (VAG) decoder ---------------------------------------------------------------
static const int adpcm_pos[5] = { 0, 60, 115, 98, 122 };
static const int adpcm_neg[5] = { 0,  0, -52, -55, -60 };

static int adpcm_decode_full(const uint8_t* buf, long off, int nbytes, int16_t* out, int outcap,
                             int* loop_start, int* loops) {
    int h1 = 0, h2 = 0, n = 0; *loop_start = -1; *loops = 0;
    for (int blk = 0; blk + 16 <= nbytes; blk += 16) {
        long b = off + blk;
        int shift  = buf[b] & 0x0F;
        int filter = (buf[b] >> 4) & 0x0F;
        int flags  = buf[b+1];
        if (filter > 4) filter = 0;
        if (shift > 12) shift = 9;
        if (flags & 0x04) *loop_start = n;
        for (int i = 0; i < 14; i++) {
            int byte = buf[b + 2 + i];
            for (int half = 0; half < 2; half++) {
                int nib = half ? (byte >> 4) : (byte & 0x0F);
                int s = (int16_t)(nib << 12);
                s >>= shift;
                s += (h1 * adpcm_pos[filter] + h2 * adpcm_neg[filter]) >> 6;
                if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
                if (n < outcap) out[n++] = (int16_t)s;
                h2 = h1; h1 = s;
            }
        }
        if (flags & 0x01) {
            if ((flags & 0x03) == 0x03) *loops = 1;
            break;
        }
    }
    if (*loops && *loop_start < 0) *loop_start = 0;
    return n;
}

// ---- VAB access ----------------------------------------------------------------------------
int na_vab_open_at(NaVab* v, const uint8_t* buf, long o) {
    v->buf = buf; v->base = o;
    if (memcmp(buf+o, "pBAV", 4)) return -1;
    v->ver=rd32(buf,o+4); v->id=rd32(buf,o+8); v->fsize=rd32(buf,o+12);
    v->ps=rd16(buf,o+16+2); v->ts=rd16(buf,o+16+4); v->vs=rd16(buf,o+16+6);
    v->progtab = o + 0x20;
    v->tonetab = o + 0x820;
    v->vagtab  = o + 0x20 + 128*16 + (long)v->ps*16*32;
    v->body    = v->vagtab + 512;
    return 0;
}

// VAG idx is 1-based (table[0] reserved). Returns file offset + size in *sz.
static long vag_off(const NaVab* v, int idx, int* sz) {
    long off = 0; for (int i = 1; i < idx; i++) off += (long)rd16(v->buf, v->vagtab + i*2) * 8;
    *sz = (int)rd16(v->buf, v->vagtab + idx*2) * 8; return v->body + off;
}

void na_tone_read(const NaVab* v, int t, NaTone* o) {
    long a = v->tonetab + (long)t*32; const uint8_t* b = v->buf;
    o->vol=b[a+2]; o->pan=b[a+3]; o->centre=b[a+4]; o->shift=(int8_t)b[a+5];
    o->nmin=b[a+6]; o->nmax=b[a+7];
    o->adsr1=rd16(b,a+0x10); o->adsr2=rd16(b,a+0x12); o->prog=rd16(b,a+0x14); o->vag=b[a+0x16];
}

int na_prog_tone_start(const NaVab* v, int p) {
    // PSX VAB tone table is a FIXED 16 tone-slots per program (this is exactly what na_vab_open_at
    // assumes: vagtab = tonetab + ps*16*32). So program p's tones start at slot p*16; progtab[p*16]
    // is how many of those 16 slots are valid. The old code summed the per-program tone COUNTS (a
    // packed layout), which only agrees for program 0 — for programs >0 it pointed into the wrong
    // program's slots, so every note past a 0xCn program-change resolved to no tone and was dropped.
    // That bug was masked while the synth forced program 0; honoring the live program exposed it.
    return p * 16;
}

int na_vab_decode_vag(const NaVab* v, int vagIdx, int16_t* out, int outcap,
                      int* loop_start, int* loops) {
    int sz; long off = vag_off(v, vagIdx, &sz);
    if (sz <= 0) { *loop_start = -1; *loops = 0; return 0; }
    return adpcm_decode_full(v->buf, off, sz, out, outcap, loop_start, loops);
}

// Program -> tones: every in-range tone of program `prog` keys its own voice (multi-voice).
static int prog_pick_tones(const NaVab* v, int prog, int note, int* out, int cap) {
    if (prog < 0 || prog >= v->ps) return 0;
    int nt = v->buf[v->progtab + prog*16]; if (!nt) return 0;
    int start = na_prog_tone_start(v, prog);
    int n = 0;
    for (int i = 0; i < nt && n < cap; i++) {
        NaTone t; na_tone_read(v, start+i, &t);
        if (note >= t.nmin && note <= t.nmax) out[n++] = start + i;
    }
    return n;
}

// ===========================================================================================
//  VOICE SYNTH — ADSR + pitch resample + vol/pan (faithful to Beetle spu.c).
// ===========================================================================================
static void calc_vc_delta(int zs, int speed, int log_mode, int dec_mode, int inv_increment,
                          int16_t Current, int* increment, int* divinco) {
    *increment = (7 - (speed & 0x3));
    if (inv_increment) *increment = ~*increment;
    *divinco = 32768;
    if (speed < 0x2C) *increment = (unsigned)*increment << ((0x2F - speed) >> 2);
    if (speed >= 0x30) *divinco >>= (speed - 0x2C) >> 2;
    if (log_mode) {
        if (dec_mode) *increment = (Current * *increment) >> 15;
        else if ((Current & 0x7FFF) >= 0x6000) {
            if (speed < 0x28) *increment >>= 2;
            else if (speed >= 0x2C) *divinco >>= 2;
            else { *increment >>= 1; *divinco >>= 1; }
        }
    }
    if (*divinco == 0 && speed < zs) *divinco = 1;
}

static void voice_cache_env(NaVoice* v, int adsr1, int adsr2) {
    uint32_t raw = (uint32_t)adsr1 | ((uint32_t)adsr2 << 16);
    int Sl = (raw >> 0) & 0x0F, Dr = (raw >> 4) & 0x0F, Ar = (raw >> 8) & 0x7F;
    int Rr = (raw >> 16) & 0x1F, Sr = (raw >> 22) & 0x7F;
    v->AttExp  = (raw >> 15) & 1;
    v->RelExp  = (raw >> 21) & 1;
    v->SusExp  = (raw >> 31) & 1;
    v->SusDec  = (raw >> 30) & 1;
    v->Ar = Ar; v->Dr = Dr << 2; v->Sr = Sr; v->Rr = Rr << 2; v->Sl = Sl;
    v->SusLevel = (Sl + 1) << 11;
}

static void voice_run_env(NaVoice* v) {
    int inc = 0, divinco = 0; int16_t uoflow = 0;
    if (v->phase == NA_ADSR_ATTACK && v->env == 0x7FFF) v->phase++;
    switch (v->phase) {
        case NA_ADSR_ATTACK:
            calc_vc_delta(0x7F, v->Ar, v->AttExp, 0, 0, (int16_t)v->env, &inc, &divinco);
            uoflow = 0x7FFF; break;
        case NA_ADSR_DECAY:
            calc_vc_delta(0x1F<<2, v->Dr, 1, 1, 1, (int16_t)v->env, &inc, &divinco);
            uoflow = 0; break;
        case NA_ADSR_SUSTAIN:
            calc_vc_delta(0x7F, v->Sr, v->SusExp, v->SusDec, v->SusDec, (int16_t)v->env, &inc, &divinco);
            uoflow = v->SusDec ? 0 : 0x7FFF; break;
        case NA_ADSR_RELEASE:
            calc_vc_delta(0x1F<<2, v->Rr, v->RelExp, 1, 1, (int16_t)v->env, &inc, &divinco);
            uoflow = 0; break;
        default: return;
    }
    v->divider += divinco;
    if (v->divider & 0x8000) {
        uint16_t prev = v->env;
        v->divider = 0;
        v->env = (uint16_t)(v->env + inc);
        if (v->phase == NA_ADSR_ATTACK) {
            if (((prev ^ v->env) & v->env) & 0x8000) v->env = uoflow;
        } else {
            if (v->env & 0x8000) v->env = uoflow;
        }
        if (v->phase == NA_ADSR_DECAY && v->env < v->SusLevel) v->phase++;
        if (v->phase == NA_ADSR_RELEASE && v->env == 0) v->active = 0;
    }
}

static uint32_t note_to_pitch(const NaTone* t, int note) {
    double semis = (double)(note - t->centre) + (double)t->shift / 128.0;
    double step = pow(2.0, semis / 12.0);
    long p = (long)(step * 4096.0 + 0.5);
    if (p < 1) p = 1; if (p > 0x3FFF) p = 0x3FFF;
    return (uint32_t)p;
}

// decoded-VAG cache lives on the NaSeq (per render).
static NaVagPcm* vag_get(NaSeq* s, int vagIdx) {
    if (vagIdx < 1 || vagIdx > 255) return NULL;
    NaVagPcm* c = &s->vagcache[vagIdx];
    if (c->data) return c;
    int sz; long off = vag_off(s->vab, vagIdx, &sz);
    if (sz <= 0) return NULL;
    int cap = (sz/16)*28 + 64;
    c->data = (int16_t*)malloc(cap * 2);
    c->nsamp = adpcm_decode_full(s->vab->buf, off, sz, c->data, cap, &c->loop_start, &c->loops);
    return c;
}

static void voice_keyon(NaVoice* vc, NaSeq* s, const NaTone* t, int note, int vel,
                        int chan, int chan_vol, int chan_pan, uint64_t age) {
    NaVagPcm* g = vag_get(s, t->vag);
    if (!g || !g->data) { vc->active = 0; return; }
    memset(vc, 0, sizeof(*vc));
    vc->active = 1; vc->age = age;
    vc->data = g->data; vc->nsamp = g->nsamp; vc->loop_start = g->loop_start; vc->loops = g->loops;
    vc->pos_int = 0; vc->pos_frac = 0;
    vc->pitch = note_to_pitch(t, note);
    voice_cache_env(vc, t->adsr1, t->adsr2);
    vc->phase = NA_ADSR_ATTACK; vc->env = 0; vc->divider = 0;
    vc->chan = chan; vc->note = note;
    double amp = (double)vel/127.0 * (double)t->vol/127.0 * (double)chan_vol/127.0;
    double pan = ((double)t->pan + (double)chan_pan) * 0.5 / 127.0;
    if (pan < 0) pan = 0; if (pan > 1) pan = 1;
    double l = cos(pan * M_PI / 2.0), r = sin(pan * M_PI / 2.0);
    vc->vol_l = (int)(amp * l * 0x3FFF);
    vc->vol_r = (int)(amp * r * 0x3FFF);
}

static void voice_render(NaVoice* v, int* outL, int* outR) {
    if (!v->active) { *outL = *outR = 0; return; }
    int i0 = v->pos_int, i1 = i0 + 1;
    if (i1 >= v->nsamp) i1 = (v->loops && v->loop_start>=0) ? v->loop_start : i0;
    int frac = v->pos_frac;
    int s = (v->data[i0] * (4096 - frac) + v->data[i1] * frac) >> 12;
    s = (s * (int16_t)v->env) >> 15;
    *outL = (s * v->vol_l) >> 15;
    *outR = (s * v->vol_r) >> 15;
    v->pos_frac += v->pitch;
    v->pos_int  += v->pos_frac >> 12;
    v->pos_frac &= 0xFFF;
    if (v->pos_int >= (uint32_t)v->nsamp) {
        if (v->loops && v->loop_start >= 0) {
            v->pos_int = v->loop_start + (v->pos_int - v->nsamp);
            if (v->pos_int >= (uint32_t)v->nsamp) v->pos_int = v->loop_start;
        } else {
            v->active = 0;
        }
    }
    voice_run_env(v);
}

// ===========================================================================================
//  SEP SEQUENCER ('pQES')
// ===========================================================================================
static int varlen(NaSeq* s) {
    int v = 0; for (;;) { int b = s->buf[s->p++]; v = (v << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
    return v;
}

static NaVoice* seq_alloc_voice(NaSeq* s) {
    NaVoice* best = NULL; uint64_t oldest = ~0ull;
    for (int i = 0; i < NA_NVOICE; i++) {
        if (!s->voice[i].active) return &s->voice[i];
        if (s->voice[i].age < oldest) { oldest = s->voice[i].age; best = &s->voice[i]; }
    }
    return best;
}

static void seq_note_on(NaSeq* s, int ch, int note, int vel) {
    if (vel == 0) {
        for (int i = 0; i < NA_NVOICE; i++)
            if (s->voice[i].active && s->voice[i].chan == ch && s->voice[i].note == note
                && s->voice[i].phase != NA_ADSR_RELEASE)
                s->voice[i].phase = NA_ADSR_RELEASE, s->voice[i].divider = 0;
        return;
    }
    // Tone selection uses the per-SEQUENCE program-set selector slot[0x26] (the SEQ byte at file
    // offset 0x0F, = 0 for every Tomba!2 sequence), NOT the live 0xCn MIDI program. RE: keyon
    // 0x80090094 passes slot+0x26 as the resolve program; the 0xCn handler writes slot[ch+0x37]
    // (used for vol/pan only). This is DATA-PROVEN: the area VABs have only 4 programs, but the
    // sequences emit 0xCn programs 9..17 — those don't index ProgAttr, so using the live program
    // dropped EVERY note (the universal-silence bug). With slot[0x26] (=program 0) all 10 area
    // sequences synth audibly. (Confirms spec §5b; contradicts the handoff's "0xCn drives tone".)
    // Use the LIVE per-channel 0xCn program when the bound VAB actually has it; fall back to the
    // per-sequence selector slot[0x26] otherwise. The area songs program-change to 1..15, which exist
    // in area VAB bank1 (0x38d4, ps=18) but NOT bank0 (0x26b4, ps=4) or TOMBA2.SND's VABs (ps<=2) — so
    // binding bank1 + honoring the live program plays the real instruments. Against a too-small VAB the
    // live program would index out of range and drop every note (the bug the old prog_set=0 forcing hid);
    // the fallback keeps such VABs (the sound test's ps<=2 banks) audible. RE: docs/journal later-220.
    int prog = s->prog[ch];
    if (prog < 0 || prog >= s->vab->ps) prog = s->prog_set;
    int tones[16];
    int ntone = prog_pick_tones(s->vab, prog, note, tones, 16);
    if (ntone == 0) return;
    for (int k = 0; k < ntone; k++) {
        NaTone t; na_tone_read(s->vab, tones[k], &t);
        NaVoice* vc = seq_alloc_voice(s);
        voice_keyon(vc, s, &t, note, vel, ch, s->vol[ch], s->pan[ch], ++s->age);
    }
}

static void seq_note_off(NaSeq* s, int ch, int note) {
    for (int i = 0; i < NA_NVOICE; i++)
        if (s->voice[i].active && s->voice[i].chan == ch && s->voice[i].note == note
            && s->voice[i].phase != NA_ADSR_RELEASE)
            s->voice[i].phase = NA_ADSR_RELEASE, s->voice[i].divider = 0;
}

// dispatch one event at s->p; returns 0 on end-of-track, 1 otherwise.
static int seq_event(NaSeq* s) {
    int status = s->buf[s->p];
    if (status & 0x80) s->p++; else status = s->running_status;
    s->running_status = status;
    int hi = status & 0xF0, ch = status & 0x0F;
    switch (hi) {
        case 0x90: { int n=s->buf[s->p++], v=s->buf[s->p++]; seq_note_on(s,ch,n,v); break; }
        case 0x80: { int n=s->buf[s->p++], v=s->buf[s->p++]; (void)v; seq_note_off(s,ch,n); break; }
        case 0xB0: { int cc=s->buf[s->p++], v=s->buf[s->p++];
                     if (cc==7) s->vol[ch]=v; else if (cc==10) s->pan[ch]=v; break; }
        case 0xC0: { s->prog[ch]=s->buf[s->p++]; break; }
        case 0xE0: s->p += 2; break;
        case 0xF0: {
            int type = s->buf[s->p++];
            if (type == 0x2F) return 0;
            s->tempo_us = (double)((s->buf[s->p]<<16)|(s->buf[s->p+1]<<8)|s->buf[s->p+2]);
            s->p += 3;
            break;
        }
        default:
            return 0;
    }
    return 1;
}

int na_seq_open(NaSeq* s, const uint8_t* buf, NaVab* vab, long seqOff) {
    memset(s, 0, sizeof(*s));
    s->buf = buf; s->vab = vab;
    for (int i = 0; i < 16; i++) { s->prog[i]=0; s->vol[i]=100; s->pan[i]=64; }
    if (memcmp(buf+seqOff, "pQES", 4)) return -1;
    s->ppqn     = rd16be(buf, seqOff + 8);
    s->tempo_us = (double)rd24be(buf, seqOff + 10);
    s->prog_set = buf[seqOff + 15];
    s->p = seqOff + 16;
    s->running_status = 0;
    s->ended = 0;
    return 0;
}

void na_seq_free(NaSeq* s) {
    for (int i = 0; i < 256; i++) { free(s->vagcache[i].data); s->vagcache[i].data = NULL; }
}

static double seq_spt(NaSeq* s, int rate) { return s->tempo_us * rate / (s->ppqn * 1.0e6); }

// Render up to nframes stereo frames; advances sequencer + voices. The SEP stream is
// [event][delta] pairs (delta-to-next FOLLOWS each event; first event has no leading delta).
// State carries across calls via s->pending / s->ended so real-time chunked rendering works.
int na_seq_render(NaSeq* s, int16_t* out, int nframes, int rate) {
    int produced = 0;
    while (produced < nframes) {
        // dispatch events whose trailing delta has elapsed
        while (!s->ended && s->_pending <= 0.0) {
            if (!seq_event(s)) { s->ended = 1; break; }
            s->_pending += (double)varlen(s) * seq_spt(s, rate);
        }
        int L = 0, R = 0, any = 0;
        for (int i = 0; i < NA_NVOICE; i++) {
            if (!s->voice[i].active) continue;
            any = 1; int l, r; voice_render(&s->voice[i], &l, &r); L += l; R += r;
        }
        L = (L * 3) >> 2; R = (R * 3) >> 2;
        if (L > 32767) L = 32767; else if (L < -32768) L = -32768;
        if (R > 32767) R = 32767; else if (R < -32768) R = -32768;
        out[produced*2] = (int16_t)L; out[produced*2+1] = (int16_t)R;
        produced++;
        s->_pending -= 1.0;
        if (s->ended && !any) break;   // song over and all voices drained
    }
    return produced;
}
