// engine/audio/music_list.c — see music_list.h.
//
// Loads scratch/bin/snd/TOMBA2.SND once and exposes the 10 BGM songs (the §6 S2SV table) as
// playable tracks. Each song resolves to (seqOff, vabOff): seqOff by linear 'pQES' scan to the
// song's scan-index, vabOff from the container's VAB offset table. native_music_play() drives the
// shared synth from those offsets.
#include "music_list.h"
#include "native_music.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Catalogue: track -> (seq scan-index in TOMBA2.SND, vab scan-index, name).
//
// The seq scan-index is RE'd from MAIN.EXE (spec §6 S2SV). The VAB column is the EMPIRICALLY
// AUDIBLE bank, NOT the documented §6 bank-slot: §6.4 warns the bank<->VAB-body binding is
// runtime/area-bound, so the spec's "bank 14 -> vab 13" slot numbers don't address a TOMBA2.SND
// VAB that actually covers the song's notes. Sweeping all 24 container VABs per seq (tools/
// snd_render song <seq> <vab>) showed seqs 4-8 sound with these VABs; seqs 0-3 & 9 (the short
// jingles) produce NO audio with ANY TOMBA2.SND VAB — their notes fall outside every container
// bank's 1-VAG-per-semitone range, so they need the per-AREA VABs (AREA_BGM.bin) which only ship
// for the loaded area. Those are flagged "(needs area VAB)" until the area-VAB loader lands.
static const struct { int seq, vab; const char* name; } S2SV[10] = {
    {3,  7, "Song 0 (jingle, needs area VAB)"},
    {2,  7, "Song 1 (jingle, needs area VAB)"},
    {1,  7, "Song 2 (jingle, needs area VAB)"},
    {0,  7, "Song 3 (jingle, needs area VAB)"},
    {4,  3, "Song 4 (cue)"},
    {5, 21, "Song 5 (cue)"},
    {6, 21, "Song 6 (cue)"},
    {7, 21, "Song 7 (field)"},
    {8, 21, "Song 8 (field)"},
    {9,  7, "Song 9 (field, needs area VAB)"},
};

static uint8_t* s_buf;     // TOMBA2.SND contents
static long     s_size;
static int      s_now = -1;

static int load_container(void) {
    if (s_buf) return 0;
    const char* path = "scratch/bin/snd/TOMBA2.SND";
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[music_list] cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); s_size = ftell(f); fseek(f, 0, SEEK_SET);
    s_buf = (uint8_t*)malloc(s_size);
    if (!s_buf || fread(s_buf, 1, s_size, f) != (size_t)s_size) {
        fprintf(stderr, "[music_list] read failed %s\n", path); fclose(f);
        free(s_buf); s_buf = NULL; return -1;
    }
    fclose(f);
    fprintf(stderr, "[music_list] loaded %s (%ld bytes)\n", path, s_size);
    return 0;
}

// byte offset of the si'th 'pQES' (linear scan from 0x30), or -1.
static long seq_off(int si) {
    long o = 0x30; int idx = 0;
    while (o < 0x51800 && o + 4 <= s_size) {
        if (!memcmp(s_buf + o, "pQES", 4)) { if (idx == si) return o; idx++; o += 4; }
        else o++;
    }
    return -1;
}

static long vab_off(int vi) { return (long)(s_buf[vi*2] | (s_buf[vi*2+1] << 8)) * 0x800; }

int music_list_count(void) { return 10; }

const char* music_list_name(int i) {
    if (i < 0 || i >= 10) return NULL;
    return S2SV[i].name;
}

int music_list_play(int i) {
    if (i < 0 || i >= 10) return -1;
    if (load_container()) return -1;
    long so = seq_off(S2SV[i].seq), vo = vab_off(S2SV[i].vab);
    if (so < 0) { fprintf(stderr, "[music_list] song %d: seq not found\n", i); return -1; }
    if (native_music_play(s_buf, so, vo)) { fprintf(stderr, "[music_list] song %d: play failed\n", i); return -1; }
    s_now = i;
    fprintf(stderr, "[music_list] playing song %d (seq@0x%lx vab@0x%lx)\n", i, so, vo);
    return 0;
}

void music_list_stop(void) { native_music_stop(); s_now = -1; }

int music_list_now_playing(void) {
    if (s_now >= 0 && !native_music_active()) s_now = -1;
    return s_now;
}
