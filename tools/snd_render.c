// tools/snd_render.c — STANDALONE offline renderer for Tomba!2 native audio engine work.
//
// Phase-0/1/2 verification harness for the PC-native audio rewrite (replacing interpreted
// libsnd + Beetle spu.c). It parses the real TOMBA2.SND container, a chosen VAB sound bank,
// PSX ADPCM (VAG) samples, and the SEP ('pQES') sequences — then SYNTHESIZES voices (ADSR +
// pitch + vol/pan) and runs the per-tick SEP sequencer, rendering a full song to a WAV.
// NO game runtime, NO PSX emulation. This lets the native VAB parser + ADPCM decoder + voice
// synth + sequencer be developed and verified offline against the Beetle reference WAVs
// (scratch/wav/*.wav captured via PSXPORT_WAV) before being lifted into engine/audio/.
//
// Container layout (see scratch/snd_container_re.md, ground-truthed from the raw file):
//   0x00000  u16[24]  VAB offset table (entry = VAB start in sectors, *0x800)
//   0x00030  SEQ section (SEP 'pQES' sequences, concatenated, BIG-ENDIAN headers)
//   0x51800  24 VABs ('pBAV'), each at table[i]*0x800
// VAB layout (CONFIRMED from raw VAB0):
//   +0x00 VabHdr(32)  : 'pBAV', u32 ver, u32 id, u32 fsize, u16 rsv, u16 ps(#prog),
//                       u16 ts(#tone), u16 vs(#vag), u8 mvol, u8 pan, ...
//   +0x20 ProgAttr[128] (16B each): +0 u8 #tones, ...
//   +0x820 ToneAttr[ps*16] (32B each), concatenated across used programs.
//          ToneAttr fields (CONFIRMED from raw bytes, standard PSX VagAtr):
//          +0x00 prior, +0x01 mode, +0x02 vol, +0x03 pan, +0x04 centre-note,
//          +0x05 shift(fine), +0x06 min-note, +0x07 max-note, ...,
//          +0x10 u16 adsr1, +0x12 u16 adsr2, +0x14 u16 prog, +0x16 u16 vag(1-based).
//   +0x20+0x800+ps*512  VAG size table (256 u16, entry=size>>3, [0] reserved)
//   then ADPCM body (cumulative per VAG)
//
// Build: gcc -O2 -w -o build/tools/snd_render tools/snd_render.c -lm
// Usage:
//   snd_render <SND> info                          # list VABs
//   snd_render <SND> vab <vabIdx>                   # dump one VAB's programs/tones
//   snd_render <SND> seqs                           # list SEP sequences
//   snd_render <SND> vag <vabIdx> <vagIdx> <out>    # decode one VAG -> WAV @44100
//   snd_render <SND> tone <vabIdx> <toneIdx> <note> <out>  # synth one tone at MIDI note -> WAV
//   snd_render <SND> song <seqIdx> <vabIdx> <out> [seconds]  # render a full SEP song -> WAV
//   snd_render <SND> songid <id> <out> [seconds]   # render BGM song <id> via the game's
//        song->(seq,vab) table (RE'd from MAIN.EXE FUN_80075448 open block, §6).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint8_t* g_buf; static long g_size;

static uint8_t* load(const char* path, long* n) {
    FILE* f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(sz); if (fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr,"short read\n"); exit(1);} fclose(f);
    *n = sz; return b;
}
static uint16_t rd16(long o){ return g_buf[o] | (g_buf[o+1]<<8); }
static uint32_t rd32(long o){ return g_buf[o] | (g_buf[o+1]<<8) | (g_buf[o+2]<<16) | ((uint32_t)g_buf[o+3]<<24); }
// big-endian readers for the SEP stream
static uint16_t rd16be(long o){ return (g_buf[o]<<8) | g_buf[o+1]; }
static uint32_t rd24be(long o){ return (g_buf[o]<<16) | (g_buf[o+1]<<8) | g_buf[o+2]; }
static uint32_t rd32be(long o){ return ((uint32_t)g_buf[o]<<24) | (g_buf[o+1]<<16) | (g_buf[o+2]<<8) | g_buf[o+3]; }

// ---- PSX ADPCM (VAG) decoder -------------------------------------------------------------
// Standard 16-byte blocks: byte0 = shift|filter<<4, byte1 = flags, bytes 2..15 = 28 nibbles.
// 2-tap predictor; coefficient table is the canonical PSX set.
static const int adpcm_pos[5] = { 0, 60, 115, 98, 122 };
static const int adpcm_neg[5] = { 0,  0, -52, -55, -60 };

// Fully decode an ADPCM stream into out[]. Tracks the loop-start sample index and whether the
// stream ends with a loop-jump (BGM/sustained) vs a hard end (one-shot). Returns sample count.
//   *loop_start = sample index of the loop point (-1 if none)
//   *loops      = 1 if the END block requested a loop jump (flags 0x03), 0 for a plain end.
static int adpcm_decode_full(long off, int nbytes, int16_t* out, int outcap,
                             int* loop_start, int* loops) {
    int h1 = 0, h2 = 0, n = 0; *loop_start = -1; *loops = 0;
    for (int blk = 0; blk + 16 <= nbytes; blk += 16) {
        long b = off + blk;
        int shift  = g_buf[b] & 0x0F;
        int filter = (g_buf[b] >> 4) & 0x0F;
        int flags  = g_buf[b+1];
        if (filter > 4) filter = 0;
        if (shift > 12) shift = 9;
        if (flags & 0x04) *loop_start = n;     // loop-start marker -> remember this sample
        for (int i = 0; i < 14; i++) {
            int byte = g_buf[b + 2 + i];
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
        if (flags & 0x01) {                    // end block
            if ((flags & 0x03) == 0x03) *loops = 1;  // 0x03 = loop-end -> jump to loop start
            break;
        }
    }
    if (*loops && *loop_start < 0) *loop_start = 0;
    return n;
}

// ---- WAV out -----------------------------------------------------------------------------
static void wav_write(const char* path, const int16_t* pcm, int frames, int ch, int rate) {
    FILE* f = fopen(path, "wb"); if (!f) { perror(path); exit(1); }
    uint32_t data = (uint32_t)frames * ch * 2, riff = 36 + data;
    fwrite("RIFF",1,4,f); fputc(riff&0xff,f);fputc((riff>>8)&0xff,f);fputc((riff>>16)&0xff,f);fputc((riff>>24)&0xff,f);
    fwrite("WAVEfmt ",1,8,f); { uint32_t v=16; fwrite(&v,4,1,f);}
    { uint16_t v=1; fwrite(&v,2,1,f); } { uint16_t v=ch; fwrite(&v,2,1,f);}
    { uint32_t v=rate; fwrite(&v,4,1,f);} { uint32_t v=rate*ch*2; fwrite(&v,4,1,f);}
    { uint16_t v=ch*2; fwrite(&v,2,1,f);} { uint16_t v=16; fwrite(&v,2,1,f);}
    fwrite("data",1,4,f); fputc(data&0xff,f);fputc((data>>8)&0xff,f);fputc((data>>16)&0xff,f);fputc((data>>24)&0xff,f);
    fwrite(pcm,2,(size_t)frames*ch,f); fclose(f);
    fprintf(stderr,"[snd_render] wrote %s: %d frames %.2fs @%dHz %dch\n", path, frames, (double)frames/rate, rate, ch);
}

// ---- VAB access --------------------------------------------------------------------------
typedef struct { long base; int ver,id,fsize,ps,ts,vs; long progtab, tonetab, vagtab, body; } Vab;
static long vab_file_off(int idx) { return (long)rd16(idx*2) * 0x800; }
static void vab_open(Vab* v, int idx) {
    long o = vab_file_off(idx); v->base = o;
    if (memcmp(g_buf+o, "pBAV", 4)) { fprintf(stderr,"VAB %d @0x%lx: bad magic\n", idx, o); exit(1); }
    v->ver=rd32(o+4); v->id=rd32(o+8); v->fsize=rd32(o+12);
    v->ps=rd16(o+16+2); v->ts=rd16(o+16+4); v->vs=rd16(o+16+6);
    v->progtab = o + 0x20;                       // ProgAttr[128]
    v->tonetab = o + 0x820;                       // ToneAttr[ts] concatenated by program
    v->vagtab = o + 0x20 + 128*16 + (long)v->ps*16*32;
    v->body   = v->vagtab + 512;
}
// VAG idx is 1-based (table[0] reserved). Returns file offset + size in *sz.
static long vag_off(Vab* v, int idx, int* sz) {
    long off = 0; for (int i = 1; i < idx; i++) off += (long)rd16(v->vagtab + i*2) * 8;
    *sz = (int)rd16(v->vagtab + idx*2) * 8; return v->body + off;
}

// ToneAttr accessors (32B each, indexed across the concatenated tone array).
typedef struct { int vol,pan,centre,shift,nmin,nmax,adsr1,adsr2,prog,vag; } Tone;
static void tone_read(Vab* v, int t, Tone* o) {
    long a = v->tonetab + (long)t*32;
    o->vol=g_buf[a+2]; o->pan=g_buf[a+3]; o->centre=g_buf[a+4]; o->shift=(int8_t)g_buf[a+5];
    o->nmin=g_buf[a+6]; o->nmax=g_buf[a+7];
    o->adsr1=rd16(a+0x10); o->adsr2=rd16(a+0x12); o->prog=rd16(a+0x14); o->vag=g_buf[a+0x16];
}
// Cumulative tone-start index for program p (tones concatenated in program order).
// This mirrors libsnd's VabOpenHead transform: it loads the on-disc concatenated tones into
// a fixed 16-tone-per-program image and rewrites ProgAttr+8 to the program's tone base. The
// note-on resolver (0x80095C40) reads `baseTone = ProgAttr[prog]+8` then scans tones
// `ToneAttr[(baseTone*16 + i)]` for i in 0..nTones. On disc baseTone == the program's
// cumulative tone start; for the shipped VABs prog0 starts at 0 (so baseTone(prog0)=0).
static int prog_tone_start(Vab* v, int p) {
    int start = 0; for (int q = 0; q < p; q++) start += g_buf[v->progtab + q*16];
    return start;
}

// AUTHORITATIVE program->tone mapping (RE'd 2026-06-23, replaces the old `p % ps` probe).
//
// The tone-selection program is NOT the live MIDI prog-change. It is the per-SEQUENCE
// program-set selector slot[0x26], set ONCE at SsSeqOpen from the SEQ byte at file offset
// 0x0F (the byte right after the 15-byte 'pQES' header). For EVERY Tomba!2 sequence that byte
// is 0x00, so tone selection always uses VAB program 0. (Verified: 0x80090094 `lb a1,38(v1)`
// passes slot+0x26 as the resolve program index; the 0xCn prog-change writes slot[ch+0x37],
// used only for vol/pan, NOT for tone selection — it never updates slot+0x26 at runtime.)
//
// Tone scan (0x80095C40) is MULTI-VOICE: EVERY tone of the program whose [min,max] note range
// covers `note` keys on its own voice. Returns the matching tone indices into `out` (capacity
// `cap`), count via the return value. 0 matches = note dropped (genuine libsnd behavior when
// the program has no tone for that note — e.g. note outside the VAB's covered range).
static int prog_pick_tones(Vab* v, int prog, int note, int* out, int cap) {
    if (prog < 0 || prog >= v->ps) return 0;          // program out of range -> note dropped
    int nt = g_buf[v->progtab + prog*16]; if (!nt) return 0;
    int start = prog_tone_start(v, prog);
    int n = 0;
    for (int i = 0; i < nt && n < cap; i++) {
        Tone t; tone_read(v, start+i, &t);
        if (note >= t.nmin && note <= t.nmax) out[n++] = start + i;
    }
    return n;
}

// ===========================================================================================
//  VOICE SYNTH — ADSR envelope + pitch resample + vol/pan, faithful to Beetle spu.c.
// ===========================================================================================
enum { ADSR_ATTACK, ADSR_DECAY, ADSR_SUSTAIN, ADSR_RELEASE, ADSR_OFF };

typedef struct {
    int      active;
    int16_t* data; int nsamp; int loop_start; int loops;   // decoded VAG (owned)
    uint32_t pos_int, pos_frac;                            // sample cursor (frac in /4096)
    uint32_t pitch;                                        // 4096 = 1.0x (44100Hz)
    // envelope (mirrors SPU_ADSR)
    int      phase; uint16_t env; int divider;
    int      Ar, Dr, Sr, Rr, Sl, SusLevel;
    int      AttExp, RelExp, SusExp, SusDec;
    // mix
    int      vol_l, vol_r;                                 // 0..0x3fff
    int      chan, note;                                   // owning MIDI channel + note
    uint64_t age;                                          // for voice stealing
} Voice;

// Beetle CalcVCDelta (spu.c:566) — verbatim.
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

// Beetle SPU_CacheEnvelope — decode adsr1/adsr2 into voice fields.
static void voice_cache_env(Voice* v, int adsr1, int adsr2) {
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

// Beetle SPU_RunEnvelope (spu.c:840) — one envelope clock; updates v->env / v->phase.
static void voice_run_env(Voice* v) {
    int inc = 0, divinco = 0; int16_t uoflow = 0;
    if (v->phase == ADSR_ATTACK && v->env == 0x7FFF) v->phase++;
    switch (v->phase) {
        case ADSR_ATTACK:
            calc_vc_delta(0x7F, v->Ar, v->AttExp, 0, 0, (int16_t)v->env, &inc, &divinco);
            uoflow = 0x7FFF; break;
        case ADSR_DECAY:
            calc_vc_delta(0x1F<<2, v->Dr, 1, 1, 1, (int16_t)v->env, &inc, &divinco);
            uoflow = 0; break;
        case ADSR_SUSTAIN:
            calc_vc_delta(0x7F, v->Sr, v->SusExp, v->SusDec, v->SusDec, (int16_t)v->env, &inc, &divinco);
            uoflow = v->SusDec ? 0 : 0x7FFF; break;
        case ADSR_RELEASE:
            calc_vc_delta(0x1F<<2, v->Rr, v->RelExp, 1, 1, (int16_t)v->env, &inc, &divinco);
            uoflow = 0; break;
        default: return;
    }
    v->divider += divinco;
    if (v->divider & 0x8000) {
        uint16_t prev = v->env;
        v->divider = 0;
        v->env = (uint16_t)(v->env + inc);
        if (v->phase == ADSR_ATTACK) {
            if (((prev ^ v->env) & v->env) & 0x8000) v->env = uoflow;
        } else {
            if (v->env & 0x8000) v->env = uoflow;
        }
        if (v->phase == ADSR_DECAY && v->env < v->SusLevel) v->phase++;
        // RELEASE that reached 0 -> voice dies
        if (v->phase == ADSR_RELEASE && v->env == 0) v->active = 0;
    }
}

// pitch (4096=1.0) for a note vs the tone centre, including fine shift (1/128 semitone units).
static uint32_t note_to_pitch(const Tone* t, int note) {
    double semis = (double)(note - t->centre) + (double)t->shift / 128.0;
    double step = pow(2.0, semis / 12.0);
    long p = (long)(step * 4096.0 + 0.5);
    if (p < 1) p = 1; if (p > 0x3FFF) p = 0x3FFF;
    return (uint32_t)p;
}

// ---- decoded-VAG cache (decode each VAG once per render) ----------------------------------
typedef struct { int16_t* data; int nsamp, loop_start, loops; } VagPcm;
static VagPcm g_vagcache[256];
static VagPcm* vag_get(Vab* v, int vagIdx) {
    if (vagIdx < 1 || vagIdx > 255) return NULL;
    VagPcm* c = &g_vagcache[vagIdx];
    if (c->data) return c;
    int sz; long off = vag_off(v, vagIdx, &sz);
    if (sz <= 0) return NULL;
    int cap = (sz/16)*28 + 64;
    c->data = malloc(cap * 2);
    c->nsamp = adpcm_decode_full(off, sz, c->data, cap, &c->loop_start, &c->loops);
    return c;
}

// keyon a voice with a chosen tone + note + velocity, plus channel vol/pan (0..127, 64=centre).
static void voice_keyon(Voice* vc, Vab* vab, const Tone* t, int note, int vel,
                        int chan, int chan_vol, int chan_pan, uint64_t age) {
    VagPcm* g = vag_get(vab, t->vag);
    if (!g || !g->data) { vc->active = 0; return; }
    memset(vc, 0, sizeof(*vc));
    vc->active = 1; vc->age = age;
    vc->data = g->data; vc->nsamp = g->nsamp; vc->loop_start = g->loop_start; vc->loops = g->loops;
    vc->pos_int = 0; vc->pos_frac = 0;
    vc->pitch = note_to_pitch(t, note);
    voice_cache_env(vc, t->adsr1, t->adsr2);
    vc->phase = ADSR_ATTACK; vc->env = 0; vc->divider = 0;
    vc->chan = chan; vc->note = note;
    // combined amplitude: note velocity * tone vol * channel vol, normalized to 0..0x3fff.
    double amp = (double)vel/127.0 * (double)t->vol/127.0 * (double)chan_vol/127.0;
    // pan blend: tone pan combined with channel pan, both 0..127 (64=centre). simple constant-power.
    double pan = ((double)t->pan + (double)chan_pan) * 0.5 / 127.0;   // 0..1
    if (pan < 0) pan = 0; if (pan > 1) pan = 1;
    double l = cos(pan * M_PI / 2.0), r = sin(pan * M_PI / 2.0);
    vc->vol_l = (int)(amp * l * 0x3FFF);
    vc->vol_r = (int)(amp * r * 0x3FFF);
}

// render one output sample (44100) from a voice; advances its cursor + envelope.
static void voice_render(Voice* v, int* outL, int* outR) {
    if (!v->active) { *outL = *outR = 0; return; }
    // linear interpolation between pos_int and pos_int+1
    int i0 = v->pos_int, i1 = i0 + 1;
    if (i1 >= v->nsamp) i1 = (v->loops && v->loop_start>=0) ? v->loop_start : i0;
    int frac = v->pos_frac;
    int s = (v->data[i0] * (4096 - frac) + v->data[i1] * frac) >> 12;
    s = (s * (int16_t)v->env) >> 15;
    *outL = (s * v->vol_l) >> 15;
    *outR = (s * v->vol_r) >> 15;
    // advance cursor
    v->pos_frac += v->pitch;
    v->pos_int  += v->pos_frac >> 12;
    v->pos_frac &= 0xFFF;
    if (v->pos_int >= (uint32_t)v->nsamp) {
        if (v->loops && v->loop_start >= 0) {
            v->pos_int = v->loop_start + (v->pos_int - v->nsamp);
            if (v->pos_int >= (uint32_t)v->nsamp) v->pos_int = v->loop_start;
        } else {
            v->active = 0;   // one-shot ran out
        }
    }
    voice_run_env(v);
}

// ===========================================================================================
//  SEP SEQUENCER ('pQES') — per-tick MIDI-style interpreter driving native voices.
// ===========================================================================================
#define NVOICE 24
typedef struct {
    Vab* vab;
    Voice voice[NVOICE];
    uint64_t age;
    // per-channel state
    int prog[16], vol[16], pan[16];
    int prog_set;        // slot[0x26]: per-SEQUENCE program-set selector (tone-selection program)
    // stream cursor
    long p, end;
    int  running_status;
    double tempo_us;     // microseconds per quarter
    int    ppqn;
} Seq;

static int varlen(Seq* s) {            // MIDI variable-length quantity
    int v = 0; for (;;) { int b = g_buf[s->p++]; v = (v << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
    return v;
}

static Voice* seq_alloc_voice(Seq* s) {
    Voice* best = NULL; uint64_t oldest = ~0ull;
    for (int i = 0; i < NVOICE; i++) {
        if (!s->voice[i].active) return &s->voice[i];
        if (s->voice[i].age < oldest) { oldest = s->voice[i].age; best = &s->voice[i]; }
    }
    return best;   // steal oldest
}

static void seq_note_on(Seq* s, int ch, int note, int vel) {
    if (vel == 0) {  // running-status note-off
        for (int i = 0; i < NVOICE; i++)
            if (s->voice[i].active && s->voice[i].chan == ch && s->voice[i].note == note
                && s->voice[i].phase != ADSR_RELEASE)
                s->voice[i].phase = ADSR_RELEASE, s->voice[i].divider = 0;
        return;
    }
    // Tone selection uses the per-SEQUENCE program-set selector (slot[0x26]), NOT the live
    // MIDI prog-change. Multi-voice: every in-range tone of that program keys on its own voice.
    int tones[16];
    int ntone = prog_pick_tones(s->vab, s->prog_set, note, tones, 16);
    if (ntone == 0) return;   // no tone covers this note in the program -> note dropped (libsnd)
    for (int k = 0; k < ntone; k++) {
        Tone t; tone_read(s->vab, tones[k], &t);
        Voice* vc = seq_alloc_voice(s);
        voice_keyon(vc, s->vab, &t, note, vel, ch, s->vol[ch], s->pan[ch], ++s->age);
        if (getenv("SND_DBG")) fprintf(stderr,"keyon ch=%d note=%d vel=%d pset=%d tone=%d vag=%d active=%d vol_l=%d pitch=%u\n",
                                       ch,note,vel,s->prog_set,tones[k],t.vag,vc->active,vc->vol_l,vc->pitch);
    }
}

static void seq_note_off(Seq* s, int ch, int note) {
    for (int i = 0; i < NVOICE; i++)
        if (s->voice[i].active && s->voice[i].chan == ch && s->voice[i].note == note
            && s->voice[i].phase != ADSR_RELEASE)
            s->voice[i].phase = ADSR_RELEASE, s->voice[i].divider = 0;
}

// dispatch one event at s->p; returns 0 on end-of-track, 1 otherwise.
static int seq_event(Seq* s) {
    int status = g_buf[s->p];
    if (status & 0x80) s->p++; else status = s->running_status;  // running status
    s->running_status = status;
    int hi = status & 0xF0, ch = status & 0x0F;
    static int dbgn; if (getenv("SND_DBG") && dbgn++ < 20) fprintf(stderr,"  ev @0x%lx status=%02x hi=%02x\n", s->p, status, hi);
    switch (hi) {
        case 0x90: { int n=g_buf[s->p++], v=g_buf[s->p++]; seq_note_on(s,ch,n,v); break; }
        case 0x80: { int n=g_buf[s->p++], v=g_buf[s->p++]; (void)v; seq_note_off(s,ch,n); break; }
        case 0xB0: { int cc=g_buf[s->p++], v=g_buf[s->p++];
                     if (cc==7) s->vol[ch]=v; else if (cc==10) s->pan[ch]=v; break; }
        case 0xC0: { int pr=g_buf[s->p++]; s->prog[ch]=pr; break; }
        case 0xA0: case 0xE0: s->p += 2; break;            // poly-aftertouch / pitchbend (ignored v1)
        case 0xD0: s->p += 1; break;                       // channel aftertouch
        case 0xF0:
            if (status == 0xFF) {                          // meta
                int type = g_buf[s->p++]; int len = varlen(s);
                long body = s->p; s->p += len;
                if (type == 0x2F) return 0;                // end of track
                if (type == 0x51 && len == 3)              // tempo
                    s->tempo_us = (double)((g_buf[body]<<16)|(g_buf[body+1]<<8)|g_buf[body+2]);
            } else {
                // SysEx / unknown F0 — skip a varlen-framed blob if present
                int len = varlen(s); s->p += len;
            }
            break;
        default:
            // resync failure — bail
            return 0;
    }
    return 1;
}

static void seq_open(Seq* s, Vab* vab, long seqOff) {
    memset(s, 0, sizeof(*s));
    s->vab = vab;
    for (int i = 0; i < 16; i++) { s->prog[i]=0; s->vol[i]=100; s->pan[i]=64; }
    if (memcmp(g_buf+seqOff, "pQES", 4)) { fprintf(stderr,"seq @0x%lx bad magic\n", seqOff); exit(1); }
    s->ppqn    = rd16be(seqOff + 8);
    s->tempo_us= (double)rd24be(seqOff + 10);
    // After the 15-byte header ('pQES'4 + ver4 + ppqn2 + tempo3 + rhythm2) SsSeqOpen consumes
    // ONE byte (file offset 0x0F) into slot[0x26] = the program-set selector (the tone-selection
    // program). The SEP event stream proper then begins at offset 0x10. (RE: 0x8008E3DC reads
    // that byte and advances the read pointer; 0x80090094 passes slot+0x26 as the resolve prog.)
    s->prog_set = g_buf[seqOff + 15];
    s->p = seqOff + 16;
    s->running_status = 0;
}

// samples per MIDI tick at the current tempo.
static double seq_spt(Seq* s, int rate) { return s->tempo_us * rate / (s->ppqn * 1.0e6); }

// Render the whole song to a stereo WAV. The SEP stream is [event][delta][event][delta]...
// (the delta-to-next FOLLOWS each event; the first event has NO leading delta — verified
// from the raw stream). So: dispatch an event, read its trailing delta, wait that many
// samples, dispatch the next event. Stops at end-of-track (draining a release tail) or at
// max_frames if the song loops past it.
static void seq_render(Seq* s, int16_t* out, int outcap_frames, int rate, int max_frames,
                       int* out_frames) {
    int frames = 0, ended = 0, tail = 0;
    double pending = 0.0;   // first event fires immediately (no leading delta)
    while (frames < outcap_frames && frames < max_frames) {
        // dispatch every event whose (trailing) delta has elapsed
        while (!ended && pending <= 0.0) {
            if (!seq_event(s)) { ended = 1; break; }
            pending += (double)varlen(s) * seq_spt(s, rate);  // delta-to-next FOLLOWS the event
        }
        // mix the 24 voices for this output frame
        int L = 0, R = 0, any = 0;
        for (int i = 0; i < NVOICE; i++) {
            if (!s->voice[i].active) continue;
            any = 1; int l, r; voice_render(&s->voice[i], &l, &r); L += l; R += r;
        }
        L = (L * 3) >> 2; R = (R * 3) >> 2;   // master scale + clamp
        if (L > 32767) L = 32767; else if (L < -32768) L = -32768;
        if (R > 32767) R = 32767; else if (R < -32768) R = -32768;
        out[frames*2] = (int16_t)L; out[frames*2+1] = (int16_t)R;
        frames++;
        pending -= 1.0;
        if (ended) { tail++; if (!any || tail > rate) break; }   // drain up to 1s of release tail
    }
    *out_frames = frames;
}

// Render the seq'th 'pQES' sequence with VAB index vi to a stereo WAV. Shared by the
// `song` (manual indices) and `songid` (game-mapped) commands.
static int render_song(int si, int vi, const char* out, int seconds) {
    long o = 0x30; int idx = 0, found = -1;
    while (o < 0x51800) {
        if (!memcmp(g_buf+o,"pQES",4)) { if (idx==si) { found=o; break; } idx++; o+=4; } else o++;
    }
    if (found < 0) { fprintf(stderr,"seq %d not found\n", si); return 1; }
    Vab v; vab_open(&v, vi);
    Seq s; seq_open(&s, &v, found);
    fprintf(stderr,"[snd_render] song seq%d @0x%lx vab%d ppqn=%d tempo=%.0f\n",
            si, found, vi, s.ppqn, s.tempo_us);
    int rate = 44100, maxf = seconds*rate;
    int16_t* pcm = malloc((size_t)maxf*2*2);
    int frames = 0; seq_render(&s, pcm, maxf, rate, maxf, &frames);
    wav_write(out, pcm, frames, 2, rate);
    free(pcm);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr,"usage: %s <TOMBA2.SND> info|vab|seqs|vag|tone|song ...\n", argv[0]); return 1; }
    g_buf = load(argv[1], &g_size);
    const char* cmd = argv[2];

    if (!strcmp(cmd, "info")) {
        printf("container %ld bytes; VAB table:\n", g_size);
        for (int i = 0; i < 24; i++) {
            long o = vab_file_off(i); if (o<=0 || o+4>g_size) break;
            if (memcmp(g_buf+o,"pBAV",4)) continue;
            Vab v; vab_open(&v, i);
            printf("  VAB %2d @0x%06lx ver=%d id=%d fsize=%d prog=%d tone=%d vag=%d\n",
                   i, o, v.ver, v.id, v.fsize, v.ps, v.ts, v.vs);
        }
        return 0;
    }
    if (!strcmp(cmd, "seqs")) {
        long o = 0x30; int idx = 0;
        while (o < 0x51800) {
            if (!memcmp(g_buf+o, "pQES", 4)) {
                int ppqn = rd16be(o+8), tempo = rd24be(o+10);
                printf("  seq %2d @0x%05lx ppqn=%d tempo=%d (%.1fbpm)\n",
                       idx++, o, ppqn, tempo, tempo? 60e6/tempo : 0);
                o += 4;
            } else o++;
        }
        return 0;
    }
    if (!strcmp(cmd, "vab")) {
        int idx = atoi(argv[3]); Vab v; vab_open(&v, idx);
        printf("VAB %d: prog=%d tone=%d vag=%d  tonetab@0x%lx vagtab@0x%lx body@0x%lx\n",
               idx, v.ps, v.ts, v.vs, v.tonetab, v.vagtab, v.body);
        for (int p = 0; p < 128; p++) {
            int nt = g_buf[v.progtab + p*16]; if (!nt) continue;
            printf("  prog %d: %d tones (start %d)\n", p, nt, prog_tone_start(&v, p));
        }
        for (int t = 0; t < v.ts; t++) {
            Tone o; tone_read(&v, t, &o);
            printf("   tone %2d: vol=%d pan=%d centre=%d shift=%d note[%d..%d] adsr=%04x/%04x prog=%d vag=%d\n",
                   t, o.vol, o.pan, o.centre, o.shift, o.nmin, o.nmax, o.adsr1, o.adsr2, o.prog, o.vag);
        }
        return 0;
    }
    if (!strcmp(cmd, "vag")) {
        int vi = atoi(argv[3]), gi = atoi(argv[4]); const char* out = argv[5];
        Vab v; vab_open(&v, vi); int sz; long off = vag_off(&v, gi, &sz);
        static int16_t pcm[8*1024*1024];
        int ls, lp; int n = adpcm_decode_full(off, sz, pcm, sizeof(pcm)/2, &ls, &lp);
        fprintf(stderr,"[snd_render] VAB%d VAG%d @0x%lx size=%dB -> %d samples (loop_start=%d loops=%d)\n",
                vi, gi, off, sz, n, ls, lp);
        wav_write(out, pcm, n, 1, 44100);
        return 0;
    }
    if (!strcmp(cmd, "tone")) {                   // synth a single tone at a MIDI note
        int vi = atoi(argv[3]), ti = atoi(argv[4]), note = atoi(argv[5]); const char* out = argv[6];
        Vab v; vab_open(&v, vi); Tone t; tone_read(&v, ti, &t);
        Voice vc; voice_keyon(&vc, &v, &t, note, 110, 0, 100, 64, 1);
        fprintf(stderr,"[snd_render] tone %d note %d: vag=%d centre=%d adsr=%04x/%04x pitch=%u\n",
                ti, note, t.vag, t.centre, t.adsr1, t.adsr2, vc.pitch);
        static int16_t pcm[44100*4*2];
        int frames = 0, rate = 44100, keyoff_at = rate*2, end = rate*3;
        for (; frames < end && vc.active; frames++) {
            if (frames == keyoff_at) { vc.phase = ADSR_RELEASE; vc.divider = 0; }
            int l, r; voice_render(&vc, &l, &r);
            if (l>32767)l=32767; if(l<-32768)l=-32768; if(r>32767)r=32767; if(r<-32768)r=-32768;
            pcm[frames*2]=l; pcm[frames*2+1]=r;
        }
        wav_write(out, pcm, frames, 2, rate);
        return 0;
    }
    if (!strcmp(cmd, "song")) {                   // render a full SEP sequence
        int si = atoi(argv[3]), vi = atoi(argv[4]); const char* out = argv[5];
        int seconds = argc > 6 ? atoi(argv[6]) : 30;
        return render_song(si, vi, out, seconds);
    }
    if (!strcmp(cmd, "songid")) {                 // render BGM by game song id
        if (argc < 5) { fprintf(stderr,"usage: %s <SND> songid <id> <out> [seconds]\n", argv[0]); return 1; }
        int id = atoi(argv[3]); const char* out = argv[4];
        int seconds = argc > 5 ? atoi(argv[5]) : 30;
        // song->(seq scan-index, vab scan-index) RE'd from MAIN.EXE FUN_80075448 open
        // block (0x80075500..0x80075648) cross-referenced with the asset offset table at
        // TOMBA2.SND+0x51000 (slot i -> seq offset[i], identical to the linear 'pQES' scan).
        // VAB column = the runtime BANK arg (FUN_800963a0, 1-based) minus 1 => scan-index;
        // its mapping to the actual SPU VAB BODY is area/runtime-bound (see §6) -- treat the
        // vab as the BANK SLOT, verify the body live.
        static const struct { int seq, vab; } S2SV[10] = {
            {3,13},{2,13},{1,13},{0,13},  // songs 0..3: bank 14 -> vab idx 13
            {4, 7},{5, 7},{6, 7},          // songs 4..6 (dialog): bank 8 -> vab idx 7
            {7,13},{8,13},{9,13},          // songs 7..9: bank 14 -> vab idx 13
        };
        if (id < 0 || id >= 10) { fprintf(stderr,"songid %d out of range 0..9\n", id); return 1; }
        fprintf(stderr,"[snd_render] songid %d -> seq%d vab%d (bank %d)\n",
                id, S2SV[id].seq, S2SV[id].vab, S2SV[id].vab+1);
        return render_song(S2SV[id].seq, S2SV[id].vab, out, seconds);
    }
    fprintf(stderr,"unknown cmd %s\n", cmd); return 1;
}
