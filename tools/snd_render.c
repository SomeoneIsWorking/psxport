// tools/snd_render.c — STANDALONE offline renderer for Tomba!2 native audio engine work.
//
// Phase-0/1 verification harness for the PC-native audio rewrite (replacing interpreted
// libsnd + Beetle spu.c). It parses the real TOMBA2.SND container, a chosen VAB sound bank,
// and PSX ADPCM (VAG) samples, decodes them with the native ADPCM decoder, and writes a WAV
// — NO game runtime, NO PSX emulation. This lets the native VAB parser + ADPCM decoder (and
// later the SEP sequencer + voice synth) be developed and verified offline against the Beetle
// reference WAVs (scratch/wav/*.wav captured via PSXPORT_WAV).
//
// Container layout (see scratch/snd_container_re.md, ground-truthed from the raw file):
//   0x00000  u16[24]  VAB offset table (entry = VAB start in sectors, *0x800)
//   0x00030  SEQ section (SEP 'pQES' sequences, concatenated)
//   0x51800  24 VABs ('pBAV'), each at table[i]*0x800
// VAB layout (CONFIRMED from raw VAB0):
//   +0x00 VabHdr(32)  : 'pBAV', u32 ver, u32 id, u32 fsize, u16 rsv, u16 ps(#prog),
//                       u16 ts(#tone), u16 vs(#vag), u8 mvol, u8 pan, ...
//   +0x20 ProgAttr[128] (16B each): +0 u8 #tones, ...
//   +0x820 ToneAttr[ps*16] (32B each)
//   +0x20+0x800+ps*512  VAG size table (256 u16, entry=size>>3, [0] reserved)
//   then ADPCM body (cumulative per VAG)
//
// Build: gcc -O2 -o build/tools/snd_render tools/snd_render.c
// Usage:
//   snd_render <TOMBA2.SND> info                       # list VABs + programs/tones
//   snd_render <TOMBA2.SND> vab <vabIdx> info          # dump one VAB's programs/tones
//   snd_render <TOMBA2.SND> vag <vabIdx> <vagIdx> <out.wav>   # decode one VAG -> WAV @44100
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t* g_buf; static long g_size;

static uint8_t* load(const char* path, long* n) {
    FILE* f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(sz); if (fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr,"short read\n"); exit(1);} fclose(f);
    *n = sz; return b;
}
static uint16_t rd16(long o){ return g_buf[o] | (g_buf[o+1]<<8); }
static uint32_t rd32(long o){ return g_buf[o] | (g_buf[o+1]<<8) | (g_buf[o+2]<<16) | ((uint32_t)g_buf[o+3]<<24); }

// ---- PSX ADPCM (VAG) decoder -------------------------------------------------------------
// Standard 16-byte blocks: byte0 = shift|filter<<4, byte1 = flags, bytes 2..15 = 28 nibbles.
// 2-tap predictor; coefficient table is the canonical PSX set.
static const int adpcm_pos[5] = { 0, 60, 115, 98, 122 };
static const int adpcm_neg[5] = { 0,  0, -52, -55, -60 };

// Decode `nbytes` of ADPCM at file offset `off` into out[] (int16, mono). Returns sample count.
// Stops early at an END block flag (bit0 of the flags byte). hist persists naturally (local).
static int adpcm_decode(long off, int nbytes, int16_t* out, int outcap) {
    int h1 = 0, h2 = 0, n = 0;
    for (int blk = 0; blk + 16 <= nbytes; blk += 16) {
        long b = off + blk;
        int shift  = g_buf[b] & 0x0F;
        int filter = (g_buf[b] >> 4) & 0x0F;
        int flags  = g_buf[b+1];
        if (filter > 4) filter = 0;            // guard (unused filter indices)
        if (shift > 12) shift = 9;             // PSX clamps weird shifts
        for (int i = 0; i < 14; i++) {
            int byte = g_buf[b + 2 + i];
            for (int half = 0; half < 2; half++) {
                int nib = half ? (byte >> 4) : (byte & 0x0F);
                int s = (int16_t)(nib << 12);  // 4-bit into top, sign-extended via int16
                s >>= shift;
                s += (h1 * adpcm_pos[filter] + h2 * adpcm_neg[filter]) >> 6;
                if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
                if (n < outcap) out[n++] = (int16_t)s;
                h2 = h1; h1 = s;
            }
        }
        if (flags & 0x01) break;               // end block
    }
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
    fprintf(stderr,"[snd_render] wrote %s: %d frames %.2fs @%dHz\n", path, frames, (double)frames/rate, rate);
}

// ---- VAB access --------------------------------------------------------------------------
typedef struct { long base; int ver,id,fsize,ps,ts,vs; long vagtab, body; } Vab;
static long vab_file_off(int idx) { return (long)rd16(idx*2) * 0x800; }
static void vab_open(Vab* v, int idx) {
    long o = vab_file_off(idx); v->base = o;
    if (memcmp(g_buf+o, "pBAV", 4)) { fprintf(stderr,"VAB %d @0x%lx: bad magic\n", idx, o); exit(1); }
    v->ver=rd32(o+4); v->id=rd32(o+8); v->fsize=rd32(o+12);
    v->ps=rd16(o+16+2); v->ts=rd16(o+16+4); v->vs=rd16(o+16+6);
    v->vagtab = o + 0x20 + 128*16 + (long)v->ps*16*32;
    v->body   = v->vagtab + 512;
}
// VAG idx is 1-based (table[0] reserved). Returns file offset + size in *sz.
static long vag_off(Vab* v, int idx, int* sz) {
    long off = 0; for (int i = 1; i < idx; i++) off += (long)rd16(v->vagtab + i*2) * 8;
    *sz = (int)rd16(v->vagtab + idx*2) * 8; return v->body + off;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr,"usage: %s <TOMBA2.SND> info|vab|vag ...\n", argv[0]); return 1; }
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
    if (!strcmp(cmd, "vab")) {
        int idx = atoi(argv[3]); Vab v; vab_open(&v, idx);
        printf("VAB %d: prog=%d tone=%d vag=%d  vagtab@0x%lx body@0x%lx\n", idx, v.ps, v.ts, v.vs, v.vagtab, v.body);
        long prog = v.base + 0x20;
        for (int p = 0; p < 128; p++) {
            int nt = g_buf[prog + p*16]; if (!nt) continue;
            printf("  prog %d: %d tones\n", p, nt);
            long tone = v.base + 0x820;  // NB tones are concatenated per used program
        }
        // dump tones linearly
        long tone = v.base + 0x820;
        for (int t = 0; t < v.ts; t++) {
            long a = tone + (long)t*32;
            int vol=g_buf[a+2], pan=g_buf[a+3], centre=g_buf[a+4], shift=(int8_t)g_buf[a+5];
            int adsr1=rd16(a+0x12), adsr2=rd16(a+0x14), vag=g_buf[a+0x16];
            printf("   tone %2d: vol=%d pan=%d centre=%d shift=%d adsr=%04x/%04x vag=%d\n",
                   t, vol, pan, centre, shift, adsr1, adsr2, vag);
        }
        return 0;
    }
    if (!strcmp(cmd, "vag")) {
        int vi = atoi(argv[3]), gi = atoi(argv[4]); const char* out = argv[5];
        Vab v; vab_open(&v, vi); int sz; long off = vag_off(&v, gi, &sz);
        static int16_t pcm[8*1024*1024];
        int n = adpcm_decode(off, sz, pcm, sizeof(pcm)/2);
        fprintf(stderr,"[snd_render] VAB%d VAG%d @0x%lx size=%dB -> %d samples\n", vi, gi, off, sz, n);
        wav_write(out, pcm, n, 1, 44100);
        return 0;
    }
    fprintf(stderr,"unknown cmd %s\n", cmd); return 1;
}
