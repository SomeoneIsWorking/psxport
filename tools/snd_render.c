// tools/snd_render.c — STANDALONE offline renderer for the Tomba!2 native audio engine.
//
// This is the OFFLINE FRONT-END to the shared synth core in engine/audio/native_audio.c — the
// SAME VAB parser + ADPCM decoder + ADSR/pitch/pan voice synth + SEP sequencer the in-game music
// engine uses (the unification: one codebase, offline AND in-game). This file does only the bits
// the engine doesn't need: file load, the listing/inspection commands (info/vab/seqs/vag), and the
// WAV writer. The render commands (tone/song/songid/raw) drive the shared core.
//
// Container layout (see scratch/snd_container_re.md):
//   0x00000  u16[24]  VAB offset table (entry = VAB start in sectors, *0x800)
//   0x00030  SEQ section (SEP 'pQES' sequences, concatenated)
//   0x51800  24 VABs ('pBAV'), each at table[i]*0x800
//
// Build: gcc -O2 -w -o build/tools/snd_render tools/snd_render.c engine/audio/native_audio.c -lm
// Usage:
//   snd_render <SND> info                          # list VABs
//   snd_render <SND> vab <vabIdx>                   # dump one VAB's programs/tones
//   snd_render <SND> seqs                           # list SEP sequences
//   snd_render <SND> vag <vabIdx> <vagIdx> <out>    # decode one VAG -> WAV @44100
//   snd_render <SND> tone <vabIdx> <toneIdx> <note> <out>  # synth one tone at MIDI note -> WAV
//   snd_render <SND> song <seqIdx> <vabIdx> <out> [seconds] # render a full SEP song -> WAV
//   snd_render <SND> songid <id> <out> [seconds]   # render BGM song <id> (game song->seq/vab table)
//   snd_render <file> raw <seqOff_hex> <vabOff_hex> <out> [secs] # render at EXPLICIT byte offsets
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../engine/audio/native_audio.h"

static uint8_t* g_buf; static long g_size;

static uint8_t* load(const char* path, long* n) {
    FILE* f = fopen(path, "rb"); if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(sz); if (fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr,"short read\n"); exit(1);} fclose(f);
    *n = sz; return b;
}
static uint16_t rd16(long o){ return g_buf[o] | (g_buf[o+1]<<8); }
static uint16_t rd16be(long o){ return (g_buf[o]<<8) | g_buf[o+1]; }
static uint32_t rd24be(long o){ return (g_buf[o]<<16) | (g_buf[o+1]<<8) | g_buf[o+2]; }
static long vab_file_off(int idx) { return (long)rd16(idx*2) * 0x800; }

// ---- WAV out -------------------------------------------------------------------------------
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

// Render a 'pQES' at byte offset seqOff with a VAB at byte offset vabOff to a stereo WAV.
static int render_raw(long seqOff, long vabOff, const char* out, int seconds) {
    NaVab v; if (na_vab_open_at(&v, g_buf, vabOff)) { fprintf(stderr,"bad VAB @0x%lx\n", vabOff); return 1; }
    NaSeq s; if (na_seq_open(&s, g_buf, &v, seqOff)) { fprintf(stderr,"bad seq @0x%lx\n", seqOff); return 1; }
    fprintf(stderr,"[snd_render] seq@0x%lx vab@0x%lx(ps=%d ts=%d vs=%d) ppqn=%d tempo=%.0f\n",
            seqOff, vabOff, v.ps, v.ts, v.vs, s.ppqn, s.tempo_us);
    int rate = 44100, maxf = seconds*rate;
    int16_t* pcm = malloc((size_t)maxf*2*2);
    int frames = na_seq_render(&s, pcm, maxf, rate);
    wav_write(out, pcm, frames, 2, rate);
    free(pcm); na_seq_free(&s);
    return 0;
}

// Find the si'th 'pQES' by linear scan from 0x30; render it with VAB scan-index vi.
static int render_song(int si, int vi, const char* out, int seconds) {
    long o = 0x30; int idx = 0, found = -1;
    while (o < 0x51800) {
        if (!memcmp(g_buf+o,"pQES",4)) { if (idx==si) { found=o; break; } idx++; o+=4; } else o++;
    }
    if (found < 0) { fprintf(stderr,"seq %d not found\n", si); return 1; }
    return render_raw(found, vab_file_off(vi), out, seconds);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr,"usage: %s <TOMBA2.SND> info|vab|seqs|vag|tone|song|songid|raw ...\n", argv[0]); return 1; }
    g_buf = load(argv[1], &g_size);
    const char* cmd = argv[2];

    if (!strcmp(cmd, "info")) {
        printf("container %ld bytes; VAB table:\n", g_size);
        for (int i = 0; i < 24; i++) {
            long o = vab_file_off(i); if (o<=0 || o+4>g_size) break;
            if (memcmp(g_buf+o,"pBAV",4)) continue;
            NaVab v; na_vab_open_at(&v, g_buf, o);
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
        int idx = atoi(argv[3]); NaVab v; na_vab_open_at(&v, g_buf, vab_file_off(idx));
        printf("VAB %d: prog=%d tone=%d vag=%d  tonetab@0x%lx vagtab@0x%lx body@0x%lx\n",
               idx, v.ps, v.ts, v.vs, v.tonetab, v.vagtab, v.body);
        for (int p = 0; p < 128; p++) {
            int nt = g_buf[v.progtab + p*16]; if (!nt) continue;
            printf("  prog %d: %d tones (start %d)\n", p, nt, na_prog_tone_start(&v, p));
        }
        for (int t = 0; t < v.ts; t++) {
            NaTone o; na_tone_read(&v, t, &o);
            printf("   tone %2d: vol=%d pan=%d centre=%d shift=%d note[%d..%d] adsr=%04x/%04x prog=%d vag=%d\n",
                   t, o.vol, o.pan, o.centre, o.shift, o.nmin, o.nmax, o.adsr1, o.adsr2, o.prog, o.vag);
        }
        return 0;
    }
    if (!strcmp(cmd, "vag")) {                    // raw-decode one VAG (inspection)
        int vi = atoi(argv[3]), gi = atoi(argv[4]); const char* out = argv[5];
        NaVab v; na_vab_open_at(&v, g_buf, vab_file_off(vi));
        static int16_t pcm[8*1024*1024];
        int ls, lp; int n = na_vab_decode_vag(&v, gi, pcm, sizeof(pcm)/2, &ls, &lp);
        fprintf(stderr,"[snd_render] VAB%d VAG%d -> %d samples (loop_start=%d loops=%d)\n", vi, gi, n, ls, lp);
        wav_write(out, pcm, n, 1, 44100);
        return 0;
    }
    if (!strcmp(cmd, "song")) {
        int si = atoi(argv[3]), vi = atoi(argv[4]); const char* out = argv[5];
        int seconds = argc > 6 ? atoi(argv[6]) : 30;
        return render_song(si, vi, out, seconds);
    }
    if (!strcmp(cmd, "songid")) {
        if (argc < 5) { fprintf(stderr,"usage: %s <SND> songid <id> <out> [seconds]\n", argv[0]); return 1; }
        int id = atoi(argv[3]); const char* out = argv[4];
        int seconds = argc > 5 ? atoi(argv[5]) : 30;
        static const struct { int seq, vab; } S2SV[10] = {
            {3,13},{2,13},{1,13},{0,13},   // songs 0..3: bank 14 -> vab idx 13
            {4, 7},{5, 7},{6, 7},           // songs 4..6 (dialog): bank 8 -> vab idx 7
            {7,13},{8,13},{9,13},           // songs 7..9: bank 14 -> vab idx 13
        };
        if (id < 0 || id >= 10) { fprintf(stderr,"songid %d out of range 0..9\n", id); return 1; }
        fprintf(stderr,"[snd_render] songid %d -> seq%d vab%d (bank %d)\n",
                id, S2SV[id].seq, S2SV[id].vab, S2SV[id].vab+1);
        return render_song(S2SV[id].seq, S2SV[id].vab, out, seconds);
    }
    if (!strcmp(cmd, "raw")) {
        if (argc < 6) { fprintf(stderr,"usage: %s <file> raw <seqOff_hex> <vabOff_hex> <out> [seconds]\n", argv[0]); return 1; }
        long seqOff = strtol(argv[3],0,16), vabOff = strtol(argv[4],0,16);
        const char* out = argv[5]; int seconds = argc > 6 ? atoi(argv[6]) : 30;
        return render_raw(seqOff, vabOff, out, seconds);
    }
    fprintf(stderr,"unknown cmd %s\n", cmd); return 1;
}
